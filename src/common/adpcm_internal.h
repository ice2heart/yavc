/* adpcm_internal.h - IMA/DVI ADPCM tables + clamp shared bit-exactly between the
 * encoder (adpcm_enc.cpp) and the decoder (adpcm_dec.c). Defined static so each
 * translation unit gets an identical private copy; the decoder TU stays free of
 * any encoder object (it must build as portable C99 for 16-bit DOS). NOT a public
 * header -- callers use adpcm.h. */
#ifndef ADPCM_INTERNAL_H
#define ADPCM_INTERNAL_H

#include <stdint.h>

/* The two canonical IMA tables. step_table is the quantizer step size indexed by
 * the running step index; index_table adjusts that index per 4-bit code. */
static const int16_t adpcm_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t adpcm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static int adpcm_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

#endif /* ADPCM_INTERNAL_H */
