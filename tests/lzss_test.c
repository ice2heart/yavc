/* lzss_test.c - LZSS compress/decompress must round-trip bit-identically, and
 * compressible input must actually shrink. */
#include "lzss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng_state = 0xC0FFEEu;
static unsigned rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int roundtrip(const uint8_t *in, long n) {
    long cap = n + n / 16 + 64;
    uint8_t *comp = malloc((size_t)cap);
    uint8_t *out = malloc((size_t)(n > 0 ? n : 1));
    long c = lzss_compress(in, n, comp, cap);
    int ok = 0;
    if (c < 0) { printf("compress overflow (n=%ld)\n", n); goto done; }
    long d = lzss_decompress(comp, c, out, n);
    if (d != n) { printf("decompress len %ld != %ld\n", d, n); goto done; }
    if (memcmp(in, out, (size_t)n) != 0) { printf("MISMATCH (n=%ld)\n", n); goto done; }
    ok = 1;
done:
    free(comp); free(out);
    return ok;
}

int main(void) {
    const long N = 200000;
    uint8_t *buf = malloc((size_t)N);

    /* 1. Highly compressible: long runs + repeated phrases. */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)((i / 64) & 0x0F);
    if (!roundtrip(buf, N)) return 1;
    {
        long cap = N + N / 16 + 64;
        uint8_t *comp = malloc((size_t)cap);
        long c = lzss_compress(buf, N, comp, cap);
        if (c <= 0 || c >= N / 2) { printf("expected strong shrink, got %ld/%ld\n", c, N); return 1; }
        free(comp);
    }

    /* 2. Incompressible random data must still round-trip. */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)(rng() & 0xFF);
    if (!roundtrip(buf, N)) return 1;

    /* 3. Mixed/edge sizes. */
    for (long n = 0; n <= 40; ++n) {
        for (long i = 0; i < n; ++i) buf[i] = (uint8_t)(rng() & 0x07);
        if (!roundtrip(buf, n)) return 1;
    }

    free(buf);
    printf("lzss_roundtrip: OK\n");
    return 0;
}
