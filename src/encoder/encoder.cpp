// encoder.cpp - raw rgb24 frames (stdin) -> .tvid v3 container (C++23).
//
// Input: cols x (rows*2) rgb24 pixels per frame, read until EOF (ffmpeg pipe).
// Output: TVID v3 header + one keyframe + quadtree block frames, in either the
// split (per-plane entropy-coded) or interleaved (whole-stream) layout.
//
// Pipeline per frame (all offline/heavy; the decoder replays only the result):
//   1. average vertical pixel pairs -> per-cell sub-pixel luma (and, with
//      --color, an xterm-256 hue) -- pass1a
//   2. temporal hysteresis: a cell's target only moves when it travels more than
//      --stable from what's on screen (kills quantization shimmer) -- pass1b
//   3. quantize the (held/updated) target to an "ideal" cell grid and run the
//      block coder: quadtree RD search picks SKIP/SHIFT/SOLID/PAL2/RAW per leaf
//      -- pass2
//   4. write the split or interleaved body + header (+ optional audio tail)
//
// The per-stage logic lives in enc_stages.cpp; main() just sequences them.
#include "enc_stages.hpp"

#include <cstdio>

int main(int argc, char **argv) {
    EncoderConfig cfg = parse_args(argc, argv);
    EncoderState st;

    // Encode the audio track up front (if any) so audio_bytes is known before the
    // video header is written. The ADPCM blob is appended after the video body at
    // whichever of the two write paths (split / whole-stream) runs.
    if (cfg.has_audio) {
        st.audio_blob = encode_audio(cfg.audio_pcm_path, &st.audio_samples);
        std::fprintf(stderr,
            "encoder: audio %ld samples @ %d Hz -> %zu B ADPCM (%.1f KB)\n",
            st.audio_samples, cfg.audio_rate, st.audio_blob.size(),
            st.audio_blob.size() / 1024.0);
    }

    pass1a_read_frames(cfg, st);
    pass1b_hysteresis(cfg, st);
    pass2_encode(cfg, st);

    if (cfg.split) write_split_output(cfg, st);
    else           write_interleaved_output(cfg, st);
    return 0;
}
