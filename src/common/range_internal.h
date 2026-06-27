/* range_internal.h - adaptive order-1 frequency model + range-coder constants,
 * shared bit-exactly between the compressor (range_enc.cpp) and the decompressor
 * (range_dec.c). Defined static inline so each translation unit gets an identical
 * private copy and the decoder TU stays self-contained (it must build as portable
 * C99 for 16-bit DOS). The model parameters ARE the wire format -- if the two
 * halves disagree, decode breaks silently, so they live here once. NOT a public
 * header; callers use range.h. See range.c history for the tuning rationale. */
#ifndef RANGE_INTERNAL_H
#define RANGE_INTERNAL_H

#include <stdint.h>

#define RANGE_INC   8u          /* per-symbol frequency increment            */
#define RANGE_CAP   (1u << 16)  /* rescale when a context's total reaches this */
#define RANGE_TOP   (1u << 24)
#define RANGE_BOT   (1u << 16)

/* ---- adaptive 256-symbol frequency model (one per context) ---- */
typedef struct { uint16_t f[256]; uint32_t tot; } Model;

static inline void model_init(Model *m) {
    for (int i = 0; i < 256; i++) m->f[i] = 1;
    m->tot = 256;
}
static inline void model_bump(Model *m, int sym) {
    m->f[sym] = (uint16_t)(m->f[sym] + RANGE_INC);
    m->tot += RANGE_INC;
    if (m->tot >= RANGE_CAP) {
        uint32_t t = 0;
        for (int i = 0; i < 256; i++) { m->f[i] = (uint16_t)((m->f[i] >> 1) | 1); t += m->f[i]; }
        m->tot = t;
    }
}

#endif /* RANGE_INTERNAL_H */
