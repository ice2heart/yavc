# termvideo — compression (v3, shipping)

The codec is a per-frame **quadtree of square cell blocks**, each leaf coded by an
encoder-chosen mode (`SKIP`/`SHIFT`/`SOLID`/`PAL2`/`RAW`), serialized into **separate planes**
that are each entropy-coded on their own, with an optional parallel xterm-256 color plane.
The decoder is O(cells), branch-light, table-driven — it reads split bits and a mode switch of
cheap fills/copies, plus a per-plane decompress. All the cleverness (which mode, which colors,
where to split, which entropy method) is encoder-side and leaves no trace but the chosen bytes.

Defining files: [tvid_format.h](../src/common/tvid_format.h) (format),
[codec.c](../src/common/codec.c) (decode), [blockcoder.cpp](../src/encoder/blockcoder.cpp)
(RD search + serialize), [enc_stages.cpp](../src/encoder/enc_stages.cpp) (pipeline + plane
entropy back-end). Levers that were measured and **rejected** live in
[abandoned-levers.md](abandoned-levers.md).

## The asymmetry that governs every decision

> The encoder may be arbitrarily smart (it runs once, offline, on a fast machine). The decoder
> must be **O(cells), branch-light, and table-driven** — no search, no floating point, no
> per-frame allocation. It runs on a weak PC and must sustain ≥ target fps from a few KB of RAM.

Every scheme is judged first by *decoder* cost, then by compression — and **every block-level
scheme is measured against the entropy-coded output, never the raw block stream** (a block-level
trick only wins if it removes redundancy the entropy coder cannot — see the FWDREF lesson in
[abandoned-levers.md](abandoned-levers.md)).

## Lineage (what v3 inherited)

v1 was a row-major delta+RLE codec; v2 introduced the quadtree block coder and the entropy
back-end. v3 retired the flat 16-color cell model: the cell byte is now a **luma level + a 2×4
Braille sub-cell glyph**, and color became a **separate optional plane** rather than nibbles
packed into the cell. The quadtree, the modes, the temporal hysteresis, and the entropy
back-end carried forward; the gains that actually shipped (the split planes, the range coder)
are below. A v2 file is rejected on read (`TVID_VERSION` is 3).

## The cell byte

```
 bit  7 6 | 5 4 3 2 1 0
      luma |   glyph
     (0..3)|  (0..63)
```

Two luma bits (4 grayscale levels) and six glyph bits (one of 64 Braille 2×4 dot patterns,
[glyphset.h](../src/common/glyphset.h)). The encoder box-averages each cell's 2×4 sub-pixel
luma block and `tvid_mono_quantize_joint`s it to the (luma, glyph) pair whose rendered ink best
matches under the cell-distortion metric ([mono_celldist.h](../src/encoder/mono_celldist.h)).
The luma/glyph split is a compile-time knob (`-DTVID_PROBE` sweeps 2+6/0+8/4+4/3+5); shipped is
2+6.

## Quadtree block frames

Each frame is tiled into 8×8-cell superblocks (`TVID_SB`); 80×24 = 30 superblocks. Each is the
root of a quadtree that splits down to 1×1 leaves. One bit per internal node says "split"; a
leaf carries a mode tag (`TVID_MODE_BITS` = 2) + payload:

| Mode | Payload | Decoder work | Encoder use |
|------|---------|--------------|-------------|
| `SKIP` (0) | none (+1 "moved" bit if SHIFT) | keep prev cells | block unchanged temporally |
| `SOLID` (1) | 1 cell byte | fill leaf with it | flat region |
| `RAW` (2) | w×h literal cell bytes | copy literals | high-detail fallback |
| `PAL2` (3) | 2 cell bytes + 1 bit/cell mask | per-cell 1-of-2 | two-tone region |

The encoder, for each leaf, evaluates every mode's `cost = lambda·bits + distortion` and keeps
the cheapest, recursively comparing "split into 4" vs "code as one leaf"
([blockcoder.cpp](../src/encoder/blockcoder.cpp): `eval_skip/shift/solid/pal2/raw`, `Tree::build`).
Distortion is luma-weighted squared error over the in-grid cells; `lambda` is the rate-distortion
knob (lower = more detail, higher = smaller file).

### SHIFT motion (opt-in, `--shift` / `TVID_FLAG_SHIFT`)

A motion-vector sub-variant of `SKIP`: each SKIP leaf carries one `moved` bit, and if set, two
biased `(dx,dy)` components (±7 cells, `TVID_SHIFT_BITS`/`TVID_SHIFT_BIAS`) selecting an offset
copy of the leaf from the **previous** frame (sources clamped to the grid). Because a SHIFT leaf
reads cells that may already be overwritten in the in-place framebuffer, the decoder reads from a
pristine `prev` snapshot (`codec_decode_block_ref`); the player allocates that extra buffer only
when the flag is set. SHIFT wins on panning but taxes every SKIP leaf 1 bit, so it loses on
low-motion footage where ~96% of cells already SKIP — **off by default, opt-in per clip**.

## Split planes (the shipped body, `TVID_FLAG_SPLIT`)

