# termvideo — abandoned compression levers (and why)

A graveyard of compression ideas that were proposed, measured, and **rejected**, kept so we
don't re-litigate them. Each entry says what it was, what was measured, and the verdict. The
ideas that *won* are in [compression.md](compression.md); this file is only the failures.

> **The governing rule for every measurement here:** judge a block-level or bit-level scheme
> against the **entropy-coded output**, never the raw block stream. A scheme only wins if it
> removes redundancy the entropy coder cannot — and any scheme that adds irregular, low-entropy
> bits actively *fights* the back-end. Most of these died on exactly that.

## The recurring lesson

Five for five (FWDREF, PAL4, entropy-widening, field-split, half-block): a new block-level or
pre-entropy scheme rarely beats the existing **quadtree + PAL2 + range coder** stack, because
that stack already captures the redundancy more cheaply, and irregular low-entropy bits fight the
back-end. Structural headroom only helps if the render target can cheaply *express* it. The one
thing that did win big — the **adaptive order-1 range coder** (shipped, method 3) — won precisely
because it is a *better back-end*, not a pre-transform: it is adaptive (no table to serialize) and
spends fractional bits on predictable symbols, the two things every static-table attempt couldn't.

---

## Forward-reference frames (FWDREF / GOP reorder)

**What:** reorder frames into a GOP with forward references so a SKIP could point at a future
keyframe, capturing returning backgrounds at the block level.

**Measured:** fully implemented, then removed. It taxed every SKIP leaf 1 bit and added a keyframe
per GOP, and **lost on every clip**.

**Why dropped:** it competes with the entropy back-end for the *same* redundancy. LZSS+Huffman
already compresses returning backgrounds to ~26% of raw as literal repeats, while FWDREF paid an
*uncompressed* per-leaf tax to capture the same thing at the block level. This is the original
"measure against the entropy-coded size" lesson.

## PAL4 — per-block 4-color sub-palette

**What:** a 4th palette mode (4 cell bytes + 2-bit selector/cell), halving RAW's per-cell payload
for moderate-variety blocks.

**Measured:** on color video it changed the raw block stream by <0.1% and **lost 0.2–0.4% after
entropy** at every useful lambda. The leaf budget on a real clip is SKIP 59% / SOLID 32% / RAW 4%
/ PAL2 5%; PAL4 can only ever cannibalize the **4% RAW slice**, and RAW cells are exactly where
LZ already matches, so most of the pre-entropy gain is clawed back.

**Why dropped:** not worth a new mode. Widening the mode tag is strictly worse (+1 bit × ~1M
leaves, 91% of them SKIP/SOLID, to chase a 4% slice). The quadtree already subdivides 3–4-tone
regions into cheap PAL2/SOLID leaves.

## Closing the entropy gap with static order-1 / context Huffman

**What:** condition the literal Huffman table on a cheap context (e.g. the previous byte's high
nibble → 16 tables), or segment the stream and emit fresh tables per segment.

**Measured (production code, on the mono cell plane):** order-1 context Huffman came in at **−1.9%
vs shipped LZSS+Huffman — *worse* than simply dropping LZ** (no-LZ order-0 was −3.6% there).

**Why dropped:** the 16 tables cost ~2.4 KB of serialized lengths plus a per-table inefficiency
loss (each table sees fewer samples → coarser codes) that more than ate the context gain. *Lesson:
always validate an entropy estimate against the real `huff_build_lengths` and the real alphabet/
limit, not an idealized −Σf·log₂p — a first Python pass mis-estimated this at −7.5%.* The real
remaining gap to xz needs LZMA-class **adaptive range coding** — which is what eventually shipped
as method 3 (see [compression.md](compression.md)), making this static-table approach moot.

## Field-split the cell byte (separate luma + glyph planes)

**What:** split `[luma:2|glyph:6]` into a luma plane (4 distinct values, smooth) and a glyph plane
(skewed 64-symbol alphabet), entropy-coding each separately.

**Measured:** combined 575 KB → luma 222 KB + glyph 474 KB = **696 KB (+20.9%)**.

**Why dropped:** separating the fields breaks whole-cell LZ matches — a repeated `[luma,glyph]`
background cell matches as one byte combined; split, neither field repeats as cleanly. Same shape
as every "decorrelate before entropy" failure.

## Per-sub-pixel luma cell (drop the glyph, store 8 lumas per cell)

