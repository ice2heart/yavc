// enc_stages.hpp - encoder pipeline broken into named stages. encoder.cpp's
// main() was a ~620-line monolith doing arg parsing, audio, two filter passes,
// the RD encode loop, and two output writers. The stages here keep that logic
// verbatim (byte-identical output) but each in one focused function. Shared
// buffers travel in EncoderConfig (parsed CLI) + EncoderState (the big working
// buffers and accumulators) rather than as a long parameter list.
#ifndef ENC_STAGES_HPP
#define ENC_STAGES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "blockcoder.hpp"

// Parsed command-line knobs (immutable once parse_args returns).
struct EncoderConfig {
    int fps = 10, cols = 0, rows = 0;
    int stable = 16;          // per-cell RGB deadband (shimmer suppression)
    int lookahead = 0;        // peek N future frames to suppress transient churn
    int shift = 0;            // SHIFT motion search radius in cells (0 = off)
    int block_stable = 2000;  // block-level SKIP hysteresis (distortion credit)
    int lambda = 6;           // RD weight: cost = lambda*bits + distortion
    bool compress = false;
    bool stats = false;       // --stats: structure-vs-literal byte accounting
    bool split = false;       // --split: per-plane entropy-coded body
    int  seg = 0;             // --seg N: v4 segmented split body (0 = v3 whole-plane)
    bool color = false;       // --color: add the xterm-256 color plane
    std::string out_path;
    std::string audio_pcm_path; // raw s16le mono PCM to embed
    int audio_rate = 8000;
    bool has_audio = false;
};

// Big working buffers + accumulators, owned across the pipeline stages.
struct EncoderState {
    // pass 1a output: per-frame sub-pixel luma (cols*rows*TVID_MONO_SUBN) and,
    // with --color, per-cell hue (cols*rows).
    std::vector<std::vector<uint8_t>> avgs, hues;
    // pass 1b output: per-frame held/updated luma + hue targets.
    std::vector<std::vector<uint8_t>> targets, ctargets;
    long held = 0, total_cells = 0;

    // pass 2 plane accumulators.
    std::vector<uint8_t> stream;        // interleaved layout
    std::vector<uint8_t> struct_stream; // split 2-plane: per-frame [u16 len][bits]
    std::vector<uint8_t> cell_plane;    // split: keyframe cells then leaf cells
    std::vector<uint8_t> struct_nomode; // split 3-plane: structure, modes pulled
    std::vector<uint8_t> mode_plane;    // split 3-plane: one mode byte per leaf
    std::vector<uint8_t> raster_plane;  // cell-split: keyframe + RAW-leaf cells
    std::vector<uint8_t> pal_plane;     // cell-split: SOLID + PAL2 palette cells
    std::vector<uint8_t> color_plane;   // color: keyframe hues then leaf hues
    // v4 segmentation: cumulative byte length of each plane after each frame, so a
    // segment's slice of a plane is [off[seg_start] .. off[seg_end]). off[0]=0;
    // off has frame_count+1 entries (entry f = length after frame f's data is
    // appended). Filled in pass2 alongside the plane accumulators above; unused
    // for v3 (cfg.seg == 0).
    std::vector<size_t> off_struct, off_nomode, off_cell, off_mode;
    std::vector<size_t> off_raster, off_pal, off_color;
    long framing_bytes = 0;
    BlockStats bstats;

    // audio (encoded up front so audio_bytes is known before the header).
    std::vector<uint8_t> audio_blob;
    long audio_samples = 0;

#ifdef TVID_PROBE
    // Probe accumulators carried from pass 2 into the split-output writer.
    std::vector<uint8_t> probe_nolen_2plane, probe_nolen_3plane;
    double probe_mono_dist = 0; long probe_mono_cells = 0;
#endif
};

// Pipeline stages. Each does exactly the work main() used to do inline.
[[noreturn]] void enc_die(const char *msg);
EncoderConfig parse_args(int argc, char **argv);
std::vector<uint8_t> encode_audio(const std::string &path, long *out_samples);
void pass1a_read_frames(const EncoderConfig &cfg, EncoderState &st);
void pass1b_hysteresis(const EncoderConfig &cfg, EncoderState &st);
void pass2_encode(const EncoderConfig &cfg, EncoderState &st);
void write_split_output(const EncoderConfig &cfg, EncoderState &st);
void write_split_segmented_output(const EncoderConfig &cfg, EncoderState &st);
void write_interleaved_output(const EncoderConfig &cfg, EncoderState &st);

#endif // ENC_STAGES_HPP
