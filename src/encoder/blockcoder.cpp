// blockcoder.cpp - v2 quadtree block-frame encoder (C++23, offline).
#include "blockcoder.hpp"

#include <array>
#include <cstring>

extern "C" {
#include "bitstream.h"
#include "glyphset.h"
#include "mono_celldist.h"
#include "tvid_format.h"
}

#if defined(TVID_VIZ)
// Codec-visualizer capture (host-only, behind TVID_VIZ; see viz_trace.hpp). The
// serializer walk below appends one VizLeaf per emitted leaf to g_viz_capture, so
// the picture is the exact tree that produced the bytestream -- no recomputation,
// no drift. The visualizer points g_viz_capture at the current frame's leaf
// vector before the canonical split encode and nulls it after, so the nomode /
// raster passes (which run with it null) capture nothing.
#include "viz_trace.hpp"
#endif

#ifdef TVID_PROBE
// Per-leaf cell-byte decorrelation probe. The RAW serializer appends, for every
// RAW leaf, that leaf's cells under several predictors, so encoder.cpp can run
// each variant through the *real* entropy coder and compare to the plain RAW
// raster plane. Leaf-local (resets at each leaf boundary) so we measure true
// in-block adjacency, not the flattened-stream artifact. Cleared by the encoder
// before the capture pass. Definitions live in encoder.cpp's TU via these
// externs to keep the buffers in one place.
#include <vector>
extern std::vector<uint8_t> g_probe_raw_plain;  // plain RAW cells (control)
extern std::vector<uint8_t> g_probe_raw_left;    // residual vs left-in-row
extern std::vector<uint8_t> g_probe_raw_up;      // residual vs cell above (same leaf)
// The split encode runs 3 passes/frame (struct, struct-nomode, raster); only
// capture during the one the encoder marks, so buffers fill exactly once/frame.
extern bool g_probe_raw_capture;
#endif