The structure bits, the cell bytes, the mode tags, and the colors have wildly different
statistics; coding them in one interleaved stream gives the entropy coder a muddled mix. The
shipped body instead splits them into **separate planes**, each entropy-coded on its own
(structure bits dominate and want a table separate from the cell bytes).

Each plane is a self-describing chunk:

```
[u8 method][u32le raw_len][u32le payload_len][payload]
```

Plane order (optional planes in brackets):

1. **structure** — per-frame `[u16 len][split bits + mode tags (if inline) + SHIFT bits + PAL2 selectors]`
2. **[mode]** — (if `MODEPLANE`) one mode byte per leaf, optionally zero-RLE'd (`MODERLE`)
3. **cell** — keyframe cells then per-leaf cells — *or*, if `CELLSPLIT`, a **raster** plane
   (keyframe + RAW cells) and a **palette** plane (SOLID + PAL2 cells)
4. **[color]** — (if `COLOR`) keyframe hues then per-leaf hues, built by the identical quadtree
   walk so the decoder advances the color cursor in lockstep with the cell cursor

### Entropy methods (per-plane auto-select)

`append_plane` ([enc_stages.cpp](../src/encoder/enc_stages.cpp)) compresses each plane with
every candidate and keeps the smallest, tagging the winner's `method`:

| method | coder | wins on |
|--------|-------|---------|
| 0 | raw (stored) | incompressible planes |
| 1 | LZSS ([lzss](../src/common/lzss_dec.c)) | repetitive byte runs |
| 2 | LZSS+Huffman, *or* no-LZ order-0 Huffman ([entropy](../src/common/entropy_dec.c)) | mixed; the no-LZ variant wins SKIP-dominated mode/palette planes where LZ tokenization hurts |
| 3 | **adaptive order-1 range coder** ([range](../src/common/range_dec.c)) | almost everywhere — the cell, structure, and mode planes |

The two method-2 variants decode identically (a no-LZ stream is just a match-free token stream),
so the decoder needs no extra branch for it. The encode halves are C++
(`src/encoder/*_enc.cpp`); the player links only the `*_dec.c` decode halves.

### Auto-selected layout levers

The encoder builds each candidate, entropy-codes it, and sets the header flag **only when it is
smaller** (never a regression):

- **`MODEPLANE`** — pull per-leaf mode tags out of the structure bits into their own byte plane.
  Wins on color video (the structure plane then carries only split/selector bits).
- **`MODERLE`** — zero-run-length-encode the mode plane before entropy. SKIP=0 dominates, so
  collapsing zero runs beats Huffman's 1-bit-per-symbol floor. The biggest post-split win on
  some inputs.
- **`CELLSPLIT`** — carry cell bytes in two planes by mode class (raster vs palette), each with
  its own table. A small color-video win.

## Color plane (`--color` / `TVID_FLAG_COLOR`)

Color is an **additive, parallel plane** — it never touches the cell byte. This is the deliberate
answer to the half-block lesson ([abandoned-levers.md](abandoned-levers.md)): that lever failed
because it *redefined* the cell (2 bytes, fought the entropy coder). Here the cell byte is
bit-identical with or without color; color is one extra plane the entropy coder handles like any
other, so a video-only stream and its colorized sibling share identical shape bytes.

- **One xterm-256 hue index per cell.** The fixed xterm 256-color palette (16 ANSI + 6×6×6 cube +
  24 grays; [xterm256.h](../src/common/xterm256.h)) — nothing to transmit, both render targets
  agree byte-for-byte on what index N means.
- **Shape and brightness stay in the cell byte; the color byte is pure hue.** At render, the
  cell's lit sub-pixels are drawn in `xterm256[hue]` scaled by the cell's luma level
  (`tvid_xterm256_rgb_dim`). A dim and a bright cell of the same hue share one color byte but
  render at different brightness, and the hue plane stays temporally stable (good for the range
  coder).
- **The plane mirrors the cell plane's leaf structure.** Per leaf the encoder emits: SOLID → 1 hue
  (leaf majority), RAW → per-cell hue (raster order), PAL2 → 2 hues (per-selector majority, applied
  with the same selector bit), SKIP → nothing (hue persists). The decoder
  ([codec.c](../src/common/codec.c) `decode_node`) walks the identical structure with a parallel
  color cursor and writes a hue framebuffer in lockstep with the cell framebuffer.

### Flag-bit reuse

All 8 header-flag bits were already used. v2 retirement freed one: the whole-stream
`TVID_FLAG_HUFF` (0x08) is impossible under `TVID_FLAG_SPLIT` (split uses per-plane auto-select),
so **0x08 is reused as `TVID_FLAG_COLOR` when SPLIT is set**. The two never co-occur; the player
disambiguates by the SPLIT bit.

### Rendering

- **ANSI terminal** ([backend_ansi.c](../src/decoder/backend_ansi.c)): always truecolor SGR.
  Grayscale → `ESC[38;2;v;v;vm`; color → `tvid_xterm256_rgb_dim(hue, luma)` → `ESC[38;2;r;g;bm`.
  The sub-cell shape is the Braille UTF-8 glyph. A cell redraws when its shape/luma *or* hue changed.
