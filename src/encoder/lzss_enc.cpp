/* lzss_enc.cpp - whole-stream LZSS compressor (encoder-side, C++). Offline
 * hash-chain match finder over the whole buffer. Moved out of src/common/ so the
 * player/decoder set carries only lzss_dec.c; the decompressor must build for
 * 16-bit DOS. Declared extern "C" (via lzss.h). Token layout: see lzss.h. */
#include "lzss.h"

#include <vector>

namespace {

constexpr int LZSS_HASH_BITS = 15;
constexpr unsigned LZSS_HASH_SIZE = 1u << LZSS_HASH_BITS;
constexpr int LZSS_MAX_CHAIN = 256; /* search effort cap; bigger = smaller, slower */

unsigned lzss_hash(const uint8_t *p) {
    return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ (unsigned)p[2]) &
           (LZSS_HASH_SIZE - 1);
}

} // namespace

long lzss_compress(const uint8_t *in, long n, uint8_t *out, long out_cap) {
    /* head[h] = most recent position with hash h; prev[i] = previous such pos. */
    std::vector<long> head(LZSS_HASH_SIZE, -1);
    std::vector<long> prev((size_t)(n > 0 ? n : 1));
    long op = 0, i = 0;
    long flag_pos;     /* index in out of the current flag byte */
    int flag_bit;      /* which bit (0..7) we're filling next */
    long h;

    flag_pos = op++;        /* reserve first flag byte */
    if (flag_pos >= out_cap) return -1;
    out[flag_pos] = 0;
    flag_bit = 0;

    while (i < n) {
        long best_len = 0, best_dist = 0;
        if (i + LZSS_MIN_MATCH <= n) {
            h = lzss_hash(&in[i]);
            long cand = head[h];
            int chain = 0;
            long min_pos = i - LZSS_WINDOW;
            if (min_pos < 0) min_pos = 0;
            while (cand >= min_pos && chain < LZSS_MAX_CHAIN) {
                long len = 0;
                long maxlen = n - i;
                if (maxlen > LZSS_MAX_MATCH) maxlen = LZSS_MAX_MATCH;
                while (len < maxlen && in[cand + len] == in[i + len]) len++;
                if (len > best_len) {
                    best_len = len;
                    best_dist = i - cand;
                    if (len >= LZSS_MAX_MATCH) break;
                }
                cand = prev[cand];
                chain++;
            }
        }

        if (flag_bit == 8) {
            flag_pos = op++;
            if (flag_pos >= out_cap) return -1;
            out[flag_pos] = 0;
            flag_bit = 0;
        }

        if (best_len >= LZSS_MIN_MATCH) {
            /* match: flag bit stays 0 */
            long d = best_dist - 1;
            long l = best_len - LZSS_MIN_MATCH;
            if (op + 2 > out_cap) return -1;
            out[op++] = (uint8_t)(d & 0xFF);
            out[op++] = (uint8_t)(((d >> 8) & 0x0F) | (l << 4));
            /* insert all consumed positions into the hash for future matches */
            long end = i + best_len;
            for (; i < end; i++) {
                if (i + LZSS_MIN_MATCH <= n) {
                    long hh = lzss_hash(&in[i]);
                    prev[i] = head[hh];
                    head[hh] = i;
                }
            }
        } else {
            out[flag_pos] |= (1 << flag_bit); /* literal */
            if (op + 1 > out_cap) return -1;
            out[op++] = in[i];
            if (i + LZSS_MIN_MATCH <= n) {
                long hh = lzss_hash(&in[i]);
                prev[i] = head[hh];
                head[hh] = i;
            }
            i++;
        }
        flag_bit++;
    }

    return op;
}
