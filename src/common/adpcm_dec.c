/* adpcm_dec.c - 4-bit IMA/DVI ADPCM decode (portable C99, shared). Integer-only
 * and table-driven so it builds and runs on 16-bit DOS unchanged. The matching
 * encode side lives in src/encoder/adpcm_enc.cpp (encoder-only). Tables/clamp are
 * shared via adpcm_internal.h. See adpcm.h for the stream/block layout. */
#include "adpcm.h"
#include "adpcm_internal.h"

long adpcm_block_bytes(int samples) {
    if (samples <= 0 || samples > ADPCM_BLOCK_SAMPLES) return -1;
    /* header + nibbles for (samples-1) coded samples, packed 2 per byte. */
    return ADPCM_BLOCK_HEADER + (long)((samples - 1) + 1) / 2;
}

int adpcm_decode_block(const uint8_t *in, long in_len, int16_t *out, int max_out) {
    if (in_len < ADPCM_BLOCK_HEADER) return -1;
    int predictor = (int16_t)(in[0] | (in[1] << 8));
    int index = in[2];
    if (index < 0 || index > 88) return -1;

    long nibbles_avail = (in_len - ADPCM_BLOCK_HEADER) * 2;
    /* Samples coded = 1 (the seed) + however many nibbles are present, capped at
     * a full block. An odd sample count leaves one padding nibble, so the byte
     * count can imply one extra "sample" - max_out (what the caller actually
     * wants from this block) is the authority and trims that padding. */
    long coded = 1 + nibbles_avail;
    if (coded > ADPCM_BLOCK_SAMPLES) coded = ADPCM_BLOCK_SAMPLES;
    if (coded > max_out) coded = max_out;
    if (max_out <= 0) return -1;

    int n = 0;
    out[n++] = (int16_t)predictor;
    long bytepos = ADPCM_BLOCK_HEADER;
    int high = 0;
    uint8_t cur = 0;
    while (n < coded) {
        int code;
        if (!high) { cur = in[bytepos++]; code = cur & 0x0F; high = 1; }
        else { code = (cur >> 4) & 0x0F; high = 0; }

        int step = adpcm_step_table[index];
        int diffq = step >> 3;
        if (code & 4) diffq += step;
        if (code & 2) diffq += step >> 1;
        if (code & 1) diffq += step >> 2;
        if (code & 8) predictor -= diffq; else predictor += diffq;
        predictor = adpcm_clampi(predictor, -32768, 32767);
        index = adpcm_clampi(index + adpcm_index_table[code], 0, 88);
        out[n++] = (int16_t)predictor;
    }
    return n;
}