namespace {

// --- per-leaf candidate modes ----------------------------------------------
// Each candidate reports its bit cost and total distortion over the leaf's
// in-grid cells, and can serialize itself. The quadtree search keeps the
// cheapest; serialization replays only the winner.

struct Ctx {
    const uint8_t *prev;      // cells currently on screen
    const uint8_t *ideal;     // cells we would like to show
    const uint8_t *sub;       // cols*rows*TVID_MONO_SUBN sub-pixel luma
    // Color plane (optional): per-cell xterm-256 hue index the encoder picked for
    // this frame (cols*rows). When non-null the serializer mirrors the cell-byte
    // leaf structure into the color plane (one hue per cell, see serialize()).
    // It does NOT affect the RD search: shape/luma drive mode selection; color is
    // additive and carried alongside whatever cells the search chose.
    const uint8_t *color;     // cols*rows xterm-256 index, or null
    int cols, rows;
    long lambda;              // /256 fixed point
    long block_stable;        // SKIP distortion credit
    int  shift_range;         // motion search radius (0 = SHIFT off, no moved bit)
    // EXPERIMENTAL split-coarsening levers (see blockcoder.hpp). 0 = no effect.
    long split_bias = 0;      // per-split surcharge added to the "split" option
    int  split_lookahead = 0; // # future frames that grant a stability credit
    const uint8_t *const *future_sub = nullptr;
    const uint8_t *const *future_ideal = nullptr;
    int  future_n = 0;
};

// Distortion of cell value `cell` shown at grid cell (xx,yy): compare the cell's
// (level,glyph) to the 2x4 sub-pixel luma block (mono_celldist.h). Every per-cell
// score in the RD search goes through here.
inline long cell_dist_at(const Ctx &c, uint8_t cell, int xx, int yy) {
    return tvid_mono_byte_distortion(
        cell, &c.sub[((size_t)(yy * c.cols + xx)) * TVID_MONO_SUBN]);
}

struct LeafChoice {
    int  mode;
    long rd;                  // bits*256 + lambda*distortion (lambda /256)
    // SOLID
    uint8_t solid;
    // PAL2
    uint8_t pal0, pal1;
    // SHIFT sub-variant of SKIP: shift=true means the leaf is a motion copy from
    // prev at (mvx,mvy); shift=false is a classic SKIP. Only meaningful when the
    // chosen mode is SKIP and the stream carries SHIFT.
    bool   shift;
    int8_t mvx, mvy;
};

// RD cost = lambda * bits + distortion. Distortion is luma-weighted squared
// error (can be thousands per cell); bits is the leaf's coded size. Higher
// lambda makes each bit more "expensive" -> the search prefers SKIP/SOLID and
// the file shrinks at the cost of detail. Low lambda buys detail (RAW/PAL2).
inline long rd_combine(const Ctx &c, long bits, long distortion) {
    return c.lambda * bits + distortion;
}

// Summed distortion of showing a single constant `cell` over the leaf's in-grid
// cells. The SKIP/SOLID/RAW evaluators all score one fixed cell value this way.
long leaf_dist(const Ctx &c, uint8_t cell, int x, int y, int s) {
    long dist = 0;
    for (int yy = y; yy < y + s; ++yy)
        for (int xx = x; xx < x + s; ++xx) {
            if (xx >= c.cols || yy >= c.rows) continue;
            dist += cell_dist_at(c, cell, xx, yy);
        }
    return dist;
}

// --- SKIP: keep prev cells. Distortion vs ideal target, minus hysteresis. Sets
//     the baseline `best` unconditionally (it is always a valid choice). ---
void eval_skip(const Ctx &c, int x, int y, int s, long skip_header, LeafChoice &best) {
    long dist = 0;
    for (int yy = y; yy < y + s; ++yy)
        for (int xx = x; xx < x + s; ++xx) {
            if (xx >= c.cols || yy >= c.rows) continue;
            dist += cell_dist_at(c, c.prev[yy * c.cols + xx], xx, yy);
        }
    dist -= c.block_stable; // temporal hysteresis: favor holding still
    if (dist < 0) dist = 0;
    best.rd = rd_combine(c, skip_header, dist);
    best.mode = TVID_MODE_SKIP;
    best.shift = false;
}

// --- SHIFT: copy the leaf from prev at a motion offset (dx,dy), sources clamped
//     to the grid (matching the decoder). A SKIP sub-variant: costs the skip
//     header + 2 vector components, no per-cell payload. Skipped when
//     shift_range==0. dx==dy==0 is just SKIP, so it is never searched. ---
void eval_shift(const Ctx &c, int x, int y, int s, long skip_header, LeafChoice &best) {
    if (c.shift_range <= 0) return;
    int range = c.shift_range;
    if (range > TVID_SHIFT_MAX) range = TVID_SHIFT_MAX;
    long bits = skip_header + 2 * TVID_SHIFT_BITS;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            if (dx == 0 && dy == 0) continue;
            long dist = 0;
            for (int yy = y; yy < y + s; ++yy)
                for (int xx = x; xx < x + s; ++xx) {
                    if (xx >= c.cols || yy >= c.rows) continue;
                    int sx = xx + dx; if (sx < 0) sx = 0; else if (sx >= c.cols) sx = c.cols - 1;
                    int sy = yy + dy; if (sy < 0) sy = 0; else if (sy >= c.rows) sy = c.rows - 1;
                    dist += cell_dist_at(c, c.prev[sy * c.cols + sx], xx, yy);
                }
            long rd = rd_combine(c, bits, dist);
            if (rd < best.rd) {
                best.rd = rd; best.mode = TVID_MODE_SKIP;
                best.shift = true; best.mvx = (int8_t)dx; best.mvy = (int8_t)dy;
            }
        }
}

