# termvideo — possible improvements (measured wins, deliberately not shipped)

A companion to [abandoned-levers.md](abandoned-levers.md). That file is the
graveyard of ideas that **lost** on measurement. This file is the opposite case:
levers that were prototyped and **measured to win**, but were *deliberately not
shipped* because the win does not justify its cost (usually decoder complexity /
the asymmetry rule, occasionally format churn). They are recorded here with enough
context to pick them up later if the cost/benefit ever shifts — e.g. if the
decoder target relaxes, or if several small levers are batched into one format bump.

> The same governing rule applies: every block/bit-level scheme is judged against
> the **entropy-coded** output, and the **decoder asymmetry rule** (O(cells),
> branch-light, table-driven, integer-only, builds on 16-bit DOS C99) is the
> tiebreaker. A lever lands here when it clears the *compression* bar but fails the
> *decoder-cost* bar.

---

## Binary context coder for the structure split bits (candidate "cabac-split")

**Status:** measured win, **shelved** — too little whole-file gain for the decoder
cost. Still reproducible behind `-DTVID_PROBE` as `probe[cabac-split]`.

### The idea

The structure plane's **split bits** (one per internal quadtree node: "subdivide
into 4?") are today byte-packed by the bitwriter and then entropy-coded as part of
the structure plane by the per-plane auto-select — in practice the order-1 **byte**
range coder (method 3). Order-1-over-bytes is a slightly odd fit for what is
fundamentally a *bitstream*: the byte boundary is meaningless to the tree.

Replace it with a **binary (CABAC-style) range coder**: code each split bit
directly with one adaptive probability per *context*, where the context is built
from information the quadtree walk already has at that node:

- **depth** (0..3 for an 8×8 superblock: sizes 8,4,2,1), clamped,
- **prior-siblings-split** (how many of this node's earlier siblings split, 0..3),
- **local split-bit history** (the last N split bits emitted).

This is a *context selector* (which probability predicts this bit), never a
residual — so it is in the "better back-end" category that is the only kind of
lever that has ever won here (the order-1 range coder itself; see
[compression.md](compression.md) and the [[mono-cell-entropy-gap]] note), not in
the "decorrelate before entropy" category that fills the graveyard.

### What was measured

`probe[cabac-split]` (in `write_split_output`, [enc_stages.cpp](../src/encoder/enc_stages.cpp))
captures every split decision + its `(depth, prior-siblings)` context in
`serialize_node` ([blockcoder.cpp](../src/encoder/blockcoder.cpp), under the
`g_probe_raw_capture` gate so only the canonical pass fills it), then codes them
through the encoder-only binary range coder in
[range_bin_probe.hpp](../src/encoder/range_bin_probe.hpp). The baseline
`bytepacked` is those same bits packed MSB-first and run through `append_plane`
(the closest stand-in for how they ship in the structure plane today).

Measured at lambda 6 / fps 10 on bif / bad / video, **on the split-bit bytes only**:

| context | bif | bad | video | verdict |
|---|---|---|---|---|
| `(depth, prior-siblings)` — 16 ctx | **+8.7 KB** | **+5.8 KB** | −3.1 KB | loses 2 of 3 |
| `… × last-4-bits` (combo4) — 256 ctx | −7.9 KB | +1.2 KB | −11.2 KB | loses bad |
| **`… × last-6-bits` (combo6) — 1024 ctx** | **−10.8 KB (−9.2%)** | **−0.6 KB (−0.9%)** | **−12.2 KB (−12.9%)** | **wins all 3** |

**Why the rich context is needed, and why it is not "order-2 too sparse":** the
byte coder is strong because packing 8 split bits into a byte gives it an effective
*order-8* view inside each byte plus cross-byte order-1 — it accidentally captures
the spatial run-structure of splits. A coarse 16-context binary coder throws that
away and loses. Restoring a 6-bit local history (combo6) plus the structural
context recovers it and then some. 1024 contexts over 0.6–1.4 M split bits per clip
is still richly fed — no sparsity tax (the order-2 *byte* model, by contrast, had
65 K contexts over <1 MB; that is the regime that was too sparse).

### Why it was not shipped

The win is real and **universal**, and because it would be an auto-selected method
(chosen only when smaller) it could **never regress**. But:

1. **Whole-file gain is small.** −0.6 to −12.9% is *of the split-bit bytes*; the
   structure plane is only ~15–25% of the video body and the video body is itself
   a minority of a shipped file (audio dominates). Net is **~−0.6 to −0.8%
   whole-file** — comparable to the already-shipped `--split-lookahead`, which is a
   pure encoder-side knob with *zero* decoder cost.
2. **The decoder cost is structural, not incremental.** A real method-4 cannot be a
   flat `read_plane` method, because the context is the quadtree-walk state, which
   only exists *during* the walk. So the structure plane's entropy decode must move
   **into** the walk in [codec.c](../src/common/codec.c) (`decode_node` /
   `codec_decode_block_split`): the simple bitreader is replaced by a live binary
   range decoder threading `(depth, prior-siblings, history)`, carrying a 1024-entry
   probability table, doing per-*bit* range-decode work. That lands new branchy,
   stateful work in the **hottest path of the DOS decoder** — exactly the file the
   asymmetry rule says must stay O(cells) and branch-light. Paying that for <1%
   whole-file fails the tiebreaker.

### How to implement it later (if the cost/benefit shifts)

The probe already proves the model and pins the winning context. To ship it:

1. **Coder, split enc/dec, bit-exact.** Promote `range_bin_probe.hpp` to a real
   pair: `src/encoder/range_ctx_enc.cpp` (C++, encoder) + `src/common/range_ctx_dec.c`
   (portable C99, player + DOS), with the probability-init, the `kNumMoveBits`
   adaptation rule, and the carry handling shared bit-exactly via a
   `range_ctx_internal.h` — the same "the model parameters ARE the wire format"
   pattern as [range_internal.h](../src/common/range_internal.h). A binary decoder
   is *smaller* than the method-3 symbol decoder (one prob per context, no
   cumulative-frequency search loop), which slightly softens the decoder-cost
   argument but does not remove the "decode moves into the walk" structural cost.
2. **Carve the split bits into their own sub-plane (recommended) or model the whole
   structure plane.** The structure-nomode plane also carries PAL2 selectors and
   (opt-in) SHIFT bits. Selectors are near-random 1-of-2 masks with little context
   gain; the measured win is essentially all in the split bits. Cleanest is to emit
   split bits as a separate context-coded sub-plane and leave selectors/shift where
   they are. Alternatively model the entire structure bitstream with a per-bit-type
   context (split vs selector vs shift as part of the context id).
3. **Method byte, no flag/version churn.** It is a per-plane `method` value (call it
   4) in the self-describing chunk `[u8 method][u32 raw_len][u32 payload_len]…`, the
   same mechanism methods 0–3 use, so it needs **no new `TVID_FLAG_*` bit and no
   version bump** (audio codec 2 shipped exactly this way). Add it to `append_plane`
   ([enc_stages.cpp](../src/encoder/enc_stages.cpp)) following the existing
   `if (rcc > 0 && rcc < best)` auto-select idiom — selected only when strictly
   smaller.
4. **Decoder dispatch.** `read_plane` / `read_plane_opt` in
   [player.c](../src/decoder/player.c) must recognize method 4 and, for the
   in-walk variant, **not** pre-decompress that plane — hand its bytes to the walk,
   which consumes them live. `codec_decode_block_split` gains the live-coder path.
5. **The gate (absolute).** The encoder re-decodes every frame and dies on
   mismatch — method 4 must pass unchanged. Add a method-4 round-trip case to
   `tests/range_test.c` / `tests/encode_decode_test.cpp`, and **pin its exact bytes**
   in `tests/coder_golden_test.cpp` by extending the method enum (never perturb the
   existing goldens). Then confirm a `TVID_DUMP=1` player dump is byte-identical to
   the method-3 build, and — the real tiebreaker — that it still sustains target fps
   on a cycle-limited DOSBox run.

### Reproduce the measurement

```sh
cmake -S . -B build-probe -DTVID_PROBE=ON && cmake --build build-probe
ENCODER=$PWD/build-probe/encoder tools/encode.sh videos/video.webm /tmp/v.tvid --lambda 6 2>&1 \
  | grep 'probe\[cabac-split\]'
```

---

## Candidates not yet probed (expected to die, listed for completeness)

From the same "structural-context back-end" investigation, two sibling candidates
were designed but **not** built, because the analysis expects them to fail for
reasons already in the graveyard. Probe them (same pattern) before assuming:

- **2D spatial context for the cell plane** — context = bucketed up-neighbor cell
  (a *selector*, not a residual). Its only *causal* bytes are the RAW + keyframe
  slice (SOLID/PAL2 go through the palette plane, not per-cell raster) — the same
  thin ~4% RAW slice PAL4 starved on — and it carries the worst decoder-RAM cost
  (per-bucket 256-symbol models). Expected: a wiggle.
- **Structural context for the mode plane** — context = quadtree depth / parent
  mode. Perfect causality and sparsity, near-zero decoder RAM, but the mode plane
  is *already* MODERLE-crushed (SKIP=0 dominated); a context coder competes with
  MODERLE for the same redundancy and must *beat* it, which it likely cannot.
