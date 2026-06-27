/* segment_test.cpp - pins the v4 segment-slicing invariant.
 *
 * write_split_segmented_output() carves each whole-movie plane into per-segment
 * byte slices using a cumulative offset table off[f] = plane length THROUGH frame
 * f (frame 0 = keyframe). The slice for a segment covering frames [fa, fb) is
 * [ (fa==0 ? 0 : off[fa-1]) .. off[fb-1] ). A single off-by-one here silently
 * drops or duplicates bytes at every segment boundary (and threw std::length_error
 * when fb indexed past the table) -- the exact bug this test guards.
 *
 * The decode-level "v3 == v4 byte-identical across seg sizes" property is checked
 * end-to-end by hand via the -DTVID_PROBE frame-dump (see doc/v3-streaming.md);
 * this test pins the slicing math itself, in-process, with no file I/O. */
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const char *what) {
    if (!ok) { std::printf("FAIL: %s\n", what); g_fail = 1; }
}

uint32_t rng_state = 0xC0FFEEu;
uint32_t rng() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

// Mirror of the writer's frame-span -> byte-span mapping (enc_stages.cpp).
size_t byte_lo(const std::vector<size_t> &off, uint32_t fa) {
    return fa == 0 ? 0 : off[fa - 1];
}

// Build a plane with `bytes_per_frame[f]` bytes for each of nframes frames, and
// the matching cumulative offset table off[f] = length through frame f. Returns
// the plane; fills `off`.
std::vector<uint8_t> make_plane(const std::vector<size_t> &bpf,
                                std::vector<size_t> &off) {
    std::vector<uint8_t> plane;
    off.clear();
    size_t cum = 0;
    for (size_t f = 0; f < bpf.size(); ++f) {
        for (size_t b = 0; b < bpf[f]; ++b)
            plane.push_back((uint8_t)(rng() & 0xFF));
        cum += bpf[f];
        off.push_back(cum); // off[f] = length through frame f
    }
    return plane;
}

// Re-slice `plane` per the writer's segmentation and concatenate; must reproduce
// the whole plane exactly. seg_frames is the segment size (frames per segment).
void roundtrip(const std::vector<uint8_t> &plane, const std::vector<size_t> &off,
               uint32_t nframes, uint32_t seg_frames, const char *label) {
    const uint32_t nseg = (nframes + seg_frames - 1) / seg_frames;
    std::vector<uint8_t> rebuilt;
    for (uint32_t s = 0; s < nseg; ++s) {
        const uint32_t fa = s * seg_frames;
        const uint32_t fb = (fa + seg_frames < nframes) ? fa + seg_frames : nframes;
        size_t lo = byte_lo(off, fa);
        size_t hi = off[fb - 1]; // fb <= nframes, so fb-1 is in range
        check(lo <= hi, label); // a valid (non-throwing) slice span
        check(hi <= plane.size(), label);
        if (lo > hi || hi > plane.size()) return;
        rebuilt.insert(rebuilt.end(), plane.begin() + (long)lo, plane.begin() + (long)hi);
    }
    check(rebuilt == plane, label);
}

} // namespace

int main() {
    // A spread of frame counts and per-frame byte sizes (including empty frames,
    // like the structure plane's keyframe slot, and bursty sizes).
    const uint32_t frame_counts[] = {1, 2, 7, 64, 200, 333};

    for (uint32_t nframes : frame_counts) {
        // Two plane shapes: structure-like (frame 0 empty, rest variable) and
        // cell-like (frame 0 a big keyframe, rest small).
        std::vector<size_t> bpf_struct, bpf_cell;
        for (uint32_t f = 0; f < nframes; ++f) {
            bpf_struct.push_back(f == 0 ? 0 : (rng() % 200));
            bpf_cell.push_back(f == 0 ? 1920 : (rng() % 64));
        }
        std::vector<size_t> off_s, off_c;
        std::vector<uint8_t> ps = make_plane(bpf_struct, off_s);
        std::vector<uint8_t> pc = make_plane(bpf_cell, off_c);

        // Sweep seg sizes: divisors, non-divisors, 1, == nframes, > nframes.
        const uint32_t segs[] = {1, 2, 3, 7, 16, 64, nframes,
                                 nframes > 1 ? nframes - 1 : 1, nframes + 5};
        for (uint32_t seg : segs) {
            if (seg == 0) continue;
            roundtrip(ps, off_s, nframes, seg, "structure-plane segment slicing");
            roundtrip(pc, off_c, nframes, seg, "cell-plane segment slicing");
        }
    }

    if (!g_fail) std::printf("segment: OK (v4 offset-table slicing reconstructs every plane)\n");
    return g_fail;
}