// --- SOLID: one cell value over the leaf. Two candidates: the most common ideal
//     cell (good for flat regions) and the joint-quantized average RGB (good when
//     the leaf has no single dominant cell). Keep the better. ---
void eval_solid(const Ctx &c, int x, int y, int s, long header, LeafChoice &best) {
    std::array<int, 256> cnt{};
    int seen = 0;
    long ssub[TVID_MONO_SUBN] = {0};                     // average sub-pixel block
    for (int yy = y; yy < y + s; ++yy)
        for (int xx = x; xx < x + s; ++xx) {
            if (xx >= c.cols || yy >= c.rows) continue;
            cnt[c.ideal[yy * c.cols + xx]]++; seen++;
            const uint8_t *sp =
                &c.sub[((size_t)(yy * c.cols + xx)) * TVID_MONO_SUBN];
            for (int p = 0; p < TVID_MONO_SUBN; ++p) ssub[p] += sp[p];
        }
    if (!seen) return;
    int mode_cell = 0, bestc = -1;
    for (int v = 0; v < 256; ++v)
        if (cnt[v] > bestc) { bestc = cnt[v]; mode_cell = v; }
    uint8_t avgblk[TVID_MONO_SUBN];
    for (int p = 0; p < TVID_MONO_SUBN; ++p)
        avgblk[p] = (uint8_t)(ssub[p] / seen);
    int avg_cell = tvid_mono_quantize_joint(avgblk);
    for (int cand : {mode_cell, avg_cell}) {
        long dist = leaf_dist(c, (uint8_t)cand, x, y, s);
        long rd = rd_combine(c, header + 8, dist);
        if (rd < best.rd) {
            best.rd = rd; best.mode = TVID_MODE_SOLID;
            best.solid = (uint8_t)cand;
        }
    }
}

// --- PAL2: two cell values + 1 bit/cell. Seeds two centroids from the
//     lightest/darkest ideal cells by luma. Cheap two-tone coder. ---
void eval_pal2(const Ctx &c, int x, int y, int s, long header, LeafChoice &best) {
    int cells = s * s;
    // Seed the two centroids from the lightest/darkest ideal cells by luma:
    // the level value scaled by the glyph's mean ink (its effective brightness).
    int lo_cell = -1, hi_cell = -1, lo_l = 1 << 20, hi_l = -1;
    for (int yy = y; yy < y + s; ++yy)
        for (int xx = x; xx < x + s; ++xx) {
            if (xx >= c.cols || yy >= c.rows) continue;
            uint8_t v = c.ideal[yy * c.cols + xx];
            const uint8_t *ink = tvid_mono_glyph(TVID_CELL_MGLYPH(v))->ink;
            int sum = 0;
            for (int p = 0; p < TVID_MONO_SUBN; ++p) sum += ink[p];
            int l = tvid_mono_level_value(TVID_CELL_LUMA(v)) * sum
                    / (255 * TVID_MONO_SUBN);
            if (l < lo_l) { lo_l = l; lo_cell = v; }
            if (l > hi_l) { hi_l = l; hi_cell = v; }
        }
    if (lo_cell < 0 || hi_cell < 0 || lo_cell == hi_cell) return;
    uint8_t c0 = (uint8_t)lo_cell, c1 = (uint8_t)hi_cell;
    long dist = 0;
    for (int yy = y; yy < y + s; ++yy)
        for (int xx = x; xx < x + s; ++xx) {
            if (xx >= c.cols || yy >= c.rows) continue;
            long d0 = cell_dist_at(c, c0, xx, yy);
            long d1 = cell_dist_at(c, c1, xx, yy);
            dist += d0 < d1 ? d0 : d1;
        }
    long bits = header + 16 + cells; // 2 cell bytes + 1 bit/cell
    long rd = rd_combine(c, bits, dist);
    if (rd < best.rd) {
        best.rd = rd; best.mode = TVID_MODE_PAL2;
        best.pal0 = c0; best.pal1 = c1;
    }
}

// --- RAW: literal ideal cells. The detail fallback; distortion is the residual
//     quantization error of the ideal grid itself. ---
void eval_raw(const Ctx &c, int x, int y, int s, long header, LeafChoice &best) {
    int cells = s * s;
    long dist = 0;
    for (int yy = y; yy < y + s; ++yy)
        for (int xx = x; xx < x + s; ++xx) {
            if (xx >= c.cols || yy >= c.rows) continue;
            dist += cell_dist_at(c, c.ideal[yy * c.cols + xx], xx, yy);
        }
    long rd = rd_combine(c, header + 8 * cells, dist);
    if (rd < best.rd) { best.rd = rd; best.mode = TVID_MODE_RAW; }
}

