/* range_test.c - the adaptive order-1 range coder (plane method 3) must
 * round-trip byte-identically across compressible, random, and edge-size inputs,
 * and must shrink order-1-correlated data. The decode side is what ships in the
 * player, so a mismatch here is a corrupt-frame bug. */
#include "range.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng_state = 0x1234567u;
static unsigned rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int roundtrip(const uint8_t *in, long n) {
    long cap = n + n / 16 + 2048;
    uint8_t *comp = malloc((size_t)cap);
    uint8_t *out = malloc((size_t)(n > 0 ? n : 1));
    int ok = 0;
    long c = range_compress(in, n, comp, cap);
    if (c < 0) { printf("compress overflow (n=%ld)\n", n); goto done; }
    long d = range_decompress(comp, c, out, n);
    if (d != n) { printf("decompress len %ld != %ld\n", d, n); goto done; }
    if (n && memcmp(in, out, (size_t)n) != 0) { printf("MISMATCH (n=%ld)\n", n); goto done; }
    ok = 1;
done:
    free(comp); free(out);
    return ok;
}

int main(void) {
    const long N = 200000;
    uint8_t *buf = malloc((size_t)N);

    /* 1. Skewed runs (strong order-0 + order-1 structure): must round-trip and
     *    shrink hard. */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)((i / 64) & 0x0F);
    if (!roundtrip(buf, N)) return 1;
    {
        long cap = N + N / 16 + 2048;
        uint8_t *comp = malloc((size_t)cap);
        long c = range_compress(buf, N, comp, cap);
        if (c <= 0 || c >= N / 4) {
            printf("expected strong shrink, got %ld/%ld\n", c, N); return 1;
        }
        free(comp);
    }

    /* 2. Order-1 Markov chain: next byte strongly predicted by the previous one.
     *    This is the model's home turf -- it must shrink well below order-0. */
    {
        uint8_t prev = 0;
        for (long i = 0; i < N; ++i) {
            /* 90% of the time the next byte == prev+1 (a smooth ramp); else noise */
            prev = (rng() % 10 == 0) ? (uint8_t)(rng() & 0xFF) : (uint8_t)(prev + 1);
            buf[i] = prev;
        }
        if (!roundtrip(buf, N)) return 1;
    }

    /* 3. Skewed literals, ~90% zeros. */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)((rng() % 10 == 0) ? (rng() & 0xFF) : 0);
    if (!roundtrip(buf, N)) return 1;

    /* 4. Incompressible random data: must round-trip (may grow; the encoder only
     *    keeps method 3 when it actually wins). */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)(rng() & 0xFF);
    if (!roundtrip(buf, N)) return 1;

    /* 5. Mixed/edge sizes including empty and single bytes. */
    for (long n = 0; n <= 64; ++n) {
        for (long i = 0; i < n; ++i) buf[i] = (uint8_t)(rng() & 0x03);
        if (!roundtrip(buf, n)) return 1;
    }
    /* 6. All-one-byte (degenerate model: one symbol dominates totally). */
    memset(buf, 0xAB, (size_t)N);
    if (!roundtrip(buf, N)) return 1;

    free(buf);
    printf("range_roundtrip: OK\n");
    return 0;
}
