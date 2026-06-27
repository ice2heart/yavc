/* adpcm_enc.cpp - 4-bit IMA/DVI ADPCM encode (encoder-side, C++). The encoder is
 * unconstrained C++; this half moved out of src/common/ so the player/decoder
 * compile set carries only adpcm_dec.c. The wire-format tables/clamp are shared
 * bit-exactly with the decoder via adpcm_internal.h, so round-trip error stays
 * bounded by quantization. Declared extern "C" (via adpcm.h) for C callers. */
#include "adpcm.h"
#include "adpcm_internal.h"

namespace {

/* Encode one 4-bit code from a sample, updating (predictor, index). Mirrors the
 * decoder exactly so round-trip error stays bounded by quantization. */
int encode_sample(int sample, int *predictor, int *index) {
    int step = adpcm_step_table[*index];
    int diff = sample - *predictor;
    int code = 0;
    int diffq = step >> 3;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; diffq += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; diffq += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; diffq += step; }

    if (code & 8) *predictor -= diffq; else *predictor += diffq;
    *predictor = adpcm_clampi(*predictor, -32768, 32767);
    *index = adpcm_clampi(*index + adpcm_index_table[code], 0, 88);
    return code & 0x0F;
}

} // namespace

long adpcm_encoded_size(long nsamples) {
    long total = 0;
    while (nsamples > 0) {
        int n = nsamples > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)nsamples;
        long bb = adpcm_block_bytes(n);
        if (bb < 0) return -1;
        total += bb;
        nsamples -= n;
    }
    return total;
}

long adpcm_encode(const int16_t *in, long nsamples, uint8_t *out, long out_cap) {
    long op = 0;
    long i = 0;
    while (i < nsamples) {
        int n = (nsamples - i) > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES
                                                     : (int)(nsamples - i);
        long bb = adpcm_block_bytes(n);
        if (bb < 0 || op + bb > out_cap) return -1;

        /* Block header: first sample is the seed predictor, index starts at 0. */
        int predictor = in[i];
        int index = 0;
        out[op++] = (uint8_t)(predictor & 0xFF);
        out[op++] = (uint8_t)((predictor >> 8) & 0xFF);
        out[op++] = (uint8_t)index;
        out[op++] = 0; /* reserved */

        /* Code samples 1..n-1, two nibbles per byte (low nibble = earlier). */
        int j;
        int have_low = 0;
        uint8_t pending = 0;
        for (j = 1; j < n; ++j) {
            int code = encode_sample(in[i + j], &predictor, &index);
            if (!have_low) { pending = (uint8_t)code; have_low = 1; }
            else { out[op++] = (uint8_t)(pending | (code << 4)); have_low = 0; }
        }
        if (have_low) out[op++] = pending; /* odd count: high nibble zero-padded */
        i += n;
    }
    return op;
}
