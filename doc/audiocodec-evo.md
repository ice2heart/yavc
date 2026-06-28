# termvideo — audio codec evolution

The audio tail is **~50%+ of a shipped file** (often more on the mono clips, where
it is ~80–90%). It is therefore the single highest-leverage place to win bytes, and
the place where this doc tracks the codec's evolution: what shipped, what is
measured-but-not-yet-wired, and the rationale gating each step.

The governing constraints are the project-wide ones: every lever is judged against
the **entropy-coded** baseline (never raw), and the **decoder asymmetry rule**
(O(samples), branch-light, table-driven, integer-only, builds on 16-bit DOS C99) is
the tiebreaker. Audio decode runs on the same DOS player that paces video off the
audio clock, so a fancy encoder is fine but the decode side must stay cheap.

See also: [compression.md](compression.md) "Audio entropy coding",
[abandoned-levers.md](abandoned-levers.md) (lossless LPC / non-integer codecs ruled
out), [possible-improvements.md](possible-improvements.md) (the companion graveyard
of measured-but-shelved video levers).

---

## codec 1 — IMA/DVI ADPCM (shipped, the baseline)

4-bit IMA ADPCM, no entropy stage. Each block is `[u16 predictor][u8 index][u8 pad]`
then `(samples-1)` nibbles packed 2/byte (low nibble first). The step index (0..88)
seeds at 0 per block and evolves per nibble via `adpcm_index_table`; the quantizer
step is `adpcm_step_table[index]`. Tables/clamp shared bit-exactly between
[adpcm_enc.cpp](../src/encoder/adpcm_enc.cpp) and the portable-C99
[adpcm_dec.c](../src/common/adpcm_dec.c) via
[adpcm_internal.h](../src/common/adpcm_internal.h). Measured 4.01 bits/sample — i.e.
ADPCM itself leaves entropy on the table (`gzip`/`xz` recover ~10–16%).

The lossy floor was settled here (see abandoned-levers.md): lossless PCM re-encode
(Shorten/FLAC LPC+Rice) is **3× larger** than ADPCM (ADPCM's lossiness does the
work), and Opus/AAC violate the integer-only asymmetry rule. So the only remaining
*lossless-vs-ADPCM* lever is to entropy-code the existing nibbles — which is what
codecs 2 and 3 do, at no quality cost.

## codec 2 — ADPCM + grouped order-1 range coder (shipped, `--audio-entropy`)

`audio_codec == 2` (`TVID_AUDIO_IMA_ADPCM_ENT`) keeps the **exact same ADPCM blocks**
but wraps each group of `TVID_AUDIO_ENT_GROUP` (256) blocks in a self-describing
entropy chunk reusing the split-body plane-chunk shape:
`[u8 method][u32le adpcm_len][u32le payload_len][payload]`, method 0 = stored, 3 =
range-coded ([range.c](../src/common/range_dec.c), the generic adaptive **order-1
byte** coder). The decoder range-decodes a chunk back to the exact codec-1 ADPCM
bytes, then runs the unchanged `adpcm_decode_block` — decoded PCM is **bit-identical
to codec 1** (no flag bit, no version bump; a codec-1-only player just plays silent).

Wrapper code: `entropy_wrap_adpcm_k` in [enc_stages.cpp](../src/encoder/enc_stages.cpp).

**Group size is a RAM-vs-ratio knee.** The adaptive coder needs a long warmup, so
small groups pay a per-reset tax (vi/sat sweep: K=4 → ~96%, K=64 → ~92%, K=256 →
~89%, K=1024 ≈ whole-stream ~87%). K=256 (~64 s @ 8 kHz) sits at the knee — within
~2% of the floor while bounding the decompressed group to 256 KB resident and giving
a clean DOS DMA restart point every ~64 s. Realized whole-file win **~10–12%** on the
mono clips. Tune via `probe[adpcm-ent]` (`-DTVID_PROBE`).

### The blind spot in codec 2

The method-3 coder is **generic**: its context is `ctx = previous byte` over the
*flattened* chunk bytes. It has no idea it is looking at ADPCM. Concretely it is
blind to two things the format makes free:

1. **The step index.** A 4-bit code's statistics depend entirely on the current step
   size (`adpcm_step_table[index]`): at a small step the codes cluster near 0; at a
   large step they spread. Order-1-over-bytes mixes all step regimes into one model.
2. **It codes the wrong unit.** It models *bytes* (= two unrelated prior nibbles
   packed) and even folds each block's 4-byte header into the same model as nibble
   data.

This is exactly the situation the order-1 range coder itself once fixed for the
video cell plane: the win in this codec has *always* been a **better back-end /
context selector**, never a pre-transform (see compression.md and the
[mono-cell-entropy-gap] note). The step index is the obvious selector — and crucially
it is **free at decode**.

## codec 3 (proposed) — ADPCM + step-index context nibble coder (MEASURED WIN, not yet wired)

> **Naming note:** [compression.md](compression.md) §"Lossy audio size knob" also
> pencils in `audio_codec == 3` for a hypothetical *3-bit IMA lossy* variant. That
> one was never built. Pick the next free `audio_codec` id when wiring this; the two
> are independent (this one is **lossless** vs codec 1, the 3-bit one is lossy).

### The lever

Keep codec 2's grouped-chunk wrapper, but replace the per-chunk entropy method:
instead of `range_compress` over the chunk *bytes*, walk the chunk's ADPCM blocks and
code each **4-bit nibble** with a model selected by a **context the decoder
reconstructs for free**:

> **context = `(step_index >> 3)` bucket × `previous nibble`** — 12 buckets × 16 = **192
> contexts**, 16-symbol (nibble) alphabet.

`step_index` evolves identically on both sides (`adpcm_dec.c:47`), reseeding to 0 at
each block start, and the previous nibble is trivially the last code decoded. So the
context needs **zero new decoder state** and adds **no asymmetry-rule cost** — the
decisive difference from the shelved `cabac-split` lever, whose context lived in a
hot quadtree walk (see [possible-improvements.md](possible-improvements.md)).

The range-coder mechanics are identical to the shipped order-1 coder
([range_internal.h](../src/common/range_internal.h): `INC=8`, `CAP=2^16`), so the
comparison is apples-to-apples — only the alphabet (16 vs 256) and the context
selection differ. A 16-symbol cumulative-frequency loop is **cheaper** than method
3's 256-symbol one, and 192 contexts is **fewer** than method 3's 256 — so the
decoder is, if anything, lighter than codec 2's.

### What was measured (`probe[adpcm-ctx]`, behind `-DTVID_PROBE`)

The probe ([adpcm_ctx_probe.hpp](../src/encoder/adpcm_ctx_probe.hpp), hooked in
[encoder.cpp](../src/encoder/encoder.cpp) next to `probe[adpcm-ent]`) re-walks the
raw ADPCM blob block-by-block exactly as the decoder does, reconstructs the
per-nibble step index, and codes the nibbles through the encoder-only context coder.
Baseline is the shipped order-1 coder at the same grouping. lambda 6, fps 10.

**Context-design sweep (whole-stream, no grouping):** order-2 nibble history wins the
ceiling, bucket granularity barely matters.

| context | bif | bad | video |
|---|---|---|---|
| `(idx>>4) × prev1` — 96 ctx | 863,901 | 781,729 | 752,482 |
| `(idx>>3) × prev1` — 192 ctx | 858,905 | 777,407 | 752,025 |
| `(idx>>2) × prev1` — 368 ctx | 857,481 | 777,023 | 752,544 |
| **`(idx>>3) × prev2` — 3072 ctx** | **851,018** | **775,026** | **745,855** |
| baseline order-1 (whole) | 878,572 | 795,954 | 761,309 |

**The decisive measurement — the group-size knee.** A shipped codec must respect
grouping (DOS restart points + bounded resident RAM, the same reason codec 2 uses
K=256). Resetting the model every K blocks taxes warmup, and the **3072-context
order-2 model cannot warm up inside a 256-block group** — it *loses to order-1 there*:

| model | clip | K=whole | **K=256** | K=64 |
|---|---|---|---|---|
| `(idx>>3) × prev2` (3072 ctx) | bif | 851,018 | 860,075 | 881,045 |
|  | bad | 775,026 | 783,238 | 801,187 |
|  | video | 745,855 | 758,617 | 779,881 |
| **`(idx>>3) × prev1` (192 ctx)** | **bif** | 858,905 | **859,018** | 861,707 |
|  | **bad** | 777,407 | **777,728** | 779,581 |
|  | **video** | 752,025 | **753,072** | 754,503 |

