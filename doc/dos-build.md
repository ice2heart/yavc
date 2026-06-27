# Building and running the DOS player

The host build (CMake) produces `player` with the ANSI terminal backend and
host audio (CoreAudio on macOS). The **DOS** build is a separate cross-compile:
a 32-bit protected-mode `PLAYER.EXE` (DOS/4GW extender) that renders to text-mode
VRAM (`backend_dos.c`) and plays audio through a Sound Blaster with auto-init DMA
(`audio_dos.c`). The player source and the shared codec are byte-for-byte the
same; only the backends differ, selected at link time.

## Why DOS/4GW (32-bit) and not 16-bit real mode

The whole ADPCM track is decoded into one flat PCM buffer at startup (≈2.8 MB for
a 3-minute 8 kHz song). That does not fit a 16-bit real-mode 640 KB map, but is
trivial in the DOS/4GW flat 32-bit address space (backed by extended memory). The
cost is that DMA and IRQ setup must go through DPMI — see "How the audio backend
works" below.

## Toolchain: OpenWatcom

The build uses **OpenWatcom v2** (`wcl386`), the same toolchain family the
16-bit `backend_dos.c` already documents. It bundles the DOS/4GW extender into
the linked `.exe`, so the output runs on any DOS with a DPMI host (DOSBox-X
provides one; real hardware uses DOS/4GW's built-in extender). There are two ways
to get the DOS player built.

### Option A — CMake fetches and builds OpenWatcom for you

`-DTVID_DOS=ON` makes CMake clone `open-watcom-v2`, bootstrap it from source, and
cross-compile `PLAYER.EXE` with the result — no separate OpenWatcom install:

```sh
cmake -S . -B build-dos -DTVID_DOS=ON
cmake --build build-dos --target dos_player   # -> build-dos/dos/PLAYER.EXE
```

The first build is **slow and one-off**: it compiles the entire OpenWatcom
toolchain. Two things worth knowing:

- **No multithreading.** OpenWatcom's bootstrap makefile has incomplete
  dependency ordering (a generated header, `usage.gh`, isn't ordered before the
  `.c` that includes it), so a parallel `make -jN` races and dies; the later
  `builder`/`wmake` walk exposes no `-j` knob either. The whole toolchain build
  is therefore serial. Once built it is cached under `build-dos/open-watcom/`, so
  rebuilding only the player after editing a `.c` is fast.
- **The DOSBOX doc-step "failure" is expected and ignored.** `build.sh rel` ends
  with a browser-help doc step that needs a configured DOSBOX and errors with
  "Missing DOSBOX configuration". The cross-compiler (`wcl386`) is copied into the
  release tree *before* that step, so the bootstrap script ignores the non-zero
  exit and instead succeeds iff `wcl386` actually landed. We don't need the docs.

Cross-compilation goes through `tools/dos-cross.cmake`, which finds the freshly
built `wcl386` under the release tree's host-arch bin dir (`armo64` on Apple
silicon, `binl64` on x86-64 Unix) and runs it. The result is a ~47 KB DOS/4G
executable (`MZ`+`LE` with the DOS/4G stub embedded).

### Option B — use an OpenWatcom you already have

If `wcl386` is already installed, `tools/build-dos.sh` is the quicker path (it
skips building the toolchain entirely):

```sh
brew install --cask open-watcom      # or the official installer from openwatcom.org
export WATCOM=/path/to/open-watcom
tools/build-dos.sh                   # -> build-dos/PLAYER.EXE
```

Either way the same sources are compiled: `player.c`, `backend_dos.c`,
`audio_dos.c` and the shared codec (`codec/lzss/entropy/huffman/range/adpcm`)
with full optimisation and C99, linking the DOS/4GW stub. Nothing here touches
the CMake host targets; keep `build-dos/` out of commits.

## Running under DOSBox-X

DOSBox-X (installed via Homebrew as `dosbox-x`) emulates a Sound Blaster, so it
is the quickest way to verify audio + video + sync without real hardware.

1. Make a clip (the encoder + ffmpeg run on the host) and put it next to
   `PLAYER.EXE` (option A leaves the exe in `build-dos/dos/`, option B in
   `build-dos/`):

   ```sh
   tools/encode.sh videos/song.mp4 CLIP.TVID --audio-rate 8000
   ```