// Evaluate all modes for a leaf [x,y) size s; return best. The leaf payload bit
// counts assume the leaf is full size*size cells (RAW/PAL2 always emit size*size
// symbols to keep the decoder in sync), but distortion only counts in-grid cells.
// SKIP runs first and sets the baseline; the rest relax `best` if cheaper.
LeafChoice eval_leaf(const Ctx &c, int x, int y, int s) {
    LeafChoice best;
    best.mode = TVID_MODE_SKIP;
    best.rd = 0;
    best.solid = best.pal0 = best.pal1 = 0;
    best.shift = false; best.mvx = best.mvy = 0;

    long header = TVID_MODE_BITS; // mode tag bits, common to every leaf
    // When SHIFT is enabled every SKIP leaf spends one "moved" bit (0=classic
    // SKIP, 1=motion copy). RAW/SOLID/PAL2 are unaffected.
    long skip_header = header + (c.shift_range > 0 ? 1 : 0);

    eval_skip(c, x, y, s, skip_header, best);
    eval_shift(c, x, y, s, skip_header, best);
    eval_solid(c, x, y, s, header, best);
    eval_pal2(c, x, y, s, header, best);
    eval_raw(c, x, y, s, header, best);
    return best;
}

// --- leaf serialization -----------------------------------------------------
// Routes a leaf's bytes to the right plane. Built once per serialize() call. In
// the interleaved layout everything goes inline into the structure bitwriter `w`;
// in split layouts cells/palette/color/mode each land in their own byte plane.
// This replaces the by-reference lambdas the recursive serializer used to capture.
struct PlaneRouter {
    bitwriter *w;
    std::vector<uint8_t> *cellplane;   // RAW cells (or null => inline in w)
    std::vector<uint8_t> *palplane;    // SOLID/PAL2 palette cells (CELLSPLIT)
    std::vector<uint8_t> *colorplane;  // per-cell hue (TVID_FLAG_COLOR)
    const uint8_t *color;              // source hue grid (cols*rows) or null
    int cols, rows;
    bool do_color;                     // colorplane && color both set

    // RAW cells go to the cell/raster plane (or inline in w when interleaved).
    void put_cell(uint8_t v) {
        if (cellplane) cellplane->push_back(v); else bw_byte(w, v);
    }
    // SOLID/PAL2 palette cells go to a dedicated palette plane when split that way
    // (TVID_FLAG_CELLSPLIT); otherwise they share the cell plane / w.
    void put_pal(uint8_t v) {
        if (palplane) palplane->push_back(v); else put_cell(v);
    }
    void put_color(uint8_t v) {
        if (do_color) colorplane->push_back(v);
    }
    uint8_t color_at(int xx, int yy) const {
        if (!do_color) return 0;
        return (xx < cols && yy < rows) ? color[yy * cols + xx] : 0;
    }
};

// Majority value over a 256-bin count table (ties resolve to the lowest index).
inline uint8_t majority256(const std::array<int, 256> &cc) {
    int best = 0, bestn = -1;
    for (int v = 0; v < 256; ++v)
        if (cc[v] > bestn) { bestn = cc[v]; best = v; }
    return (uint8_t)best;
}

void serialize_skip(const Ctx &c, PlaneRouter &pr, const LeafChoice &lf,
                    BlockStats *st) {
    if (st) st->n_skip++;
    if (c.shift_range > 0) {
        bw_bit(pr.w, lf.shift ? 1 : 0);
        if (st) st->shift_bits++;
        if (lf.shift) {
            bw_bits(pr.w, (uint32_t)(lf.mvx + TVID_SHIFT_BIAS), TVID_SHIFT_BITS);
            bw_bits(pr.w, (uint32_t)(lf.mvy + TVID_SHIFT_BIAS), TVID_SHIFT_BITS);
            if (st) st->shift_bits += 2 * TVID_SHIFT_BITS;
        }
    }
}

