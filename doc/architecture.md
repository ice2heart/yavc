# termvideo — architecture

Play a ~3-minute video in a terminal as an **80×24 character grid** (a 2×4 Braille
sub-cell glyph per cell carrying a luminance shape, with an optional xterm-256 color
plane), where the **encoded video *plus* the decoder binary fit on a single 1.44 MB
floppy** and the decoder runs on a weak (1980s-class) PC.

## The two halves

The work splits into an **offline encoder** (runs on the dev box, may be heavy) and a tiny
**decoder/player** (ships on the floppy, must be small + fast). The only thing crossing
between them is the **`.tvid` container** — that file format is the contract.

```
source.mp4 / video.webm
  │  ffmpeg: downscale to 80×96 px, aspect-correct, rawvideo rgb24 → pipe
  ▼
encoder (C++23)        per-cell 2×4 sub-pixel luma → Braille glyph + luma level;
                       optional RGB → xterm-256 hue; quadtree block frames
  ▼
video.tvid            TVID v3 header + 1 keyframe + quadtree block frames, in a
                       split (per-plane entropy-coded) body (~1.3 MB budget)
                       + optional IMA-ADPCM audio tail
  ▼
player (portable C)    streams frames, replays the quadtree, draws via a video
                       backend and (if present) decodes audio to an audio backend,
                       pacing video off the audio clock for A/V sync.
                       video ├─ ANSI terminal (escape codes; host / Linux / macOS)
                             └─ DOS  (Mode 13h graphics, or 0xB800 text; real mode)
                       audio ├─ CoreAudio (AudioQueue; macOS host)
                             ├─ null stub   (silent; other hosts)
                             └─ Sound Blaster auto-init DMA + ISR (DOS)
```

### Language split — and why

| Part | Language | Reason |
|------|----------|--------|
| `src/encoder/` | **C++23** | Offline, unconstrained. Free to use `std::span`, `std::vector`, heavy RD search. Owns **all the compression** (the `*_enc.cpp` coders). |
| `src/decoder/` | **portable C99** | Ships on the floppy; must also build for **16-bit/32-bit DOS** (OpenWatcom), which can't do C++23. Small + fast. |
| `src/common/` | **C headers + the shared *decode* half** | Format definitions live in headers both halves include, so they agree byte-for-byte. The `.c` files here are **decode only** (`codec.c`, `*_dec.c`) — exactly what the player links. The matching compress code lives in `src/encoder/` (`*_enc.cpp`), so the player carries no encoder code. |

The codec is **split at the encode/decode line**: each entropy coder is a `*_dec.c`
(common, C, player-linked) plus a `*_enc.cpp` (encoder, C++). The bit-exact pieces both
halves need — the range coder's frequency model, the ADPCM tables, the entropy alphabet
sizes — live once in private `*_internal.h` headers so the wire format is single-sourced.

## Budget math (the design driver)

- Floppy 1.44 MB = 1,474,560 B; ~1.38 MB usable after FAT12. Reserve ~60 KB for the
  decoder binary → **~1.3 MB for video data** (`tools/encode.sh` default budget = 1,300,000).
- 180 s @ **10 fps** = 1800 frames → **~722 B/frame**. Raw frame = 80×24 = **1920 B**, so
  we need ≈2.7× compression — met by the quadtree block coder + per-plane entropy coding.
- `tools/encode.sh --fit-size` auto-steps fps **12 → 10 → 8**, keeping the highest rate that
  fits the budget.

Temporal hysteresis (`--stable`) is what makes a busy full-length clip fit: a cell's target
only updates when it moves more than the deadband, which kills quantization shimmer and
turns most cells into cheap SKIP leaves.

## The cell model (v3)

Each grid cell is one byte: a brightness level plus a sub-cell shape glyph.

```
 bit  7 6 | 5 4 3 2 1 0
      luma |   glyph
     (0..3)|  (0..63)
```

- **luma** (high `TVID_MONO_LUMA_BITS` = 2 bits) is one of 4 grayscale levels. With the
  color plane it scales the cell's hue; without it, it is the grayscale foreground level.
- **glyph** (low `TVID_MONO_GLYPH_BITS` = 6 bits) indexes a **Braille 2×4 dot pattern**
  ([glyphset.h](../src/common/glyphset.h)) — the exact sub-cell ink mask, so one character
  cell renders 2×4 sub-pixels of detail. Luma comes from BT.601 on the source pixels.
