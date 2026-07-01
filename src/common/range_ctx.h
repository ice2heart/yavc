/* range_ctx.h - step-index-context nibble range coder for IMA-ADPCM (codec 3).
 *
 * The codec-3 audio back-end (doc/audiocodec-evo.md). It replaces codec 2's generic
 * order-1 *byte* coder (range.h, method 3) for one chunk of ADPCM blocks with a
 * 16-symbol *nibble* coder whose per-symbol model is selected by the ADPCM step
 * index and the previous nibble -- a context the decoder reconstructs for free (the
 * step index already evolves inside adpcm_decode_block; range_ctx_internal.h).
 *
 * Both sides walk the chunk's ADPCM blocks in lockstep with the ADPCM state machine
 * (reseeding index=0 / prev=0 at each block start), so decode rebuilds the EXACT
 * codec-1 ADPCM block bytes -- header + packed nibbles -- byte-for-byte, and the
 * unchanged adpcm_decode_block then yields PCM bit-identical to codec 1. The chunk
 * is stored as split-body plane method 4 inside the codec-3 audio tail.
 *
 * `samples` is the PCM sample count coded by the chunk (1..GROUP*ADPCM_BLOCK_SAMPLES);
 * it tells both sides how many blocks / nibbles the chunk holds. The block headers
 * ([s16 predictor][u8 index][u8 reserved]) are NOT entropy-coded here -- they are
 * copied verbatim -- only the nibble payload is (that is where the entropy is). */
#ifndef RANGE_CTX_H
#define RANGE_CTX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compress the ADPCM blob in[0..n) (a run of whole blocks coding `samples` PCM
 * samples) into out (capacity out_cap). Returns the compressed length, or -1 if it
 * would not fit / on malformed input. Offline (encoder) use. */
long range_ctx_compress_adpcm(const uint8_t *in, long n, long samples,
                              uint8_t *out, long out_cap);

/* Decompress a codec-3 chunk from in[0..n) back into out (capacity adpcm_len),
 * reconstructing the exact ADPCM block bytes for `samples` PCM samples. Returns
 * adpcm_len on success, or -1 on malformed input / overflow. The caller knows
 * adpcm_len and samples from the chunk header. */
long range_ctx_decompress_adpcm(const uint8_t *in, long n, long samples,
                                uint8_t *out, long adpcm_len);

#ifdef __cplusplus
}
#endif

#endif /* RANGE_CTX_H */
