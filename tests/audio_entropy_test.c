/* audio_entropy_test.c - the codec-2 audio path: ADPCM blocks wrapped in
 * range-coded entropy chunks must reconstruct the EXACT original ADPCM bytes
 * (codec 2 is lossless w.r.t. codec 1, so decoded PCM is bit-identical). This
 * pins the chunk format and the group-restart streaming the player relies on.
 *
 * The wrap mirrors entropy_wrap_adpcm_k in src/encoder/enc_stages.cpp; the unwrap
 * mirrors audio_fill_group in src/decoder/player.c. We keep local copies (rather
 * than linking the encoder pipeline / player main) so the test stays small, and
 * assert the two are exact inverses through the real range coder + real ADPCM. */
#include "adpcm.h"
#include "range.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GROUP 256 /* TVID_AUDIO_ENT_GROUP; kept local to avoid the format header */

static void put_u32le(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
}
static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Wrap a raw ADPCM blob into the codec-2 entropy tail (mirror of the encoder). */
static long wrap(const uint8_t *blob, long blob_len, long samples,
                 uint8_t *out, long out_cap) {
    long op = 0, ip = 0, rem = samples;
    while (rem > 0 && ip < blob_len) {
        long gs = ip;
        for (int b = 0; b < GROUP && rem > 0 && ip < blob_len; ++b) {
            int n = rem > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)rem;
            long bb = adpcm_block_bytes(n);
            if (bb < 0 || ip + bb > blob_len) return -1;
            ip += bb; rem -= n;
        }
        long glen = ip - gs;
        uint8_t *rc = malloc((size_t)(glen + glen / 16 + 1024));
        long rcc = range_compress(blob + gs, glen, rc, glen + glen / 16 + 1024);
        int method = 0; long plen = glen; const uint8_t *psrc = blob + gs;
        if (rcc > 0 && rcc < glen) { method = 3; plen = rcc; psrc = rc; }
        if (op + 9 + plen > out_cap) { free(rc); return -1; }
        out[op++] = (uint8_t)method;
        put_u32le(out + op, (uint32_t)glen); op += 4;
        put_u32le(out + op, (uint32_t)plen); op += 4;
        memcpy(out + op, psrc, (size_t)plen); op += plen;
        free(rc);
    }
    return op;
}

/* Unwrap the codec-2 tail back to raw ADPCM bytes (mirror of audio_fill_group),
 * chunk by chunk, so we also exercise the per-group restart the player uses. */
static long unwrap(const uint8_t *in, long in_len, uint8_t *out, long out_cap) {
    long ip = 0, op = 0;
    while (ip < in_len) {
        if (ip + 9 > in_len) return -1;
        int method = in[ip];
        long alen = (long)get_u32le(in + ip + 1);
        long plen = (long)get_u32le(in + ip + 5);
        if (ip + 9 + plen > in_len || op + alen > out_cap) return -1;
        const uint8_t *payload = in + ip + 9;
        if (method == 0) {
            if (plen != alen) return -1;
            memcpy(out + op, payload, (size_t)alen);
        } else if (method == 3) {
            if (range_decompress(payload, plen, out + op, alen) != alen) return -1;
        } else return -1;
        ip += 9 + plen; op += alen;
    }
    return op;
}

static int roundtrip(const int16_t *pcm, long nsamples) {
    long cap = adpcm_encoded_size(nsamples);
    uint8_t *adpcm = malloc((size_t)(cap > 0 ? cap : 1));
    long alen = adpcm_encode(pcm, nsamples, adpcm, cap);
    if (alen != cap) { printf("adpcm_encode %ld != %ld\n", alen, cap); free(adpcm); return 0; }

    long wcap = alen + alen / 8 + 4096;
    uint8_t *ent = malloc((size_t)wcap);
    long elen = wrap(adpcm, alen, nsamples, ent, wcap);
    if (elen < 0) { printf("wrap failed\n"); free(adpcm); free(ent); return 0; }

    uint8_t *back = malloc((size_t)(alen > 0 ? alen : 1));
    long blen = unwrap(ent, elen, back, alen);
    int ok = (blen == alen) && (memcmp(adpcm, back, (size_t)alen) == 0);
    if (!ok) printf("unwrap mismatch: blen=%ld alen=%ld\n", blen, alen);
    free(adpcm); free(ent); free(back);
    return ok;
}

int main(void) {
    /* ~70 s so we cross several 64 s groups (multi-group restart coverage). */
    const long N = 560000;
    int16_t *pcm = malloc((size_t)N * sizeof(int16_t));
    unsigned r = 0x13572468u;

    for (long i = 0; i < N; ++i) {
        double s = 9000.0 * sin(2.0 * 3.14159265 * 330.0 * i / 8000.0);
        r = r * 1664525u + 1013904223u;
        s += (double)((int)((r >> 8) & 0x7FF) - 1024);
        pcm[i] = (int16_t)s;
    }
    if (!roundtrip(pcm, N)) return 1;

    /* Silence (range coder should crush it) and short/edge lengths. */
    memset(pcm, 0, (size_t)N * sizeof(int16_t));
    if (!roundtrip(pcm, N)) return 1;
    for (long n = 0; n <= ADPCM_BLOCK_SAMPLES * 2 + 5; n += 257) {
        for (long i = 0; i < n; ++i) pcm[i] = (int16_t)(4000.0 * sin(0.03 * (double)i));
        if (!roundtrip(pcm, n)) { printf("  (failed at n=%ld)\n", n); return 1; }
    }

    free(pcm);
    printf("audio_entropy_roundtrip: OK\n");
    return 0;
}