- The luma/glyph split is a compile-time knob (`-DTVID_PROBE` can sweep 2+6, 0+8, 4+4, 3+5);
  the shipped default is **2 luma + 6 glyph**, always 8 bits total.

**Color is a separate, optional plane** ([xterm256.h](../src/common/xterm256.h)), not packed
in the cell byte: one xterm-256 hue index per cell, carried alongside the cells and mirroring
their leaf structure (`TVID_FLAG_COLOR`). The cell byte is identical whether or not a stream
carries color — a video-only stream and its colorized sibling share the same shape bytes.

### Aspect correction ("text is taller")

A character cell is ~2× taller than wide and holds a 2×4 sub-cell grid. ffmpeg renders to
**80×96** pixels (aspect-preserving pad/letterbox), and the encoder box-averages each cell's
sub-pixel block ([quantize.cpp](../src/encoder/quantize.cpp)) before quantizing to a glyph.
The 80×24 grid then occupies a proportional area on screen, matching source aspect.

Full codec details are in [compression.md](compression.md); levers that were measured and
rejected (with why) are recorded in [abandoned-levers.md](abandoned-levers.md).

## Container format (`.tvid`)

Defined in [tvid_format.h](../src/common/tvid_format.h). `TVID_VERSION` is **3**; a v2 file
is rejected on read (the flat-color model is retired).

```
Header (fixed, little-endian, written field-by-field — no struct packing):
  "TVID"                            4-byte magic
  u8  version (=3)
  u8  flags                         see the flag table below
  u8  cols (80), u8 rows (24), u8 fps
  u32 frame_count
  u8  ramp_len, char ramp[ramp_len] legacy glyph ramp (kept for header stability)

Audio sub-header (present iff flags & AUDIO, immediately after the ramp):
  u8  audio_codec (=1 IMA ADPCM)    u8 audio_channels (=1 mono)
  u16 audio_rate (Hz, e.g. 8000)
  u32 audio_samples                 total PCM samples (exact end + A/V sync)
  u32 audio_bytes                   length of the ADPCM payload at the file tail

Body: either the SPLIT plane sequence (the shipped path, see below) or the
  whole-stream layout (keyframe + block frames, optionally LZSS/Huffman-compressed).

Audio payload (present iff flags & AUDIO): the last audio_bytes of the file,
  appended after the entire video body.
```

### Header flags

| Flag | Bit | Meaning |
|------|-----|---------|
| `TVID_FLAG_LZSS` | 0x01 | whole-stream body is `[u32 raw_len][LZSS]` |
| `TVID_FLAG_AUDIO` | 0x02 | file carries an IMA-ADPCM audio tail |
| `TVID_FLAG_SHIFT` | 0x04 | SKIP leaves carry a motion-vector sub-variant |
| `TVID_FLAG_HUFF` | 0x08 | (whole-stream) body is `[u32 raw_len][LZSS+Huffman]` |
| `TVID_FLAG_COLOR` | 0x08 | (with SPLIT) a parallel xterm-256 color plane follows the cells — 0x08 is reused; HUFF and SPLIT never co-occur |
| `TVID_FLAG_SPLIT` | 0x10 | body is a sequence of independently entropy-coded **planes** |
| `TVID_FLAG_MODEPLANE` | 0x20 | (with SPLIT) per-leaf mode tags in their own byte plane |
| `TVID_FLAG_CELLSPLIT` | 0x40 | (with SPLIT) cell bytes split into raster (RAW) + palette (SOLID/PAL2) planes |
| `TVID_FLAG_MODERLE` | 0x80 | (with SPLIT+MODEPLANE) the mode plane is zero-RLE'd before entropy |

The SPLIT levers (`MODEPLANE`, `CELLSPLIT`, `MODERLE`) are **auto-selected per file** — the
encoder builds each candidate, entropy-codes it, and sets the flag only when it is smaller.

### Split body (the shipped layout)

With `TVID_FLAG_SPLIT`, the body is a sequence of **plane chunks**, each self-describing:

```
[u8 method][u32le raw_len][u32le payload_len][payload]
  method 0 = raw         (incompressible plane, stored)
         1 = LZSS
         2 = LZSS+Huffman (or no-LZ order-0 Huffman; same decode)
         3 = adaptive order-1 range coder (range.c)
```

