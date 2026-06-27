// enc_stages.cpp - encoder pipeline stages (see enc_stages.hpp). Each function
// holds, verbatim, the logic that used to live inline in encoder.cpp's main().
// The output is byte-identical to the pre-split encoder; this is purely a
// readability refactor (one focused function per pipeline phase).
#include "enc_stages.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "quantize.hpp"
#include "mono_celldist.h"

extern "C" {
#include "adpcm.h"
#include "codec.h"
#include "entropy.h"
#include "glyphset.h"
#include "lzss.h"
#include "mode_rle.h"
#include "ramp.h"
#include "range.h"
#include "tvid_format.h"
#include "xterm256.h"
}

#ifdef TVID_PROBE
// Per-leaf cell decorrelation probe buffers, filled by blockcoder.cpp's RAW
// serializer (declared extern there). Global scope to match the extern linkage.
std::vector<uint8_t> g_probe_raw_plain;
std::vector<uint8_t> g_probe_raw_left;
std::vector<uint8_t> g_probe_raw_up;
bool g_probe_raw_capture = false;
#endif

namespace {

void put_u32le(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}

void put_u16le(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}

// Append the audio sub-header (TVID_AUDIO_SUBHEADER_BYTES) to a file header, in
// the same field order tvid_format.h documents. Call only when audio is present;
// the caller has already set TVID_FLAG_AUDIO in the flags byte.
void put_audio_subheader(std::vector<uint8_t> &header, int rate, long samples,
                         long bytes) {
    header.push_back(TVID_AUDIO_IMA_ADPCM); // codec
    header.push_back(1);                    // channels (mono)
    put_u16le(header, (uint16_t)rate);
    put_u32le(header, (uint32_t)samples);
    put_u32le(header, (uint32_t)bytes);
}

// Entropy-code one plane (best of raw / LZSS / LZSS+Huffman / no-LZ Huffman) and
// append a self-describing chunk to `body`:
//   [u8 method][u32le raw_len][u32le payload_len][payload]
// method: 0=raw, 1=lzss, 2=huff. Used per-plane in the split body so each plane
// gets a Huffman table matched to its own (very different) statistics. The no-LZ
// candidate (entropy_compress_nolz, also a method-2 blob) wins on planes whose
// bytes lack LZ-friendly repeats -- the SKIP-dominated mode plane and some
// palette planes -- e.g. bad.webm color -4.7%. Auto-selected; never regresses.
void append_plane(std::vector<uint8_t> &body, const std::vector<uint8_t> &plane) {
    const long n = (long)plane.size();
    std::vector<uint8_t> lz(n + n / 16 + 64);
    long lc = lzss_compress(plane.data(), n, lz.data(), (long)lz.size());
    std::vector<uint8_t> hf(n + n / 16 + 1024);
    long hc = entropy_compress(plane.data(), n, hf.data(), (long)hf.size());
    // No-LZ order-0 Huffman: another method-2 blob (decoder decodes it the same).
    // Wins where LZ tokenization compresses worse than the raw bytes -- e.g. the
    // mono cell plane (doc/abandoned-levers.md, entropy-gap notes). Auto-selected.
    std::vector<uint8_t> hf0(n + n / 16 + 1024);
    long hc0 = entropy_compress_nolz(plane.data(), n, hf0.data(), (long)hf0.size());
    // Adaptive order-1 range coder (method 3): no LZ, no serialized table, frac-
    // tional bits. Beats Huffman *and* xz on the mono cell plane (doc/compression.md,
    // entropy methods). Auto-selected like the others; decoder gains one branch.
    std::vector<uint8_t> rc(n + n / 16 + 1024);
    long rcc = range_compress(plane.data(), n, rc.data(), (long)rc.size());
    long best = n; int method = 0;
    const uint8_t *src = nullptr;
    if (lc > 0 && lc < best)  { best = lc;  method = 1; src = lz.data(); }
    if (hc > 0 && hc < best)  { best = hc;  method = 2; src = hf.data(); }
    if (hc0 > 0 && hc0 < best) { best = hc0; method = 2; src = hf0.data(); }
#ifndef TVID_NO_RANGE
    if (rcc > 0 && rcc < best) { best = rcc; method = 3; src = rc.data(); }
#endif
#ifdef TVID_PROBE
    // Per-plane back-end accounting: candidate sizes (lzss / lzss+huffman / no-LZ
    // order-0 huffman / order-1 range) and which won. The range coder wins the
    // cell plane; no-LZ huffman wins the mode + some palette planes. Prints
    // automatically under -DTVID_PROBE.
    std::fprintf(stderr,
        "probe[plane]: n=%ld lz=%ld huf=%ld nolz=%ld range=%ld -> method=%d size=%ld\n",
        n, lc, hc, hc0, rcc, method, best);
#endif
    body.push_back((uint8_t)method);
    put_u32le(body, (uint32_t)n);
    long plen = (method == 0) ? n : best;
    put_u32le(body, (uint32_t)plen);
    if (method == 0) body.insert(body.end(), plane.begin(), plane.end());
    else             body.insert(body.end(), src, src + best);
}

} // namespace

[[noreturn]] void enc_die(const char *msg) {
    std::fprintf(stderr, "encoder: %s\n", msg);
    std::exit(1);
}

// Read raw s16le mono PCM from `path` and IMA-encode it to an in-memory ADPCM
// blob. Sets *out_samples to the PCM sample count. The blob and sample count go
// into the audio sub-header so the decoder knows the exact stream end. Done up
// front (before the video header is written) so audio_bytes is known in time.
std::vector<uint8_t> encode_audio(const std::string &path, long *out_samples) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) enc_die("cannot open --audio-pcm file");
    std::vector<int16_t> pcm;
    int16_t buf[4096];
    size_t got;
    while ((got = std::fread(buf, sizeof(int16_t), 4096, fp)) > 0)
        pcm.insert(pcm.end(), buf, buf + got);
    std::fclose(fp);

    long n = (long)pcm.size();
    long cap = adpcm_encoded_size(n);
    if (cap < 0) enc_die("audio too large to encode");
    std::vector<uint8_t> blob((size_t)(cap > 0 ? cap : 1));
    long w = adpcm_encode(pcm.data(), n, blob.data(), cap);
    if (w != cap) enc_die("adpcm encode failed");
    blob.resize((size_t)w);
    *out_samples = n;
    return blob;
}