2. A ready-made config lives at [`tools/dosbox-x.conf`](../tools/dosbox-x.conf).
   It mounts the directory you launch from as `C:`, sets Pentium-1-class CPU
   cycles, and configures a Sound Blaster Pro 2 at `A220 I5 D1` (matching the
   `BLASTER` string the audio backend reads). From the directory holding
   `PLAYER.EXE` + `CLIP.TVID` (i.e. `build-dos/dos/` for option A, `build-dos/`
   for option B):

   ```sh
   cd build-dos/dos        # wherever PLAYER.EXE lives
   dosbox-x -conf /ABS/PATH/TO/tools/dosbox-x.conf PLAYER.EXE CLIP.TVID
   ```

   `mount c .` in the config mounts the launch directory, so the `PLAYER.EXE`
   and clip you pass on the command line resolve against `C:`. The CPU line is
   `cycles=fixed 77000` (a real Pentium 1) — DOSBox-X ignores a bare
   `cycles=auto` ramp and jumps to "max", so a fixed count is what actually pins
   the speed. The `BLASTER` env var is what the audio backend reads for the base
   port, IRQ and DMA channel (it defaults to `A220 I5 D1` if unset). The video
   appears in 80×25 text mode; the song plays in sync. **Press ESC to stop
   playback** (the player polls the keyboard once per frame and exits cleanly).

   > DOS truncates filenames to 8.3, so name clips with a short extension —
   > e.g. `bif.vi` rather than `bif.tvid` — and pass that name to the player.

### Monochrome render: Mode 13h graphics (default) vs the text font

Monochrome streams (`TVID_VERSION_MONO`) render in **VGA Mode 13h graphics by
default** (32-bit path). The player sets 320×200×256, loads a grayscale DAC, and
blits each cell's Braille 2×4 ink mask as a small sprite straight into the linear
framebuffer at `0xA0000`: the 160×96 sub-pixel grid (80×24 cells × 2×4) is drawn
at 2×2 px per sub-pixel = 320×192, centered with a 4 px letterbox top/bottom. Each
lit sub-pixel is tinted by the cell's gray level, so every pixel is under direct
control — strictly sharper than text mode, which can only set one CGA attribute
(one gray) across a whole cell. Same shadow/dirty-cell diff as text mode (each
cell is a fixed 4×8 pixel rect; only changed cells reblit). It reuses the shared
`tvid_mono_pattern_ink` / `tvid_mono_level_value` from `glyphset.h` — no codec or
encoder change, purely render-side, behind `__386__`.

> **`TVID_TEXT=1`** forces the older **custom-font text path** instead: in 80×25
> text mode `backend_dos.c` builds a 256-glyph 8×16 font where glyph slot *g*
> draws pattern *g*'s 2×4 mask, uploads it into VGA font RAM by programming the
> Sequencer + Graphics Controller, and writes the glyph index straight to VRAM —
> the true sub-cell shape at one of four CGA grays (effective 160×100 sub-cells).
> This was the previous default; keep it for terminals/emulators where Mode 13h
> is unavailable, or to compare. Profiling shows the player is wait-bound, so the
> graphics path is a fidelity upgrade, not a speed one.

Color streams (v3, `TVID_FLAG_COLOR`) render in Mode 13h with the VGA DAC
programmed to the xterm-256 palette: each lit sub-pixel takes the cell's hue
index, scaled by its luma. If the color plane won't allocate on a RAM-starved
target the player degrades gracefully to grayscale. `TVID_TEXT=1` still forces the
0xB800 text path (CP437 shades, no per-cell hue) for terminals/emulators where
Mode 13h is unavailable.

On real hardware: copy `PLAYER.EXE` and the `.tvid` onto the machine, make sure
`BLASTER` is set (most DOS installs set it from the sound-card driver), and run
`PLAYER CLIP.TVID`.

## Profiling the player (`-DTVID_PROF`) — find the choke point

Before changing *how* the player renders (e.g. text vs graphics mode), measure
where a frame's time actually goes. The DOS player has an opt-in profiler gated
behind `-DTVID_PROF`; the production `PLAYER.EXE` built without it contains none
of this code (it all lives behind `#if defined(TVID_PROF) && defined(__DOS__)`,
mirroring the host `-DTVID_PROBE` convention).

A frame's wall time splits into three measured phases plus the blit workload:

- **decode** — `codec_decode_block_*` unpacking the next frame into the
  framebuffer (timed on the PIT channel-0 down-counter, ~838 ns resolution).
- **blit** — `backend_present`: shadow-diff + the byte-pokes to text VRAM (PIT).
- **wait** — `sync_wait` pacing to the audio clock / frame timer. This routinely
  exceeds the PIT's ~55 ms wrap, so it is timed on the coarse 18.2 Hz BIOS tick.
- **chg** — cells changed this frame (how much work the blit had to do).

Build a profiling EXE in a scratch tree so `build-dos/` stays clean:

