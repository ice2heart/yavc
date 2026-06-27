/* xterm256.h - the fixed xterm 256-color palette + helpers (portable C, shared
 * encoder/decoder). Used by the v3 color layer (TVID_FLAG_COLOR).
 *
 * The color layer stores ONE byte per cell: an index 0..255 into the standard
 * xterm 256-color palette (the same palette every 256-color terminal renders):
 *   0..15   the 16 ANSI system colors
 *   16..231 a 6x6x6 RGB cube (level steps 0,95,135,175,215,255)
 *   232..255 a 24-step grayscale ramp (8,18,...,238)
 * Because the palette is fixed and well-known there is nothing to transmit: the
 * ANSI backend emits the index directly (ESC[38;5;Nm) and the DOS backend loads
 * the identical RGBs into the VGA DAC, so both targets render the same colors.
 *
 * Brightness still comes from the mono cell's luma level (see glyphset.h): the
 * color byte is the cell's HUE; tvid_xterm256_rgb_dim() scales the palette RGB by
 * the luma fraction at render time, so a dim cell of hue H and a bright cell of
 * the same hue share one color byte but render at different brightness. This
 * keeps the color plane a pure, temporally-stable hue index (good for the entropy
 * coder) and reuses the luma resolution the mono path already carries.
 */
#ifndef XTERM256_H
#define XTERM256_H

#include <stdint.h>
#include "glyphset.h" /* tvid_mono_level_value: luma level -> 0..255 */

#ifdef __cplusplus
extern "C" {
#endif

/* The 6x6x6 cube level steps and the 24-step gray ramp, per the xterm spec. */
static inline int tvid_xterm256_cube_step(int i) {
    return i == 0 ? 0 : 55 + i * 40; /* 0,95,135,175,215,255 */
}

/* The 16 ANSI system colors (indices 0..15), standard VGA/xterm values. Shared
 * with the DOS DAC load so slot N renders identically on both backends. */
static const uint8_t TVID_XTERM16[16][3] = {
    {  0,  0,  0}, {128,  0,  0}, {  0,128,  0}, {128,128,  0},
    {  0,  0,128}, {128,  0,128}, {  0,128,128}, {192,192,192},
    {128,128,128}, {255,  0,  0}, {  0,255,  0}, {255,255,  0},
    {  0,  0,255}, {255,  0,255}, {  0,255,255}, {255,255,255},
};

/* The RGB of xterm-256 palette index `idx` (0..255). */
static inline void tvid_xterm256_rgb(int idx, int *r, int *g, int *b) {
    if (idx < 16) {
        *r = TVID_XTERM16[idx][0];
        *g = TVID_XTERM16[idx][1];
        *b = TVID_XTERM16[idx][2];
    } else if (idx < 232) {
        int c = idx - 16;
        *r = tvid_xterm256_cube_step((c / 36) % 6);
        *g = tvid_xterm256_cube_step((c / 6) % 6);
        *b = tvid_xterm256_cube_step(c % 6);
    } else {
        int v = 8 + (idx - 232) * 10; /* 8..238 */
        *r = *g = *b = v;
    }
}

/* The cell's shown RGB: the palette hue of `color_idx` scaled to the brightness
 * of luma level `luma` (0..TVID_MONO_LUMA_LEVELS-1). Both backends and the
 * encoder MUST agree on this, so it lives here. luma 0 renders black (matches the
 * grayscale-mono model where a level-0 cell is dark). */
static inline void tvid_xterm256_rgb_dim(int color_idx, int luma,
                                         int *r, int *g, int *b) {
    int pr, pg, pb, lv;
    tvid_xterm256_rgb(color_idx, &pr, &pg, &pb);
    lv = tvid_mono_level_value(luma); /* 0..255 brightness for this level */
    *r = pr * lv / 255;
    *g = pg * lv / 255;
    *b = pb * lv / 255;
}

/* Nearest xterm-256 index to an RGB triple, by squared error over the full 256
 * palette. Encoder-only in practice (the decoder works from stored indices), but
 * header-only so the probe build can use it too. */
static inline uint8_t tvid_xterm256_nearest(int r, int g, int b) {
    int best = 0; long best_d = 0x7fffffffL;
    for (int i = 0; i < 256; ++i) {
        int pr, pg, pb;
        tvid_xterm256_rgb(i, &pr, &pg, &pb);
        long dr = pr - r, dg = pg - g, db = pb - b;
        long d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

#ifdef __cplusplus
}
#endif

#endif /* XTERM256_H */