**What:** replace the `[luma:2|glyph:6]` cell byte with **8 independent sub-pixel luma values**
(one per 2×4 Braille sub-pixel), so a cell carries real per-dot grayscale instead of a shared
level × one of 64 binary ink shapes. Motivated by wanting finer fidelity on the DOS Mode 13h
graphics renderer (the only target that can show per-dot gray; an ANSI Braille glyph paints all
lit dots one color).

**Measured (synthetic, through the real entropy back-end — `measure_plane_size`, the shipped
lzss/huffman/no-LZ-huffman/order-1-range candidate set — off the post-hysteresis `st.targets`
sub-luma blocks; whole clips, iso-quality by reporting mean per-sub-pixel SSE alongside coded
bytes):**

| representation | bad.webm coded (SSE) | video.webm coded (SSE) |
|---|---|---|
| **glyph (shipped)** | **399 KB (140)** | **834 KB (528)** |
| 1 bit/sub-pixel | 266 KB (234) — cheaper but *worse* quality | 469 KB (3343) |
| 2 bit/sub-pixel | 754 KB (50) +89% | 1744 KB (421) +109% |
| 3 bit/sub-pixel | 1241 KB (11) +211% | 3662 KB (84) +339% |
| 2-endpoint (min/max + 1 sel bit/subpx, 2 B/cell) | 1488 KB (81) +273% | 2629 KB (100) +215% |

Also tried on the same quantized values, one byte per sub-luma so runs survive: **byte-run RLE**,
**per-cell-mean normalization** (residual centered on 0), and **per-position deinterleave** (all
sub-pixel-0 bytes together, etc.). None crossed the frontier — RLE helped the packed plane a
little (bad 2-bit 1078→919 KB) but stayed +130%; deinterleave made it *worse* (+246…+540%);
normalization was ≈flat. Full sweep is reproducible by re-adding the `probe[v5cell]` block (it was
a go/no-go probe, not kept in tree).

**Why dropped:** the 6-bit glyph is effectively a **learned vector-quantization codebook** — the
64 patterns are the most-common real sub-pixel shapes, so 6 bits + a shared level reconstruct the
cell almost as well as 8 independent lumas at 1/8 the raw size. The glyph cell sits **on the
rate–distortion frontier**; every per-sub-pixel representation is strictly behind it (1.3–3.7×
larger at equal quality, or worse quality when cheaper). Rearranging the same 8 luma values (RLE /
normalize / deinterleave) cannot recover information the codebook already captured — the same
"decorrelate/rearrange before entropy can't beat a good representation" lesson as field-split and
the left/up predictor below. Per-sub-pixel luma *does* win on DOS fidelity (3-bit is near
lossless) but that is a quality-for-size trade that breaks the floppy budget, not compression.

## Motion compensation on this footage (SHIFT + MV prediction)

**What:** the SHIFT motion sub-variant (opt-in, `--shift`), and on top of it a motion-vector
**predictor** — code each moved leaf's `(dx,dy)` as a residual against the componentwise median
of its causal-neighbor MVs (left / up / up-left cells), H.264-MVp style, instead of the shipped
absolute (biased) coding.

**Measured (all six `videos/` clips, fps 10, `--stable 32 --lookahead 8 --block-stable 2000
--split-lookahead 2 --lambda 6`):**

| clip | no-shift → `--shift 7` | MV-prediction Δ (of the MV-component bytes) |
|---|---|---|
| bad | 400 KB → 464 KB (**+16%**) | +0.1% |
| video | 595 KB → 776 KB (**+30%**) | +0.2% |
| bif | 802 KB → 965 KB (**+20%**) | +0.9% |
| vi | 183 KB → 293 KB (**+60%**) | −0.1% |
| sat | 135 KB → 216 KB (**+60%**) | +0.4% |
| metallica | 694 KB → 1000 KB (**+44%**) | +0.4% |

**Why dropped, two independent reasons:** (1) **SHIFT itself loses 16–60%** on every clip — the
"moved" bit is spent on *every* SKIP leaf, and SKIP dominates (1–2.8 M leaves per clip), so the
moved-bit tax on the SKIP majority dwarfs any copy savings on the handful of moved leaves. This is
exactly why SHIFT ships off-by-default. (2) **MV prediction is neutral (±1%)** and the MV component
bytes are only ~1–2% of the file to begin with (5–16 KB), so even a *perfect* predictor saves <1%
whole-file — and only on clips where SHIFT is already a 16–60% regression. Moved leaves are sparse
and spatially scattered (~4–8 per frame), so a leaf's neighbors are almost always non-moving
(predictor ≈ 0 ⇒ residual ≈ absolute) — there is no coherent-pan structure for the predictor to
exploit. Block motion compensation simply does not beat the SKIP + order-1-range baseline on this
sparse-motion content; the shipped glyph codec is already near the rate–distortion frontier for it.
Sub-pixel SHIFT would only refine the same sparse moved leaves (and cannot touch the moved-bit tax),
so it was not built. *Measurable behind `-DTVID_PROBE` as `probe[mvpred]`.*

