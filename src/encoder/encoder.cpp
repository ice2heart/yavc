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

#ifdef TVID_PROBE
#include <vector>
extern "C" {
#include "range.h"
#ifdef TVID_PROBE
#include "adpcm_ctx_probe.hpp"
#endif
}
#endif

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
#ifdef TVID_PROBE
        // probe[adpcm-ent]: measure the entropy-coded audio tail against the raw
        // ADPCM blob (the only DOS-viable audio lever -- doc/abandoned-levers.md
        // ruled out lossless LPC and non-integer codecs). Reports the shipped
        // grouped size and the whole-stream range ceiling so the group knee
        // (TVID_AUDIO_ENT_GROUP) is chosen from data. Measure-first per CLAUDE.md.
        {
            long raw = (long)st.audio_blob.size();
            std::vector<uint8_t> whole(raw + raw / 16 + 1024);
            long wc = range_compress(st.audio_blob.data(), raw, whole.data(),
                                     (long)whole.size());
            std::fprintf(stderr, "probe[adpcm-ent]: raw=%ld whole-stream=%ld (%.1f%%)\n",
                raw, wc, wc > 0 ? 100.0 * wc / raw : 0.0);
            /* Sweep group sizes K to find the knee: the per-group model reset taxes
             * small K (the adaptive coder barely warms up). Pick the smallest K
             * within ~1% of whole-stream that still bounds the decompressed group. */
            for (int K : {1, 4, 16, 64, 256, 1024}) {
                long sz = entropy_wrap_adpcm_k(st.audio_blob, st.audio_samples, K).size();
                std::fprintf(stderr, "probe[adpcm-ent]:   K=%-4d grouped=%8ld (%.1f%%)\n",
                    K, sz, 100.0 * sz / raw);
            }
            // probe[adpcm-ctx]: the same nibble stream, but entropy-coded with a
            // model keyed on the ADPCM step index (free decoder context) instead
            // of the generic order-1 byte coder. Baseline = whole-stream above (wc).
            tvid_probe::measure_adpcm_ctx(st.audio_blob, st.audio_samples);
        }
#endif
        // --audio-entropy: replace the raw ADPCM tail with the entropy-coded one
        // (codec 3). Lossless vs codec 1; the decoder decompresses each chunk back
        // to identical ADPCM bytes before adpcm_decode_block. Codec 3 auto-selects
        // per chunk among stored / order-1-byte range (codec 2's method 3) / the
        // step-index-context nibble coder (method 4), so it is a strict superset of
        // codec 2 -- it never regresses and wins ~2-3% off the audio where the
        // context coder fires. See doc/audiocodec-evo.md.
        if (cfg.audio_entropy) {
            std::vector<uint8_t> ent =
                ctx_wrap_adpcm(st.audio_blob, st.audio_samples);
            std::fprintf(stderr,
                "encoder: audio entropy-coded %zu B -> %zu B (%.1f%%)\n",
                st.audio_blob.size(), ent.size(),
                100.0 * ent.size() / st.audio_blob.size());
            st.audio_blob = std::move(ent);
            st.audio_codec = TVID_AUDIO_IMA_ADPCM_CTX;
        }
    }

    pass1a_read_frames(cfg, st);
    pass1b_hysteresis(cfg, st);
    pass2_encode(cfg, st);

    if (cfg.split && cfg.seg > 0) write_split_segmented_output(cfg, st);
    else if (cfg.split)           write_split_output(cfg, st);
    else                          write_interleaved_output(cfg, st);
    return 0;
}