EncoderConfig parse_args(int argc, char **argv) {
    EncoderConfig cfg;
    cfg.cols = TVID_COLS;
    cfg.rows = TVID_ROWS;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) enc_die("missing argument value");
            return argv[++i];
        };
        if (a == "--fps") cfg.fps = std::atoi(next());
        else if (a == "--out") cfg.out_path = next();
        else if (a == "--cols") cfg.cols = std::atoi(next());
        else if (a == "--rows") cfg.rows = std::atoi(next());
        else if (a == "--stable") cfg.stable = std::atoi(next());
        else if (a == "--lookahead") cfg.lookahead = std::atoi(next());
        else if (a == "--shift") cfg.shift = std::atoi(next());
        else if (a == "--block-stable") cfg.block_stable = std::atoi(next());
        else if (a == "--lambda") cfg.lambda = std::atoi(next());
        else if (a == "--mono") { /* v3 is always mono-shape; accepted for compat */ }
        else if (a == "--quant") { (void)next(); /* legacy v2 knob; ignored in v3 */ }
        else if (a == "--color") cfg.color = true;
        else if (a == "--compress") cfg.compress = true;
        else if (a == "--stats") cfg.stats = true;
        else if (a == "--split") { cfg.split = true; cfg.compress = true; }
        else if (a == "--seg") {
            // Bare `--seg` (no following number, or end of args) means the default
            // segment size; `--seg N` overrides it. Peek so a trailing `--seg` works.
            if (i + 1 < argc && argv[i + 1][0] != '-' &&
                argv[i + 1][std::strspn(argv[i + 1], "0123456789")] == '\0')
                cfg.seg = std::atoi(argv[++i]);
            else
                cfg.seg = TVID_SEG_DEFAULT;
            cfg.split = true; cfg.compress = true;
        }
        else if (a == "--audio-pcm") cfg.audio_pcm_path = next();
        else if (a == "--audio-rate") cfg.audio_rate = std::atoi(next());
        else enc_die(("unknown arg: " + a).c_str());
    }
    // The color plane lives only in the split body (per-plane auto-select); it has
    // no whole-stream home. Adding color implies --split.
    if (cfg.color) { cfg.split = true; cfg.compress = true; }
    if (cfg.out_path.empty()) enc_die("missing --out");
    if (cfg.fps < 1 || cfg.fps > 255) enc_die("fps out of range");
    if (cfg.stable < 0 || cfg.block_stable < 0 || cfg.lambda < 0)
        enc_die("knobs must be >= 0");
    if (cfg.lookahead < 0 || cfg.shift < 0) enc_die("knobs must be >= 0");
    if (cfg.seg < 0 || cfg.seg > 65535) enc_die("--seg out of range (0..65535)");
    if (cfg.shift > TVID_SHIFT_MAX) cfg.shift = TVID_SHIFT_MAX;
    if (!cfg.audio_pcm_path.empty() && (cfg.audio_rate < 1 || cfg.audio_rate > 65535))
        enc_die("--audio-rate out of range (1..65535)");
    cfg.has_audio = !cfg.audio_pcm_path.empty();
    return cfg;
}

// --- pass 1a: read the whole source into per-cell sub-pixel luma blocks (and,
//     with --color, per-cell hue). We buffer all frames (offline encoder) so
//     pass 1b can look ahead. ---
void pass1a_read_frames(const EncoderConfig &cfg, EncoderState &st) {
    const size_t frame_bytes =
        (size_t)cfg.cols * TVID_MONO_SUBW * (cfg.rows * TVID_MONO_SUBH) * 3;
    std::vector<uint8_t> raw(frame_bytes);
    for (;;) {
        size_t got = std::fread(raw.data(), 1, frame_bytes, stdin);
        if (got == 0) break;
        if (got != frame_bytes) enc_die("partial frame on stdin (size mismatch?)");
        std::span<const uint8_t> sp(raw);
        st.avgs.push_back(subpixel_frame(sp, cfg.cols, cfg.rows));
        if (cfg.color) st.hues.push_back(cellcolor_frame(sp, cfg.cols, cfg.rows));
    }
    if (st.avgs.empty()) enc_die("no frames read from stdin");
}

// --- pass 1b: per-cell temporal hysteresis (deadband). A cell's target luma
//     block only moves when it travels more than --stable from what's on screen,
//     which kills quantization shimmer. With --lookahead N, an excursion that
//     reverts within the next N frames is treated as a transient blip and held.
//     The hue track (color) follows the SAME hold decisions. ---
void pass1b_hysteresis(const EncoderConfig &cfg, EncoderState &st) {
    const int elem = TVID_MONO_SUBN;
    const int ncells = cfg.cols * cfg.rows;
    const long stable_sq = (long)cfg.stable * cfg.stable;

    st.targets.reserve(st.avgs.size());
    if (cfg.color) st.ctargets.reserve(st.hues.size());
    std::vector<uint8_t> ref = st.avgs[0];   // first frame: everything is new
    std::vector<uint8_t> cref = cfg.color ? st.hues[0] : std::vector<uint8_t>();
    // Deadband test over the TVID_MONO_SUBN luma sub-pixels. Compare the MEAN
    // squared per-sub-pixel delta to stable_sq so --stable keeps one meaning.
    auto moved = [&](const std::vector<uint8_t> &a, size_t o) -> bool {
        long sq = 0;
        for (int k = 0; k < elem; ++k) {
            long d = (long)a[o + k] - ref[o + k];
            sq += d * d;
        }
        return sq / elem > stable_sq;
    };
    for (size_t f = 0; f < st.avgs.size(); ++f) {
        const std::vector<uint8_t> &avg = st.avgs[f];
        for (int c = 0; c < ncells; ++c) {
            size_t o = (size_t)c * elem;
            if (moved(avg, o)) {
                // The cell wants to move. If lookahead is on, hold it ONLY when
                // the excursion is a true transient blip: it must return to
                // within the deadband of the current ref by the end of the
                // window, AND every frame in between must also stay within the
                // deadband. A sustained or growing change (even one that happens
                // to revert later) must update now -- holding it would freeze a
                // stale value on screen, which reads as ghosting/artifacts.
                bool transient = false;
                if (cfg.lookahead > 0) {
                    size_t end = f + (size_t)cfg.lookahead;
                    if (end >= st.avgs.size()) end = st.avgs.size() - 1;
                    bool stays = (end > f); // need at least one future frame
                    for (size_t g = f + 1; g <= end; ++g) {
                        if (moved(st.avgs[g], o)) { stays = false; break; }
                    }
                    transient = stays;
                }
                if (transient) { ++st.held; }
                else {
                    for (int k = 0; k < elem; ++k) ref[o + k] = avg[o + k];
                    if (cfg.color) cref[c] = st.hues[f][c]; // hue follows the luma update
                }
            } else {
                ++st.held;
            }
        }
        st.total_cells += ncells;
        st.targets.push_back(ref);          // copy of the held/updated luma target
        if (cfg.color) st.ctargets.push_back(cref);
    }
}

