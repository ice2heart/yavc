/* codec.c - v2 quadtree block-frame decoder (portable C).
 *
 * Bitstream layout (MSB-first, see bitstream.h), per frame:
 *   for each superblock (TVID_SB x TVID_SB cells, raster order):
 *     a quadtree:
 *       node(x,y,size):
 *         if size > 1: read 1 split bit
 *           split=1 -> recurse into 4 quadrants of size/2 (raster: TL,TR,BL,BR)
 *           split=0 -> this node is a leaf
 *         if size == 1: always a leaf (no split bit)
 *         leaf: read TVID_MODE_BITS mode bits, then the mode payload:
 *           SKIP  : nothing (cells keep previous-frame value), unless the stream
 *                   carries SHIFT (caps & TVID_CAP_SHIFT): then a "moved" bit, and
 *                   if set, dx,dy (TVID_SHIFT_BITS each, biased) -> copy the leaf
 *                   from the previous frame at (x+dx,y+dy), sources clamped to grid
 *           SOLID : 1 cell byte, fills the leaf
 *           RAW   : w*h cell bytes, in raster order within the leaf
 *           PAL2  : 2 cell bytes (c0,c1) then w*h selector bits (0->c0,1->c1)
 *
 * Superblocks that extend past the grid edge are clipped: a node fully outside
 * the grid is skipped entirely (no bits), partial nodes write only in-grid cells.
 * The 80x24 default grid is covered exactly by 8x8 superblocks, so clipping only
 * matters for non-default sizes - but it keeps the decoder robust either way. */
#include "codec.h"
#include "bitstream.h"
#include "tvid_format.h"
#include <string.h>

/* Side-plane sources for the split stream (TVID_FLAG_SPLIT). In the classic
 * interleaved stream cell bytes and mode tags are read inline from the structure
 * bitreader; with split, cell bytes come from a separate byte-aligned plane, and
 * (with TVID_FLAG_MODEPLANE) the per-leaf mode tags come from a third byte plane.
 * A NULL buf selects inline reads. Centralizing keeps decode_node uniform. */
typedef struct {
    const uint8_t *cellbuf; /* split: cell/raster plane base; NULL = inline */
    long cellpos, celllen;  /* split: cursor into cellbuf */
    const uint8_t *modebuf; /* mode-plane: mode-tag plane base; NULL = inline */
    long modepos, modelen;  /* mode-plane: cursor into modebuf */
    const uint8_t *palbuf;  /* cell-split: palette plane (SOLID/PAL2); NULL = use cellbuf */
    long palpos, pallen;    /* cell-split: cursor into palbuf */
    /* Color plane (TVID_FLAG_COLOR): one xterm-256 hue index per cell, mirroring
     * the cell-byte leaf structure. colorbuf is the source plane; colfb the
     * per-cell hue framebuffer we fill in lockstep with fb (SKIP keeps prev color,
     * so colfb persists across frames). NULL colorbuf disables all color writes. */
    const uint8_t *colorbuf;
    long colorpos, colorlen;
    uint8_t *colfb;
} cellsrc;

/* Next hue byte from the color plane (defensive on overrun). */
static inline uint8_t next_color(cellsrc *cs) {
    if (cs->colorpos >= cs->colorlen) return 0;
    return cs->colorbuf[cs->colorpos++];
}

/* RAW-leaf cell bytes (and the keyframe) read from the raster plane (or inline). */
static inline uint8_t next_cell(bitreader *r, cellsrc *cs) {
    if (cs->cellbuf) {
        if (cs->cellpos >= cs->celllen) return 0; /* defensive, matches br_byte */
        return cs->cellbuf[cs->cellpos++];
    }
    return br_byte(r);
}

/* SOLID/PAL2 palette bytes read from the dedicated palette plane when present
 * (TVID_FLAG_CELLSPLIT); otherwise they share the cell/raster plane (or inline). */
