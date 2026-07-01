/* range_ctx_enc.cpp - codec-3 audio compressor: step-index-context nibble range
 * encoder for IMA-ADPCM (encoder-side C++). The matching decoder is
 * src/common/range_ctx_dec.c; the model + context selector + range-coder constants
 * are shared bit-exactly via range_ctx_internal.h. See range_ctx.h / doc/
 * audiocodec-evo.md "codec 3".
 *
 * Wire format of one chunk's payload:
 *   [4 bytes/block: the verbatim ADPCM block headers, in order]
 *   [range-coded nibble stream: every block's (n-1) coded nibbles, concatenated]
 * Block headers are copied verbatim (they seed the ADPCM state and carry the step
 * index the context needs); only the nibble payload is entropy-coded. The decoder
 * knows the block count and each block's sample count from `samples`, so it can
 * reconstruct the exact original packed-nibble bytes -- header + nibbles -- making
 * decoded PCM bit-identical to codec 1. */
#include "range_ctx.h"
#include "range_ctx_internal.h"

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
    while ((e->low ^ (e->low + e->range)) < RANGE_CTX_TOP ||
           (e->range < RANGE_CTX_BOT && ((e->range = -e->low & (RANGE_CTX_BOT - 1)), 1))) {
        enc_byte(e, (uint8_t)(e->low >> 24));
        e->low <<= 8; e->range <<= 8;
    }
}
void enc_sym(Enc *e, CtxModel *m, int sym) {
    uint32_t cum = 0;
    for (int i = 0; i < sym; i++) cum += m->f[i];
    e->range /= m->tot;
    e->low += cum * e->range;
    e->range *= m->f[sym];
    enc_renorm(e);
    ctx_model_bump(m, sym);
}
void enc_flush(Enc *e) {
    for (int i = 0; i < 4; i++) { enc_byte(e, (uint8_t)(e->low >> 24)); e->low <<= 8; }
}

} // namespace

long range_ctx_compress_adpcm(const uint8_t *in, long n, long samples,
                              uint8_t *out, long out_cap) {
    if (n < 0 || samples < 0) return -1;

    /* First pass: verify the blob is well-formed and copy the block headers out
     * verbatim (they seed the ADPCM state / carry the context step index). */
    std::vector<CtxModel> m(RANGE_CTX_NCTX);
    for (int i = 0; i < RANGE_CTX_NCTX; i++) ctx_model_init(&m[i]);

    long out_pos = 0;
    long rem = samples;
    long ip = 0;
    /* Emit headers first. */
    {
        long r = rem, p = 0;
        while (r > 0 && p < n) {
            int sn = r > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)r;
            long bb = adpcm_block_bytes(sn);
            if (bb < 0 || p + bb > n) return -1;
            for (int k = 0; k < ADPCM_BLOCK_HEADER; k++) {
                if (out_pos < out_cap) out[out_pos] = in[p + k];
                out_pos++;
            }
            p += bb;
            r -= sn;
        }
    }

    /* Second pass: range-code every block's nibbles, context = (index>>3, prev). */
    Enc e; enc_init(&e, out + (out_pos < out_cap ? out_pos : out_cap),
                    out_cap - out_pos > 0 ? out_cap - out_pos : 0);
    while (rem > 0 && ip < n) {
        int sn = rem > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)rem;
        long bb = adpcm_block_bytes(sn);
        if (bb < 0 || ip + bb > n) return -1;
        int index = in[ip + 2];
        if (index < 0 || index > 88) return -1;
        long nib_byte = ip + ADPCM_BLOCK_HEADER;
        int prev = 0;
        int high = 0; uint8_t cur = 0;
        for (int j = 1; j < sn; ++j) {
            int code;
            if (!high) { cur = in[nib_byte++]; code = cur & 0x0F; high = 1; }
            else       { code = (cur >> 4) & 0x0F; high = 0; }
            int ctx = range_ctx_select(index, prev);
            enc_sym(&e, &m[ctx], code);
            index = adpcm_clampi(index + adpcm_index_table[code], 0, 88);
            prev = code;
        }
        ip += bb;
        rem -= sn;
    }
    enc_flush(&e);
    out_pos += e.pos;
    if (out_pos > out_cap) return -1; /* did not fit */
    return out_pos;
}
