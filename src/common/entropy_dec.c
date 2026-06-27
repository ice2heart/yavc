/* entropy_dec.c - LZSS + static-Huffman entropy decode (see entropy.h). Portable
 * C99, part of the player/decoder set. Decodes both the LZ (method 2) and the
 * match-free no-LZ blobs unchanged -- a no-LZ stream is just a token stream with
 * no match symbols, so no decoder branch is needed. The compressor is encoder-
 * only and lives in src/encoder/entropy_enc.cpp. Alphabet sizes are shared via
 * entropy_internal.h. */
#include "entropy.h"
#include "entropy_internal.h"

#include "bitstream.h"
#include "huffman.h"
#include "lzss.h"

#include <stdlib.h>

/* ---- code-length table deserialization: raw nsym bytes (see entropy_enc.c) ---- */
static long get_lengths(const uint8_t *in, long ip, long n,
                        uint8_t *lengths, int nsym) {
    if (ip + nsym > n) return -1;
    for (int i = 0; i < nsym; ++i) lengths[i] = in[ip++];
    return ip;
}

long entropy_decompress(const uint8_t *in, long n, uint8_t *out, long out_cap) {
    long ip = 0;
    uint8_t llen[ENT_NLITLEN], dlen[ENT_NDIST];
    ip = get_lengths(in, ip, n, llen, ENT_NLITLEN); if (ip < 0) return -1;
    ip = get_lengths(in, ip, n, dlen, ENT_NDIST);   if (ip < 0) return -1;

    huff_decoder *ld = (huff_decoder *)malloc(sizeof(huff_decoder));
    huff_decoder *dd = (huff_decoder *)malloc(sizeof(huff_decoder));
    if (!ld || !dd) { free(ld); free(dd); return -1; }
    huff_decoder_init(ld, llen, ENT_NLITLEN);
    huff_decoder_init(dd, dlen, ENT_NDIST);

    bitreader r;
    br_init(&r, in + ip, n - ip);

    long op = 0;
    for (;;) {
        /* Decode one litlen symbol via the fast table, fall back to slow walk. */
        int sym;
        {
            /* Peek HUFF_DECODE_BITS without consuming. */
            long save_byte = r.byte; int save_bit = r.bit;
            uint32_t peek = br_bits(&r, HUFF_DECODE_BITS);
            r.byte = save_byte; r.bit = save_bit;
            huff_dentry e = ld->fast[peek];
            if (e.sym >= 0) { sym = e.sym; br_bits(&r, e.len); }
            else {
                /* slow canonical walk */
                uint32_t code = 0; int len = 0; sym = -1;
                while (len < HUFF_MAX_BITS) {
                    code = (code << 1) | (uint32_t)br_bit(&r); len++;
                    if (ld->count[len] &&
                        code < (uint32_t)(ld->first_code[len] + ld->count[len]) &&
                        code >= ld->first_code[len]) {
                        sym = ld->sorted[ld->first_sym[len] + (code - ld->first_code[len])];
                        break;
                    }
                }
                if (sym < 0) { free(ld); free(dd); return -1; }
            }
        }
        if (sym == ENT_SYM_END) break;
        if (sym < 256) {
            if (op >= out_cap) { free(ld); free(dd); return -1; }
            out[op++] = (uint8_t)sym;
            continue;
        }
        /* Match. */
        int len = (sym - ENT_SYM_LEN0) + LZSS_MIN_MATCH;
        /* Decode distance code. */
        int dc;
        {
            long save_byte = r.byte; int save_bit = r.bit;
            uint32_t peek = br_bits(&r, HUFF_DECODE_BITS);
            r.byte = save_byte; r.bit = save_bit;
            huff_dentry e = dd->fast[peek];
            if (e.sym >= 0) { dc = e.sym; br_bits(&r, e.len); }
            else {
                uint32_t code = 0; int l = 0; dc = -1;
                while (l < HUFF_MAX_BITS) {
                    code = (code << 1) | (uint32_t)br_bit(&r); l++;
                    if (dd->count[l] &&
                        code >= dd->first_code[l] &&
                        code < (uint32_t)(dd->first_code[l] + dd->count[l])) {
                        dc = dd->sorted[dd->first_sym[l] + (code - dd->first_code[l])];
                        break;
                    }
                }
                if (dc < 0) { free(ld); free(dd); return -1; }
            }
        }
        unsigned d1 = 0;
        if (dc) d1 = br_bits(&r, dc);
        long dist = (long)d1 + 1;
        if (dist > op || op + len > out_cap) { free(ld); free(dd); return -1; }
        for (int k = 0; k < len; ++k) { out[op] = out[op - dist]; op++; }
    }

    free(ld); free(dd);
    return op;
}
