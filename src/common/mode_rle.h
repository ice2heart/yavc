/* mode_rle.h - zero-run-length codec for the SPLIT mode-tag plane (portable C).
 *
 * The mode plane is one byte per quadtree leaf, values 0..3, overwhelmingly 0
 * (SKIP). Huffman floors every symbol at 1 bit, so a long SKIP run still costs
 * ~1 bit/leaf; collapsing the runs first beats that floor (TVID_FLAG_MODERLE).
 *
 * Encoding: every byte is emitted literally; immediately after a 0 byte comes a
 * count byte of *additional* consecutive zeros (0..255). Runs longer than 256
 * are split into multiple (0, count) pairs. Non-zero bytes carry no count.
 * Shared so the C++ encoder and the C decoder cannot drift. */
#ifndef MODE_RLE_H
#define MODE_RLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded length of a zero-RLE buffer in[0..n), or -1 if it is malformed (a
 * trailing 0 with no count byte). Pure size pass; no output written. */
static inline long mode_rle_decoded_len(const uint8_t *in, long n) {
    long total = 0, i = 0;
    while (i < n) {
        uint8_t v = in[i++];
        if (v == 0) {
            if (i >= n) return -1;        /* run count byte is missing */
            total += 1 + (long)in[i++];   /* the literal 0 plus the run */
        } else {
            total += 1;
        }
    }
    return total;
}

/* Expand zero-RLE in[0..n) into out (must hold mode_rle_decoded_len bytes).
 * Returns the number of bytes written, or -1 on malformed input. */
static inline long mode_rle_decode(const uint8_t *in, long n, uint8_t *out) {
    long o = 0, i = 0;
    while (i < n) {
        uint8_t v = in[i++];
        out[o++] = v;
        if (v == 0) {
            if (i >= n) return -1;
            int run = in[i++];
            while (run--) out[o++] = 0;
        }
    }
    return o;
}

/* Encode src[0..n) as zero-RLE into out (must hold at least 2*n bytes worst
 * case). Returns the encoded length. */
static inline long mode_rle_encode(const uint8_t *src, long n, uint8_t *out) {
    long o = 0, i = 0;
    while (i < n) {
        uint8_t v = src[i++];
        out[o++] = v;
        if (v == 0) {
            long run = 0;
            while (i < n && src[i] == 0 && run < 255) { ++i; ++run; }
            out[o++] = (uint8_t)run;
        }
    }
    return o;
}

#ifdef __cplusplus
}
#endif

#endif /* MODE_RLE_H */