// --- pass 2: encode frame stream. Frame 0 = keyframe (the ideal cells); rest =
//     quadtree block frames vs the previously *decoded* cells. Two layouts:
//     interleaved (`stream`) or split (`struct_stream` + planes). ---
void pass2_encode(const EncoderConfig &cfg, EncoderState &st) {
    const int ncells = cfg.cols * cfg.rows;
    auto quantize = [&](const std::vector<uint8_t> &avg) {
        return quantize_mono(avg, cfg.cols, cfg.rows);
    };

    std::vector<uint8_t> shown = quantize(st.targets[0]); // what the decoder shows now

    if (cfg.split) {
        st.cell_plane.insert(st.cell_plane.end(), shown.begin(), shown.end()); // keyframe
        st.raster_plane.insert(st.raster_plane.end(), shown.begin(), shown.end()); // keyframe
        if (cfg.color) // keyframe hues: one per cell, raster order, matching the cells
            st.color_plane.insert(st.color_plane.end(),
                                  st.ctargets[0].begin(), st.ctargets[0].end());
    } else {
        st.stream.push_back(TVID_FRAME_KEY);
        st.stream.insert(st.stream.end(), shown.begin(), shown.end());
    }

    BlockCoderParams bp;
    bp.cols = cfg.cols; bp.rows = cfg.rows;
    bp.lambda = cfg.lambda;
    bp.block_stable = (long)cfg.block_stable;
    bp.shift_range = cfg.shift;
    // bp.sub is set per-frame to targets[f] below
    const int caps = cfg.shift > 0 ? TVID_CAP_SHIFT : 0;

    long cell_pos = (long)st.cell_plane.size();   // split: decoder's cell cursor mirror
    long color_pos = (long)st.color_plane.size(); // split: decoder's color cursor mirror

    // v4 segmentation: seed each plane's per-frame offset table with the post-
    // keyframe lengths (frame 0). Each entry f records the plane length *after*
    // frame f's bytes are appended; off[0] is the length after the keyframe (which
    // is in cell/raster/color, zero in the structure/mode/pal planes). Only
    // recorded for the split path; the v3 writer ignores them.
    if (cfg.split && cfg.seg > 0) {
        st.off_struct.push_back(st.struct_stream.size());
        st.off_nomode.push_back(st.struct_nomode.size());
        st.off_cell.push_back(st.cell_plane.size());
        st.off_mode.push_back(st.mode_plane.size());
        st.off_raster.push_back(st.raster_plane.size());
        st.off_pal.push_back(st.pal_plane.size());
        st.off_color.push_back(st.color_plane.size());
    }
    // Decoder-side color framebuffer for the round-trip (persists like `shown`).
    // Seeded from the keyframe hues so a SKIP cell on frame 1 holds the right color.
    std::vector<uint8_t> shown_color;
    if (cfg.color) shown_color = st.ctargets[0];
    std::vector<uint8_t> prev_snapshot(shown.size());
    for (size_t f = 1; f < st.targets.size(); ++f) {
        std::vector<uint8_t> ideal = quantize(st.targets[f]);
        prev_snapshot = shown;
        // The block coder scores cells against the per-cell sub-pixel luma block;
        // point bp.sub at this frame's blocks.
        bp.sub = st.targets[f].data();
        const uint8_t *frame_color = cfg.color ? st.ctargets[f].data() : nullptr;

#ifdef TVID_PROBE
        {
            const uint8_t *sub = st.targets[f].data();
            for (int cc = 0; cc < ncells; ++cc) {
                st.probe_mono_dist += (double)tvid_mono_byte_distortion(
                    ideal[cc], &sub[(size_t)cc * TVID_MONO_SUBN]);
                ++st.probe_mono_cells;
            }
        }
#endif

        if (cfg.split) {
            // Build the split candidates from the same RD tree this frame:
            //  - structure: 2-plane (modes inline, `sbits`) and 3-plane (modes in
            //    mode_plane, rest in `sbits_nm`) -- auto-selected at body build.
            //  - cells: the combined cell_plane (canonical, drives round-trip) and
            //    the by-mode split raster_plane(RAW)+pal_plane(SOLID/PAL2), also
            //    auto-selected at body build (TVID_FLAG_CELLSPLIT).
            // Canonical pass: fills cell_plane (and, with --color, color_plane).
            // This is the one that drives the round-trip below.
            std::vector<uint8_t> sbits = blockcoder_encode_split(
                shown.data(), ideal.data(), st.targets[f].data(), bp, st.cell_plane,
                cfg.stats ? &st.bstats : nullptr, nullptr, nullptr,
                frame_color, cfg.color ? &st.color_plane : nullptr);
            if (sbits.size() > 0xFFFF) enc_die("split structure frame exceeds u16 length");
            std::vector<uint8_t> discard_cells; // cells already captured above
            std::vector<uint8_t> sbits_nm = blockcoder_encode_split(
                shown.data(), ideal.data(), st.targets[f].data(), bp, discard_cells,
                nullptr, &st.mode_plane);
            if (sbits_nm.size() > 0xFFFF) enc_die("split structure frame exceeds u16 length");
            // By-mode cell capture: RAW cells -> raster_plane, SOLID/PAL2 -> pal_plane.
            // (The probe buffers accumulate RAW residuals here, across all frames;
            // gate so only this pass fills them, not the two structure passes.)
#ifdef TVID_PROBE
            g_probe_raw_capture = true;
#endif
            (void)blockcoder_encode_split(
                shown.data(), ideal.data(), st.targets[f].data(), bp, st.raster_plane,
                nullptr, nullptr, &st.pal_plane);
#ifdef TVID_PROBE
            g_probe_raw_capture = false;
#endif

            // Round-trip exactly as the player will (2-plane path), threading the
            // cell (and color) cursors frame to frame.
            long np = cell_pos;
            long dummy_mode = 0, dummy_pal = 0;
            if (codec_decode_block_split(
                    sbits.data(), (long)sbits.size(),
                    st.cell_plane.data(), (long)st.cell_plane.size(), &np,
                    nullptr, 0, &dummy_mode,
                    nullptr, 0, &dummy_pal,
                    cfg.color ? st.color_plane.data() : nullptr,
                    cfg.color ? (long)st.color_plane.size() : 0, &color_pos,
                    shown.data(), cfg.color ? shown_color.data() : nullptr,
                    prev_snapshot.data(), cfg.cols, cfg.rows, caps) != 0)
                enc_die("internal: split frame failed to round-trip");
            cell_pos = np;

            st.struct_stream.push_back((uint8_t)(sbits.size() & 0xFF));
            st.struct_stream.push_back((uint8_t)((sbits.size() >> 8) & 0xFF));
            st.struct_stream.insert(st.struct_stream.end(), sbits.begin(), sbits.end());
            st.struct_nomode.push_back((uint8_t)(sbits_nm.size() & 0xFF));
            st.struct_nomode.push_back((uint8_t)((sbits_nm.size() >> 8) & 0xFF));
            st.struct_nomode.insert(st.struct_nomode.end(), sbits_nm.begin(), sbits_nm.end());
#ifdef TVID_PROBE
            st.probe_nolen_2plane.insert(st.probe_nolen_2plane.end(), sbits.begin(), sbits.end());
            st.probe_nolen_3plane.insert(st.probe_nolen_3plane.end(), sbits_nm.begin(), sbits_nm.end());
#endif
            st.framing_bytes += 2;
            if (cfg.seg > 0) {
                st.off_struct.push_back(st.struct_stream.size());
                st.off_nomode.push_back(st.struct_nomode.size());
                st.off_cell.push_back(st.cell_plane.size());
                st.off_mode.push_back(st.mode_plane.size());
                st.off_raster.push_back(st.raster_plane.size());
                st.off_pal.push_back(st.pal_plane.size());
                st.off_color.push_back(st.color_plane.size());
            }
            continue;
        }

        std::vector<uint8_t> payload =
            blockcoder_encode(shown.data(), ideal.data(), st.targets[f].data(), bp,
                              cfg.stats ? &st.bstats : nullptr);
        if (payload.size() > 0xFFFF) enc_die("block frame exceeds u16 length");
        st.framing_bytes += 3;

        // The decoder applies this payload to `shown`; SHIFT leaves read the
        // *previous* frame, so snapshot it first and decode reference-aware,
        // exactly mirroring the player. Then the next frame deltas against what
        // the decoder will hold.
        if (codec_decode_block_ref(payload.data(), (long)payload.size(),
                                   shown.data(), prev_snapshot.data(),
                                   cfg.cols, cfg.rows, caps) != 0)
            enc_die("internal: block frame failed to round-trip");

        st.stream.push_back(TVID_FRAME_BLOCK);
        st.stream.push_back((uint8_t)(payload.size() & 0xFF));
        st.stream.push_back((uint8_t)((payload.size() >> 8) & 0xFF));
        st.stream.insert(st.stream.end(), payload.begin(), payload.end());
    }
}

