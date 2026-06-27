/* glyphset.h - monochrome sub-cell glyph table (portable C, shared enc/dec).
 *
 * In monochrome mode (TVID_VERSION_MONO) a cell's low TVID_MONO_GLYPH_BITS bits
 * index a BRAILLE dot pattern. A Unicode Braille glyph (U+2800..U+28FF) is a
 * 2-wide x 4-tall grid of 8 dots - exactly our 2x4 sub-cell layout - so every
 * pattern is an EXACT sub-cell ink mask, not an approximation. The cell's gray
 * level (high TVID_MONO_LUMA_BITS) scales the lit dots' brightness.
 *
 * The glyph index does not address Braille bytes directly: an indirection table
 * TVID_MONO_PATTERN[] maps a glyph index -> an 8-bit Braille dot pattern, so a
 * narrow (e.g. 6-bit/64-entry) glyph field can carry the *best* 64 of the 256
 * patterns rather than an arbitrary contiguous range. The decoder and encoder
 * share this table. Everything else (UTF-8 string, CP437 byte, 2x4 ink mask) is
 * DERIVED from the pattern byte - nothing is hand-written.
 *
 * Braille dot numbering (Unicode): bit i set => dot lit. Dot-to-cell layout:
 *     bit0 -> (row0,col0)   bit3 -> (row0,col1)
 *     bit1 -> (row1,col0)   bit4 -> (row1,col1)
 *     bit2 -> (row2,col0)   bit5 -> (row2,col1)
 *     bit6 -> (row3,col0)   bit7 -> (row3,col1)
 * which we flatten to ink[row*2 + col].
 */
#ifndef GLYPHSET_H
#define GLYPHSET_H

#include <stdint.h>
#include "tvid_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TVID_MONO_SUBW 2 /* sub-pixels per cell, horizontally */
#define TVID_MONO_SUBH 4 /* sub-pixels per cell, vertically   */
#define TVID_MONO_SUBN (TVID_MONO_SUBW * TVID_MONO_SUBH) /* = 8 */

/* Brightness shown for luma level `lvl` (0..TVID_MONO_LUMA_LEVELS-1) as an 8-bit
 * gray value, evenly spread over 0..255. The decoder paints this as the cell's
 * grayscale foreground; the encoder predicts sub-pixels with it (mono_celldist.h).
 * Both sides MUST agree, so it lives here. With LUMA_BITS==0 there is one level ==
 * full brightness (the dot pattern carries all the brightness via dot count). */
static inline int tvid_mono_level_value(int lvl) {
    if (TVID_MONO_LUMA_LEVELS <= 1) return 255;
    return lvl * 255 / (TVID_MONO_LUMA_LEVELS - 1);
}

/* Map a glyph index (0..TVID_MONO_GLYPH_COUNT-1) to a Braille dot-pattern byte.
 *
 * For the 0+8 split (256 glyphs) this is the identity: index == pattern, so the
 * full Braille alphabet is available (the quality ceiling). For narrower glyph
 * fields the first TVID_MONO_GLYPH_COUNT entries are the *selected best* patterns
 * (filled by the selection below); index 0 is always the empty pattern so a blank
 * cell stays blank. Selection is by training-clip distortion (see encoder probe);
 * until selected we use a principled default ramp (see tvid_mono_pattern_init). */
static const uint8_t *tvid_mono_pattern_table(void);

static inline uint8_t tvid_mono_pattern(int glyph_idx) {
    return tvid_mono_pattern_table()[glyph_idx & (TVID_MONO_GLYPH_COUNT - 1)];
}

/* The 2x4 binary ink mask of a Braille pattern byte: lit dot -> 255, else 0.
 * ink[row*2 + col], row 0 on top. */
static inline void tvid_mono_pattern_ink(uint8_t pat, uint8_t ink[TVID_MONO_SUBN]) {
    /* dot bit -> (row,col), per the layout in the file header. */
    ink[0 * 2 + 0] = (pat & 0x01) ? 255 : 0; /* bit0 r0c0 */
    ink[1 * 2 + 0] = (pat & 0x02) ? 255 : 0; /* bit1 r1c0 */
    ink[2 * 2 + 0] = (pat & 0x04) ? 255 : 0; /* bit2 r2c0 */
    ink[0 * 2 + 1] = (pat & 0x08) ? 255 : 0; /* bit3 r0c1 */
    ink[1 * 2 + 1] = (pat & 0x10) ? 255 : 0; /* bit4 r1c1 */
    ink[2 * 2 + 1] = (pat & 0x20) ? 255 : 0; /* bit5 r2c1 */
    ink[3 * 2 + 0] = (pat & 0x40) ? 255 : 0; /* bit6 r3c0 */
    ink[3 * 2 + 1] = (pat & 0x80) ? 255 : 0; /* bit7 r3c1 */
}

