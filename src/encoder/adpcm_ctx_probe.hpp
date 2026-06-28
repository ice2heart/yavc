// adpcm_ctx_probe.hpp - encoder-only measurement harness for candidate
// "adpcm-ctx": entropy-code the IMA-ADPCM nibble stream with a context-adaptive
// model keyed on the ADPCM *step index* (0..88), instead of the shipped generic
// order-1 byte coder (range_compress, method 3) that is blind to it.
//
// Rationale: the meaning of a 4-bit ADPCM code depends entirely on the running
// step size (adpcm_step_table[index]); two equal nibbles at different step
// indices have completely different distributions. The shipped coder keys only on
// the previous *byte* (two unrelated prior nibbles) and even mixes the per-block
// 4-byte headers into the same model. The step index is a *context selector* (the
// only lever category that has ever won here -- the order-1 range coder itself),
// and it is FREE at decode: adpcm_decode_block already computes `index` before
// reading each nibble (adpcm_dec.c:47), so a real method would add zero decoder
// state and no asymmetry-rule cost -- unlike cabac-split. Audio is ~50%+ of a
// shipped file, so the whole-file leverage is large.
//
// This is a MEASUREMENT harness: it produces only a byte SIZE (no decoder, no
// header), never linked into the player, only under -DTVID_PROBE. The range-coder
// mechanics mirror range_enc.cpp / range_internal.h exactly so the comparison
// against the shipped coder is apples-to-apples (same renorm, same adaptation),
// only the alphabet (16 nibble symbols) and the context selection differ.
#ifndef ADPCM_CTX_PROBE_HPP
#define ADPCM_CTX_PROBE_HPP
#ifdef TVID_PROBE

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "adpcm.h"

// Pull in the canonical IMA tables (adpcm_step_table / adpcm_index_table). This
// header is encoder-only and already used by adpcm_enc.cpp.
#include "adpcm_internal.h"