```sh
# with WATCOM set as for build-dos.sh:
wcl386 -bt=dos -l=dos4g -mf -5r -ox -ot -za99 -DTVID_PROF \
  -Isrc/common -Isrc/decoder -fe=PLAYER.EXE \
  src/decoder/player.c src/decoder/backend_dos.c src/decoder/audio_dos.c \
  src/common/codec.c src/common/lzss.c src/common/entropy.c \
  src/common/huffman.c src/common/range.c src/common/adpcm.c
```

What it gives you:

- **On-screen HUD** (top row, white-on-blue): live `fps dec<us> blt<us> wt<us>
  chg<cells>`. Toggle with **`d`**; ESC still quits. On by default.
- **Exit summary** to stdout after the text screen resets: avg decode / blit /
  wait µs per frame, avg cells changed, and the **active-only fps ceiling**
  (`decode+blit`, i.e. what the player could sustain if it never waited). That
  ceiling is the number that says whether rendering is the bottleneck at all.
- **`TVID_PROFFRAMES=N`** caps playback at N frames so a scripted/headless run
  terminates and prints the summary instead of playing for minutes. Combine with
  `TVID_NOAUDIO=1` to isolate decode+blit without Sound Blaster setup. (The cap
  reads `getenv`; if a particular DPMI host doesn't propagate the var it fails
  safe to full playback — the summary still prints at the end either way.)

### First measurement (what it told us)

A full-clip run of the shipped mono encoding (3645 frames, `TVID_NOAUDIO=1`,
DOSBox-X) reported: **avg decode ~0 us, avg blit ~0 us, worst active frame
2569 us (decode+blit), avg 426 cells changed/frame**, with the whole rest of
each frame (~55 ms) spent in the timer wait. **Rendering is not the bottleneck:**
even the worst frame's decode+blit is ~2.6 ms (a >300 fps ceiling), and the
player is entirely wait-bound at its target fps. Switching text→graphics mode
would only *add* work to a phase that is already ~0 % of the frame budget — it
buys fidelity (per-pixel control, more gray levels), never speed. Optimise the
blit only if a future higher-resolution/graphics render makes it show up here.

Headless capture under DOSBox-X (macOS lacks `timeout`; background + poll):

```sh
# autoexec: set TVID_NOAUDIO=1 / TVID_PROFFRAMES=150, then
#   PLAYER.EXE CLIP.VI > PROF.TXT  /  exit
SDL_VIDEODRIVER=dummy dosbox-x -conf prof.conf -nogui &
until [ -s PROF.TXT ]; do sleep 2; done; kill %1; cat PROF.TXT
```

## How the audio backend works (`audio_dos.c`)

Same `audio.h` interface as the host backends, so `player.c` is unchanged.

- **Auto-init double buffer.** One DMA buffer (two halves) is played forever by
  the 8237. While the card plays half A, the SB IRQ handler fills half B from the
  decoded-PCM ring, and vice-versa — glitch-free, no per-frame DMA reprogramming.
- **The audio clock drives video.** The ISR bumps a played-sample counter each
  half-buffer; `player.c`'s `sync_wait` paces frames off `audio_played_samples()`
  exactly as it does on the host, so A/V stays locked. On DOS the wait loop
  `HLT`s until the next interrupt instead of `nanosleep`.
- **8-bit unsigned PCM.** IMA decodes to s16; the ISR downshifts to u8
  (`(s16>>8)^0x80`) so it can use the 8-bit DMA channel (channel 1), avoiding any
  SB16 dependency. Quality is a non-goal for this track.

### DOS/4GW-specific care

- **DMA buffer in low memory.** The 8237 addresses physical memory below 1 MB and
  cannot cross a 64 KB page. The backend asks DOS (via the DPMI "allocate DOS
  memory" call) for a real-mode block — identity-mapped, so its physical address
  is just `segment<<4` — over-allocates, and aligns the buffer inside it so it
  never straddles a 64 KB boundary.
- **Page-locking.** The ISR, the DMA buffer, the ring and the counters are all
  DPMI-locked so the host can't page them out while an interrupt is pending.
- **Protected-mode IRQ vector.** The SB IRQ's PM vector is saved, replaced with
  our handler (our CS selector + the flat address of the ISR), and restored on
  shutdown; the PIC is unmasked/remasked to match.

## What is and isn't verified

`tools/build-dos.sh` and this doc are written against OpenWatcom + DOSBox-X. The
host CMake build and the `adpcm_roundtrip` ctest exercise the shared codec on
every commit. The DOS binary itself is verified manually under DOSBox-X (Sound
Blaster emulation) — it is not part of `ctest`, matching how `backend_dos.c` has
always been treated.
