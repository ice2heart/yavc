/* huffman.h - canonical static Huffman over a small alphabet (portable C, shared).
 *
 * Used by the entropy back-end to code the LZSS token alphabets (DEFLATE-style:
 * one tree for literals+lengths, one for distances). The encoder (offline) builds
 * an optimal-ish length-limited code; only the per-symbol *code lengths* travel in
 * the stream, and both sides rebuild the identical canonical code from them, so the
 * decoder is pure table lookup - no float, DOS-feasible.
 *
 * Canonical rule: symbols are assigned codes in order of (length, symbol index);
 * shorter codes first, ties broken by symbol value. This is the same convention as
 * DEFLATE, so the code lengths alone fully determine every code. */
#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max code length the builder will produce (length-limited). 15 matches DEFLATE
 * and keeps decode tables small. */
#define HUFF_MAX_BITS 15

/* Build canonical code lengths for `nsym` symbols from their frequencies.
 * freq[i] is the count of symbol i (0 allowed). Writes lengths[i] in [0,HUFF_MAX_BITS];
 * a symbol with freq 0 gets length 0 (no code). Lengths are length-limited to
 * HUFF_MAX_BITS. Returns the number of symbols with a nonzero code. */
int huff_build_lengths(const uint32_t *freq, int nsym, uint8_t *lengths);

/* From canonical code lengths, fill codes[i] with the canonical code (right-
 * aligned, lengths[i] bits, MSB-first when emitted). Symbols with length 0 are
 * left as 0. Shared by encoder (to emit) and is the basis of the decode tables. */
void huff_build_codes(const uint8_t *lengths, int nsym, uint16_t *codes);

/* ---- decode side: a flat first-bits lookup table for fast canonical decode ----
 * The table is indexed by the next HUFF_DECODE_BITS peeked bits. Each entry gives
 * the decoded symbol and the real code length to consume. Codes longer than
 * HUFF_DECODE_BITS fall back to a slow bit-by-bit walk (rare for these alphabets).
 */
#define HUFF_DECODE_BITS 10
#define HUFF_DECODE_SIZE (1 << HUFF_DECODE_BITS)

typedef struct {
    int16_t  sym; /* decoded symbol, or -1 if the prefix needs the slow walk */
    uint8_t  len; /* bits to consume (valid when sym >= 0)                   */
} huff_dentry;

typedef struct {
    huff_dentry fast[HUFF_DECODE_SIZE];
    /* For the slow path: first code and symbol base per length (canonical). */
    uint16_t first_code[HUFF_MAX_BITS + 1];
    uint16_t first_sym[HUFF_MAX_BITS + 1];
    uint16_t count[HUFF_MAX_BITS + 1];
    uint16_t sorted[512]; /* symbols sorted by (len,sym); >= max alphabet here */
    int      nsym;
} huff_decoder;

/* Prepare a decoder from canonical code lengths (built/sent by the encoder). */
void huff_decoder_init(huff_decoder *d, const uint8_t *lengths, int nsym);

#ifdef __cplusplus
}
#endif

#endif /* HUFFMAN_H */
