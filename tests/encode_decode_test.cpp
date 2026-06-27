/* encode_decode_test.cpp - end-to-end seam test across the encoder/decoder split.
 *
 * codec_test.cpp round-trips the block layer with RAW (uncompressed) planes. This
 * test adds the missing layer: it runs each plane through the entropy back-end
 * (the encoder-side compress that moved to C++) and then the player-side
 * decompress (which stays C), and asserts the decoded framebuffer is byte-
 * identical to the uncompressed-plane decode. It exercises every method
 * (1=lzss, 2=lzss+huffman / no-LZ huffman, 3=range) over real encoder planes, so
 * any drift between the compress and decompress halves is caught here. */
#include "blockcoder.hpp"
#include "mono_celldist.h"
#include "codec.h"
#include "tvid_format.h"
#include "glyphset.h"
#include "lzss.h"
#include "range.h"
#include "entropy.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

uint32_t rng_state = 0x1234567u;
uint32_t rng() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

int g_fail = 0;
void check(bool ok, const char *what, int trial) {
    if (!ok) { std::printf("FAIL: %s (trial %d)\n", what, trial); g_fail = 1; }
}

/* Compress `plane` with the given method into `out`; returns payload length, or
 * -1 if that method can't produce a smaller-than-or-equal blob (we still test the
 * roundtrip regardless of whether it shrank). method: 1=lzss, 2=huff, 3=range,
 * 4=no-LZ huff (decoded as method 2). */
long do_compress(int method, const std::vector<uint8_t> &plane, std::vector<uint8_t> &out) {
    long n = (long)plane.size();
    out.assign((size_t)(n + n / 16 + 1024), 0);
    switch (method) {
        case 1: return lzss_compress(plane.data(), n, out.data(), (long)out.size());
        case 2: return entropy_compress(plane.data(), n, out.data(), (long)out.size());
        case 3: return range_compress(plane.data(), n, out.data(), (long)out.size());
        case 4: return entropy_compress_nolz(plane.data(), n, out.data(), (long)out.size());
    }
    return -1;
}

/* Decompress a method-tagged blob back to `raw_len` bytes (mirrors the player's
 * per-plane dispatch in player.c). method 4 decodes as 2 (both are entropy). */
bool do_decompress(int method, const std::vector<uint8_t> &comp, long clen,
                   long raw_len, std::vector<uint8_t> &out) {
    out.assign((size_t)raw_len, 0);
    long d = -1;
    switch (method) {
        case 1: d = lzss_decompress(comp.data(), clen, out.data(), raw_len); break;
        case 2:
        case 4: d = entropy_decompress(comp.data(), clen, out.data(), raw_len); break;
        case 3: d = range_decompress(comp.data(), clen, out.data(), raw_len); break;
    }
    return d == raw_len;
}

/* Round-trip one plane through method and assert it survives byte-identically. */
void plane_roundtrip(int method, const std::vector<uint8_t> &plane,
                     const char *name, int trial) {
    if (plane.empty()) return;
    std::vector<uint8_t> comp;
    long clen = do_compress(method, plane, comp);
    if (clen <= 0) return; // method declined (e.g. overflow on incompressible) -- ok
    std::vector<uint8_t> back;
    bool ok = do_decompress(method, comp, clen, (long)plane.size(), back);
    check(ok && back == plane, name, trial);
}

} // namespace

