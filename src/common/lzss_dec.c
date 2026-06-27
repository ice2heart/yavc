/* lzss_dec.c - whole-stream LZSS decode (ring-buffer copy; weak-PC friendly).
 * Portable C99, part of the player/decoder set. The match-finding compressor is
 * encoder-only and lives in src/encoder/lzss_enc.cpp. Token layout: see lzss.h. */
#include "lzss.h"

long lzss_decompress(const uint8_t *in, long n, uint8_t *out, long out_cap) {
    long ip = 0, op = 0;

    while (ip < n) {
        int flag = in[ip++];
        for (int bit = 0; bit < 8; ++bit) {
            if (ip >= n) return op; /* trailing bits of the last flag byte */
            if (flag & (1 << bit)) {           /* literal */
                if (op >= out_cap) return -1;
                out[op++] = in[ip++];
            } else {                            /* match */
                if (ip + 2 > n) return -1;
                long d = in[ip++];
                long b1 = in[ip++];
                long dist = (d | ((b1 & 0x0F) << 8)) + 1;
                long len = (b1 >> 4) + LZSS_MIN_MATCH;
                if (dist > op || op + len > out_cap) return -1;
                for (long k = 0; k < len; ++k) { out[op] = out[op - dist]; op++; }
            }
        }
    }
    return op;
}
