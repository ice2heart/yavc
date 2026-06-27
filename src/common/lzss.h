/* lzss.h - whole-stream LZSS (LZ77 + flag bits) for the .tvid frame stream.
 *
 * Decoder-cheap by design: a 4096-byte sliding window, 3..18 byte matches, no
 * Huffman/range stage. The encoder (offline) does the match search; the decoder
 * is a ring-buffer copy loop that fits a weak/DOS-class PC. Portable C, shared. */
#ifndef LZSS_H
#define LZSS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LZSS_WINDOW   4096   /* sliding window / max match distance */
#define LZSS_MIN_MATCH 3     /* shorter "matches" are cheaper as literals */
#define LZSS_MAX_MATCH 18    /* 3 + 4-bit length field */

/* Compress in[0..n) into out (capacity out_cap). Returns compressed length, or
 * -1 if it would not fit out_cap. */
long lzss_compress(const uint8_t *in, long n, uint8_t *out, long out_cap);

/* Decompress in[0..n) into out (capacity out_cap, must hold the full original).
 * Returns the decompressed length, or -1 on malformed input / overflow. */
long lzss_decompress(const uint8_t *in, long n, uint8_t *out, long out_cap);

#ifdef __cplusplus
}
#endif

#endif /* LZSS_H */
