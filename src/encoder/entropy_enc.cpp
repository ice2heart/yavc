/* entropy_enc.cpp - LZSS + static-Huffman entropy compressor (encoder side, C++).
 * DEFLATE-style coder: LZ-parse into a token list gathering symbol frequencies,
 * build canonical Huffman tables, then emit. Moved out of src/common/ so the
 * player set carries only entropy_dec.c. Alphabet sizes are shared with the
 * decoder via entropy_internal.h. Declared extern "C" (via entropy.h). The heap
 * scratch that was malloc/free in C is std::vector here. */
#include "entropy.h"
#include "entropy_internal.h"

#include "bitstream.h"
#include "huffman.h"
#include "lzss.h"

#include <vector>

namespace {

/* number of significant bits of v (0 -> 0). */
int nbits(unsigned v) { int n = 0; while (v) { n++; v >>= 1; } return n; }

/* code-length table serialization: raw nsym bytes (the tables are small -- 273+13
 * -- and compress under the outer design; simplicity wins over a few dozen bytes.
 * The inverse get_lengths lives in entropy_dec.c). */
long put_lengths(uint8_t *out, long op, long cap, const uint8_t *lengths, int nsym) {
    if (op + nsym > cap) return -1;
    for (int i = 0; i < nsym; ++i) out[op++] = lengths[i];
    return op;
}

constexpr int ENT_HASH_BITS = 15;
constexpr unsigned ENT_HASH_SIZE = 1u << ENT_HASH_BITS;
constexpr int ENT_MAX_CHAIN = 256;

unsigned ent_hash(const uint8_t *p) {
    return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ (unsigned)p[2]) &
           (ENT_HASH_SIZE - 1);
}

/* Core: same DEFLATE-style coder. use_lz=0 disables the LZ match search so every
 * byte is emitted as a literal -- a pure order-0 static-Huffman coder, which wins
 * on planes (e.g. the mono [luma|glyph] cell plane) where the LZ tokenization
 * compresses worse than the raw bytes. The output is an ordinary method-2 blob;
 * entropy_decompress decodes a match-free token stream unchanged. */
long entropy_compress_impl(const uint8_t *in, long n, uint8_t *out,
                           long out_cap, int use_lz) {
    if (n < 0) return -1;

    /* Pass 1: LZ parse into a token list, gathering symbol frequencies. We store
     * the parse so pass 2 can emit it once the Huffman tables are known. */
    std::vector<long> head(ENT_HASH_SIZE, -1);
    std::vector<long> prev((size_t)(n > 0 ? n : 1));
    /* token list: parallel arrays. litsym[k] is a litlen symbol; if it is a match
     * (>= ENT_SYM_LEN0) then dcode[k]/dextra[k] carry the distance. */
    std::vector<int16_t>  litsym((size_t)(n + 1));
    std::vector<uint16_t> dextra((size_t)(n + 1));
    std::vector<uint8_t>  dcode((size_t)(n + 1));

    uint32_t fl[ENT_NLITLEN]; for (int i = 0; i < ENT_NLITLEN; ++i) fl[i] = 0;
    uint32_t fd[ENT_NDIST];   for (int i = 0; i < ENT_NDIST; ++i) fd[i] = 0;

    long ntok = 0, i = 0;
    while (i < n) {
        long best_len = 0, best_dist = 0;
        if (use_lz && i + LZSS_MIN_MATCH <= n) {
            unsigned h = ent_hash(&in[i]);
            long cand = head[h];
            int chain = 0;
            long min_pos = i - LZSS_WINDOW; if (min_pos < 0) min_pos = 0;
            while (cand >= min_pos && chain < ENT_MAX_CHAIN) {
                long len = 0, maxlen = n - i;
                if (maxlen > LZSS_MAX_MATCH) maxlen = LZSS_MAX_MATCH;
                while (len < maxlen && in[cand + len] == in[i + len]) len++;
                if (len > best_len) {
                    best_len = len; best_dist = i - cand;
                    if (len >= LZSS_MAX_MATCH) break;
                }
                cand = prev[cand]; chain++;
            }
        }

        if (best_len >= LZSS_MIN_MATCH) {
            int lsym = ENT_SYM_LEN0 + (int)(best_len - LZSS_MIN_MATCH);
            unsigned d1 = (unsigned)(best_dist - 1);
            /* dc = number of significant bits of d1; d1 < 2^dc, so the low dc
             * bits hold d1 exactly and the decoder rebuilds d1 = extra. */
            int dc = nbits(d1);                 /* 0..12 */
            litsym[ntok] = (int16_t)lsym;
            dcode[ntok]  = (uint8_t)dc;
            dextra[ntok] = (uint16_t)d1;
            fl[lsym]++; fd[dc]++;
            ntok++;
            long end = i + best_len;
            for (; i < end; ++i)
                if (i + LZSS_MIN_MATCH <= n) {
                    unsigned hh = ent_hash(&in[i]);
                    prev[i] = head[hh]; head[hh] = i;
                }
        } else {
            litsym[ntok] = (int16_t)in[i];
            dcode[ntok] = 0; dextra[ntok] = 0;
            fl[in[i]]++;
            ntok++;
            if (use_lz && i + LZSS_MIN_MATCH <= n) {
                unsigned hh = ent_hash(&in[i]);
                prev[i] = head[hh]; head[hh] = i;
            }
            i++;
        }
    }
    /* End-of-stream symbol. */
    fl[ENT_SYM_END]++;

    /* Build canonical code lengths + codes for both alphabets. */
    uint8_t llen[ENT_NLITLEN], dlen[ENT_NDIST];
    uint16_t lcode[ENT_NLITLEN], dcode_tab[ENT_NDIST];
    huff_build_lengths(fl, ENT_NLITLEN, llen);
    huff_build_lengths(fd, ENT_NDIST, dlen);
    huff_build_codes(llen, ENT_NLITLEN, lcode);
    huff_build_codes(dlen, ENT_NDIST, dcode_tab);

    /* Pass 2: serialize. Header tables, then the Huffman bitstream. */
    long op = 0;
    op = put_lengths(out, op, out_cap, llen, ENT_NLITLEN);
    if (op < 0) return -1;
    op = put_lengths(out, op, out_cap, dlen, ENT_NDIST);
    if (op < 0) return -1;

    bitwriter w;
    bw_init(&w, out + op, out_cap - op);
    for (long k = 0; k < ntok; ++k) {
        int ls = litsym[k];
        bw_bits(&w, lcode[ls], llen[ls]);
        if (ls >= ENT_SYM_LEN0) {
            int dc = dcode[k];
            bw_bits(&w, dcode_tab[dc], dlen[dc]);
            if (dc) bw_bits(&w, (uint32_t)(dextra[k] & ((1u << dc) - 1)), dc);
        }
    }
    bw_bits(&w, lcode[ENT_SYM_END], llen[ENT_SYM_END]);
    if (w.overflow) return -1;
    op += bw_len(&w);
    return op;
}

} // namespace

long entropy_compress(const uint8_t *in, long n, uint8_t *out, long out_cap) {
    return entropy_compress_impl(in, n, out, out_cap, 1);
}

long entropy_compress_nolz(const uint8_t *in, long n, uint8_t *out, long out_cap) {
    return entropy_compress_impl(in, n, out, out_cap, 0);
}
