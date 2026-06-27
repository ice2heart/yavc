/* mono_celldist.h - the encoder's monochrome cell distortion metric (header-only).
 *
 * The color path scores a cell as one palette color drawn with one ramp glyph
 * (celldist.h). The monochrome path (TVID_VERSION_MONO) instead scores a cell as
 * a grayscale luma level modulated by a glyph's 2x4 sub-cell ink mask, against
 * the cell's actual 2x4 sub-pixel luma block:
 *
 *   predicted(p) = luma_value[level] * ink_mask[glyph][p] / 255
 *   distortion   = sum_p (predicted(p) - source_subpixel_luma(p))^2
 *
 * This is a true sub-pixel SSE - the dimension a single averaged-luma cell can't
 * use. It is the single source of truth for mono cell ranking, so the quantizer
 * (tvid_mono_quantize_joint) and the block coder's RD search never disagree about
 * what "best" means - exactly mirroring celldist.h's role for color.
 *
 * The luma level (high TVID_MONO_LUMA_BITS of the cell byte) is rendered by the
 * decoder as a grayscale foreground; the values here MUST match the decoder LUT
 * (tvid_mono_level_value), so both live in this header and the decoder includes it.
 */
#ifndef MONO_CELLDIST_H
#define MONO_CELLDIST_H

#include "glyphset.h"
#include "tvid_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The level->gray LUT (tvid_mono_level_value) lives in glyphset.h so the decoder
 * shares it without pulling in the encoder-only distortion/quantizer below. */

/* Squared distortion of a mono cell (level,glyph) vs a 2x4 sub-pixel luma block
 * `sub` (TVID_MONO_SUBN bytes, row-major, matching the glyph ink layout). */
static inline long tvid_mono_cell_distortion(int level, int glyph,
                                             const uint8_t *sub) {
    int lv = tvid_mono_level_value(level);
    const uint8_t *ink = tvid_mono_glyph(glyph)->ink;
    long d = 0;
    for (int p = 0; p < TVID_MONO_SUBN; ++p) {
        int predicted = lv * ink[p] / 255;
        long e = (long)predicted - (long)sub[p];
        d += e * e;
    }
    return d;
}

/* Distortion of a packed mono cell byte vs a sub-pixel block (the form the block
 * coder needs: it holds opaque cell bytes). */
static inline long tvid_mono_byte_distortion(uint8_t cell, const uint8_t *sub) {
    return tvid_mono_cell_distortion(TVID_CELL_LUMA(cell), TVID_CELL_MGLYPH(cell),
                                     sub);
}

/* Joint level+glyph quantizer: brute-force all levels x glyphs and keep the
 * packed cell byte with the lowest distortion to the sub-pixel block. Same cost
 * shape as the color joint pick (e.g. 4x64 = 256 candidates for the 2+6 split). */
static inline uint8_t tvid_mono_quantize_joint(const uint8_t *sub) {
    int best_lvl = 0, best_glyph = 0;
    long best_d = 0x7fffffffL;
    for (int lvl = 0; lvl < TVID_MONO_LUMA_LEVELS; ++lvl)
        for (int g = 0; g < TVID_MONO_GLYPH_COUNT; ++g) {
            long d = tvid_mono_cell_distortion(lvl, g, sub);
            if (d < best_d) { best_d = d; best_lvl = lvl; best_glyph = g; }
        }
    return TVID_MONO_CELL(best_lvl, best_glyph);
}

#ifdef __cplusplus
}
#endif

#endif /* MONO_CELLDIST_H */