/* The glyph table entry the backends need, derived from a pattern byte. The
 * struct is filled on demand into a small static cache so callers keep the old
 * `tvid_mono_glyph(idx)->utf8 / ->ink` interface. */
typedef struct {
    char        utf8[4];               /* Braille glyph, 3-byte UTF-8 + NUL    */
    uint8_t     cp437;                 /* DOS fallback (CP437 lacks Braille)   */
    uint8_t     ink[TVID_MONO_SUBN];   /* 2x4 binary coverage, row-major       */
} tvid_glyph;

/* Nearest CP437 block/shade for a Braille pattern, by lit-dot count (CP437 has no
 * Braille; DOS degrades to a uniform shade of matching density). */
static inline uint8_t tvid_mono_cp437(uint8_t pat) {
    int dots = 0, i;
    for (i = 0; i < 8; ++i) if (pat & (1 << i)) ++dots;
    if (dots == 0) return 0x20;       /* space   */
    if (dots <= 2) return 0xB0;       /* light   */
    if (dots <= 4) return 0xB1;       /* medium  */
    if (dots <= 6) return 0xB2;       /* dark    */
    return 0xDB;                      /* full    */
}

/* Resolve a glyph index to its renderable entry (cached). Encoder and decoder
 * both call this; the cache is per-process and identical on both. */
static inline const tvid_glyph *tvid_mono_glyph(int idx) {
    static tvid_glyph cache[256];
    static uint8_t built[256];
    int gi = idx & 0xFF;
    if (!built[gi]) {
        uint8_t pat = tvid_mono_pattern(gi);
        /* U+2800 + pat in UTF-8: 0xE2 0xA0+(pat>>6) 0x80+(pat&0x3F). */
        cache[gi].utf8[0] = (char)0xE2;
        cache[gi].utf8[1] = (char)(0xA0 + (pat >> 6));
        cache[gi].utf8[2] = (char)(0x80 + (pat & 0x3F));
        cache[gi].utf8[3] = '\0';
        cache[gi].cp437 = tvid_mono_cp437(pat);
        tvid_mono_pattern_ink(pat, cache[gi].ink);
        built[gi] = 1;
    }
    return &cache[gi];
}

/* The 64 most-used Braille patterns, ranked by how often the joint quantizer
 * picks them across the training clips (bad.webm B&W + video.webm color) when all
 * 256 are available. These 64 cover 99.5% of real cell selections, so the 6-bit
 * glyph field rarely has to settle for a worse-fitting pattern. Index 0 is the
 * empty pattern (a blank cell stays blank). Regenerate with the encoder's 0+8
 * probe + the pattern-frequency script if the corpus changes. */
static const uint8_t TVID_MONO_PATTERN64[64] = {
    0x00,0xff,0xc0,0x09,0x1b,0x3f,0x08,0xe4,0x40,0x80,0x01,0x7f,0xbf,0xf6,0xb8,0x3b,
    0x1f,0x19,0xc4,0xf7,0xe0,0xfe,0x47,0x0b,0x18,0x03,0x44,0x5f,0x04,0xa0,0x24,0x12,
    0x10,0x02,0xe6,0xbb,0xf4,0x20,0xe7,0xb9,0xfc,0x38,0x36,0x30,0x07,0x06,0xa4,0xf0,
    0xdb,0x0f,0xf8,0x46,0x4f,0x32,0x39,0xc7,0x1a,0x13,0x3e,0x37,0xb0,0xc8,0xc6,0xc9,
};

/* The pattern table. For the 0+8 (256-glyph) ceiling it is the identity (every
 * Braille byte addressable - the quality ceiling / probe). For narrower glyph
 * fields it is the measured best-N prefix of TVID_MONO_PATTERN64 (or, below 64,
 * the most-used N of those). */
static const uint8_t *tvid_mono_pattern_table(void) {
    static uint8_t tab[256];
    static int built = 0;
    if (built) return tab;
    if (TVID_MONO_GLYPH_COUNT >= 256) {
        for (int i = 0; i < 256; ++i) tab[i] = (uint8_t)i; /* identity ceiling */
    } else {
        int i;
        for (i = 0; i < TVID_MONO_GLYPH_COUNT && i < 64; ++i)
            tab[i] = TVID_MONO_PATTERN64[i];
        for (; i < 256; ++i) tab[i] = 0x00; /* unused slots */
    }
    built = 1;
    return tab;
}

#ifdef __cplusplus
}
#endif

#endif /* GLYPHSET_H */
