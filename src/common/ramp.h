/* ramp.h - shared luminance helper (portable C).
 *
 * v3 has no ASCII glyph ramp (the cell byte carries a Braille sub-cell shape +
 * luma level, see glyphset.h / tvid_format.h); the retired v2 ramp lived here.
 * What remains is the one shared utility both encoder and decoder still need:
 * BT.601 luma from RGB. The header still stores a fixed ramp[] field for on-disk
 * stability (TVID_HEADER_RAMP), but it is informational only. */
#ifndef RAMP_H
#define RAMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A fixed string written into the (now informational) header ramp[] field, so the
 * file format's ramp_len/ramp bytes stay well-formed for older tooling. */
#define TVID_HEADER_RAMP " .,:;!+*oxc#XW%@"
#define TVID_HEADER_RAMP_LEN 16

/* BT.601 luma from RGB. */
static inline int rgb_luma(int r, int g, int b) {
    return (r * 77 + g * 150 + b * 29) >> 8;
}

#ifdef __cplusplus
}
#endif

#endif /* RAMP_H */
