/* range_enc.cpp - adaptive order-1 range encoder (plane method 3, encoder-side
 * C++). Moved out of src/common/ so the player set carries only range_dec.c. The
 * frequency model + constants are shared bit-exactly with the decoder via
 * range_internal.h. Declared extern "C" (via range.h). See range.c history for the
 * tuning rationale (order-1, inc=8, cap=2^16; beats static Huffman and xz on the
 * mono cell plane). */
#include "range.h"
#include "range_internal.h"

#include <vector>

namespace {

typedef struct { uint32_t low, range; uint8_t *out; long pos, cap; } Enc;

void enc_init(Enc *e, uint8_t *out, long cap) {
    e->low = 0; e->range = 0xFFFFFFFFu; e->out = out; e->pos = 0; e->cap = cap;
}
void enc_byte(Enc *e, uint8_t b) {
    if (e->pos < e->cap) e->out[e->pos] = b;
    e->pos++;
}
void enc_renorm(Enc *e) {
    while ((e->low ^ (e->low + e->range)) < RANGE_TOP ||
           (e->range < RANGE_BOT && ((e->range = -e->low & (RANGE_BOT - 1)), 1))) {
        enc_byte(e, (uint8_t)(e->low >> 24));
        e->low <<= 8; e->range <<= 8;
    }
}
void enc_sym(Enc *e, Model *m, int sym) {
    uint32_t cum = 0;
    for (int i = 0; i < sym; i++) cum += m->f[i];
    e->range /= m->tot;
    e->low += cum * e->range;
    e->range *= m->f[sym];
    enc_renorm(e);
    model_bump(m, sym);
}
void enc_flush(Enc *e) {
    for (int i = 0; i < 4; i++) { enc_byte(e, (uint8_t)(e->low >> 24)); e->low <<= 8; }
}

} // namespace

long range_compress(const uint8_t *in, long n, uint8_t *out, long out_cap) {
    if (n < 0) return -1;
    std::vector<Model> m(256);
    for (int i = 0; i < 256; i++) model_init(&m[i]);
    Enc e; enc_init(&e, out, out_cap);
    int ctx = 0;
    for (long i = 0; i < n; i++) {
        enc_sym(&e, &m[ctx], in[i]);
        ctx = in[i];
    }
    enc_flush(&e);
    if (e.pos > out_cap) return -1;   /* did not fit */
    return e.pos;
}
