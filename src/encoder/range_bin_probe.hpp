// range_bin_probe.hpp - encoder-only binary (CABAC-style) range coder, used ONLY
// behind -DTVID_PROBE to measure candidate C from the plan: model the structure
// plane's split/selector bits with a per-context adaptive probability, instead of
// byte-packing them and order-1 range-coding the bytes (method 3).
//
// This is a MEASUREMENT harness, not a wire format. It produces only a byte SIZE
// (no decoder, no header) and is never linked into the player or referenced outside
// #ifdef TVID_PROBE. The measurement is in: the context binary coder beats the
// shipped byte-order-1 coder on the split bits (see probe[cabac-split] in
// enc_stages.cpp), but the lever was SHELVED -- moving entropy decode into the
// quadtree walk costs the DOS decoder too much for <1% whole-file. The rationale
// and a full implementation sketch live in doc/possible-improvements.md.
//
// Design: a standard carry-less binary range coder (the classic LZMA rc_bit shape).
// Each context holds an 11-bit probability of bit==0, adapted toward the observed
// bit by a shift (the LZMA kNumMoveBits=5 rule). Decode is two multiplies + a
// compare per bit and a tiny prob[] table -- simpler and more asymmetry-rule-
// friendly than the 256-symbol cumulative-frequency search of method 3.
#ifndef RANGE_BIN_PROBE_HPP
#define RANGE_BIN_PROBE_HPP
#ifdef TVID_PROBE

#include <cstdint>
#include <vector>

namespace tvid_probe {

// 11-bit probability space (LZMA convention): prob in (0, 2048), start at 1024.
static constexpr uint32_t kBinTopBits = 11;
static constexpr uint32_t kBinTop = 1u << kBinTopBits;      // 2048
static constexpr uint32_t kBinMoveBits = 5;
static constexpr uint32_t kBinProbInit = kBinTop >> 1;      // 1024

class BinRangeEncoder {
public:
    explicit BinRangeEncoder(int n_contexts)
        : prob_(n_contexts, (uint16_t)kBinProbInit) {}

    // Encode one bit under context `ctx`. Adapts the context probability.
    void encode(int ctx, int bit) {
        uint32_t p = prob_[ctx];
        uint32_t bound = (range_ >> kBinTopBits) * p;
        if (bit == 0) {
            range_ = bound;
            prob_[ctx] = (uint16_t)(p + ((kBinTop - p) >> kBinMoveBits));
        } else {
            low_ += bound;
            range_ -= bound;
            prob_[ctx] = (uint16_t)(p - (p >> kBinMoveBits));
        }
        while (range_ < kRcTop) { shift_low(); range_ <<= 8; }
    }

    // Flush and return the encoded byte length.
    long finish() {
        for (int i = 0; i < 5; ++i) shift_low();
        return (long)out_.size();
    }

    long size() const { return (long)out_.size(); }

private:
    static constexpr uint32_t kRcTop = 1u << 24;

    void shift_low() {
        if ((uint32_t)(low_ >> 32) != 0 || low_ < 0xFF000000ULL) {
            uint8_t carry = (uint8_t)(low_ >> 32);
            if (started_) {
                do { out_.push_back((uint8_t)(cache_ + carry)); }
                while (--cache_size_);
            }
            cache_ = (uint8_t)(low_ >> 24);
            started_ = true;
        }
        cache_size_++;
        low_ = (low_ << 8) & 0xFFFFFFFFULL;
    }

    std::vector<uint16_t> prob_;
    std::vector<uint8_t> out_;
    uint64_t low_ = 0;
    uint32_t range_ = 0xFFFFFFFFu;
    uint8_t cache_ = 0;
    uint64_t cache_size_ = 0;
    bool started_ = false;
};

} // namespace tvid_probe

#endif // TVID_PROBE
#endif // RANGE_BIN_PROBE_HPP