void serialize_solid(const Ctx &c, PlaneRouter &pr, const LeafChoice &lf,
                     int x, int y, int s, BlockStats *st) {
    pr.put_pal(lf.solid);
    // One representative hue for the whole leaf: the majority per-cell color.
    // Tracks the running max *during* counting (ties resolve to the first value
    // to reach that count) -- preserved exactly to keep the bytestream stable.
    if (pr.do_color) {
        std::array<int, 256> ccnt{};
        int best = 0, bestn = -1;
        for (int yy = y; yy < y + s; ++yy)
            for (int xx = x; xx < x + s; ++xx) {
                if (xx >= c.cols || yy >= c.rows) continue;
                int v = pr.color_at(xx, yy);
                if (++ccnt[v] > bestn) { bestn = ccnt[v]; best = v; }
            }
        pr.put_color((uint8_t)best);
    }
    if (st) { st->n_solid++; st->cell_bits += 8; }
}

void serialize_raw(const Ctx &c, PlaneRouter &pr, int x, int y, int s,
                   BlockStats *st) {
    if (st) { st->n_raw++; st->cell_bits += 8 * s * s; }
#ifdef TVID_PROBE
    // Buffer the leaf's cells in row-major so predictors see real in-leaf
    // neighbors before they are flattened into the plane.
    uint8_t blk[TVID_SB * TVID_SB];
#endif
    for (int j = 0; j < s; ++j)
        for (int i = 0; i < s; ++i) {
            int xx = x + i, yy = y + j;
            uint8_t v = (xx < c.cols && yy < c.rows)
                            ? c.ideal[yy * c.cols + xx] : 0;
            pr.put_cell(v);
            pr.put_color(pr.color_at(xx, yy)); // per-cell hue, same raster order
#ifdef TVID_PROBE
            blk[j * s + i] = v;
#endif
        }
#ifdef TVID_PROBE
    for (int j = 0; g_probe_raw_capture && j < s; ++j)
        for (int i = 0; i < s; ++i) {
            uint8_t v = blk[j * s + i];
            uint8_t left = i ? blk[j * s + (i - 1)] : 0;
            uint8_t up   = j ? blk[(j - 1) * s + i] : 0;
            g_probe_raw_plain.push_back(v);
            g_probe_raw_left.push_back((uint8_t)(v - left));
            g_probe_raw_up.push_back((uint8_t)(v - up));
        }
#endif
}

void serialize_pal2(const Ctx &c, PlaneRouter &pr, const LeafChoice &lf,
                    int x, int y, int s, BlockStats *st) {
    if (st) { st->n_pal2++; st->cell_bits += 16; st->sel_bits += s * s; }
    pr.put_pal(lf.pal0);
    pr.put_pal(lf.pal1);
    // Per-selector majority hue, accumulated as we write the selectors so the two
    // color bytes pair with pal0/pal1 (decoder applies the same selector to pick
    // the cell's hue). Written AFTER the selectors.
    std::array<int, 256> c0cnt{}, c1cnt{};
    for (int yy = y; yy < y + s; ++yy)
        for (int xx = x; xx < x + s; ++xx) {
            int sel = 0;
            if (xx < c.cols && yy < c.rows) {
                long d0 = cell_dist_at(c, lf.pal0, xx, yy);
                long d1 = cell_dist_at(c, lf.pal1, xx, yy);
                sel = d1 < d0 ? 1 : 0;
                if (pr.do_color) {
                    int hv = pr.color_at(xx, yy);
                    if (sel) c1cnt[hv]++; else c0cnt[hv]++;
                }
            }
            bw_bit(pr.w, sel);
        }
    if (pr.do_color) {
        pr.put_color(majority256(c0cnt));
        pr.put_color(majority256(c1cnt));
    }
}

// --- quadtree RD search -----------------------------------------------------
// Returns the RD cost of the best coding of node [x,y) size s, and records the
// decision (split vs which leaf mode) so a second pass can serialize it.

struct Node {
    bool split;
    LeafChoice leaf; // valid when !split
    int kids[4];     // indices into the node pool, when split
};