// --- split body: independently entropy-coded planes, with auto-selection between
//     the 2-plane (modes inline) and 3-plane (modes in their own plane) layouts.
//     The cell plane is shared; only the structure side differs. Keep whichever
//     compresses smaller (see doc/compression.md, split planes). ---
void write_split_output(const EncoderConfig &cfg, EncoderState &st) {
    const int ncells = cfg.cols * cfg.rows;

    // Structure side: 2-plane (modes inline) vs 3-plane (mode plane), pick smaller.
    std::vector<uint8_t> body2; // 2-plane structure
    append_plane(body2, st.struct_stream);

    // Mode plane, optionally zero-run-length-encoded (TVID_FLAG_MODERLE):
    // SKIP=0 dominates, so collapsing zero runs beats Huffman's 1-bit floor.
    // Auto-select plain vs RLE by compressed size. Codec shared with the decoder
    // via mode_rle.h.
    std::vector<uint8_t> mode_rle(st.mode_plane.size() * 2 + 1);
    mode_rle.resize((size_t)mode_rle_encode(
        st.mode_plane.data(), (long)st.mode_plane.size(), mode_rle.data()));
    std::vector<uint8_t> mode_body_plain, mode_body_rle;
    append_plane(mode_body_plain, st.mode_plane);
    append_plane(mode_body_rle, mode_rle);
    bool use_moderle = mode_body_rle.size() < mode_body_plain.size();

    std::vector<uint8_t> body3; // 3-plane: mode-free structure + mode plane
    append_plane(body3, st.struct_nomode);
    if (use_moderle) body3.insert(body3.end(), mode_body_rle.begin(), mode_body_rle.end());
    else             body3.insert(body3.end(), mode_body_plain.begin(), mode_body_plain.end());
    const size_t body2_sz = body2.size(), body3_sz = body3.size();
    bool use_modeplane = body3_sz < body2_sz;

    // Cell side: one combined plane vs two by-mode planes (raster + palette),
    // independently entropy-coded; pick smaller (TVID_FLAG_CELLSPLIT).
    std::vector<uint8_t> cell_body1; // combined
    append_plane(cell_body1, st.cell_plane);
    std::vector<uint8_t> cell_body2; // raster + palette
    append_plane(cell_body2, st.raster_plane);
    append_plane(cell_body2, st.pal_plane);
    const size_t cell1_sz = cell_body1.size(), cell2_sz = cell_body2.size();
    bool use_cellsplit = cell2_sz < cell1_sz;

#ifdef TVID_PROBE
    // Lever: drop the per-frame [u16 len] from the structure plane (frame
    // boundaries are implicit in the quadtree walk). Measured ~3-4 KB; NOT
    // shipped (would need a 9th header-flag bit / 16-bit flags). Re-check here.
    {
        std::vector<uint8_t> b2, b2n, b3, b3n;
        append_plane(b2, st.struct_stream);   append_plane(b2n, st.probe_nolen_2plane);
        append_plane(b3, st.struct_nomode);   append_plane(b3n, st.probe_nolen_3plane);
        std::fprintf(stderr,
            "probe[nolen]: 2-plane struct len=%zu nolen=%zu (Δ %+zd)  "
            "3-plane struct len=%zu nolen=%zu (Δ %+zd)\n",
            b2.size(), b2n.size(), (ssize_t)b2n.size() - (ssize_t)b2.size(),
            b3.size(), b3n.size(), (ssize_t)b3n.size() - (ssize_t)b3.size());
    }
    // Lever (mono only): split the cell byte [luma:2|glyph:6] into two field
    // planes -- a luma plane (one byte/cell, only TVID_MONO_LUMA_LEVELS distinct
    // values, smooth + temporally stable) and a glyph plane (the 6-bit index, a
    // skewed 64-symbol alphabet). The fields have very different statistics;
    // coding them together pollutes both Huffman contexts. Measure each separately
    // vs the combined cell plane. Compared against the entropy-coded combined
    // plane per CLAUDE.md.
    {
        std::vector<uint8_t> luma_plane, glyph_plane;
        luma_plane.reserve(st.cell_plane.size());
        glyph_plane.reserve(st.cell_plane.size());
        for (uint8_t c : st.cell_plane) {
            luma_plane.push_back(TVID_CELL_LUMA(c));
            glyph_plane.push_back(TVID_CELL_MGLYPH(c));
        }
        std::vector<uint8_t> comb, lp, gp;
        append_plane(comb, st.cell_plane);
        append_plane(lp, luma_plane);
        append_plane(gp, glyph_plane);
        std::fprintf(stderr,
            "probe[mono-cellsplit]: combined=%zu  luma=%zu + glyph=%zu = %zu "
            "(Δ %+zd, %.1f%%)\n",
            comb.size(), lp.size(), gp.size(), lp.size() + gp.size(),
            (ssize_t)(lp.size() + gp.size()) - (ssize_t)comb.size(),
            100.0 * (double)(lp.size() + gp.size()) / (double)comb.size());
        // Entropy-gap probe: dump the raw cell plane so an external xz/gzip run can
        // bound how much a stronger back-end (#2) could win on the mono cell stream
        // specifically. Our entropy coder result is `comb`.
        if (const char *p = getenv("TVID_CELLDUMP")) {
            FILE *cf = fopen(p, "wb");
            if (cf) { fwrite(st.cell_plane.data(), 1, st.cell_plane.size(), cf); fclose(cf); }
            std::fprintf(stderr,
                "probe[celldump]: wrote %zu raw cell bytes to %s "
                "(ours entropy-coded=%zu)\n", st.cell_plane.size(), p, comb.size());
        }
    }
    // Per-leaf cell-byte decorrelation lever (color + mono). For each RAW leaf,
    // predict each cell from its left/up in-leaf neighbor and store the residual;
    // entropy-code the residual plane vs the plain RAW raster plane. If residuals
    // don't beat plain through the REAL coder, the predictor is dead. Measured
    // against the entropy-coded plane per CLAUDE.md, never raw.
    if (!g_probe_raw_plain.empty()) {
        std::vector<uint8_t> plain_b, left_b, up_b;
        append_plane(plain_b, g_probe_raw_plain);
        append_plane(left_b,  g_probe_raw_left);
        append_plane(up_b,    g_probe_raw_up);
        std::fprintf(stderr,
            "probe[raw-predict]: n=%zu  plain=%zu  left=%zu (Δ %+zd)  "
            "up=%zu (Δ %+zd)\n",
            g_probe_raw_plain.size(), plain_b.size(),
            left_b.size(), (ssize_t)left_b.size() - (ssize_t)plain_b.size(),
            up_b.size(),   (ssize_t)up_b.size()   - (ssize_t)plain_b.size());
    }
#endif

    std::vector<uint8_t> body = use_modeplane ? std::move(body3)
                                              : std::move(body2);
    if (use_cellsplit) body.insert(body.end(), cell_body2.begin(), cell_body2.end());
    else               body.insert(body.end(), cell_body1.begin(), cell_body1.end());
    // Color plane last (TVID_FLAG_COLOR): one entropy-coded plane, auto-selected
    // like the rest. Its bytes mirror the cell plane's leaf structure.
    size_t color_body_sz = 0;
    if (cfg.color) {
        std::vector<uint8_t> color_body;
        append_plane(color_body, st.color_plane);
        color_body_sz = color_body.size();
        body.insert(body.end(), color_body.begin(), color_body.end());
    }

    std::vector<uint8_t> header;
    header.push_back(TVID_MAGIC0); header.push_back(TVID_MAGIC1);
    header.push_back(TVID_MAGIC2); header.push_back(TVID_MAGIC3);
    header.push_back(TVID_VERSION_V3); // non-segmented whole-plane body
    {
        uint8_t hflags = TVID_FLAG_SPLIT;
        if (use_modeplane) hflags |= TVID_FLAG_MODEPLANE;
        if (use_modeplane && use_moderle) hflags |= TVID_FLAG_MODERLE;
        if (use_cellsplit) hflags |= TVID_FLAG_CELLSPLIT;
        if (cfg.shift > 0) hflags |= TVID_FLAG_SHIFT;
        if (cfg.has_audio) hflags |= TVID_FLAG_AUDIO;
        if (cfg.color) hflags |= TVID_FLAG_COLOR;
        header.push_back(hflags);
    }
    header.push_back((uint8_t)cfg.cols);
    header.push_back((uint8_t)cfg.rows);
    header.push_back((uint8_t)cfg.fps);
    put_u32le(header, (uint32_t)st.targets.size());
    {
        const char *ramp = TVID_HEADER_RAMP;
        uint8_t ramp_len = (uint8_t)TVID_HEADER_RAMP_LEN;
        header.push_back(ramp_len);
        for (uint8_t i = 0; i < ramp_len; ++i) header.push_back((uint8_t)ramp[i]);
    }
    if (cfg.has_audio)
        put_audio_subheader(header, cfg.audio_rate, st.audio_samples,
                            (long)st.audio_blob.size());
    FILE *fp = std::fopen(cfg.out_path.c_str(), "wb");
    if (!fp) enc_die("cannot open output file");
    std::fwrite(header.data(), 1, header.size(), fp);
    std::fwrite(body.data(), 1, body.size(), fp);
    if (cfg.has_audio) std::fwrite(st.audio_blob.data(), 1, st.audio_blob.size(), fp);
    std::fclose(fp);

    size_t total = header.size() + body.size();
    std::fprintf(stderr,
        "encoder: %zu frames @ %d fps, %dx%d -> %zu bytes (%.1f KB), "
        "%.0f B/frame [SPLIT%s%s%s] struct=%zu cell=%zu raw B "
        "(struct 2-plane=%zu 3-plane=%zu; cell combined=%zu split=%zu; "
        "mode plain=%zu rle=%zu; color raw=%zu coded=%zu)\n",
        st.targets.size(), cfg.fps, cfg.cols, cfg.rows, total, total / 1024.0,
        (double)total / st.targets.size(),
        use_modeplane ? (use_moderle ? "+MODE/RLE" : "+MODE") : "",
        use_cellsplit ? "+CELL" : "", cfg.color ? "+COLOR" : "",
        st.struct_stream.size(), st.cell_plane.size(),
        body2_sz, body3_sz, cell1_sz, cell2_sz,
        mode_body_plain.size(), mode_body_rle.size(),
        st.color_plane.size(), color_body_sz);
    if (cfg.stats) {
        long sb = st.bstats.split_bits + st.bstats.mode_bits + st.bstats.shift_bits +
                  st.bstats.sel_bits;
        std::fprintf(stderr,
            "stats: leaves SKIP=%ld SOLID=%ld RAW=%ld PAL2=%ld  "
            "struct_bits=%ld cell_bits=%ld\n",
            st.bstats.n_skip, st.bstats.n_solid, st.bstats.n_raw, st.bstats.n_pal2,
            sb, st.bstats.cell_bits);
    }
#ifdef TVID_PROBE
    if (cfg.color)
        std::fprintf(stderr,
            "probe[color]: %zu cells -> raw=%zu entropy-coded=%zu B "
            "(%.1f%%, %.2f B/frame added)\n",
            st.color_plane.size(), st.color_plane.size(), color_body_sz,
            st.color_plane.empty() ? 0.0 :
                100.0 * (double)color_body_sz / (double)st.color_plane.size(),
            (double)color_body_sz / (double)st.targets.size());
    if (st.probe_mono_cells)
        std::fprintf(stderr,
            "probe[mono-split]: %d luma + %d glyph (%d levels x %d glyphs)  "
            "mean ideal-cell distortion=%.1f  (file %.0f B/frame)\n",
            TVID_MONO_LUMA_BITS, TVID_MONO_GLYPH_BITS,
            TVID_MONO_LUMA_LEVELS, TVID_MONO_GLYPH_COUNT,
            st.probe_mono_dist / (double)st.probe_mono_cells,
            (double)total / st.targets.size());
#endif
    (void)ncells;
}