namespace tvid_probe {

// ---- order-1-style range coder, parameters identical to range_internal.h ----
static constexpr uint32_t kAdpRangeInc  = 8u;
static constexpr uint32_t kAdpRangeCap  = 1u << 16;
static constexpr uint32_t kAdpRangeTop  = 1u << 24;
static constexpr uint32_t kAdpRangeBot  = 1u << 16;

// 16-symbol adaptive frequency model (one per context).
struct NibModel {
    uint16_t f[16];
    uint32_t tot;
    void init() { for (int i = 0; i < 16; i++) f[i] = 1; tot = 16; }
    void bump(int s) {
        f[s] = (uint16_t)(f[s] + kAdpRangeInc);
        tot += kAdpRangeInc;
        if (tot >= kAdpRangeCap) {
            uint32_t t = 0;
            for (int i = 0; i < 16; i++) { f[i] = (uint16_t)((f[i] >> 1) | 1); t += f[i]; }
            tot = t;
        }
    }
};

// A context-capable nibble range encoder that only reports its output size.
class NibCtxEncoder {
public:
    explicit NibCtxEncoder(int n_ctx) : m_(n_ctx) {
        for (auto &mm : m_) mm.init();
    }
    void encode(int ctx, int sym) {
        NibModel &m = m_[ctx];
        uint32_t cum = 0;
        for (int i = 0; i < sym; i++) cum += m.f[i];
        range_ /= m.tot;
        low_ += cum * range_;
        range_ *= m.f[sym];
        renorm();
        m.bump(sym);
    }
    long finish() {
        for (int i = 0; i < 4; i++) { emit((uint8_t)(low_ >> 24)); low_ <<= 8; }
        return pos_;
    }
private:
    void emit(uint8_t) { pos_++; }   // size-only: count bytes, don't store them
    void renorm() {
        while ((low_ ^ (low_ + range_)) < kAdpRangeTop ||
               (range_ < kAdpRangeBot && ((range_ = -low_ & (kAdpRangeBot - 1)), 1))) {
            emit((uint8_t)(low_ >> 24));
            low_ <<= 8; range_ <<= 8;
        }
    }
    std::vector<NibModel> m_;
    uint32_t low_ = 0, range_ = 0xFFFFFFFFu;
    long pos_ = 0;
};

// Walk a raw IMA-ADPCM blob (codec 1) block-by-block exactly as the decoder does,
// reconstructing the per-nibble step `index`, and feed each nibble to several
// candidate context coders. Reports each candidate's total byte size on stderr.
// `samples` is the PCM sample count (to recover each block's coded length).
// Encode the leader context model -- (index>>3 bucket) x (prev2 nibbles), 12*256
// contexts -- over a run of ADPCM blocks [blk_start, blk_end) of `blob`, with a
// fresh coder (models reset). Returns the coded byte size. Mirrors how a shipped
// grouped chunk would reset its model at each group boundary. The context is fully
// reconstructible at decode: each block reseeds index=0 / prev=0, and `index`
// evolves exactly as adpcm_dec.c:47.
// model: 0 = (idx>>3)xprev2 (12*256), 1 = (idx>>3)xprev1 (12*16),
//        2 = (idx>>4)xprev1 (6*16)
inline long code_group_ctx(const std::vector<uint8_t> &blob, long samples,
                           long blk_start, long blk_end, int model) {
    int nctx = model == 0 ? 12 * 256 : model == 1 ? 12 * 16 : 6 * 16;
    NibCtxEncoder enc(nctx);
    long rem = samples;
    size_t ip = 0;
    long blk = 0;
    while (rem > 0 && ip < blob.size()) {
        int n = rem > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)rem;
        long bb = adpcm_block_bytes(n);
        if (bb < 0 || ip + (size_t)bb > blob.size()) break;
        if (blk >= blk_start && blk < blk_end) {
            int index = blob[ip + 2];
            size_t nib_byte = ip + ADPCM_BLOCK_HEADER;
            int prev = 0, prev2v = 0;
            int high = 0; uint8_t cur = 0;
            for (int j = 1; j < n; ++j) {
                int code;
                if (!high) { cur = blob[nib_byte++]; code = cur & 0x0F; high = 1; }
                else { code = (cur >> 4) & 0x0F; high = 0; }
                int ctx;
                if (model == 0)      { int b=index>>3; if(b>11)b=11; ctx=b*256+(prev2v*16+prev); }
                else if (model == 1) { int b=index>>3; if(b>11)b=11; ctx=b*16+prev; }
                else                 { int b=index>>4; if(b>5)b=5;   ctx=b*16+prev; }
                enc.encode(ctx, code);
                index = adpcm_clampi(index + adpcm_index_table[code], 0, 88);
                prev2v = prev; prev = code;
            }
        }
        ip += (size_t)bb;
        rem -= n;
        if (++blk >= blk_end) break;
    }
    return enc.finish();
}

inline long total_blocks(long samples) {
    long blk = 0, rem = samples;
    while (rem > 0) { rem -= rem > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : rem; blk++; }
    return blk;
}

// Sweep group sizes K for the leader model: code the whole stream in K-block
// chunks, each chunk with a fresh coder (the real shippable shape -- a grouped
// codec-3 audio tail with DOS restart points). K=0 means one whole-stream chunk.
inline void measure_adpcm_ctx(const std::vector<uint8_t> &blob, long samples) {
    long nblk = total_blocks(samples);
    const char *mname[3] = {"(idx>>3)xp2 12*256", "(idx>>3)xp1 12*16 ", "(idx>>4)xp1 6*16  "};
    std::fprintf(stderr, "probe[adpcm-ctx]: %ld blocks; grouped coded sizes per model:\n", nblk);
    for (int model = 0; model < 3; ++model) {
        std::fprintf(stderr, "probe[adpcm-ctx]:   %s :", mname[model]);
        for (long K : {0L, 256L, 64L}) {
            long grp = (K == 0) ? nblk : K;
            long total = 0;
            for (long s = 0; s < nblk; s += grp)
                total += code_group_ctx(blob, samples, s, s + grp < nblk ? s + grp : nblk, model);
            std::fprintf(stderr, "  K=%s %ld", (K == 0 ? "whole" : std::to_string(K).c_str()), total);
        }
        std::fprintf(stderr, "\n");
    }
}

} // namespace tvid_probe

#endif // TVID_PROBE
#endif // ADPCM_CTX_PROBE_HPP