struct Tree {
    std::vector<Node> pool;
    long lambda = 0; // copied from Ctx so node_rd can price split bits
    long split_bias = 0; // copied from Ctx so node_rd matches build's split cost
    int build(const Ctx &c, int x, int y, int s) {
        lambda = c.lambda;
        split_bias = c.split_bias;
        int idx = (int)pool.size();
        pool.push_back(Node{});
        // Leaf option.
        LeafChoice leaf = eval_leaf(c, x, y, s);
        // EXPERIMENTAL lookahead: if this leaf SKIPs and would keep SKIPping for
        // the next N future frames, credit its cost so a temporally stable
        // partition prefers staying whole (its structure plane then repeats and
        // the range coder codes it near-free). No-op when split_lookahead == 0.
        if (c.split_lookahead > 0 && leaf.mode == TVID_MODE_SKIP && !leaf.shift) {
            long credit = future_skip_credit(c, x, y, s);
            leaf.rd -= credit;
            if (leaf.rd < 0) leaf.rd = 0;
        }
        long split_bit = (s > 1) ? c.lambda : 0; // cost of the split flag bit
        long leaf_rd = leaf.rd + split_bit;  // a size>1 leaf still spends 1 bit

        if (s == 1) { // forced leaf, no split possible
            pool[idx].split = false;
            pool[idx].leaf = leaf;
            return idx; // caller compares via re-eval; store rd in leaf.rd
        }

        // Split option: recurse into 4 quadrants.
        int half = s >> 1;
        int k0 = build(c, x,        y,        half);
        int k1 = build(c, x + half, y,        half);
        int k2 = build(c, x,        y + half, half);
        int k3 = build(c, x + half, y + half, half);
        long split_rd = c.lambda + c.split_bias // split flag bit + coarsening bias
            + node_rd(k0) + node_rd(k1) + node_rd(k2) + node_rd(k3);

        if (split_rd < leaf_rd) {
            pool[idx].split = true;
            pool[idx].kids[0] = k0; pool[idx].kids[1] = k1;
            pool[idx].kids[2] = k2; pool[idx].kids[3] = k3;
        } else {
            pool[idx].split = false;
            pool[idx].leaf = leaf;
            // fold the split bit into the leaf's stored rd for the parent
            pool[idx].leaf.rd += split_bit;
        }
        return idx;
    }
    long node_rd(int idx) const {
        const Node &n = pool[idx];
        if (n.split)
            return lambda + split_bias + node_rd(n.kids[0]) + node_rd(n.kids[1])
                          + node_rd(n.kids[2]) + node_rd(n.kids[3]);
        return n.leaf.rd; // already includes its own split bit if size>1
    }
    // EXPERIMENTAL lookahead credit (Lever B): distortion the leaf's *current*
    // cells (c.prev) would accrue against each of the next N future frames if held
    // in place. A future frame counts as "still SKIP" while its summed distortion
    // stays under block_stable (the same threshold the SKIP credit uses); each such
    // stable frame contributes that frame's slack (block_stable - dist) as a credit.
    // A leaf that diverges in the future earns nothing past that point.
    long future_skip_credit(const Ctx &c, int x, int y, int s) const {
        long total = 0;
        int n = c.split_lookahead < c.future_n ? c.split_lookahead : c.future_n;
        for (int k = 0; k < n; ++k) {
            const uint8_t *fsub = c.future_sub ? c.future_sub[k] : nullptr;
            if (!fsub) break;
            long dist = 0;
            for (int yy = y; yy < y + s; ++yy)
                for (int xx = x; xx < x + s; ++xx) {
                    if (xx >= c.cols || yy >= c.rows) continue;
                    dist += tvid_mono_byte_distortion(
                        c.prev[yy * c.cols + xx],
                        &fsub[((size_t)(yy * c.cols + xx)) * TVID_MONO_SUBN]);
                }
            if (dist >= c.block_stable) break; // diverges -> no further credit
            total += c.block_stable - dist;
        }
        return total;
    }
    // Serialize the tree. `cellplane` non-null => split mode: cell bytes go there
    // (byte-aligned, contiguous across frames) instead of into the structure
    // bitwriter w. `modeplane` non-null (measurement only) => mode tags go there
    // (one byte per leaf) instead of into w, to gauge a 3rd-plane split's payoff.
    void serialize(const Ctx &c, bitwriter *w, std::vector<uint8_t> *cellplane,
                   std::vector<uint8_t> *modeplane, std::vector<uint8_t> *palplane,
                   std::vector<uint8_t> *colorplane,
                   int idx, int x, int y, int s, BlockStats *st) const {
        // Color plane (TVID_FLAG_COLOR): mirrors the cell-byte leaf structure, so
        // the decoder advances one color cursor in lockstep with the cell cursor.
        // Active only when BOTH colorplane and c.color are set (the canonical pass);
        // the structure-only / raster-only passes pass neither, so it is a no-op
        // and never dereferences a null c.color.
        PlaneRouter pr{w, cellplane, palplane, colorplane, c.color,
                       c.cols, c.rows,
                       (colorplane != nullptr) && (c.color != nullptr)};
        serialize_node(c, pr, modeplane, idx, x, y, s, st);
    }