// --- v4 segmented split body: the same planes as write_split_output, but each is
//     sliced by segment (seg_frames frames per segment) and the chunks are emitted
//     segment-major, so the player only ever holds one segment of each plane
//     resident (see doc/v3-streaming.md). The whole-movie layout decisions
//     (MODEPLANE / MODERLE / CELLSPLIT) are still made once and carried in the
//     header flags; only the framing changes. This first cut emits segment 0 as a
//     keyframe segment and every later segment as a CARRY segment (fb persists,
//     cursors reset) -- byte-exact streaming with no re-encode. ---
void write_split_segmented_output(const EncoderConfig &cfg, EncoderState &st) {
    const int ncells = cfg.cols * cfg.rows;
    const uint32_t nframes = (uint32_t)st.targets.size();
    const uint32_t seg_frames = (uint32_t)cfg.seg;

    // Map a frame span [fa, fb) to a plane's byte span, then entropy-code that
    // slice into `out`. The offset tables follow the convention off[f] = cumulative
    // plane length *through* frame f (frame 0 = keyframe), so the byte span for
    // frames fa..fb-1 is [ (fa==0 ? 0 : off[fa-1]) .. off[fb-1] ). fb <= nframes, so
    // off[fb-1] is always in range (max index nframes-1).
    auto byte_lo = [](const std::vector<size_t> &off, uint32_t fa) -> size_t {
        return fa == 0 ? 0 : off[fa - 1];
    };
    auto append_slice = [](std::vector<uint8_t> &out, const std::vector<uint8_t> &plane,
                           size_t lo, size_t hi) {
        std::vector<uint8_t> slice(plane.begin() + (long)lo, plane.begin() + (long)hi);
        append_plane(out, slice);
    };

    // Whole-movie auto-select, exactly as write_split_output: structure 2-plane vs
    // 3-plane, mode plain vs RLE, cell combined vs cell-split. Decide globally so
    // the header carries one set of flags; each segment then emits the chosen
    // variant's slice. (Per-segment auto-select would need per-segment flags; keep
    // it simple and let the entropy coder reset per segment do the adapting.)
    std::vector<uint8_t> b2; append_plane(b2, st.struct_stream);
    std::vector<uint8_t> mode_rle(st.mode_plane.size() * 2 + 1);
    mode_rle.resize((size_t)mode_rle_encode(
        st.mode_plane.data(), (long)st.mode_plane.size(), mode_rle.data()));
    std::vector<uint8_t> mbp, mbr;
    append_plane(mbp, st.mode_plane);
    append_plane(mbr, mode_rle);
    bool use_moderle = mbr.size() < mbp.size();
    std::vector<uint8_t> b3; append_plane(b3, st.struct_nomode);
    if (use_moderle) b3.insert(b3.end(), mbr.begin(), mbr.end());
    else             b3.insert(b3.end(), mbp.begin(), mbp.end());
    bool use_modeplane = b3.size() < b2.size();
    std::vector<uint8_t> c1; append_plane(c1, st.cell_plane);
    std::vector<uint8_t> c2; append_plane(c2, st.raster_plane); append_plane(c2, st.pal_plane);
    bool use_cellsplit = c2.size() < c1.size();

    // Pick the structure plane + its offset table, and the cell plane(s) + tables.
    const std::vector<uint8_t> &struct_plane = use_modeplane ? st.struct_nomode : st.struct_stream;
    const std::vector<size_t>  &off_struct   = use_modeplane ? st.off_nomode   : st.off_struct;

    const uint32_t nseg = (nframes + seg_frames - 1) / seg_frames;

#ifdef TVID_PROBE
    // probe[seg-kf]: with per-segment keyframes (segmentation makes keyframes go
    // from once-per-movie to potentially once-per-segment) keyframe-cell
    // compression matters more. The keyframe is `ncells` raster-order cell bytes
    // (the first ncells of cell_plane / raster_plane). Measure whether a spatial
    // intra predictor on those bytes beats plain entropy THROUGH THE REAL CODER:
    // left = cur - left-neighbor, up = cur - up-neighbor (raster wrap), residual
    // mod 256. doc/abandoned-levers.md found PER-LEAF raw prediction dead (residuals
    // broke LZ matches); this re-measures it at WHOLE-KEYFRAME scope, which is the
    // open question. Negative Δ would justify a per-keyframe predictor method; we
    // ship plain until then. (Today the encoder emits CARRY for all later segments,
    // so this is the segment-0 keyframe -- representative of any future one.)
    if ((int)st.cell_plane.size() >= ncells) {
        std::vector<uint8_t> kf(st.cell_plane.begin(), st.cell_plane.begin() + ncells);
        std::vector<uint8_t> left(ncells), up(ncells);
        for (int i = 0; i < ncells; ++i) {
            int col = i % cfg.cols;
            uint8_t lp = (col == 0) ? 0 : kf[i - 1];          // left neighbor (0 at row start)
            uint8_t up_p = (i < cfg.cols) ? 0 : kf[i - cfg.cols]; // up neighbor (0 on first row)
            left[i] = (uint8_t)(kf[i] - lp);
            up[i]   = (uint8_t)(kf[i] - up_p);
        }
        std::vector<uint8_t> plain_b, left_b, up_b;
        append_plane(plain_b, kf);
        append_plane(left_b,  left);
        append_plane(up_b,    up);
        std::fprintf(stderr,
            "probe[seg-kf]: ncells=%d  plain=%zu  left=%zu (Δ %+zd)  up=%zu (Δ %+zd)\n",
            ncells, plain_b.size(),
            left_b.size(), (ssize_t)left_b.size() - (ssize_t)plain_b.size(),
            up_b.size(),   (ssize_t)up_b.size()   - (ssize_t)plain_b.size());
    }
#endif

    std::vector<uint8_t> body;
    for (uint32_t s = 0; s < nseg; ++s) {
        const uint32_t fa = s * seg_frames;
        const uint32_t fb = (fa + seg_frames < nframes) ? fa + seg_frames : nframes;

        // Structure chunk: a leading keyframe-vs-carry byte, then the frame slice.
        // Segment 0 is keyframe-led (its cell/color slice carries the keyframe);
        // later segments carry forward the previous segment's framebuffer.
        std::vector<uint8_t> sframed;
        sframed.push_back(s == 0 ? (uint8_t)TVID_SEG_KEY : (uint8_t)TVID_SEG_CARRY);
        sframed.insert(sframed.end(),
                       struct_plane.begin() + (long)byte_lo(off_struct, fa),
                       struct_plane.begin() + (long)off_struct[fb - 1]);
        append_plane(body, sframed);

        if (use_modeplane) {
            if (use_moderle) {
                // RLE is whole-plane; re-encode just this segment's mode slice.
                std::vector<uint8_t> mslice(st.mode_plane.begin() + (long)byte_lo(st.off_mode, fa),
                                            st.mode_plane.begin() + (long)st.off_mode[fb - 1]);
                std::vector<uint8_t> mr(mslice.size() * 2 + 1);
                mr.resize((size_t)mode_rle_encode(mslice.data(), (long)mslice.size(), mr.data()));
                append_plane(body, mr);
            } else {
                append_slice(body, st.mode_plane, byte_lo(st.off_mode, fa), st.off_mode[fb - 1]);
            }
        }
        if (use_cellsplit) {
            append_slice(body, st.raster_plane, byte_lo(st.off_raster, fa), st.off_raster[fb - 1]);
            append_slice(body, st.pal_plane,    byte_lo(st.off_pal, fa),    st.off_pal[fb - 1]);
        } else {
            append_slice(body, st.cell_plane, byte_lo(st.off_cell, fa), st.off_cell[fb - 1]);
        }
        if (cfg.color)
            append_slice(body, st.color_plane, byte_lo(st.off_color, fa), st.off_color[fb - 1]);
    }

    std::vector<uint8_t> header;
    header.push_back(TVID_MAGIC0); header.push_back(TVID_MAGIC1);
    header.push_back(TVID_MAGIC2); header.push_back(TVID_MAGIC3);
    header.push_back(TVID_VERSION); // 4
    {
        uint8_t hflags = TVID_FLAG_SPLIT;
        if (use_modeplane) hflags |= TVID_FLAG_MODEPLANE;
        if (use_modeplane && use_moderle) hflags |= TVID_FLAG_MODERLE;
        if (use_cellsplit) hflags |= TVID_FLAG_CELLSPLIT;
        if (cfg.shift > 0) hflags |= TVID_FLAG_SHIFT;
        if (cfg.has_audio) hflags |= TVID_FLAG_AUDIO;
        if (cfg.color) hflags |= TVID_FLAG_COLOR;
        header.push_back(hflags);
    }
    header.push_back((uint8_t)cfg.cols);
    header.push_back((uint8_t)cfg.rows);
    header.push_back((uint8_t)cfg.fps);
    put_u32le(header, nframes);
    put_u16le(header, (uint16_t)seg_frames); // v4 field, right after frame_count
    {
        const char *ramp = TVID_HEADER_RAMP;
        uint8_t ramp_len = (uint8_t)TVID_HEADER_RAMP_LEN;
        header.push_back(ramp_len);
        for (uint8_t i = 0; i < ramp_len; ++i) header.push_back((uint8_t)ramp[i]);
    }
    if (cfg.has_audio)
        put_audio_subheader(header, cfg.audio_rate, st.audio_samples,
                            (long)st.audio_blob.size());

    FILE *fp = std::fopen(cfg.out_path.c_str(), "wb");
    if (!fp) enc_die("cannot open output file");
    std::fwrite(header.data(), 1, header.size(), fp);
    std::fwrite(body.data(), 1, body.size(), fp);
    if (cfg.has_audio) std::fwrite(st.audio_blob.data(), 1, st.audio_blob.size(), fp);
    std::fclose(fp);

    size_t total = header.size() + body.size();
    std::fprintf(stderr,
        "encoder: %u frames @ %d fps, %dx%d -> %zu bytes (%.1f KB), %.0f B/frame "
        "[SPLIT/SEG=%u nseg=%u%s%s%s]\n",
        nframes, cfg.fps, cfg.cols, cfg.rows, total, total / 1024.0,
        (double)total / nframes, seg_frames, nseg,
        use_modeplane ? (use_moderle ? "+MODE/RLE" : "+MODE") : "",
        use_cellsplit ? "+CELL" : "", cfg.color ? "+COLOR" : "");
    (void)ncells;
}