Each plane is entropy-coded on its own (its statistics differ wildly), and the method is
chosen by trying all four and keeping the smallest. Plane order: **structure**, *[mode if
MODEPLANE]*, **cell** *(or raster + palette if CELLSPLIT)*, *[color if COLOR]*. The structure
plane is a per-frame `[u16 len][bits]`; the cell/color planes are the keyframe cells/hues
followed by per-leaf cells/hues, built by the identical quadtree walk so the decoder advances
one cursor per plane in lockstep.

### Audio track (IMA ADPCM)

Audio is **additive and optional**: a video-only `.tvid` is byte-identical to the no-audio
case, and the payload sits at the **file tail**, so the front-to-back video parser is
untouched — the player seeks to `filesize − audio_bytes` to find it.

The codec is **4-bit IMA/DVI ADPCM** (0.5 byte/sample, integer-only table-driven decode — the
classic Sound Blaster choice, satisfying the decoder-asymmetry rule). ~8 kHz mono fits a
3-minute song in well under a megabyte (~860 KB). The `audio_codec` sub-header byte selects:
**1** = raw ADPCM blocks (above); **2** = the same blocks **entropy-coded**
(`--audio-entropy`, codec 2) — each group of `TVID_AUDIO_ENT_GROUP` blocks is range-coded back
to the exact same ADPCM bytes, ~10–12% smaller off the audio (which is ~90% of file bytes) with
no quality change and full DOS decodability (see [compression.md](compression.md)); **3** is
reserved for a 3-bit IMA variant, not built. The lossy size knob is `--audio-rate` (orthogonal
to the codec field). Stream framing is **self-contained blocks**
([adpcm.h](../src/common/adpcm.h)): each block carries its own `[s16 predictor][u8 step_index]
[u8 reserved]` then `ADPCM_BLOCK_SAMPLES−1` nibbles, with **no cross-block state**, so a DOS
DMA double-buffer can start mid-stream and a late/dropped block self-heals. Encode lives in
[adpcm_enc.cpp](../src/encoder/adpcm_enc.cpp); the tiny decode in
[adpcm_dec.c](../src/common/adpcm_dec.c).

## Decoder / playback loop

[player.c](../src/decoder/player.c): validate header → allocate the framebuffer (+ hue
framebuffer if color) and per-plane buffers → decode each block frame off the planes,
`backend_present(fb, hue)`, then `sync_wait(...)`. The split planes are decompressed per their
method tag; the block layer ([codec.c](../src/common/codec.c)) replays the quadtree the
encoder chose, writing cells (and, in lockstep, hues) into the framebuffers.

### A/V sync (audio-clock pacing)

When the stream carries audio, the whole ADPCM tail is decoded once into a PCM buffer (3 min
@ 8 kHz ≈ 2.8 MB, fine on the host) and the video loop **paces off the audio clock** instead
of a fixed timer: before each frame it tops up the audio backend's ring (`audio_pump`) and
`sync_wait` blocks until the speaker has played past that frame's timestamp
(`frame_index * rate / fps` samples). With no audio it falls back to the timer
(`backend_wait_frame(fps)`).

`sync_wait` sleeps **once** for (almost) the whole remaining sample gap, not a busy-poll of
short naps. Critically, the clock comes from the audio backend's **hardware playback timeline**
(`audio_played_samples`), not a buffer-fill counter: a counter that advances one whole
DMA/queue buffer at a time stalls between refills, bunching frames into bursts. The CoreAudio
backend reads `AudioQueueGetCurrentTime`'s sample-accurate `mSampleTime` for this.

### Video backend abstraction

[backend.h](../src/decoder/backend.h) — `init / present / wait_frame / shutdown`. A backend
keeps its **own shadow framebuffer** and diffs the incoming frame against it, emitting only
the cells that actually changed.

- **ANSI** ([backend_ansi.c](../src/decoder/backend_ansi.c)): builds one output buffer of
  cursor-move + SGR + glyph for changed cells, suppressing redundant escapes; one `fwrite` per
  frame. With color it emits a truecolor SGR per cell from `tvid_xterm256_rgb_dim(hue, luma)`.
  `nanosleep` paces fps.