  private:
    // Recursive walk: emit the split bit, recurse, or dispatch a leaf by mode.
    void serialize_node(const Ctx &c, PlaneRouter &pr,
                        std::vector<uint8_t> *modeplane,
                        int idx, int x, int y, int s, BlockStats *st) const {
        const Node &n = pool[idx];
#if defined(TVID_VIZ)
        // Bit offset at this node's boundary, before its split flag, so a leaf's
        // captured structure bits include its own terminal split=0 flag.
        const long viz_node_bit0 = g_viz_capture ? (pr.w->byte * 8L + pr.w->bit) : 0;
#endif
        if (s > 1) { bw_bit(pr.w, n.split ? 1 : 0); if (st) st->split_bits++; }
        if (n.split) {
            int half = s >> 1;
            serialize_node(c, pr, modeplane, n.kids[0], x,        y,        half, st);
            serialize_node(c, pr, modeplane, n.kids[1], x + half, y,        half, st);
            serialize_node(c, pr, modeplane, n.kids[2], x,        y + half, half, st);
            serialize_node(c, pr, modeplane, n.kids[3], x + half, y + half, half, st);
            return;
        }
        const LeafChoice &lf = n.leaf;
#if defined(TVID_VIZ)
        // Snapshot the plane cursors *before* this leaf so the exact bytes/bits it
        // writes can be sliced out afterward (the leaf's wire representation). The
        // structure-bit start is the node boundary (includes the split=0 flag).
        const long viz_bit0 = viz_node_bit0;
        const size_t viz_cell0 = (g_viz_capture && pr.cellplane)
            ? pr.cellplane->size() : 0;
        const size_t viz_pal0 = (g_viz_capture && pr.palplane)
            ? pr.palplane->size() : 0;
        const size_t viz_color0 = (g_viz_capture && pr.colorplane)
            ? pr.colorplane->size() : 0;
#endif
        if (modeplane) modeplane->push_back((uint8_t)lf.mode);
        else bw_bits(pr.w, (uint32_t)lf.mode, TVID_MODE_BITS);
        if (st) st->mode_bits += TVID_MODE_BITS;
        switch (lf.mode) {
        case TVID_MODE_SKIP:  serialize_skip(c, pr, lf, st); break;
        case TVID_MODE_SOLID: serialize_solid(c, pr, lf, x, y, s, st); break;
        case TVID_MODE_RAW:   serialize_raw(c, pr, x, y, s, st); break;
        case TVID_MODE_PAL2:  serialize_pal2(c, pr, lf, x, y, s, st); break;
        }
#if defined(TVID_VIZ)
        if (g_viz_capture) {
            VizLeaf vl;
            vl.x = x; vl.y = y; vl.s = s;
            vl.mode = (uint8_t)lf.mode; vl.rd = lf.rd;
            vl.shift = lf.shift; vl.mvx = lf.mvx; vl.mvy = lf.mvy;
            vl.solid = lf.solid; vl.pal0 = lf.pal0; vl.pal1 = lf.pal1;
            // Structure bits this leaf wrote (mode tag + any shift/selector bits),
            // read back from the bitwriter buffer MSB-first.
            vl.struct_bit0 = viz_bit0;
            const long bit1 = pr.w->byte * 8L + pr.w->bit;
            for (long b = viz_bit0; b < bit1; ++b) {
                int byte = (int)(b >> 3), off = (int)(b & 7);
                int set = (pr.w->buf[byte] >> (7 - off)) & 1;
                vl.struct_bits.push_back(set ? '1' : '0');
            }
            // Cell-plane bytes (RAW raster, or SOLID/PAL2 palette when not split
            // into palplane) plus any palette plane bytes, in write order.
            if (pr.cellplane)
                vl.cell_bytes.assign(pr.cellplane->begin() + viz_cell0,
                                     pr.cellplane->end());
            if (pr.palplane)
                vl.cell_bytes.insert(vl.cell_bytes.end(),
                                     pr.palplane->begin() + viz_pal0,
                                     pr.palplane->end());
            if (pr.colorplane)
                vl.color_bytes.assign(pr.colorplane->begin() + viz_color0,
                                      pr.colorplane->end());
            g_viz_capture->push_back(vl);
        }
#endif
    }
};

} // namespace