static inline uint8_t next_pal(bitreader *r, cellsrc *cs) {
    if (cs->palbuf) {
        if (cs->palpos >= cs->pallen) return 0; /* defensive */
        return cs->palbuf[cs->palpos++];
    }
    return next_cell(r, cs);
}

static inline int next_mode(bitreader *r, cellsrc *cs) {
    if (cs->modebuf) {
        if (cs->modepos >= cs->modelen) return 0; /* defensive */
        return cs->modebuf[cs->modepos++];
    }
    return (int)br_bits(r, TVID_MODE_BITS);
}

/* Decode one quadtree node covering cells [x,x+size) x [y,y+size). */
static void decode_node(bitreader *r, cellsrc *cs, uint8_t *fb, const uint8_t *prev,
                        int cols, int rows, int x, int y, int size, int caps) {
    /* Fully outside the grid: the encoder emits no bits for it. */
    if (x >= cols || y >= rows) return;

    if (size > 1) {
        int half = size >> 1;
        /* A node is only split if it could meaningfully subdivide in-grid. The
         * encoder applies the same rule, so the bit is present iff we read it. */
        if (br_bit(r)) {
            decode_node(r, cs, fb, prev, cols, rows, x,        y,        half, caps);
            decode_node(r, cs, fb, prev, cols, rows, x + half, y,        half, caps);
            decode_node(r, cs, fb, prev, cols, rows, x,        y + half, half, caps);
            decode_node(r, cs, fb, prev, cols, rows, x + half, y + half, half, caps);
            return;
        }
    }

    /* Leaf. */
    int mode = next_mode(r, cs);
    int x1 = x + size; if (x1 > cols) x1 = cols;
    int y1 = y + size; if (y1 > rows) y1 = rows;

    switch (mode) {
    case TVID_MODE_SKIP:
        /* Classic SKIP keeps the previous frame; with SHIFT, a set "moved" bit
         * means copy the leaf from prev[] at a clamped motion offset instead. */
        if (caps & TVID_CAP_SHIFT) {
            if (br_bit(r)) {
                int dx = (int)br_bits(r, TVID_SHIFT_BITS) - TVID_SHIFT_BIAS;
                int dy = (int)br_bits(r, TVID_SHIFT_BITS) - TVID_SHIFT_BIAS;
                for (int cy = y; cy < y1; ++cy)
                    for (int cx = x; cx < x1; ++cx) {
                        int sx = cx + dx; if (sx < 0) sx = 0; else if (sx >= cols) sx = cols - 1;
                        int sy = cy + dy; if (sy < 0) sy = 0; else if (sy >= rows) sy = rows - 1;
                        fb[cy * cols + cx] = prev[sy * cols + sx];
                    }
            }
        }
        break; /* keep previous frame contents */
    case TVID_MODE_SOLID: {
        uint8_t v = next_pal(r, cs);
        uint8_t cv = cs->colorbuf ? next_color(cs) : 0; /* one hue for the leaf */
        for (int cy = y; cy < y1; ++cy)
            for (int cx = x; cx < x1; ++cx) {
                fb[cy * cols + cx] = v;
                if (cs->colfb) cs->colfb[cy * cols + cx] = cv;
            }
        break;
    }
    case TVID_MODE_RAW:
        for (int cy = y; cy < y + size; ++cy)
            for (int cx = x; cx < x + size; ++cx) {
                uint8_t v = next_cell(r, cs); /* read every cell to stay in sync */
                uint8_t cv = cs->colorbuf ? next_color(cs) : 0;
                if (cy < rows && cx < cols) {
                    fb[cy * cols + cx] = v;
                    if (cs->colfb) cs->colfb[cy * cols + cx] = cv;
                }
            }
        break;
    case TVID_MODE_PAL2: {
        uint8_t c0 = next_pal(r, cs), c1 = next_pal(r, cs);
        /* Two hues paired with c0/c1; selected by the same per-cell selector. Read
         * after the cell-byte pair, before the selectors (matching the encoder). */
        uint8_t h0 = 0, h1 = 0;
        if (cs->colorbuf) { h0 = next_color(cs); h1 = next_color(cs); }
        for (int cy = y; cy < y + size; ++cy)
            for (int cx = x; cx < x + size; ++cx) {
                int sel = br_bit(r); /* read every selector to stay in sync */
                if (cy < rows && cx < cols) {
                    fb[cy * cols + cx] = sel ? c1 : c0;
                    if (cs->colfb) cs->colfb[cy * cols + cx] = sel ? h1 : h0;
                }
            }
        break;
    }
    default:
        break;
    }
}

