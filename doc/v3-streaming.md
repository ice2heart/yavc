# Streaming the split planes (segmented planes) — v4 spec

**Status:** specified, not yet implemented. This captures the problem, why the
fix is a format change (a clean bump to **v4**), and the concrete design across
format / encoder / player, so it can be built directly.

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

## Why a version bump (v4), not a new flag

All 8 header flag bits are already consumed
(`TVID_FLAG_LZSS..TVID_FLAG_MODERLE`,
[tvid_format.h](../src/common/tvid_format.h)), so there is no `TVID_FLAG_SEGMENTED`
to spend. A second flags byte would still be a format the v3 decoder cannot read,
so it buys nothing over a clean version bump — and the decoder already rejects any
unknown flag and any `version != 3`. So segmentation lands as **v4**:

> **v4 = a SPLIT body whose every plane is segmented, with the audio payload
> consumed in ADPCM blocks on demand.**

v4 *implies* `SPLIT` (segmentation only makes sense for the split body) and
reuses every existing SPLIT flag (`MODEPLANE`, `MODERLE`, `CELLSPLIT`, `COLOR`,
`SHIFT`) with identical meaning. Only the *framing* of each plane changes. v3
files keep decoding the whole-resident way unchanged; the host player reads both.

## Format (v4)

New constants in [tvid_format.h](../src/common/tvid_format.h):

```c
#define TVID_VERSION_V3 3
#define TVID_VERSION    4   /* segmented split planes; streamable */
#define TVID_SEG_DEFAULT 64 /* frames per segment, unless the header overrides */
```

The header gains one field after `fps`, present only when `version >= 4`:

```
[u16le seg_frames]   /* frames per segment; lets the encoder trade RAM vs ratio */
```

It sits where the v3 parse never looks, so v3 readers are untouched.

### Segmented plane body

In a v4 SPLIT body the planes are no longer one chunk each over the whole movie;
they are **interleaved by segment**, so the player only ever holds the current
segment of every plane:

```
nsegments = ceil(frame_count / seg_frames)
for seg in 0 .. nsegments-1:
    structure-chunk            # [u8 method][u32 raw][u32 plen][payload]
    [mode-chunk if MODEPLANE]
    cell-chunk   (or raster-chunk + palette-chunk if CELLSPLIT)
    [color-chunk if COLOR]
```

Every chunk keeps the **exact existing on-disk shape**, so the player's
`read_plane` / `read_plane_opt` are reused verbatim and per-segment auto-select
stays (a segment can pick a different entropy method than its neighbour). The
structure plane keeps its per-frame `[u16 len]` prefix, so frame boundaries
*inside* a segment are found exactly as today.

Segment-major interleave (rather than plane-major) is the key choice: the player
reads the N+1th segment of all planes in one forward sweep with no seeking, and
the whole-movie resident set never materialises.

### Segment boundaries: encoder decides keyframe-vs-carry

Across a boundary the live question is inter-frame state — SKIP/SHIFT leaves
reference the *previous* frame. The encoder chooses **per segment** whether the
segment's first frame is:

- a **keyframe** — the segment is then independently decodable (a true
  seek/restart point, and the natural choice at a scene cut); or
- a **carry** — no keyframe; the first frame is a normal block frame whose
  SKIP/SHIFT leaves read the previous segment's last `fb`. Cheaper on a
  continuous scene.

One bit per segment records the choice, carried as a **leading byte in that
segment's structure chunk** (before the first frame's `[u16 len]`), so the player
learns it while reading the segment — no extra plane. Segment 0 is always
keyframe-led, as today. `fb` / `colfb` persist across boundaries regardless: a
carry segment keeps using them; a keyframe segment overwrites them from its
keyframe bytes (the first `ncells` of its cell/color chunk).

Consequence: keyframes go from once-per-movie to potentially **once-per-segment**
— "more often" — so keyframe-cell compression matters more than it did in v3.

### Keyframe compression — probe first, do not assume

A keyframe is `cols*rows` cell bytes that already flow through the cell plane's
per-chunk auto-select (methods 0–3, incl. the order-1 range coder), so it is
already entropy-coded. The open question is whether a **spatial (left/up) intra
predictor on keyframe cells** before entropy pays off. It is **not** a foregone
win: [abandoned-levers.md](abandoned-levers.md) found *per-leaf* raw prediction
DEAD (residuals destroyed the LZ matches). So measure whole-keyframe intra
specifically, as a `probe[seg-kf]` line under `-DTVID_PROBE` (see
[CLAUDE.md](../CLAUDE.md)): keyframe-chunk bytes plain-entropy vs
intra-then-entropy, against the entropy baseline, never raw. v4 ships plain
entropy for keyframes; intra is wired in only if the probe wins.

### Audio (v4)

The ADPCM payload stays at EOF, but the player decodes **one block at a time on
demand** in `audio_pump` into a small ring, instead of `audio_load` decoding all
of it up front. Blocks are already self-contained ([adpcm.h](../src/common/adpcm.h)),
so this needs no format change — only a v4-gated player change. The audio
sub-header is unchanged. This reclaims the ~3.3 MB PCM buffer from the resident
set.

## Encoder changes

Files: [encoder.cpp](../src/encoder/encoder.cpp),
[enc_stages.cpp](../src/encoder/enc_stages.cpp) (`write_split_output`,
`append_plane`), [blockcoder.cpp](../src/encoder/blockcoder.cpp).