std::vector<uint8_t> blockcoder_encode(const uint8_t *prev, const uint8_t *ideal,
                                       const uint8_t *sub,
                                       const BlockCoderParams &p,
                                       BlockStats *stats) {
    Ctx c;
    c.prev = prev; c.ideal = ideal;
    c.sub = sub ? sub : p.sub; c.color = nullptr;
    c.cols = p.cols; c.rows = p.rows;
    c.lambda = p.lambda; c.block_stable = p.block_stable;
    c.shift_range = p.shift_range;
    c.split_bias = p.split_bias; c.split_lookahead = p.split_lookahead;
    c.future_sub = p.future_sub; c.future_ideal = p.future_ideal;
    c.future_n = p.future_n;

    // Build a tree per superblock; concatenate into one bitstream.
    std::vector<uint8_t> out((size_t)p.cols * p.rows * 2 + 64);
    bitwriter w;
    bw_init(&w, out.data(), (long)out.size());

    for (int sy = 0; sy < p.rows; sy += TVID_SB)
        for (int sx = 0; sx < p.cols; sx += TVID_SB) {
            Tree t;
            t.pool.reserve(TVID_SB * TVID_SB);
            int root = t.build(c, sx, sy, TVID_SB);
            t.serialize(c, &w, nullptr, nullptr, nullptr, nullptr, root, sx, sy, TVID_SB, stats);
        }

    out.resize((size_t)bw_len(&w));
    return out;
}

std::vector<uint8_t> blockcoder_encode_split(const uint8_t *prev,
                                             const uint8_t *ideal,
                                             const uint8_t *sub,
                                             const BlockCoderParams &p,
                                             std::vector<uint8_t> &cellplane,
                                             BlockStats *stats,
                                             std::vector<uint8_t> *modeplane,
                                             std::vector<uint8_t> *palplane,
                                             const uint8_t *color,
                                             std::vector<uint8_t> *colorplane) {
    Ctx c;
    c.prev = prev; c.ideal = ideal;
    c.sub = sub ? sub : p.sub; c.color = color;
    c.cols = p.cols; c.rows = p.rows;
    c.lambda = p.lambda; c.block_stable = p.block_stable;
    c.shift_range = p.shift_range;
    c.split_bias = p.split_bias; c.split_lookahead = p.split_lookahead;
    c.future_sub = p.future_sub; c.future_ideal = p.future_ideal;
    c.future_n = p.future_n;

    // Structure bits -> returned byte-aligned buffer; cell bytes -> appended to
    // the shared cellplane (contiguous across all frames). Same RD tree as the
    // interleaved path; only the serialization target for cell bytes differs.
    // modeplane (optional, measurement) pulls mode tags into a 3rd byte plane.
    // colorplane (optional, TVID_FLAG_COLOR) receives one hue per cell mirroring
    // the cell-byte leaf structure; `color` is this frame's per-cell hue grid.
    std::vector<uint8_t> out((size_t)p.cols * p.rows * 2 + 64);
    bitwriter w;
    bw_init(&w, out.data(), (long)out.size());

    for (int sy = 0; sy < p.rows; sy += TVID_SB)
        for (int sx = 0; sx < p.cols; sx += TVID_SB) {
            Tree t;
            t.pool.reserve(TVID_SB * TVID_SB);
            int root = t.build(c, sx, sy, TVID_SB);
            t.serialize(c, &w, &cellplane, modeplane, palplane, colorplane, root, sx, sy, TVID_SB, stats);
        }

    out.resize((size_t)bw_len(&w));
    return out;
}