int codec_decode_block_ref(const uint8_t *in, long len, uint8_t *fb,
                           const uint8_t *prev, int cols, int rows, int caps) {
    bitreader r;
    cellsrc cs;
    memset(&cs, 0, sizeof(cs)); /* interleaved: cells/modes inline, no color plane */
    br_init(&r, in, len);
    for (int sy = 0; sy < rows; sy += TVID_SB)
        for (int sx = 0; sx < cols; sx += TVID_SB)
            decode_node(&r, &cs, fb, prev, cols, rows, sx, sy, TVID_SB, caps);
    return r.byte > r.len ? -1 : 0;
}

int codec_decode_block(const uint8_t *in, long len, uint8_t *fb,
                       int cols, int rows) {
    /* Legacy in-place path: no SHIFT, so prev==fb is never dereferenced. */
    return codec_decode_block_ref(in, len, fb, fb, cols, rows, 0);
}

int codec_decode_block_split(const uint8_t *sbits, long sbits_len,
                             const uint8_t *cells, long cells_len, long *cells_pos,
                             const uint8_t *modes, long modes_len, long *modes_pos,
                             const uint8_t *pals, long pals_len, long *pals_pos,
                             const uint8_t *color, long color_len, long *color_pos,
                             uint8_t *fb, uint8_t *colfb, const uint8_t *prev,
                             int cols, int rows, int caps) {
    /* Split layout (TVID_FLAG_SPLIT): structure bits in sbits (self-delimiting
     * via the quadtree walk), cell bytes in a separate plane. With
     * TVID_FLAG_MODEPLANE, per-leaf mode tags come from a third plane `modes`
     * (pass modes==NULL for inline). With TVID_FLAG_CELLSPLIT, SOLID/PAL2 cells
     * come from the palette plane `pals` and RAW/keyframe from `cells` (pass
     * pals==NULL to share `cells`). With TVID_FLAG_COLOR, a per-cell xterm-256 hue
     * comes from `color` and is written into `colfb` mirroring the cell-byte leaf
     * structure (pass color==NULL/colfb==NULL to disable). All cursors advance
     * across frames, so the caller threads them. Returns 0 on success, -1 on
     * malformed structure. */
    bitreader r;
    cellsrc cs;
    memset(&cs, 0, sizeof(cs));
    cs.cellbuf = cells; cs.cellpos = *cells_pos; cs.celllen = cells_len;
    cs.modebuf = modes; cs.modepos = modes ? *modes_pos : 0; cs.modelen = modes_len;
    cs.palbuf = pals; cs.palpos = pals ? *pals_pos : 0; cs.pallen = pals_len;
    cs.colorbuf = color; cs.colorpos = color ? *color_pos : 0; cs.colorlen = color_len;
    cs.colfb = colfb;
    br_init(&r, sbits, sbits_len);
    for (int sy = 0; sy < rows; sy += TVID_SB)
        for (int sx = 0; sx < cols; sx += TVID_SB)
            decode_node(&r, &cs, fb, prev, cols, rows, sx, sy, TVID_SB, caps);
    *cells_pos = cs.cellpos;
    if (modes) *modes_pos = cs.modepos;
    if (pals) *pals_pos = cs.palpos;
    if (color) *color_pos = cs.colorpos;
    if (r.byte > r.len) return -1;
    return 0;
}
