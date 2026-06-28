# termvideo — codec visualizer

A host-only web tool to *see* what the codec does to a clip: step stage-by-stage,
then frame-by-frame, through the quadtree regions, the per-leaf modes, the
plane/byte breakdown, and the decode reassembly. Built to reason about better ways
to store video data without reading `-DTVID_PROBE` stderr lines.

It drives the **real** v3 encode pipeline (`pass1a`/`pass1b`/the canonical split
encode) and captures the *same* quadtree the serializer walks — the picture is
exactly what would ship, not a re-implementation. The capture hook lives in
[blockcoder.cpp](../src/encoder/blockcoder.cpp) behind `#if defined(TVID_VIZ)`
(`g_viz_capture`), defined **only** on the `visualizer` target, so the shipped
`encoder`/`player` are byte-identical (the `coder_golden` test proves it).

## Build

Opt-in, like the DOS cross-build. It fetches **civetweb** (MIT, embedded HTTP
server) at configure time via `FetchContent`, so the first configure needs network:

```sh
cmake -S . -B build-viz -DTVID_VIZ=ON
cmake --build build-viz --target visualizer
```

Keep `build-viz/` out of commits (like `build-probe/`).

## Run

The easiest way is `tools/visualize.sh`, the sibling of `tools/encode.sh`: it
takes the **same flags** (`--hi-res`, `--lambda`, `--color`, `--stable`, …) and
builds the **same ffmpeg pipe**, but pipes into the visualizer instead of the
encoder, so encode-time and inspect-time parameters stay in sync. It auto-builds
the `visualizer` target if missing (first configure needs network for civetweb)
and runs from the repo root so the webroot resolves:

```sh
tools/visualize.sh in.mp4 --hi-res --lambda 6 [--color]   # open the printed URL
```

Unlike `encode.sh` it takes no OUTPUT argument and does no audio handling (the
visualizer ignores audio). Override the binary with `VISUALIZER=…` like
`encode.sh`'s `ENCODER=…`.

Under the hood it reads the **same rgb24 stream** `tools/encode.sh` feeds the
encoder (`(cols*2) x (rows*4)` per frame); to drive it by hand:

```sh
ffmpeg -i in.mp4 -vf "fps=10,scale=160:96" -pix_fmt rgb24 -f rawvideo - \
  | ./build-viz/visualizer --lambda 6 --fps 10 [--color] [--port 8080]
```

It runs the encode once at startup, then prints a URL — open it. Codec knobs are
the encoder's (`--lambda`, `--stable`, `--shift`, `--color`, …); visualizer-only
flags are `--port` (default 8080) and `--webroot` (default `src/visualizer/web`,
served live so editing the JS/CSS only needs a browser reload, no rebuild).

Run it **from the repo root** (or pass an absolute `--webroot`): the default
webroot is relative, so a different cwd will 404 the static assets.

### Chunked trace (don't ship the whole thing at once)

The full trace — every leaf, its per-region wire bytes, and a decoded framebuffer
per frame — is large (hundreds of KB per frame; many MB for a real clip), too big
to hand the browser in one JSON. So the server splits it:

- `GET /trace.json` — a small **index**: metadata, the global plane summary, and a
  *light* per-frame summary (keyframe flag, mode tallies, byte breakdown). No
  leaves, no framebuffers, no wire bytes. A few KB regardless of clip length.
- `GET /frame/N.json` — one frame's **heavy** data (leaves with their per-region
  wire bytes, plus the decoded `fb`/`colfb`), serialized on demand.

The UI fetches the index once, then fetches and caches each frame chunk the first
time you step to it, so the browser only ever holds the frames you've visited.

## UI

Stepping is **per stage, then per frame** (◀/▶ stage, ◀◀/▶▶ frame, slider, arrow
keys). The five stages per frame:

1. **input / quantize** — the decoded cell grid (2×4 luma → (luma,glyph) Braille).
2. **quadtree split** — the superblock → leaf partition (region boundaries).
3. **leaf modes** — regions tinted by mode (SKIP/SHIFT/SOLID/PAL2/RAW); click a
   region for its mode, size, RD cost, and payload.
4. **plane assembly** — bytes this frame routed into structure/cell/[color].
5. **decode reassembly** — the round-tripped framebuffer (byte-identical to the
   player; see verification below).

The side panel always shows the frame's mode tally + byte breakdown, the selected
region's detail, and the whole-clip plane sizes after the real per-plane entropy
auto-select (`raw → coded`, with the winning method).

### Per-region wire bytes

Click a region and the detail panel also shows the **exact bytes that region
contributed to each plane** — its on-the-wire representation, sliced out of the
real serializer (not recomputed), captured around the leaf as it is written:

- **structure plane** — the literal bits the region wrote, MSB-first, grouped in
  nibbles: its terminal split flag, the mode tag, and any SHIFT vector / PAL2
  selector bits, with the bit count and start offset into the frame's structure
  chunk.
- **cell plane** — the actual cell bytes in hex: SOLID = 1 palette byte, PAL2 = 2,
  RAW = `s×s` raster bytes (SKIP/SHIFT store none here).
- **color plane** — the per-region hue byte(s), when `--color`.

The panel highlights whichever plane the current stage is about (structure on the
split/mode stages, cell on quantize/decode, all three on plane assembly), so you
can watch a region's bytes accumulate stage by stage. The capture lives in the
same `#if defined(TVID_VIZ)` hook in [blockcoder.cpp](../src/encoder/blockcoder.cpp),
reading the bytes/bits back out of the serializer's own buffers.
check
## Verification

The visualizer's per-frame framebuffer must equal the player's decode — the same
round-trip gate the codec lives by:

```sh
# real encode + player dump of the same frames
./build/encoder --split --lambda 6 --fps 10 --out clip.tvid < frames.rgb
TVID_DUMP=1 ./build-probe/player clip.tvid > dump.raw     # needs -DTVID_PROBE
# the trace's per-frame "fb" arrays must match dump.raw byte-for-byte
```

(Measured: 0 mismatched frames on a 30-frame 80×24 testsrc clip.) Because the
trace is captured from the real `Tree`, the mode tallies and structure-plane bytes
also agree with a `--stats` / `-DTVID_PROBE` run on the same clip.
