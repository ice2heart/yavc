# Yet another stupid video compression

Compress a ~3-minute video **with its soundtrack** so the **encoded clip + the
decoder** together fit on a **single 1.44MB floppy**, and play it back in a
terminal — or on a real DOS PC with a Sound Blaster — as an 80×24 grid of 2×4
sub-cell glyphs, grayscale or with an optional **256-color** layer. Designed to
run on a weak PC.

## How it works

```
source.mp4 (+ its audio)
  │  ffmpeg: downscale to 80×96 px* + aspect-correct rgb24; audio → s16 mono
  ▼
encoder (C++23)     block codec: variable blocks, per-block quantizer search,
                    luma+glyph cells (+ optional 256-color plane); IMA-ADPCM audio
  ▼   entropy stage: LZSS / Huffman / adaptive order-1 range coder, plane-split
video.tvid          TVID header + keyframe + delta frames + ADPCM audio tail
  ▼
player (portable C) decodes frames, paces to the audio clock, draws via a backend
                    ├─ ANSI terminal  (escape codes; host audio via CoreAudio)
                    └─ DOS  (Mode 13h graphics blit by default, or 0xB800 text with
                            TVID_TEXT=1; Sound Blaster auto-init DMA audio).
                            32-bit DOS/4GW is the shipped target.
```

\* The grid is 80×24 cells but each cell is ~2× taller than wide, so ffmpeg renders
double-height pixels and the encoder averages each vertical pixel pair into one cell —
correcting the aspect so the picture isn't squished.

### Cell & format
- **Cell = 1 byte** (v3): high bits = grayscale luma level, low bits = a glyph
  index that maps to a Braille **2×4 sub-cell ink mask** (see
  [glyphset.h](src/common/glyphset.h)) — so a "cell" carries real sub-cell detail,
  not just one glyph. (The old v2 flat-16-color cell model is retired; v3 is the
  only format.)
- **Color** (optional, `--color`, `TVID_FLAG_COLOR`): a parallel plane carrying
  one **xterm-256** hue index per cell, rendered on top of the luma/glyph so the
  same shape gains color. Additive — the cell byte is unchanged with or without
  it. See [doc/compression.md](doc/compression.md) and
  [xterm256.h](src/common/xterm256.h).
- **Codec** ([codec.c](src/common/codec.c), [blockcoder](src/encoder/blockcoder.cpp)):
  a per-frame quadtree of variable-size cell blocks, each leaf coded as
  `SKIP`/`SHIFT`/`SOLID`/`PAL2`/`RAW` by an offline rate-distortion search. Lossless
  cell reconstruction → no drift.
- **Entropy stage**: the body is plane-split (structure bits vs. cell bytes vs.
  mode tags vs. color) and each plane is coded with the best of LZSS, Huffman, or an
  **adaptive order-1 range coder** ([range.c](src/common/range.c)) — auto-selected
  per plane. This is what frees room for audio under the floppy budget.
- **Audio**: the source soundtrack is encoded as 4-bit **IMA ADPCM** mono (~8 kHz)
  in self-contained blocks ([adpcm.c](src/common/adpcm.c)) and appended as a tail
  the player streams; A/V stays locked by pacing video off the audio clock.
- **Container** ([tvid_format.h](src/common/tvid_format.h)): fixed header (+ audio
  sub-header when present), the video body, then the ADPCM tail.

