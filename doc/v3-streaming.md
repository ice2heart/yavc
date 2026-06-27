# Streaming the split planes (chunked planes) — design note / TODO

**Status:** not implemented. This captures the problem, why the obvious fix is a
format change, and a concrete design, so it can be picked up later.

## The problem

In the `TVID_FLAG_SPLIT` body, each plane (structure, mode, cell/raster, palette,
color) is **one continuous entropy stream** over the *whole movie*, and the
player decompresses each one **fully resident** before playback
([player.c](../src/decoder/player.c), the `read_plane` calls). On the host that's
nothing; on the DOS/4GW target it adds up. For a 3-minute clip the resident set is
roughly:

| plane (raw, hi-res 160×48, ~2000 frames) | approx |
|------------------------------------------|--------|
| structure | ~3 MB |
| cell / raster | ~2.5 MB |
| mode | ~0.5 MB |
| **color** (new, `TVID_FLAG_COLOR`) | ~2.5 MB |
| decoded audio PCM (8 kHz, decoded whole in `audio_load`) | ~3.3 MB |

That total can exceed what the DOS/4GW DPMI host hands out, which is the "out of
memory" the color layer surfaced (the color plane is the extra ~2.5 MB that tips
hi-res clips over). The color path now degrades to grayscale rather than aborting
(`read_plane_opt`), but the underlying whole-file-resident model is the real
limit, and it caps how long/large any v3 clip can be on DOS regardless of color.

## Why it's a format change, not a player tweak

You can't stream a plane as-is. Each plane is a single range-coder / Huffman /
LZSS stream: **symbol N depends on the entire history 0..N-1**, so there is no
seek point and no way to decode "just the next frame's bytes" without having
decoded everything before it. To stream, the *format* must expose independent
restart points.

The audio track already does exactly this and is the model to copy: the IMA-ADPCM
payload is a sequence of **self-contained blocks**, so the DOS SB DMA path starts
at any block boundary and refills as it plays
([tvid_format.h](../src/common/tvid_format.h) audio section, `adpcm.h`). The video
planes need the same treatment.

## Proposed design: segmented planes

Introduce a new flag (e.g. `TVID_FLAG_SEGMENTED`) under `SPLIT`. With it, each
plane is emitted as **independent segments**, each segment covering a fixed span
of frames (e.g. `SEG = 64` frames, or a keyframe group):

```
plane := [u16 seg_frames][u32 nsegments] then nsegments x:
           [u8 method][u32 raw_len][u32 payload_len][payload]
```

Each segment is its own entropy stream (its own range-coder state / Huffman
table), so it can be decoded in isolation. Properties:

- **Encoder** ([encoder.cpp](../src/encoder/encoder.cpp)): currently it
  accumulates each plane across all frames into one `std::vector` and entropy-codes
  it once (`append_plane`). Segmented: flush a plane segment every `SEG` frames —
  i.e. close the current segment's byte buffer, `append_plane` it, and start a
  fresh one. The cell/color cursors already reset naturally per segment if the
  segment boundary is also a frame boundary (it is). The per-plane auto-select
  runs per segment (a segment can pick a different method than its neighbour).
- **Player**: keep a small ring of decoded segments per plane (current + maybe
  next, prefetched). At a segment boundary, free the old segment and
  `read_plane` the next from `fp` (so the file is no longer fully consumed up
  front — the player seeks/streams). Resident memory drops from "whole movie" to
  "≈ SEG frames × planes" + the audio ring. The per-frame decode loop is
  unchanged except it indexes into the current segment and refills at the
  boundary.
- **Cursors**: `cell_pos` / `color_pos` / `mode_pos` / `pal_pos` become
  segment-relative (reset to 0 — or to `ncells` for the keyframe in segment 0 —
  at each segment start). The keyframe only exists in segment 0.
- **Audio**: already streamable in principle; today `audio_load` decodes the
  whole PCM up front. A parallel improvement is to decode ADPCM blocks on demand
  in `audio_pump` instead of all at once, cutting the ~3.3 MB PCM buffer to a
  small ring. Independent of the plane work but the same spirit.

### Trade-offs

- **Size**: per-segment entropy resets cost a little — each segment re-learns the
  range-coder model from scratch and re-emits any Huffman table. Smaller `SEG` =
  less RAM but worse ratio. Measure the size hit per `SEG` (probe build) and pick
  the knee; `SEG` could even be a header field so the encoder can trade RAM vs
  size per target. The structure plane already carries a per-frame `[u16 len]`, so
  frame boundaries inside a segment are still found the same way.
- **Decoder cost**: a model reset every `SEG` frames is negligible vs the
  per-frame work; the DOS player is wait-bound anyway (see the
  `dos-player-render-not-bottleneck` finding / `-DTVID_PROF`).
- **Compatibility**: gate behind `TVID_FLAG_SEGMENTED`; non-segmented v3 files
  still decode the whole-plane way. The host player doesn't need it (plenty of
  RAM) but should read both.

## Scope

This touches the format doc, the encoder's plane emission, and the player's plane
loading — a focused but real change, separate from the color layer. Until it
lands, the guidance for DOS is: for `--hi-res --color`, lower `--fps`, shorten the
clip, or `--no-audio`; or accept the automatic grayscale fallback.