- **DOS** ([backend_dos.c](../src/decoder/backend_dos.c)): defaults to **Mode 13h graphics**
  (the Braille sub-cell ink rendered as pixels; VGA DAC programmed with the xterm-256 palette
  for color), with a **0xB800 text-mode** path available (`TVID_TEXT=1`). Timing spins on the
  BIOS 18.2 Hz tick at `0040:006C`. Cross-compiled with OpenWatcom; not part of the host CMake
  build. See [dos-build.md](dos-build.md).

### Audio backend abstraction

[audio.h](../src/decoder/audio.h) — `audio_init / submit / played_samples / finish /
shutdown`. The player decodes ADPCM and `audio_submit`s PCM; the backend plays it and exposes
the played-sample count the video loop syncs to. One backend links per host (CMake-selected):
CoreAudio (macOS), a null stub (other hosts), or Sound Blaster auto-init DMA + ISR (DOS).

## File layout

```
termvideo/
  CMakeLists.txt            C++23 encoder + C player (host); DOS build is opt-in
  doc/
    architecture.md         this file
    compression.md          the v3 codec (quadtree, planes, entropy, color)
    abandoned-levers.md     compression ideas measured and rejected, with why
    v3-streaming.md         design note / TODO: stream the split planes in chunks
    dos-build.md            OpenWatcom cross-build + DOS render/audio notes
  tools/encode.sh           ffmpeg → encoder wrapper, --fit-size auto fps
  src/common/               format headers + the shared DECODE half (portable C)
    tvid_format.h           container constants, header + audio sub-header, cell macros
    glyphset.h              Braille 2×4 glyph ink masks + luma level table
    xterm256.h              xterm-256 palette snap + dim-by-luma
    ramp.h                  legacy luma ramp (kept for header stability)
    codec.c                 quadtree block-frame DECODE (shared by encoder round-trip)
    {lzss,range,entropy,huffman,adpcm}_dec.c   decode halves (player + DOS)
    {range,entropy,adpcm}_internal.h           bit-exact shared model/tables/consts
    mode_rle.h              mode-plane zero-RLE codec (header-only)
  src/encoder/              C++23 (offline) — owns all compression
    encoder.cpp             thin main(): sequences the pipeline stages
    enc_stages.{hpp,cpp}    parse → pass1a/1b → pass2 → split/interleaved writer
    blockcoder.cpp/.hpp     quadtree RD search + bitstream serialize
    quantize.cpp/.hpp       rgb24 → sub-pixel luma + glyph (and xterm-256 hue)
    mono_celldist.h         per-cell distortion metric (shared with decoder build)
    {lzss,range,entropy,huffman,adpcm}_enc.cpp compress halves (encoder only)
  src/decoder/              portable C (ships on floppy)
    player.c                playback loop + A/V sync
    backend_ansi.c / backend_dos.c            video backends
    audio_coreaudio.c / audio_null.c / audio_dos.c   audio backends
  tests/                    ctest round-trips + golden-byte + end-to-end seam
```

## Build & run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build                          # 8 self-tests (round-trips + seam)

tools/encode.sh video.webm video.tvid --fit-size 1300000
./build/player video.tvid

# DOS decoder (OpenWatcom): see doc/dos-build.md, or tools/build-dos.sh.
```

## Status

Built and verified on host: clean CMake build, `ctest` 8/8 (codec/ADPCM round-trips, the
entropy/LZSS/RLE/range plane round-trips, the golden-byte pin, and the end-to-end encode→
decode seam), real clips encode within budget, ANSI playback correct (aspect, color, diffing).
**Audio** plays in sync on macOS via CoreAudio. The **DOS player** is cross-built with
OpenWatcom (Mode 13h graphics default, Sound Blaster audio); see [dos-build.md](dos-build.md).

## Verification checklist

- `ls -l video.tvid` + decoder binary size **≤ 1,400,000 B**.
- `ctest` — encode→decode round-trips are **bit-identical**, and the golden-byte test pins the
  exact compressed output so a refactor can't silently change the bytestream.
- ANSI playback: eyeball aspect (no squish), color, smoothness.
- DOS path: cross-build `PLAYER.EXE`, run in DOSBox-X; confirm Mode 13h rendering + tick timing
  + Sound Blaster audio; confirm it sustains target fps on a cycle-limited run.
