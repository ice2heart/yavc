/* range.h - adaptive order-1 range coder (plane method 3, shared).
 *
 * A binary-exact alternative back-end to the static-Huffman entropy.c, used as a
 * per-plane auto-selected candidate (the [u8 method] in a split-body plane: 3 =
 * range-coded). Unlike the DEFLATE-class coder it spends *fractional* bits per
 * symbol and adapts a per-context (previous-byte) frequency model in lockstep on
 * both sides, so no model table is serialized. On the mono cell plane this beats
 * both our Huffman path and xz -9 (doc/compression.md, entropy methods).
 *
 * Model: 256 contexts keyed on the previous byte; each a 256-symbol adaptive
 * frequency table, increment 8, rescale at total >= 2^16. The decoder mirrors the
 * encoder's updates exactly. Carryless 32-bit Subbotin-style range coder, byte
 * renormalized. No float, small fixed tables -> DOS/decoder-budget friendly.
 *
 * The blob carries no header beyond what the plane chunk already stores (raw_len
 * is known from the chunk); decompress stops after producing raw_len bytes. */
#ifndef RANGE_H
#define RANGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compress in[0..n) into out (capacity out_cap). Returns compressed length, or
 * -1 if it would not fit / on error. Offline (encoder) use. */
long range_compress(const uint8_t *in, long n, uint8_t *out, long out_cap);

/* Decompress exactly out_len bytes from in[0..n) into out (capacity out_len).
 * Returns out_len on success, or -1 on malformed input / overflow. The caller
 * knows the original length from the plane chunk's raw_len. */
long range_decompress(const uint8_t *in, long n, uint8_t *out, long out_len);

#ifdef __cplusplus
}
#endif

#endif /* RANGE_H */
