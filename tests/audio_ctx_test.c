/* audio_ctx_test.c - the codec-3 audio path: the step-index-context nibble coder
 * (range_ctx.h, method 4) must reconstruct the EXACT original ADPCM bytes, so
 * decoded PCM is bit-identical to codec 1. This exercises the real
 * range_ctx_compress_adpcm / range_ctx_decompress_adpcm through the real ADPCM
 * encoder, both whole-stream and chunked at the K=256 group boundary the player
 * uses (so the per-group restart / index-reseed logic is covered).
 *
 * The chunked wrap mirrors ctx_wrap_adpcm_k in src/encoder/enc_stages.cpp; the
 * chunked unwrap mirrors audio_fill_group in src/decoder/player.c. */
#include "adpcm.h"
#include "range_ctx.h"

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

/* Derive a chunk's PCM sample count from its reconstructed ADPCM byte length --
 * exactly the derivation the player uses (adpcm_bytes_to_samples). */
static long bytes_to_samples(long nbytes) {
    long samples = 0;
    while (nbytes >= ADPCM_BLOCK_BYTES) { samples += ADPCM_BLOCK_SAMPLES; nbytes -= ADPCM_BLOCK_BYTES; }
    if (nbytes > ADPCM_BLOCK_HEADER)      samples += 1 + (nbytes - ADPCM_BLOCK_HEADER) * 2;
    else if (nbytes == ADPCM_BLOCK_HEADER) samples += 1;
    return samples;
}

/* Wrap into codec-3 chunks with method 4 only (mirror of ctx_wrap_adpcm_k, but we
 * force method 4 so the context coder itself is under test, not the auto-select). */
static long wrap(const uint8_t *blob, long blob_len, long samples,
                 uint8_t *out, long out_cap) {
    long op = 0, ip = 0, rem = samples;
    while (rem > 0 && ip < blob_len) {
        long gs = ip, gsamp = 0;
        for (int b = 0; b < GROUP && rem > 0 && ip < blob_len; ++b) {
            int n = rem > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)rem;
            long bb = adpcm_block_bytes(n);
            if (bb < 0 || ip + bb > blob_len) return -1;
            ip += bb; rem -= n; gsamp += n;
        }
        long glen = ip - gs;
        uint8_t *cx = malloc((size_t)(glen + glen / 16 + 1024));
        long cxc = range_ctx_compress_adpcm(blob + gs, glen, gsamp, cx,
                                            glen + glen / 16 + 1024);
        if (cxc < 0) { free(cx); return -1; }
        if (op + 9 + cxc > out_cap) { free(cx); return -1; }
        out[op++] = 4;
        put_u32le(out + op, (uint32_t)glen); op += 4;
        put_u32le(out + op, (uint32_t)cxc); op += 4;
        memcpy(out + op, cx, (size_t)cxc); op += cxc;
        free(cx);
    }
    return op;
}

/* Unwrap the codec-3 tail back to raw ADPCM bytes (mirror of audio_fill_group). */
static long unwrap(const uint8_t *in, long in_len, uint8_t *out, long out_cap) {
    long ip = 0, op = 0;
    while (ip < in_len) {
        if (ip + 9 > in_len) return -1;
        int method = in[ip];
        long alen = (long)get_u32le(in + ip + 1);
        long plen = (long)get_u32le(in + ip + 5);
        if (ip + 9 + plen > in_len || op + alen > out_cap) return -1;
        const uint8_t *payload = in + ip + 9;
        if (method == 4) {
            long gs = bytes_to_samples(alen);
            if (range_ctx_decompress_adpcm(payload, plen, gs, out + op, alen) != alen)
                return -1;
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
    unsigned r = 0x2468ACE0u;

    for (long i = 0; i < N; ++i) {
        double s = 9000.0 * sin(2.0 * 3.14159265 * 330.0 * i / 8000.0);
        r = r * 1664525u + 1013904223u;
        s += (double)((int)((r >> 8) & 0x7FF) - 1024);
        pcm[i] = (int16_t)s;
    }
    if (!roundtrip(pcm, N)) return 1;

    /* Silence and short/edge lengths (odd nibble counts, sub-block tails). */
    memset(pcm, 0, (size_t)N * sizeof(int16_t));
    if (!roundtrip(pcm, N)) return 1;
    for (long n = 0; n <= ADPCM_BLOCK_SAMPLES * 2 + 5; n += 257) {
        for (long i = 0; i < n; ++i) pcm[i] = (int16_t)(4000.0 * sin(0.03 * (double)i));
        if (!roundtrip(pcm, n)) { printf("  (failed at n=%ld)\n", n); return 1; }
    }

    free(pcm);
    printf("audio_ctx_roundtrip: OK\n");
    return 0;
}
