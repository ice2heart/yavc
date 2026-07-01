/* range_ctx_internal.h - step-index-context 16-symbol (nibble) adaptive range
 * coder, shared bit-exactly between the compressor (range_ctx_enc.cpp) and the
 * decompressor (range_ctx_dec.c). This is the codec-3 audio back-end: it codes the
 * IMA-ADPCM nibble stream directly, with a model selected by a context the decoder
 * reconstructs for free -- the running ADPCM step index and the previous nibble --
 * instead of the generic order-1 *byte* coder (range_internal.h) that is blind to
 * both. See doc/audiocodec-evo.md "codec 3".
 *
 * The range-coder mechanics (INC=8, CAP=2^16, TOP/BOT, renorm) are identical to
 * range_internal.h, so codec 3 is apples-to-apples with codec 2's method 3; only
 * the alphabet (16 vs 256) and the context selection differ. The model parameters
 * ARE the wire format -- if the two halves disagree, decode breaks silently, so
 * they live here once. Defined static inline so each translation unit (incl. the
 * portable-C99 decoder that ships to 16-bit DOS) gets an identical private copy.
 * NOT a public header; callers use range_ctx.h.
 *
 * Context = (step_index >> 3 bucket, clamped to 11) * 16 + previous nibble
 *         = 12 buckets * 16 nibbles = 192 contexts.
 * The step index evolves identically on both sides (adpcm_index_table, reseeding to
 * 0 at each block start) and the previous nibble is the last code coded, so the
 * context needs ZERO new decoder state (adpcm_dec.c already tracks the index) -- the
 * decisive difference from the shelved CABAC split lever. Measured group-size-
 * insensitive (holds its win at the K=256 grouping); see doc/audiocodec-evo.md. */
#ifndef RANGE_CTX_INTERNAL_H
#define RANGE_CTX_INTERNAL_H

#include <stdint.h>

#include "adpcm.h"
#include "adpcm_internal.h" /* adpcm_step_table / adpcm_index_table / adpcm_clampi */

#define RANGE_CTX_INC   8u          /* per-symbol frequency increment            */
#define RANGE_CTX_CAP   (1u << 16)  /* rescale when a context's total reaches this */
#define RANGE_CTX_TOP   (1u << 24)
#define RANGE_CTX_BOT   (1u << 16)

#define RANGE_CTX_BUCKETS 12        /* step-index buckets: (index >> 3), 0..11     */
#define RANGE_CTX_NCTX    (RANGE_CTX_BUCKETS * 16) /* buckets * 16 prev-nibbles    */

/* Map the running (step_index, prev_nibble) to a context slot. Both sides call
 * this with values they reconstruct identically. */
static inline int range_ctx_select(int index, int prev_nibble) {
    int b = index >> 3;
    if (b > RANGE_CTX_BUCKETS - 1) b = RANGE_CTX_BUCKETS - 1;
    return b * 16 + prev_nibble;
}

/* ---- adaptive 16-symbol (nibble) frequency model (one per context) ---- */
typedef struct { uint16_t f[16]; uint32_t tot; } CtxModel;

static inline void ctx_model_init(CtxModel *m) {
    for (int i = 0; i < 16; i++) m->f[i] = 1;
    m->tot = 16;
}
static inline void ctx_model_bump(CtxModel *m, int sym) {
    m->f[sym] = (uint16_t)(m->f[sym] + RANGE_CTX_INC);
    m->tot += RANGE_CTX_INC;
    if (m->tot >= RANGE_CTX_CAP) {
        uint32_t t = 0;
        for (int i = 0; i < 16; i++) { m->f[i] = (uint16_t)((m->f[i] >> 1) | 1); t += m->f[i]; }
        m->tot = t;
    }
}

#endif /* RANGE_CTX_INTERNAL_H */