## Per-leaf cell-byte decorrelation (left/up predictor)

**What:** within each RAW leaf, store `cell − left` (or `− up`) so residuals cluster near zero.

**Measured (through the real entropy coder):** strictly worse on every input — +24.7%/+29.2%
(bad.webm color), +28.7%/+33.3% (video.webm color), +10.0%/+13.7% (bif.webm mono).

**Why dropped, two fundamental reasons:** (1) the residual destroys LZ matches — a run
`[42,42,42]` becomes `[42,0,0]` whose values depend on leaf position, so cross-leaf matches
vanish; (2) the cell byte is a packed categorical index (`[luma|glyph]`), not a scalar magnitude,
so `v − left` carries/borrows across the field boundary and produces high-entropy noise.
Subtraction is the wrong operator for the byte. This was the third "decorrelate before entropy"
lever to die on the cell plane. *Still measurable behind `-DTVID_PROBE` as `probe[raw-predict]`.*

## Whole-keyframe intra prediction (v4 segmentation)

**What:** v4 segmentation can place a fresh keyframe at each segment boundary (vs once per
movie), so keyframe-cell bytes ship more often and their compression matters more. The open
question the per-leaf result above did *not* settle: does a left/up spatial predictor over the
**whole** `cols×rows` keyframe raster (not per-leaf) beat plain entropy? More context per
predictor could in principle help where the tiny per-leaf windows could not.

**Measured (through the real range coder, `probe[seg-kf]`):** still strictly worse. Smooth
synthetic gradient — plain 246 B vs left 305 (+59) / up 310 (+64). Textured+noise — plain 917 B
vs left 1161 (+244) / up 1330 (+413). The deficit *widens* with content complexity.

**Why dropped:** the same two reasons as the per-leaf case carry over at full-frame scope — the
keyframe is a raster of packed categorical `[luma|glyph]` bytes, not a scalar field, so
`cur − neighbor` carries/borrows across the field boundary into high-entropy noise, and the
residual destroys the cross-cell runs the range coder feeds on. Wider prediction context does not
rescue an operator that is wrong for the byte. v4 ships **plain entropy** for keyframes (the
format already auto-selects methods 0–3 per chunk, range coder included). *Still measurable behind
`-DTVID_PROBE` as `probe[seg-kf]` in `write_split_segmented_output`.*

## Split-bias — surcharge to force bigger quadtree leaves

**What:** a per-split penalty (`--split-bias N`) added to the "subdivide into 4" option in
`Tree::build`, modeling the post-entropy structure/mode-tag cost the flat-`lambda` objective
omits, to bias the tree toward **bigger leaves**. The intuition: a bigger leaf removes split bits
*and* the mode tags they spawn, which the range coder would otherwise have to code, so the
per-frame RD optimum splits more finely than the post-entropy optimum.

**Measured (entropy-coded whole-file vs `--split-bias 0`):** DEAD. Best case was a −0.12%/−0.06%
wiggle (noise); larger bias regresses (**+2.75%** at bias 64). Both the structure plane *and* the
cell plane grow.

**Why dropped:** `Tree::build` is already an **exact** RD optimum, and the range coder already
codes the split bits near-free (they are highly predictable order-1). Forcing coarser leaves
trades those cheap split bits for **larger cell payloads** — the bytes a coarse leaf must spend to
approximate detail it would otherwise have subdivided away. This is the same lesson as the
"decorrelate before entropy" levers above, in a different guise: don't fight a decision the entropy
back-end has already priced correctly. The companion lever that *did* ship from the same
investigation was **`--split-lookahead`** (temporal-stability credit, not a coarseness surcharge);
see [compression.md](compression.md). *Still measurable behind `-DTVID_PROBE` as the
`probe[bigchunk]:` line (`--split-bias` field).*

## Glyph-as-shape template matching (single-color)

**What:** pick the glyph whose ink pattern best matches the cell's sub-pixel luminance gradient,
rather than by luma alone.

**Measured:** the *ceiling* — choosing the glyph with full knowledge of both vertical sub-pixels
(still one value/cell) gives **100.0% of the current distortion**, i.e. exactly zero improvement.

