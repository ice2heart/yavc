/* entropy_internal.h - the litlen/dist alphabet contract shared by the entropy
 * compressor (entropy_enc.cpp) and decompressor (entropy_dec.c). These sizes ARE
 * the wire format: if the two halves disagree on the alphabet, decode breaks
 * silently, so they live here once. Pulls LZSS_MAX_MATCH/LZSS_MIN_MATCH from
 * lzss.h (header-only dependency). NOT a public header; callers use entropy.h.
 *
 * litlen: 0..255 literal bytes, 256 = end-of-stream, 257.. = match length codes
 *         (one per length LZSS_MIN_MATCH..LZSS_MAX_MATCH, so 16 length codes).
 * dist:   a "distance code" = number of significant bits of (dist-1), 0..12, then
 *         that many extra bits carry the low part. 13 codes cover 1..4096. */
#ifndef ENTROPY_INTERNAL_H
#define ENTROPY_INTERNAL_H

#include "lzss.h"

#define ENT_NLEN     (LZSS_MAX_MATCH - LZSS_MIN_MATCH + 1) /* 16 */
#define ENT_SYM_END  256
#define ENT_SYM_LEN0 257
#define ENT_NLITLEN  (ENT_SYM_LEN0 + ENT_NLEN)             /* 273 */
#define ENT_NDIST    13                                    /* dist-1 < 2^12 */

#endif /* ENTROPY_INTERNAL_H */
