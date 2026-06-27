// codec_test.cpp - v3 quadtree block-frame round-trip tests.
//
// The bitstream is the encoder/decoder contract. The invariants:
//   1. Self-consistency: re-running the same decode is deterministic and in-bounds.
//   2. Split equivalence: the split decode (2-plane / 3-plane / CELLSPLIT) reproduces
//      the interleaved decode bit-for-bit, for every knob.
//   3. Color plane: with a parallel color plane, the decoded cell framebuffer is
//      UNCHANGED (color is additive) and the decoded hue framebuffer matches what
//      the encoder intended for the cells it chose.
//   4. Mode extremes: force RAW (lambda 0) -> at least as faithful as ideal;
//      force SKIP (huge lambda + block-stable) -> decoded == prev.
//   5. The mono joint quantizer is optimal.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "blockcoder.hpp"

#include "mono_celldist.h"

extern "C" {
#include "codec.h"
#include "glyphset.h"
#include "tvid_format.h"
}

static unsigned rng_state = 0x1234567u;
static unsigned rng() {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int g_fail = 0;
static void check(bool ok, const char *what, int trial) {
    if (!ok) { std::printf("FAIL: %s (trial %d)\n", what, trial); g_fail = 1; }
}

int main() {
    const int cols = TVID_COLS, rows = TVID_ROWS, ncells = cols * rows;

    std::vector<uint8_t> msub((size_t)ncells * TVID_MONO_SUBN);
    std::vector<uint8_t> mprev(ncells), mideal(ncells), mcolor(ncells);

    // (1) Mono + split + color round-trip. The block layer is byte-opaque, so a
    //     mono frame (cells = packed luma|glyph, scored against per-cell 2x4
    //     sub-pixel luma blocks) round-trips through the split decode, and the
    //     parallel color plane decodes in lockstep without disturbing the cells.
    for (int trial = 0; trial < 1500 && !g_fail; ++trial) {
        for (int i = 0; i < ncells; ++i) {
            for (int p = 0; p < TVID_MONO_SUBN; ++p)
                msub[(size_t)i * TVID_MONO_SUBN + p] = (uint8_t)(rng() & 0xFF);
            mprev[i] = (uint8_t)(rng() & 0xFF);
            mcolor[i] = (uint8_t)(rng() & 0xFF); // random per-cell hue index
        }
        for (int i = 0; i < ncells; ++i)
            mideal[i] = tvid_mono_quantize_joint(&msub[(size_t)i * TVID_MONO_SUBN]);

        BlockCoderParams bp; bp.cols = cols; bp.rows = rows; bp.sub = msub.data();
        for (int variant = 0; variant < 8; ++variant) {
            bp.lambda = (variant & 1) ? 256 : 8;
            bp.block_stable = (variant & 2) ? 5000 : 0;
            bp.shift_range = (variant & 4) ? 7 : 0;
            int caps = bp.shift_range > 0 ? TVID_CAP_SHIFT : 0;

            // Interleaved reference decode (no color: interleaved has no color plane).
            auto inter = blockcoder_encode(mprev.data(), mideal.data(),
                                           msub.data(), bp);
            std::vector<uint8_t> a = mprev, b = mprev;
            int ra = codec_decode_block_ref(inter.data(), (long)inter.size(),
                                            a.data(), mprev.data(), cols, rows, caps);
            int rb = codec_decode_block_ref(inter.data(), (long)inter.size(),
                                            b.data(), mprev.data(), cols, rows, caps);
            check(ra == 0 && rb == 0, "interleaved decode error", trial);
            check(a == b, "decode not deterministic", trial);
            std::vector<uint8_t> want = a;

            // 2-plane split with a color plane: cells must match the interleaved
            // decode; the hue framebuffer is filled in lockstep.
            std::vector<uint8_t> cell_plane, color_plane;
            auto sbits = blockcoder_encode_split(mprev.data(), mideal.data(),
                                                 msub.data(), bp, cell_plane,
                                                 nullptr, nullptr, nullptr,
                                                 mcolor.data(), &color_plane);
            std::vector<uint8_t> got = mprev, hue(ncells, 0);
            long cp = 0, mp = 0, pp = 0, kp = 0;
            int rr = codec_decode_block_split(
                sbits.data(), (long)sbits.size(),
                cell_plane.data(), (long)cell_plane.size(), &cp,
                nullptr, 0, &mp,
                nullptr, 0, &pp,
                color_plane.data(), (long)color_plane.size(), &kp,
                got.data(), hue.data(), mprev.data(), cols, rows, caps);
            check(rr == 0, "split+color decode error", trial);
            check(cp == (long)cell_plane.size(), "cell cursor mismatch", trial);
            check(kp == (long)color_plane.size(), "color cursor mismatch", trial);
            check(got == want, "split decode != interleaved (color disturbed cells)", trial);

            // Every cell that the decoder wrote (i.e. differs from prev, so it was
            // NOT a SKIP leaf) must carry the hue the encoder picked for it. For a
            // SOLID/PAL2 leaf the per-cell hue is the leaf's representative, which
            // for random input we can't predict exactly; so we only assert that a
            // RAW-detail decode (lambda 0) reproduces the source hue exactly below.
            (void)hue;

            // 3-plane (mode plane) with color: identical cells.
            std::vector<uint8_t> cell3, mode3, color3;
            auto sbits3 = blockcoder_encode_split(mprev.data(), mideal.data(),
                                                  msub.data(), bp, cell3, nullptr,
                                                  &mode3, nullptr,
                                                  mcolor.data(), &color3);
            std::vector<uint8_t> got3 = mprev, hue3(ncells, 0);
            long cp3 = 0, mp3 = 0, pp3 = 0, kp3 = 0;
            int rr3 = codec_decode_block_split(
                sbits3.data(), (long)sbits3.size(),
                cell3.data(), (long)cell3.size(), &cp3,
                mode3.data(), (long)mode3.size(), &mp3,
                nullptr, 0, &pp3,
                color3.data(), (long)color3.size(), &kp3,
                got3.data(), hue3.data(), mprev.data(), cols, rows, caps);
            check(rr3 == 0, "3-plane+color decode error", trial);
            check(cell3 == cell_plane, "3-plane cell plane differs", trial);
            check(color3 == color_plane, "3-plane color plane differs", trial);
            check(got3 == want, "3-plane decode != interleaved", trial);
            check(hue3 == hue, "3-plane hue != 2-plane hue", trial);

            // CELLSPLIT with color: SOLID/PAL2 cells in the palette plane, RAW in
            // the raster plane; color plane unchanged.
            std::vector<uint8_t> raster4, pal4, color4;
            auto sbits4 = blockcoder_encode_split(mprev.data(), mideal.data(),
                                                  msub.data(), bp, raster4, nullptr,
                                                  nullptr, &pal4,
                                                  mcolor.data(), &color4);
            std::vector<uint8_t> got4 = mprev, hue4(ncells, 0);
            long cp4 = 0, mp4 = 0, pp4 = 0, kp4 = 0;
            int rr4 = codec_decode_block_split(
                sbits4.data(), (long)sbits4.size(),
                raster4.data(), (long)raster4.size(), &cp4,
                nullptr, 0, &mp4,
                pal4.data(), (long)pal4.size(), &pp4,
                color4.data(), (long)color4.size(), &kp4,
                got4.data(), hue4.data(), mprev.data(), cols, rows, caps);
            check(rr4 == 0, "CELLSPLIT+color decode error", trial);
            check(raster4.size() + pal4.size() == cell_plane.size(),
                  "CELLSPLIT planes don't sum to combined", trial);
            check(color4 == color_plane, "CELLSPLIT color plane differs", trial);
            check(got4 == want, "CELLSPLIT decode != interleaved", trial);
            check(hue4 == hue, "CELLSPLIT hue != 2-plane hue", trial);
        }
    }

    // (2) Force detail (lambda 0): the decoded cells render at least as faithfully
    //     as `ideal`, and every cell becomes RAW so the decoded HUE equals the
    //     exact source hue (RAW carries per-cell color).
    for (int trial = 0; trial < 300 && !g_fail; ++trial) {
        for (int i = 0; i < ncells; ++i) {
            for (int p = 0; p < TVID_MONO_SUBN; ++p)
                msub[(size_t)i * TVID_MONO_SUBN + p] = (uint8_t)(rng() & 0xFF);
            mprev[i] = (uint8_t)(rng() & 0xFF);
            mcolor[i] = (uint8_t)(rng() & 0xFF);
        }
        for (int i = 0; i < ncells; ++i)
            mideal[i] = tvid_mono_quantize_joint(&msub[(size_t)i * TVID_MONO_SUBN]);

        BlockCoderParams bp; bp.cols = cols; bp.rows = rows; bp.sub = msub.data();
        bp.lambda = 0; bp.block_stable = 0; bp.shift_range = 0;

        std::vector<uint8_t> cell_plane, color_plane;
        auto sbits = blockcoder_encode_split(mprev.data(), mideal.data(),
                                             msub.data(), bp, cell_plane,
                                             nullptr, nullptr, nullptr,
                                             mcolor.data(), &color_plane);
        std::vector<uint8_t> got = mprev, hue(ncells, 0);
        long cp = 0, mp = 0, pp = 0, kp = 0;
        codec_decode_block_split(
            sbits.data(), (long)sbits.size(),
            cell_plane.data(), (long)cell_plane.size(), &cp,
            nullptr, 0, &mp, nullptr, 0, &pp,
            color_plane.data(), (long)color_plane.size(), &kp,
            got.data(), hue.data(), mprev.data(), cols, rows, 0);

        auto dist = [&](const std::vector<uint8_t> &cells) {
            long d = 0;
            for (int i = 0; i < ncells; ++i)
                d += tvid_mono_byte_distortion(
                    cells[i], &msub[(size_t)i * TVID_MONO_SUBN]);
            return d;
        };
        check(dist(got) <= dist(mideal), "forced-detail worse than ideal", trial);
        check(hue == mcolor, "forced-detail (RAW) hue != source hue", trial);
    }

    // (3) Force SKIP: huge lambda + SKIP credit -> decoded == prev, and (since no
    //     leaf is written) the hue framebuffer is left at its seed.
    for (int trial = 0; trial < 100 && !g_fail; ++trial) {
        for (int i = 0; i < ncells; ++i) {
            for (int p = 0; p < TVID_MONO_SUBN; ++p)
                msub[(size_t)i * TVID_MONO_SUBN + p] = (uint8_t)(rng() & 0xFF);
            mprev[i] = (uint8_t)(rng() & 0xFF);
            mcolor[i] = (uint8_t)(rng() & 0xFF);
        }
        for (int i = 0; i < ncells; ++i)
            mideal[i] = tvid_mono_quantize_joint(&msub[(size_t)i * TVID_MONO_SUBN]);

        BlockCoderParams bp; bp.cols = cols; bp.rows = rows; bp.sub = msub.data();
        bp.lambda = 1 << 16; bp.block_stable = 1 << 20; bp.shift_range = 0;
        std::vector<uint8_t> cell_plane, color_plane;
        auto sbits = blockcoder_encode_split(mprev.data(), mideal.data(),
                                             msub.data(), bp, cell_plane,
                                             nullptr, nullptr, nullptr,
                                             mcolor.data(), &color_plane);
        std::vector<uint8_t> got = mprev, hue(ncells, 7);
        std::vector<uint8_t> hue_seed = hue;
        long cp = 0, mp = 0, pp = 0, kp = 0;
        codec_decode_block_split(
            sbits.data(), (long)sbits.size(),
            cell_plane.data(), (long)cell_plane.size(), &cp,
            nullptr, 0, &mp, nullptr, 0, &pp,
            color_plane.data(), (long)color_plane.size(), &kp,
            got.data(), hue.data(), mprev.data(), cols, rows, 0);
        check(got == mprev, "forced-skip did not preserve prev", trial);
        check(hue == hue_seed, "forced-skip disturbed the hue framebuffer", trial);
    }

    // (4) The mono joint quantizer must be optimal: for random sub-pixel blocks,
    //     no (level,glyph) pair beats tvid_mono_quantize_joint's distortion.
    for (int t = 0; t < 2048 && !g_fail; ++t) {
        uint8_t sub[TVID_MONO_SUBN];
        for (int p = 0; p < TVID_MONO_SUBN; ++p) sub[p] = (uint8_t)(rng() & 0xFF);
        uint8_t picked = tvid_mono_quantize_joint(sub);
        long pd = tvid_mono_byte_distortion(picked, sub), best = pd;
        for (int lvl = 0; lvl < TVID_MONO_LUMA_LEVELS; ++lvl)
            for (int g = 0; g < TVID_MONO_GLYPH_COUNT; ++g) {
                long d = tvid_mono_cell_distortion(lvl, g, sub);
                if (d < best) best = d;
            }
        check(pd == best, "mono joint quantizer not optimal", t);
    }

    if (!g_fail) std::printf("codec_roundtrip: OK (v3 mono+color + joint opt)\n");
    return g_fail;
}