**Why dropped:** under squared error, the single best value for the sub-pixels *is* their mean,
which the box-average already produces. A uniform single-value cell has no shape dimension to
exploit. Only genuinely *separate* sub-cell values could use the gradient — which is the half-block
lever below.

## Half-block sub-cell color (`▀` with separate fg/bg)

**What:** use the upper-half-block glyph with independently chosen top/bottom colors to double
vertical sub-cell color resolution — the one lever with large measured *ceiling* headroom (it
could roughly halve color error / cut B&W error ~9×).

**Measured:** built end-to-end twice. On `video.webm` at 160×48, lambda 6, the realized result was
**+18.6% B/frame** — the RD search almost never picks it, and when it does it pays the per-leaf bit
for ~no fidelity gain.

**Why dropped — the palette luma wall:** a glyph cell gets brightness from the ramp (many luma
levels) and hue from the palette; a half-block cell has no glyph, so the two solid halves must
carry *both* hue and brightness from the coarse palette alone, quantizing a vertically varying cell
to nearly the same color. The xterm-256 fix that would supply real per-half luma doubles the cell
to 2 bytes, fights the entropy coder (high-entropy indices), and grows decoder cost — erasing the
win at this content/resolution. The ceiling assumed a color space the renderer doesn't cheaply
have. If ever revisited, only as a deliberate **quality-only** mode with xterm-256 fg/bg, never as
a compression lever.

## Drop the per-frame `[u16 len]` from the structure plane

**What:** omit the per-frame length prefix on the structure plane (frame boundaries are implicit in
the quadtree walk).

**Measured:** ~3–4 KB saved. **Not shipped** — it would need a 9th header-flag bit (the flags byte
is full) / 16-bit flags, and the saving doesn't justify the format churn. *Still measurable behind
`-DTVID_PROBE` as `probe[nolen]`.*

## Lossless audio re-encode (Shorten/FLAC-style LPC + Rice)

**What:** replace lossy IMA-ADPCM with a true lossless codec — fixed-order
polynomial (LPC) prediction of the PCM, Rice/Golomb coding of the residuals (the
classic DOS-era integer-only design, decodable under the asymmetry rule).

**Measured (on the decoded PCM of vi/sat/bif):** fixed-order 1/2/3 LPC + per-block
optimal-k Rice came out at **300–366% of the ADPCM size** — 3× *larger*.

**Why dropped:** ADPCM is *lossy* (4 bits/sample); lossless 8 kHz speech/music
needs ~12 bits/sample. Lossless is simply the wrong target when the goal is
*smaller*. The win is to entropy-code the existing ADPCM nibbles (lossless w.r.t.
the current audio) — shipped as audio codec 2, see [compression.md](compression.md).

## Non-integer audio codecs (Opus / AAC / Vorbis)

**What:** swap IMA-ADPCM for a modern perceptual codec — far smaller at equal
quality.

**Why dropped (not measured):** none decode integer-only in real-mode/DOS with
two small tables; they need float/MDCT/large state. They violate the decoder
asymmetry rule that governs the whole project (the decoder ships on the floppy
and runs on a 1980s PC). Out of scope by construction, not by measurement.

## In-loop deblocking / sample-adaptive-offset filter (HEVC/VVC-style)

**What:** a post-decode smoothing pass — the deblocking + sample-adaptive-offset (SAO) in-loop
filters that H.265/HEVC and H.266/VVC apply to hide block-boundary artifacts (surveyed in
[compression_reserch.md](compression_reserch.md) §3.2.5). Runs on the reconstructed frame to raise
perceptual quality at a given bitrate.

**Why dropped (not measured — out of scope by construction):** it is a **quality-only, decoder-side**
tool. It does not shrink the file — it *adds* per-pixel decode work to make an already-decoded frame
look better. Under the project's decoder asymmetry rule (O(cells), branch-light, integer-only, ships
on the floppy and runs on a 1980s PC) a filter that spends decode cycles for zero size win is
categorically out — the same reason the half-block / xterm-256 quality levers were rejected. Noted
here only so the paper's mention doesn't get re-litigated as a compression lever.

## Higher-order / affine extensions (noted, not pursued)

- **Range coder order-2** — regresses: 65 K contexts are far too sparse on a <1 MB plane (the same
  sparsity tax that killed static order-1's tables). Order-1 already clears xz; no point going
  higher.
- **Affine/scaled SHIFT** (zoom/rotation motion) — would need per-cell source interpolation (float
  or large LUTs) on the decoder. Violates the asymmetry rule; listed for completeness only.
- **Region-adaptive lambda** — a quality reallocation, not a size win at fixed quality; pure
  encoder-side if ever wanted, but not a compression lever.
