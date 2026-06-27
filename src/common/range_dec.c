/* range_dec.c - adaptive order-1 range decoder (plane method 3). Portable C99,
 * part of the player/decoder set. The matching encoder is in
 * src/encoder/range_enc.cpp. The frequency model + constants are shared bit-
 * exactly via range_internal.h. Hardened for decode safety (clamps malformed
 * input). See range.h. */
#include "range.h"
#include "range_internal.h"

#include <stdlib.h>

typedef struct { const uint8_t *in; long pos, len; uint32_t low, range, code; } Dec;

static uint8_t dec_get(Dec *d) { return d->pos < d->len ? d->in[d->pos++] : 0; }
static void dec_init(Dec *d, const uint8_t *in, long len) {
    d->in = in; d->pos = 0; d->len = len;
    d->low = 0; d->range = 0xFFFFFFFFu; d->code = 0;
    for (int i = 0; i < 4; i++) d->code = (d->code << 8) | dec_get(d);
}
static void dec_renorm(Dec *d) {
    while ((d->low ^ (d->low + d->range)) < RANGE_TOP ||
           (d->range < RANGE_BOT && ((d->range = -d->low & (RANGE_BOT - 1)), 1))) {
        d->code = (d->code << 8) | dec_get(d);
        d->low <<= 8; d->range <<= 8;
    }
}
static int dec_sym(Dec *d, Model *m) {
    d->range /= m->tot;
    uint32_t target = (d->code - d->low) / d->range;
    if (target >= m->tot) target = m->tot - 1;   /* clamp malformed input */
    uint32_t cum = 0;
    int sym = 0;
    while (sym < 255 && cum + m->f[sym] <= target) { cum += m->f[sym]; sym++; }
    d->low += cum * d->range;
    d->range *= m->f[sym];
    dec_renorm(d);
    model_bump(m, sym);
    return sym;
}

long range_decompress(const uint8_t *in, long n, uint8_t *out, long out_len) {
    if (out_len < 0 || n < 0) return -1;
    Model *m = (Model *)malloc(sizeof(Model) * 256);
    if (!m) return -1;
    for (int i = 0; i < 256; i++) model_init(&m[i]);
    Dec d; dec_init(&d, in, n);
    int ctx = 0;
    for (long i = 0; i < out_len; i++) {
        int sym = dec_sym(&d, &m[ctx]);
        out[i] = (uint8_t)sym;
        ctx = sym;
    }
    free(m);
    return out_len;
}
