/* mode_rle_test.c - round-trip + edge cases for the SPLIT mode-tag plane RLE.
 *
 * The encoder zero-RLEs the mode plane and the player expands it (mode_rle.h);
 * a drift between the two corrupts every leaf after the first SKIP run, so the
 * codec must be exactly invertible for any byte sequence, and must reject a
 * truncated stream (trailing 0 with no count) instead of reading past the end. */
#include "mode_rle.h"

#include <stdio.h>
#include <string.h>

static unsigned rng_state = 0xC0FFEEu;
static unsigned rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int g_fail = 0;
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); g_fail = 1; }
}

/* Encode then decode `src[0..n)` and assert it survives byte-for-byte, and that
 * the size pass agrees with the actual decode length. */
static void roundtrip(const uint8_t *src, long n, const char *what) {
    static uint8_t enc[1 << 17];      /* 2*n worst case; n stays < 64 KiB here */
    static uint8_t dec[1 << 16];
    long en = mode_rle_encode(src, n, enc);
    long want = mode_rle_decoded_len(enc, en);
    check(want == n, what);
    long dn = mode_rle_decode(enc, en, dec);
    check(dn == n, what);
    check(memcmp(dec, src, (size_t)n) == 0, what);
}

int main(void) {
    static uint8_t buf[1 << 16];

    /* (1) Empty, all-zero (one huge run > 255, must split), all-nonzero. */
    roundtrip(buf, 0, "empty");
    memset(buf, 0, sizeof buf);
    roundtrip(buf, sizeof buf, "all-zero (run split)");
    for (size_t i = 0; i < sizeof buf; ++i) buf[i] = (uint8_t)(1 + (i & 3)); /* 1..3,1.. */
    roundtrip(buf, sizeof buf, "all-nonzero");

    /* (2) Random {0,1,2,3} streams, SKIP-heavy like real mode planes. */
    for (int t = 0; t < 2000 && !g_fail; ++t) {
        long n = (long)(rng() % sizeof buf);
        for (long i = 0; i < n; ++i) {
            unsigned r = rng() & 0xFF;
            buf[i] = (uint8_t)(r < 215 ? 0 : (r & 3)); /* ~84% zeros */
        }
        roundtrip(buf, n, "random SKIP-heavy");
    }

    /* (3) A run of exactly 256 zeros encodes as 0,255 then 0,0 -> still inverts. */
    memset(buf, 0, 256);
    roundtrip(buf, 256, "exactly-256 zero run");

    /* (4) Malformed: a lone trailing 0 with no count byte must be rejected, not
     *     read past the end. */
    {
        uint8_t bad[1] = { 0 };
        check(mode_rle_decoded_len(bad, 1) == -1, "truncated run rejected (len)");
        uint8_t out[1];
        check(mode_rle_decode(bad, 1, out) == -1, "truncated run rejected (decode)");
    }

    if (!g_fail) printf("mode_rle: OK\n");
    return g_fail;
}