The 192-context order-1 model is **group-size-insensitive** — it holds essentially
its full win at K=256 and even K=64 (low RAM, frequent restart). That is the ship
choice. Net vs the shipped codec-2 K=256 size:

| clip | codec 2 (K=256) | **codec 3 (K=256)** | Δ audio | Δ whole-file |
|---|---|---|---|---|
| bif | 878,581 | **859,018** | −19.6 KB (−2.2%) | **−1.1%** |
| bad | 795,963 | **777,728** | −18.2 KB (−2.3%) | **−1.5%** |
| video | 761,318 | **753,072** | −8.2 KB (−1.1%) | **−0.6%** |

(Whole-file % uses total file ≈ video body + audio per clip; audio is the majority,
so the audio % and whole-file % are close.)

This is **2–3× the whole-file gain of the shelved `cabac-split` lever, with none of
its decoder cost** — a clean win that clears both the compression bar and the
asymmetry-rule bar.

### How to wire it (codec 3)

1. **Coder, split enc/dec, bit-exact.** Add an encoder C++ coder + a portable-C99
   decoder sharing constants and the 192-context model layout bit-exactly via a
   `range_ctx_internal.h` (the "model parameters ARE the wire format" pattern of
   [range_internal.h](../src/common/range_internal.h)). The decoder is a 16-symbol
   adaptive range decoder keyed on `(index>>3)*16 + prev_nibble`.
2. **Reuse the grouped-chunk wrapper.** Codec 3 keeps codec 2's
   `[u8 method][u32 adpcm_len][u32 payload_len][payload]` chunk shape and K=256
   grouping. The new entropy method (call it `4`, a per-chunk method byte alongside
   0/3) means "context-coded nibbles"; the chunk still decodes back to the **exact**
   codec-1 ADPCM block bytes, so PCM stays bit-identical. Auto-select per chunk:
   emit method 4 only when strictly smaller than method 0/3 — **never a regression**.
3. **Decode path.** The player's audio chunk reader recognizes method 4 and, per
   block in the chunk, range-decodes nibbles with the reconstructed
   `(step_index, prev_nibble)` context, rebuilding the ADPCM bytes, then runs the
   unchanged `adpcm_decode_block`. No flag bit, no version bump — the `audio_codec`
   sub-header byte selects it (codec 2 shipped exactly this way).
4. **The gate (absolute).** Add a method-4 audio round-trip to the coder tests and
   **pin its exact bytes** in `coder_golden` (extend the method enum; never perturb
   existing goldens). Confirm a `TVID_DUMP=1`-equivalent audio decode is
   byte-identical to codec 1's PCM, and that the DOS player still sustains A/V sync
   on a cycle-limited run (the real tiebreaker — but this path is *lighter* than
   codec 2, so it should pass comfortably).

### Reproduce the measurement

```sh
cmake -S . -B build-probe -DTVID_PROBE=ON && cmake --build build-probe --target encoder
ENCODER=$PWD/build-probe/encoder tools/encode.sh videos/video.webm /tmp/v.tvid --lambda 6 2>&1 \
  | grep 'probe\[adpcm-ctx\]'
```

---

## Other audio levers (status)

- **`--audio-rate` (lossy, shipped).** Lowering the sample rate scales ADPCM size
  linearly — the *largest* audio lever, at a quality cost. 6 kHz ≈ 64% of audio,
  4 kHz ≈ 43% (with entropy stacked). Ship default 8 kHz; 6 kHz is the first step for
  an over-budget clip. Orthogonal to the entropy codecs and stacks with them.
- **3-bit IMA (`audio_codec == 3`-as-pencilled, lossy, not built).** The next lossy
  step after 6 kHz; needs a new decode path. Gated on whether 6 kHz already covers
  the sweet spot. (Distinct from the codec-3 entropy lever above — pick a different
  id.)
- **Lossless PCM (LPC+Rice) — dead.** 3× larger than ADPCM; see abandoned-levers.md.
- **Opus/AAC — dead.** Violate the integer-only DOS asymmetry rule.