1. **`--seg N` knob.** Absent / `0` = legacy v3 whole-plane output; `N>0` = v4
   segmented (default `TVID_SEG_DEFAULT` when `--seg` is given without a value).
   `tools/encode.sh` already forwards unknown flags, so no wrapper change.
2. **Segment-major emission.** `pass2_encode` already builds the full per-frame
   plane vectors. Record each plane's cumulative byte offset at every frame
   boundary during pass2 (a small `vector<size_t>` per plane); a segment's byte
   slice for a plane is then `[off[seg_start] .. off[seg_end])`. In
   `write_split_output`, instead of one `append_plane` per plane over all frames,
   emit the per-segment slices in segment-major order, each via the existing
   `append_plane` (methods 0–3, keeps smallest — reused unchanged).
3. **Keyframe-vs-carry per segment.** For each segment after 0, evaluate emitting
   the boundary frame as a keyframe (segment self-contained) vs a carry block
   frame; write the chosen bit into that segment's structure chunk header.
   Default heuristic: keyframe at scene cuts / when cheaper, else carry. A
   keyframe-led segment re-seeds the block coder's "previous frame" to the
   keyframe so its RD search matches decode.
4. **Header.** Write `version = 4` and the `[u16 seg_frames]` field; keep all
   SPLIT flags as today.
5. **Self-check.** The encoder links the decode halves; the round-trip check
   should confirm segmented decode is byte-identical to the non-segmented one.

## Player changes

File: [player.c](../src/decoder/player.c), the SPLIT branch.

1. **Version gate.** Accept `version == 4` alongside 3; read `[u16 seg_frames]`
   when v4. The v3 path is untouched.
2. **Segment holder.** A small `plane_seg` per plane — `{uint8_t *buf; long len;
   long pos;}` — and a `load_segment(fp, ...)` that reads one segment's chunks for
   every active plane via the existing `read_plane` / `read_plane_opt` (color
   stays non-fatal). Mode-RLE expansion stays, applied per segment.
3. **Boundary refill.** The per-frame loop body is unchanged
   (`codec_decode_block_split` just indexes the current segment's buffers). At
   each boundary (`f % seg_frames == 0`): free the old segment, `load_segment` the
   next, reset `cell_pos/mode_pos/pal_pos/color_pos/sp` to 0, and read the
   keyframe-vs-carry bit. **Keyframe segment** → copy the chunk's leading `ncells`
   bytes into `fb`/`colfb`, set `cell_pos=color_pos=ncells` (segment 0 always
   takes this). **Carry segment** → no keyframe bytes; `fb`/`colfb` persist and
   SKIP/SHIFT read them; cursors start at 0.
4. **Audio on demand.** Replace the upfront `audio_load` PCM decode with a small
   ADPCM-block ring: `audio_pump` decodes the next block(s) when the backend
   buffer drains, v4-gated so v3 keeps the current behaviour
   ([adpcm_dec.c](../src/common/adpcm_dec.c)).
5. **Prefetch (optional, later).** Keeping current+next segment hides the read,
   but a synchronous boundary load is fine to start: the DOS player is wait-bound
   (the `dos-player-render-not-bottleneck` finding / `-DTVID_PROF`).

The resident set drops from "whole movie" to "≈ seg_frames × planes" + the audio
ring.

## Trade-offs

- **Size.** Per-segment entropy resets cost a little — each segment re-learns its
  model. The penalty is monotonic in segment count, not a sharp knee: on a 200-frame
  80×24 clip the body grew smoothly from the whole-movie 28.0 KB to 30.4 KB at
  SEG=64 (~8%), 35.6 KB at SEG=16, 39.0 KB at SEG=8. So `seg_frames` is a clean
  RAM-vs-ratio dial; `TVID_SEG_DEFAULT 64` sits at a modest ~8% overhead while
  bounding the resident set to 64 frames. It is a header field, so a target can
  trade RAM vs size per encode. (The current encoder emits CARRY for all later
  segments — no per-segment keyframes yet — so the keyframe-frequency cost is not
  in these numbers; `probe[seg-kf]` measured whole-keyframe intra prediction DEAD,
  so keyframes ship plain-entropy whenever they are introduced. See
  doc/abandoned-levers.md.)
- **Decoder cost.** A model reset every `seg_frames` is negligible against the
  per-frame work, and DOS is wait-bound anyway.
- **Compatibility.** v3 files still decode the whole-plane way; the host player
  reads both. v2 has no path (already removed).

## Verification

- **Byte-identical decode** via the frame-dump:
  ```sh
  cmake -S . -B build-probe -DTVID_PROBE=ON && cmake --build build-probe
  TVID_DUMP=1 ./build-probe/player v3.tvid   > v3.raw
  TVID_DUMP=1 ./build-probe/player v4seg.tvid > v4.raw
  cmp v3.raw v4.raw     # silent == segmentation is lossless vs whole-plane
  ```
- **`--seg` knee + `probe[seg-kf]`** swept in parallel (per
  [CLAUDE.md](../CLAUDE.md)) on a representative color clip.
- **Tests**: a `segment_test` round-tripping a multi-segment clip (identical
  framebuffers, segment buffers freed at each boundary); `coder_golden_test` pins
  the per-chunk bytes.
- **DOS**: build `PLAYER.EXE` (`-DTVID_DOS=ON`), play a hi-res `--color` clip
  that OOMs under v3 and confirm it plays under v4; check the resident set with
  `-DTVID_PROF`.

Until v4 lands, the DOS guidance for `--hi-res --color` is unchanged: lower
`--fps`, shorten the clip, or `--no-audio`; or accept the grayscale fallback.
