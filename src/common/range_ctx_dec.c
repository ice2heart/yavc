/* range_ctx_dec.c - codec-3 audio decompressor: step-index-context nibble range
 * decoder for IMA-ADPCM (portable C99, part of the player/decoder set). The
 * matching encoder is src/encoder/range_ctx_enc.cpp; the model + context selector +
 * range-coder constants are shared bit-exactly via range_ctx_internal.h. Hardened
 * for decode safety (clamps malformed input). See range_ctx.h / doc/audiocodec-evo.md.
 *
 * It reconstructs the EXACT codec-1 ADPCM block bytes for `samples` PCM samples:
 * the block headers are copied verbatim from the front of the chunk, then each
 * block's nibbles are range-decoded (context = (step_index>>3, prev_nibble),
 * re-derived exactly as adpcm_dec.c evolves the index) and re-packed 2/byte,
 * low nibble first, with the final byte's high nibble zero-padded on an odd count --
 * byte-for-byte what adpcm_enc.cpp wrote. The player then runs the unchanged
 * adpcm_decode_block, so decoded PCM is bit-identical to codec 1. This decode path
 * is LIGHTER than codec 2's (16-symbol / 192-context vs 256/256). */
#include "range_ctx.h"
#include "range_ctx_internal.h"

#include <stdlib.h>

typedef struct { const uint8_t *in; long pos, len; uint32_t low, range, code; } CtxDec;

static uint8_t cdec_get(CtxDec *d) { return d->pos < d->len ? d->in[d->pos++] : 0; }
static void cdec_init(CtxDec *d, const uint8_t *in, long len) {
    d->in = in; d->pos = 0; d->len = len;
    d->low = 0; d->range = 0xFFFFFFFFu; d->code = 0;
    for (int i = 0; i < 4; i++) d->code = (d->code << 8) | cdec_get(d);
}
static void cdec_renorm(CtxDec *d) {
    while ((d->low ^ (d->low + d->range)) < RANGE_CTX_TOP ||
           (d->range < RANGE_CTX_BOT && ((d->range = -d->low & (RANGE_CTX_BOT - 1)), 1))) {
        d->code = (d->code << 8) | cdec_get(d);
        d->low <<= 8; d->range <<= 8;
    }
}
static int cdec_sym(CtxDec *d, CtxModel *m) {
    d->range /= m->tot;
    uint32_t target = (d->code - d->low) / d->range;
    if (target >= m->tot) target = m->tot - 1;   /* clamp malformed input */
    uint32_t cum = 0;
    int sym = 0;
    while (sym < 15 && cum + m->f[sym] <= target) { cum += m->f[sym]; sym++; }
    d->low += cum * d->range;
    d->range *= m->f[sym];
    cdec_renorm(d);
    ctx_model_bump(m, sym);
    return sym;
}

long range_ctx_decompress_adpcm(const uint8_t *in, long n, long samples,
                                uint8_t *out, long adpcm_len) {
    if (n < 0 || samples < 0 || adpcm_len < 0) return -1;

    CtxModel *m = (CtxModel *)malloc(sizeof(CtxModel) * RANGE_CTX_NCTX);
    if (!m) return -1;
    for (int i = 0; i < RANGE_CTX_NCTX; i++) ctx_model_init(&m[i]);

    /* Count blocks and locate where the range-coded nibble stream begins: the
     * chunk is [ADPCM_BLOCK_HEADER bytes/block of headers][nibble stream]. */
    long nblocks = 0, rem = samples;
    while (rem > 0) { rem -= rem > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : rem; nblocks++; }
    long hdr_bytes = nblocks * ADPCM_BLOCK_HEADER;
    if (hdr_bytes > n || hdr_bytes > adpcm_len) { free(m); return -1; }

    CtxDec d; cdec_init(&d, in + hdr_bytes, n - hdr_bytes);

    long op = 0;   /* cursor into `out` (the reconstructed ADPCM blob) */
    long hp = 0;   /* cursor into the verbatim header region of `in`   */
    rem = samples;
    while (rem > 0) {
        int sn = rem > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)rem;
        long bb = adpcm_block_bytes(sn);
        if (bb < 0 || op + bb > adpcm_len) { free(m); return -1; }

        /* Copy the 4-byte header verbatim; the step index seeds the context. */
        for (int k = 0; k < ADPCM_BLOCK_HEADER; k++) out[op + k] = in[hp + k];
        int index = out[op + 2];
        if (index < 0 || index > 88) { free(m); return -1; }
        hp += ADPCM_BLOCK_HEADER;

        long nib_byte = op + ADPCM_BLOCK_HEADER;
        int prev = 0;
        int high = 0; uint8_t cur = 0;
        for (int j = 1; j < sn; ++j) {
            int ctx = range_ctx_select(index, prev);
            int code = cdec_sym(&d, &m[ctx]);
            if (!high) { cur = (uint8_t)code; high = 1; }
            else { out[nib_byte++] = (uint8_t)(cur | (code << 4)); high = 0; }
            index = adpcm_clampi(index + adpcm_index_table[code], 0, 88);
            prev = code;
        }
        if (high) out[nib_byte++] = cur; /* odd count: high nibble zero-padded */
        op += bb;
        rem -= sn;
    }
    free(m);
    return op;
}
