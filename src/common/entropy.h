/* entropy.h - LZSS + static-Huffman entropy back-end (DEFLATE-class, shared).
 *
 * The whole-stream LZSS in lzss.c packs matches/literals into bytes with no
 * entropy stage, so its output still carries ~order-0 redundancy (measured ~73%
 * of raw, vs gzip ~61%). This back-end runs the same LZ77 match search but emits
 * DEFLATE-style symbols into two alphabets - literal/length and distance - and
 * codes each with a canonical static Huffman table whose code *lengths* are sent
 * in the header. The decoder rebuilds the canonical codes and decodes by table
 * lookup (huffman.c): no float, tiny tables, DOS-feasible.
 *
 * Layout of a compressed blob:
 *   [u8 litlen code-length table, RLE]  (ENT_NLITLEN entries)
 *   [u8 dist   code-length table, RLE]  (ENT_NDIST entries)
 *   [Huffman bitstream: litlen symbols; matches followed by dist code + extra]
 * An explicit ENT_SYM_END litlen symbol terminates the stream. */
#ifndef ENTROPY_H
#define ENTROPY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compress in[0..n) into out (capacity out_cap). Returns compressed length, or
 * -1 if it would not fit / on error. Offline (encoder) use. */
long entropy_compress(const uint8_t *in, long n, uint8_t *out, long out_cap);

/* As entropy_compress but with the LZ match search disabled: a pure order-0
 * static-Huffman coder (every byte a literal). Wins on planes where LZ
 * tokenization compresses worse than the raw bytes (e.g. the mono cell plane,
 * measured -3.6% vs the LZ path). The blob is an ordinary method-2 stream;
 * entropy_decompress decodes it unchanged. */
long entropy_compress_nolz(const uint8_t *in, long n, uint8_t *out, long out_cap);

/* Decompress in[0..n) into out (capacity out_cap, must hold the full original).
 * Returns the decompressed length, or -1 on malformed input / overflow. */
long entropy_decompress(const uint8_t *in, long n, uint8_t *out, long out_cap);

#ifdef __cplusplus
}
#endif

#endif /* ENTROPY_H */