int main() {
    const int cols = TVID_COLS, rows = TVID_ROWS, ncells = cols * rows;
    std::vector<uint8_t> sub((size_t)ncells * TVID_MONO_SUBN);
    std::vector<uint8_t> prev(ncells), ideal(ncells), color(ncells);

    for (int trial = 0; trial < 200 && !g_fail; ++trial) {
        for (int i = 0; i < ncells; ++i) {
            for (int p = 0; p < TVID_MONO_SUBN; ++p)
                sub[(size_t)i * TVID_MONO_SUBN + p] = (uint8_t)(rng() & 0xFF);
            prev[i] = (uint8_t)(rng() & 0xFF);
            color[i] = (uint8_t)(rng() & 0xFF);
        }
        for (int i = 0; i < ncells; ++i)
            ideal[i] = tvid_mono_quantize_joint(&sub[(size_t)i * TVID_MONO_SUBN]);

        BlockCoderParams bp; bp.cols = cols; bp.rows = rows; bp.sub = sub.data();
        bp.lambda = (trial & 1) ? 256 : 8;
        bp.block_stable = 0;
        bp.shift_range = (trial & 2) ? 7 : 0;
        int caps = bp.shift_range > 0 ? TVID_CAP_SHIFT : 0;

        // Build the full set of split planes (struct + mode + cell + pal + color).
        std::vector<uint8_t> raster, pal, mode, col_plane;
        auto sbits = blockcoder_encode_split(prev.data(), ideal.data(), sub.data(),
                                             bp, raster, nullptr, &mode, &pal,
                                             color.data(), &col_plane);

        // Reference: decode straight from the uncompressed planes.
        std::vector<uint8_t> want = prev, want_hue(ncells, 0);
        long cp = 0, mp = 0, pp = 0, kp = 0;
        int rr = codec_decode_block_split(
            sbits.data(), (long)sbits.size(),
            raster.data(), (long)raster.size(), &cp,
            mode.data(), (long)mode.size(), &mp,
            pal.data(), (long)pal.size(), &pp,
            col_plane.data(), (long)col_plane.size(), &kp,
            want.data(), want_hue.data(), prev.data(), cols, rows, caps);
        check(rr == 0, "reference split decode error", trial);

        // Every plane survives every entropy method byte-identically. Decoding the
        // entropy-restored planes therefore reproduces `want` exactly (the decode
        // is byte-opaque to how the plane was stored).
        for (int method = 1; method <= 4; ++method) {
            plane_roundtrip(method, sbits,     "struct plane entropy roundtrip", trial);
            plane_roundtrip(method, raster,    "raster plane entropy roundtrip", trial);
            plane_roundtrip(method, pal,       "pal plane entropy roundtrip", trial);
            plane_roundtrip(method, mode,      "mode plane entropy roundtrip", trial);
            plane_roundtrip(method, col_plane, "color plane entropy roundtrip", trial);

            // Tie it back to a real decode: restore each plane via this method, then
            // decode and require byte-identical cells + hue vs the reference.
            std::vector<uint8_t> rs, ra, pl, md, cl;
            std::vector<uint8_t> cs, ca, cpl, cmd, ccl;
            long sc = do_compress(method, sbits, cs);
            long rc = do_compress(method, raster, ca);
            long plc = do_compress(method, pal, cpl);
            long mdc = do_compress(method, mode, cmd);
            long clc = do_compress(method, col_plane, ccl);
            if (sc <= 0 || rc <= 0) continue; // method declined; covered above
            bool ok = do_decompress(method, cs, sc, (long)sbits.size(), rs)
                   && do_decompress(method, ca, rc, (long)raster.size(), ra)
                   && (pal.empty()  || do_decompress(method, cpl, plc, (long)pal.size(), pl))
                   && (mode.empty() || do_decompress(method, cmd, mdc, (long)mode.size(), md))
                   && do_decompress(method, ccl, clc, (long)col_plane.size(), cl);
            check(ok, "plane restore failed", trial);
            if (!ok) continue;
            if (pal.empty()) pl = pal;
            if (mode.empty()) md = mode;

            std::vector<uint8_t> got = prev, hue(ncells, 0);
            long c2 = 0, m2 = 0, p2 = 0, k2 = 0;
            int r2 = codec_decode_block_split(
                rs.data(), (long)rs.size(),
                ra.data(), (long)ra.size(), &c2,
                md.data(), (long)md.size(), &m2,
                pl.data(), (long)pl.size(), &p2,
                cl.data(), (long)cl.size(), &k2,
                got.data(), hue.data(), prev.data(), cols, rows, caps);
            check(r2 == 0, "restored-plane decode error", trial);
            check(got == want, "restored-plane decode != reference cells", trial);
            check(hue == want_hue, "restored-plane decode != reference hue", trial);
        }
    }

    if (!g_fail) std::printf("encode_decode: OK (entropy plane seam, all methods)\n");
    return g_fail;
}