## Build (host: encoder + ANSI player)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build          # codec round-trip self-test
```

Produces `build/encoder` (C++23) and `build/player` (portable C, ANSI backend).

## Encode & play

```sh
# requires ffmpeg on PATH (brew install ffmpeg)
tools/encode.sh input.mp4 video.tvid --fit-size 1300000   # auto-fit to budget
./build/player video.tvid
```

By default the encoder also pulls the source's own audio track (s16 mono → IMA
ADPCM); `--audio-rate N` sets the rate (default 8000), `--no-audio` keeps it
video-only. Audio is a fixed cost subtracted from the `--fit-size` budget so the
combined file still fits the floppy. On the host the player decodes and plays the
audio in sync (CoreAudio on macOS); press **ESC** (or `q`) to stop.

`--fit-size B` fits the output into B bytes by **holding the frame rate and raising the
`--stable` temporal-hysteresis threshold** (32→48→64→96), only dropping fps (12→10→8) as a
last resort — trading detail before smoothness. Use fixed `--fps N` / `--stable T` to pin
them. The ~1.3MB default budget leaves room for the decoder binary on a 1.44MB floppy.

`--stable T` is the key size lever: a cell only changes when its averaged RGB moves more
than `T`, so quantization shimmer becomes a no-op instead of costing bytes. A 3.5-min 1080p
clip fits in **1.17 MB at full 12 fps** at `--stable 64`. It's encoder-only — the format and
decoder are unchanged. Details in [doc/compression.md](doc/compression.md).

## DOS build (decoder)

The decoder is plain C. The **shipped** DOS player is 32-bit **DOS/4GW** (OpenWatcom
`wcl386`) so the whole decoded audio track fits in flat memory. By default it renders
in **Mode 13h graphics** (the Braille sub-cell ink drawn as pixels, with the xterm-256
palette programmed into the VGA DAC for color); `TVID_TEXT=1` forces the legacy 0xB800
text path (a cell *is* `[char][attr]`). Audio plays through a Sound Blaster with
auto-init DMA + an IRQ ISR. The same C also builds 16-bit real mode for a video-only
player. Only the decode half of the codec compiles here — the C++ compress code never
ships on the floppy.

```sh
# Fetches + bootstraps OpenWatcom from source, then cross-compiles PLAYER.EXE:
cmake -S . -B build-dos -DTVID_DOS=ON
cmake --build build-dos --target dos_player   # -> build-dos/dos/PLAYER.EXE

# Or, if you already have wcl386 on PATH (faster):
WATCOM=/path/to/open-watcom tools/build-dos.sh
```

Run it under **DOSBox-X** with the bundled config (mounts the launch dir as `C:`,
sets up Sound Blaster Pro 2 at `A220 I5 D1`, fast core):

```sh
cd build-dos/dos
dosbox-x -conf "$PWD/../../tools/dosbox-x.conf" PLAYER.EXE CLIP.VI
```

DOS truncates filenames to 8.3, so use a short extension (`.vi`). Mode 13h renders
the Braille 2×4 sub-cell masks directly as pixels (≈160×96 sub-cells); the `TVID_TEXT=1`
text path instead uploads a **custom 8×16 VGA font** built from the same masks so sub-cell
detail still shows on text-mode VGA instead of flat shade blocks. Press **ESC** to quit.
Full details, including the real-Pentium-1 cycle option, are in
[doc/dos-build.md](doc/dos-build.md).

## Layout

| Path | What |
|------|------|
| [src/common/](src/common/) | format headers + the shared **decode** half (portable C): `codec.c`, the `*_dec.c` entropy/ADPCM decoders, glyphset, xterm256 palette, the `*_internal.h` shared models |
| [src/encoder/](src/encoder/) | C++23 offline encoder: pipeline stages, block coder + quantizer, and all the **compress** code (`*_enc.cpp`) |
| [src/decoder/](src/decoder/) | portable C player + ANSI / DOS backends + host (CoreAudio) and DOS (Sound Blaster) audio |
| [tools/encode.sh](tools/encode.sh) | ffmpeg → encoder wrapper (`--fit-size`, `--audio-rate`, `--no-audio`) |
| [tools/build-dos.sh](tools/build-dos.sh) · [tools/dosbox-x.conf](tools/dosbox-x.conf) | DOS cross-build + DOSBox-X run config |
| [tests/](tests/) | round-trip self-tests (codec, lzss, entropy, range, mode-RLE, ADPCM), a golden-byte pin, and an end-to-end encode→decode seam test |

## Docs
- [doc/architecture.md](doc/architecture.md) — full system architecture: the two
  halves, the language split, the container format, the cell model.
- [doc/compression.md](doc/compression.md) — the v3 codec: quadtree block frames,
  split planes, the four entropy methods, the xterm-256 color plane, encoder knobs.
- [doc/abandoned-levers.md](doc/abandoned-levers.md) — compression ideas that were
  measured and rejected (PAL4, field-split, half-block, FWDREF, …) and why.
- [doc/v3-streaming.md](doc/v3-streaming.md) — design note / TODO for streaming the
  split planes in chunks (the DOS whole-file-resident memory limit).
- [doc/dos-build.md](doc/dos-build.md) — the 32-bit DOS/4GW player: cross-build, Sound
  Blaster audio, Mode 13h graphics, DOSBox-X run config.
