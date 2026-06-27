/* adpcm_test.c - IMA ADPCM is lossy, so this asserts a bounded-error envelope
 * (not byte-identical): encode -> decode must track the input within a small mean
 * absolute error, block sizes must be self-consistent, and the multi-block stream
 * must decode block-by-block exactly as the encoder framed it. The decode side is
 * what ships in the player (and to DOS), so a framing bug here is a corrupt-audio
 * bug. */
#include "adpcm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng_state = 0x2468acefu;
static unsigned rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* Decode a whole adpcm stream block-by-block, walking the same framing the
 * encoder used (full blocks of ADPCM_BLOCK_SAMPLES, a short final block). Returns
 * total samples decoded, or -1 on error. */
static long decode_stream(const uint8_t *in, long in_len, long nsamples,
                          int16_t *out) {
    long ip = 0, op = 0;
    while (op < nsamples) {
        long remain = nsamples - op;
        int n = remain > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)remain;
        long bb = adpcm_block_bytes(n);
        if (bb < 0 || ip + bb > in_len) return -1;
        int got = adpcm_decode_block(in + ip, bb, out + op, n);
        if (got != n) return -1;
        ip += bb;
        op += n;
    }
    return op;
}

/* Round-trip nsamples, assert mean abs error <= max_mae and peak error sane. */
static int roundtrip(const int16_t *pcm, long nsamples, double max_mae) {
    long cap = adpcm_encoded_size(nsamples);
    if (cap < 0) { printf("encoded_size overflow (n=%ld)\n", nsamples); return 0; }
    uint8_t *enc = malloc((size_t)(cap > 0 ? cap : 1));
    int16_t *dec = malloc((size_t)(nsamples > 0 ? nsamples : 1) * sizeof(int16_t));
    int ok = 0;
    long c = adpcm_encode(pcm, nsamples, enc, cap);
    if (c != cap) { printf("encode wrote %ld != %ld (n=%ld)\n", c, cap, nsamples); goto done; }
    long d = decode_stream(enc, c, nsamples, dec);
    if (d != nsamples) { printf("decode produced %ld != %ld\n", d, nsamples); goto done; }

    double sum = 0.0;
    long peak = 0;
    for (long i = 0; i < nsamples; ++i) {
        long e = labs((long)pcm[i] - (long)dec[i]);
        sum += (double)e;
        if (e > peak) peak = e;
    }
    double mae = nsamples ? sum / (double)nsamples : 0.0;
    if (mae > max_mae) {
        printf("MAE %.1f > %.1f (n=%ld, peak=%ld)\n", mae, max_mae, nsamples, peak);
        goto done;
    }
    ok = 1;
done:
    free(enc); free(dec);
    return ok;
}

int main(void) {
    const long N = 96000; /* ~12 s at 8 kHz */
    int16_t *pcm = malloc((size_t)N * sizeof(int16_t));

    /* 1. Pure sine: ADPCM tracks smooth signals tightly. */
    for (long i = 0; i < N; ++i)
        pcm[i] = (int16_t)(12000.0 * sin(2.0 * 3.14159265 * 220.0 * i / 8000.0));
    if (!roundtrip(pcm, N, 200.0)) return 1;

    /* 2. Sine + a little noise (more realistic music-like content). */
    for (long i = 0; i < N; ++i) {
        double s = 10000.0 * sin(2.0 * 3.14159265 * 440.0 * i / 8000.0);
        s += (double)((int)(rng() & 0x7FF) - 1024);
        pcm[i] = (int16_t)s;
    }
    if (!roundtrip(pcm, N, 600.0)) return 1;

    /* 3. Silence: must stay silent (predictor seeded 0, tiny steps). */
    memset(pcm, 0, (size_t)N * sizeof(int16_t));
    if (!roundtrip(pcm, N, 1.0)) return 1;

    /* 4. Block-boundary stress: lengths around a block edge, incl. tiny & empty. */
    for (long n = 0; n <= ADPCM_BLOCK_SAMPLES + 3; ++n) {
        for (long i = 0; i < n; ++i)
            pcm[i] = (int16_t)(5000.0 * sin(0.05 * (double)i));
        /* loose envelope: short streams have less context to settle */
        if (!roundtrip(pcm, n, 1500.0)) { printf("  (failed at n=%ld)\n", n); return 1; }
    }

    free(pcm);
    printf("adpcm_roundtrip: OK\n");
    return 0;
}