- **DOS Mode 13h** ([backend_dos.c](../src/decoder/backend_dos.c), the default v3 render): the
  256-entry VGA DAC is programmed grayscale (no color) or with the full xterm-256 palette (color),
  and the per-lit-sub-pixel index written to 0xA0000 is the cell's luma or its hue respectively. In
  the color path brightness comes from *dot coverage* (a dim glyph lights fewer dots) rather than
  per-pixel dimming, because 256 DAC slots can't hold hue×luma; lit dots show full-saturation hue.
  `TVID_TEXT=1` forces the text fallback (grayscale only).

### Measured cost

On a 30-frame 80×24 slice of `video.webm` (color source), lambda 6: the color plane is 11,239 B
raw → **4,362 B entropy-coded (38.8%, ~145 B/frame)** through the range coder. The cell plane is
**byte-identical** with and without color (verified via `TVID_DUMP=1` cmp), and ~75% of decoded
cells render as a genuinely non-gray color from the decoded hue plane.

### DOS memory note

The split planes (struct, cell, mode, **color**) each decompress fully resident. On a RAM-starved
DOS/4GW target, hi-res (160×48 = 4× the cells) + color + a decoded audio track can exceed memory.
The player decodes the color plane **non-fatally**: if it or its hue framebuffer won't allocate, it
drops to grayscale and plays on instead of aborting (`read_plane_opt` in
[player.c](../src/decoder/player.c)). To keep color on a hi-res DOS clip, lower `--fps`, shorten it,
or drop audio. The real fix — streaming planes in chunks instead of whole-file-resident — is its own
task; see [v3-streaming.md](v3-streaming.md).

### Encoding

```sh
tools/encode.sh in.mp4 out.tvid --color          # 80x24, color
tools/encode.sh in.mp4 out.tvid --color --hi-res # 160x48, color (RAM-heavy on DOS)
```

`--color` implies `--split` (the color plane lives only in the split body). The encoder reads the
same `(cols*SUBW) x (rows*SUBH)` rgb24 sub-pixel source as the grayscale path: it derives luma
(shape/brightness) from it and, with `--color`, box-averages each cell's hue to an xterm-256 index.
Probe the added cost with the `-DTVID_PROBE` build (`probe[color]:` line) — see
[CLAUDE.md](../CLAUDE.md).

## Encoder-side knobs (no format/decoder change)

- **`--stable N`** — per-cell temporal hysteresis: a cell's target only updates when its averaged
  sub-pixel luma moves more than the deadband, turning quantization shimmer into SKIPs. The single
  biggest lever for fitting a busy clip.
- **`--lookahead N`** — extends the deadband into a forward window: an excursion that reverts
  within N frames is treated as a transient blip and held, instead of updated-then-reverted (which
  costs bits twice).
- **`--block-stable`** — block-level SKIP distortion credit (squared-error units): discounts a
  SKIP leaf's cost so a barely-changed block prefers holding still.
- **`--lambda`** — the RD weight. Lower buys detail (bits cheap → RAW/PAL2); higher shrinks the
  file (bits expensive → SOLID/SKIP). `tools/encode.sh --fit-size` raises lambda to fit the budget,
  stepping fps 12→10→8 only as a last resort.

## Shipped vs experimental

| Lever | Status |
|-------|--------|
| Quadtree block coder (SKIP/SOLID/RAW/PAL2) | **shipped** |
| SHIFT motion sub-variant | **shipped** (opt-in) |
| Split planes (structure / cell / mode / palette / color) | **shipped** |
| Mode plane (`MODEPLANE`) | **shipped** (auto) |
| Mode-plane zero-RLE (`MODERLE`) | **shipped** (auto) |
| Cell-split (`CELLSPLIT`) | **shipped** (auto) |
| Entropy method 1 (LZSS) | **shipped** |
| Entropy method 2 (LZSS+Huffman / no-LZ Huffman) | **shipped** |
| **Entropy method 3 (adaptive order-1 range coder)** | **shipped** — the largest single win (−13.7% to −23.7% across inputs) |
| Optional xterm-256 color plane | **shipped** |
| Temporal hysteresis / lookahead / block-stable / lambda | **shipped** (encoder knobs) |
| PAL4, field-split cells, order-1 *static* Huffman, glyph-as-shape, half-block sub-cell color, forward-reference frames, per-leaf cell predictor, drop-per-frame-length | **rejected** — see [abandoned-levers.md](abandoned-levers.md) |

## The round-trip gate

Every mode must decode bit-identically to what the encoder believes it emitted — the bitstream is
lossless end-to-end; "lossy" lives only in the *quantization* step, never in the container. The
encoder re-decodes its own payload each frame and dies on mismatch; `ctest` re-decodes
independently (`codec_roundtrip`, `encode_decode`), and `coder_golden` pins the *exact* compressed
bytes so a refactor can't silently change the output. Measure every block-level scheme against the
entropy-coded size, and against decode time on a cycle-limited DOSBox run — the asymmetry rule is
the tiebreaker.
