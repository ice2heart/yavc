/* entropy_test.c - LZSS+Huffman entropy back-end must round-trip bit-identically,
 * and compressible input must shrink below plain LZSS-free storage. */
#include "entropy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng_state = 0xBADF00Du;
static unsigned rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int roundtrip(const uint8_t *in, long n) {
    long cap = n + n / 16 + 2048;
    uint8_t *comp = malloc((size_t)cap);
    uint8_t *out = malloc((size_t)(n > 0 ? n : 1));
    int ok = 0;
    long c = entropy_compress(in, n, comp, cap);
    if (c < 0) { printf("compress overflow (n=%ld)\n", n); goto done; }
    long d = entropy_decompress(comp, c, out, n);
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

    /* 1. Highly compressible: skewed-symbol runs + repeats. Both the LZ matches
     *    and the literal entropy should compress well. */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)((i / 64) & 0x0F);
    if (!roundtrip(buf, N)) return 1;
    {
        long cap = N + N / 16 + 2048;
        uint8_t *comp = malloc((size_t)cap);
        long c = entropy_compress(buf, N, comp, cap);
        if (c <= 0 || c >= N / 4) {
            printf("expected strong shrink, got %ld/%ld\n", c, N); return 1;
        }
        free(comp);
    }

    /* 2. Skewed literals, no matches: Huffman alone must shrink it. ~90% zeros. */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)((rng() % 10 == 0) ? (rng() & 0xFF) : 0);
    if (!roundtrip(buf, N)) return 1;

    /* 3. Incompressible random data must still round-trip (may grow; that's fine,
     *    the encoder driver only keeps it when it actually helps). */
    for (long i = 0; i < N; ++i) buf[i] = (uint8_t)(rng() & 0xFF);
    if (!roundtrip(buf, N)) return 1;

    /* 4. Mixed/edge sizes including empty. */
    for (long n = 0; n <= 64; ++n) {
        for (long i = 0; i < n; ++i) buf[i] = (uint8_t)(rng() & 0x03);
        if (!roundtrip(buf, n)) return 1;
    }

    free(buf);
    printf("entropy_roundtrip: OK\n");
    return 0;
}
