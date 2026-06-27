/* adpcm.h - 4-bit IMA/DVI ADPCM audio codec (portable C99, shared).
 *
 * The audio track for the .tvid container: ~8 kHz mono, 4 bits/sample, so a
 * 3-minute song lands in well under a megabyte (0.5 byte/sample + tiny per-block
 * headers). IMA ADPCM is the classic Sound Blaster choice - the decoder is
 * integer-only, two small constant tables, no float, no allocation - which keeps
 * it inside the decoder-asymmetry rule (it ships to 16-bit DOS).
 *
 * Stream layout: a sequence of self-contained blocks. Each block resets the
 * predictor/step state from its own header, so the decoder can start at any
 * block boundary and a lost block self-heals on the next one. This matters for
 * the DOS auto-init DMA path, which decodes one block per half-buffer interrupt.
 *
 * Block layout (little-endian):
 *   [s16  predictor]   initial predicted sample
 *   [u8   step_index]  initial step table index (0..88)
 *   [u8   reserved=0]
 *   [u8   nibbles[]]   2 samples per byte (low nibble first), ADPCM_BLOCK_SAMPLES
 *                      samples total. The first sample of the block IS the
 *                      predictor (not nibble-coded); the nibbles code samples
 *                      1..ADPCM_BLOCK_SAMPLES-1, so a full block holds
 *                      (ADPCM_BLOCK_SAMPLES-1) nibbles.
 */
#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Samples decoded per full block. 2041 samples => 1 header (4 B) + 2040 nibbles
 * (1020 B) = 1024 B/block, a clean DMA-friendly size; the decoded half-buffer is
 * 2041 samples. The final block may be short. */
#define ADPCM_BLOCK_SAMPLES 2041
#define ADPCM_BLOCK_HEADER 4
/* Bytes a full block occupies on disk (header + ceil((SAMPLES-1)/2) nibbles). */
#define ADPCM_BLOCK_BYTES (ADPCM_BLOCK_HEADER + (ADPCM_BLOCK_SAMPLES - 1 + 1) / 2)

/* Encoded size in bytes for nsamples PCM samples (whole-stream). Offline use. */
long adpcm_encoded_size(long nsamples);

/* Encode nsamples of s16 PCM in[] into out[] (capacity out_cap) as a sequence of
 * IMA blocks. Returns bytes written, or -1 if it would not fit. Offline use. */
long adpcm_encode(const int16_t *in, long nsamples, uint8_t *out, long out_cap);

/* Decode one block from in[0..in_len) into out[], producing up to max_out
 * samples. Returns the number of samples produced (== samples coded by the
 * block, <= ADPCM_BLOCK_SAMPLES), or -1 on malformed input / overflow. The
 * decoder ships to DOS: integer-only, no allocation. */
int adpcm_decode_block(const uint8_t *in, long in_len, int16_t *out, int max_out);

/* Bytes consumed on disk by a block that codes `samples` PCM samples (so the
 * caller can advance to the next block). samples must be 1..ADPCM_BLOCK_SAMPLES. */
long adpcm_block_bytes(int samples);

#ifdef __cplusplus
}
#endif

#endif /* ADPCM_H */