// --- optional whole-stream entropy back-end (orthogonal to the block coder).
//     Try both LZSS and LZSS+Huffman; keep whichever is smaller (and only if it
//     beats storing raw). Each prepends a u32le raw_len so the decoder can size
//     its output buffer. ---
void write_interleaved_output(const EncoderConfig &cfg, EncoderState &st) {
    const int ncells = cfg.cols * cfg.rows;
    const size_t raw_stream_len = st.stream.size();
    std::vector<uint8_t> body;
    bool used_lzss = false, used_huff = false;
    if (cfg.compress) {
        std::vector<uint8_t> lz(raw_stream_len + raw_stream_len / 16 + 64);
        long lc = lzss_compress(st.stream.data(), (long)raw_stream_len, lz.data(),
                                (long)lz.size());
        std::vector<uint8_t> hf(raw_stream_len + raw_stream_len / 16 + 1024);
        long hc = entropy_compress(st.stream.data(), (long)raw_stream_len, hf.data(),
                                   (long)hf.size());
        long best = (long)raw_stream_len; // raw, no length prefix
        int which = 0;                    // 0=raw, 1=lzss, 2=huff
        if (lc > 0 && lc + 4 < best) { best = lc + 4; which = 1; }
        if (hc > 0 && hc + 4 < best) { best = hc + 4; which = 2; }
        if (which == 1) {
            used_lzss = true;
            put_u32le(body, (uint32_t)raw_stream_len);
            body.insert(body.end(), lz.begin(), lz.begin() + lc);
        } else if (which == 2) {
            used_huff = true;
            put_u32le(body, (uint32_t)raw_stream_len);
            body.insert(body.end(), hf.begin(), hf.begin() + hc);
        }
    }
    if (!used_lzss && !used_huff) body = std::move(st.stream);

    // --- header ---
    std::vector<uint8_t> header;
    header.push_back(TVID_MAGIC0);
    header.push_back(TVID_MAGIC1);
    header.push_back(TVID_MAGIC2);
    header.push_back(TVID_MAGIC3);
    header.push_back(TVID_VERSION_V3); // whole-stream interleaved body
    {
        uint8_t hflags = 0;
        if (used_lzss) hflags |= TVID_FLAG_LZSS;
        if (used_huff) hflags |= TVID_FLAG_HUFF;
        if (cfg.shift > 0) hflags |= TVID_FLAG_SHIFT;
        if (cfg.has_audio) hflags |= TVID_FLAG_AUDIO;
        header.push_back(hflags);
    }
    header.push_back((uint8_t)cfg.cols);
    header.push_back((uint8_t)cfg.rows);
    header.push_back((uint8_t)cfg.fps);
    put_u32le(header, (uint32_t)st.targets.size());
    const char *ramp = TVID_HEADER_RAMP;
    uint8_t ramp_len = (uint8_t)TVID_HEADER_RAMP_LEN;
    header.push_back(ramp_len);
    for (uint8_t i = 0; i < ramp_len; ++i) header.push_back((uint8_t)ramp[i]);
    if (cfg.has_audio)
        put_audio_subheader(header, cfg.audio_rate, st.audio_samples,
                            (long)st.audio_blob.size());

    FILE *fp = std::fopen(cfg.out_path.c_str(), "wb");
    if (!fp) enc_die("cannot open output file");
    std::fwrite(header.data(), 1, header.size(), fp);
    std::fwrite(body.data(), 1, body.size(), fp);
    if (cfg.has_audio) std::fwrite(st.audio_blob.data(), 1, st.audio_blob.size(), fp);
    std::fclose(fp);

    size_t total = header.size() + body.size();
    std::fprintf(stderr,
                 "encoder: %zu frames @ %d fps, %dx%d -> %zu bytes (%.1f KB), "
                 "%.0f B/frame, stable=%d lookahead=%d shift=%d block-stable=%d "
                 "lambda=%d (%.0f%% cells held)%s\n",
                 st.targets.size(), cfg.fps, cfg.cols, cfg.rows, total, total / 1024.0,
                 (double)raw_stream_len / st.targets.size(), cfg.stable, cfg.lookahead,
                 cfg.shift, cfg.block_stable, cfg.lambda,
                 st.total_cells ? 100.0 * (double)st.held / (double)st.total_cells : 0.0,
                 (used_lzss || used_huff) ? ""
                     : (cfg.compress ? " [compress: no gain, stored raw]" : ""));
    if (used_lzss || used_huff)
        std::fprintf(stderr, "encoder: %s %zu -> %zu B (%.0f%% of raw)\n",
                     used_huff ? "lzss+huff" : "lzss",
                     raw_stream_len, body.size(),
                     100.0 * (double)body.size() / (double)raw_stream_len);

    if (cfg.stats) {
        // Convert the per-frame bit tallies to bytes. structure = everything
        // bit-packed that is NOT a literal cell byte; literal = the cell bytes
        // (which the stream-split prototype would keep byte-aligned in a 2nd plane).
        auto B = [](long bits) { return (bits + 7) / 8; };
        long struct_bits = st.bstats.split_bits + st.bstats.mode_bits +
                           st.bstats.shift_bits + st.bstats.sel_bits;
        long struct_B = B(struct_bits), cell_B = B(st.bstats.cell_bits);
        long key_B = (long)ncells; // the single raw keyframe
        long align_B = (long)(raw_stream_len) - key_B - st.framing_bytes
                       - B(struct_bits + st.bstats.cell_bits);
        std::fprintf(stderr,
            "stats: raw_stream=%zu B  keyframe=%ld B  framing=%ld B  "
            "byte-align-slack=%ld B\n",
            raw_stream_len, key_B, st.framing_bytes, align_B);
        std::fprintf(stderr,
            "stats: structure=%ld B (split=%ld mode=%ld sel=%ld shift=%ld)  "
            "cells=%ld B  -> structure/cells = %.2f\n",
            struct_B, B(st.bstats.split_bits), B(st.bstats.mode_bits),
            B(st.bstats.sel_bits), B(st.bstats.shift_bits), cell_B,
            cell_B ? (double)struct_bits / (double)st.bstats.cell_bits : 0.0);
        std::fprintf(stderr,
            "stats: leaves  SKIP=%ld SOLID=%ld RAW=%ld PAL2=%ld\n",
            st.bstats.n_skip, st.bstats.n_solid, st.bstats.n_raw, st.bstats.n_pal2);
    }
}
