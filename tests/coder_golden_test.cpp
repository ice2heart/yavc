/* coder_golden_test.cpp - pins the EXACT compressed output of each entropy method
 * (lzss / range / lzss+huffman / no-LZ huffman) for a fixed battery of inputs.
 *
 * The per-coder roundtrip tests only prove decompress(compress(x)) == x; they do
 * NOT catch a refactor that still round-trips but emits a different bytestream.
 * This test fingerprints the compressed bytes against checked-in golden hashes,
 * so the C->C++ split (and any later change) is held to byte-identical output.
 *
 * Each input's compressed payload is reduced to a 64-bit FNV-1a hash + length;
 * the goldens below were generated from the pre-refactor binaries. Run with a
 * "--gen" argument to print fresh golden lines (used when an output change is
 * intentional and reviewed). */
#include "lzss.h"
#include "range.h"
#include "entropy.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

uint64_t fnv1a(const uint8_t *p, long n) {
    uint64_t h = 1469598103934665603ull;
    for (long i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

uint32_t rng_state = 0xC0FFEEu;
uint32_t rng() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

/* Deterministic input generators, keyed by name. */
std::vector<uint8_t> gen_input(const char *kind, long n) {
    std::vector<uint8_t> v((size_t)n);
    rng_state = 0xC0FFEEu;
    if (!std::strcmp(kind, "zero")) {
        /* all-zero: best case for RLE-ish coders */
    } else if (!std::strcmp(kind, "ramp")) {
        for (long i = 0; i < n; ++i) v[i] = (uint8_t)((i / 64) & 0x0F);
    } else if (!std::strcmp(kind, "skip")) {
        /* SKIP-heavy: mostly zero with sparse bytes, like the mode plane */
        for (long i = 0; i < n; ++i) v[i] = (uint8_t)((rng() % 16 == 0) ? (rng() & 0xFF) : 0);
    } else if (!std::strcmp(kind, "rand")) {
        for (long i = 0; i < n; ++i) v[i] = (uint8_t)(rng() & 0xFF);
    } else if (!std::strcmp(kind, "cells")) {
        /* mono cell-plane-like: 2-bit luma high, low entropy glyph low */
        for (long i = 0; i < n; ++i) {
            uint8_t luma = (uint8_t)(rng() & 0x03);
            uint8_t glyph = (uint8_t)(rng() % 8);
            v[i] = (uint8_t)((luma << 6) | glyph);
        }
    }
    return v;
}

enum Method { M_LZSS, M_RANGE, M_HUFF, M_NOLZ };

long compress(Method m, const uint8_t *in, long n, std::vector<uint8_t> &out) {
    out.assign((size_t)(n + n / 16 + 1024), 0);
    switch (m) {
        case M_LZSS:  return lzss_compress(in, n, out.data(), (long)out.size());
        case M_RANGE: return range_compress(in, n, out.data(), (long)out.size());
        case M_HUFF:  return entropy_compress(in, n, out.data(), (long)out.size());
        case M_NOLZ:  return entropy_compress_nolz(in, n, out.data(), (long)out.size());
    }
    return -1;
}

const char *method_name(Method m) {
    switch (m) { case M_LZSS: return "lzss"; case M_RANGE: return "range";
                 case M_HUFF: return "huff"; case M_NOLZ: return "nolz"; }
    return "?";
}

struct Case { const char *kind; long n; };
const Case kCases[] = {
    {"zero", 4096}, {"ramp", 65536}, {"skip", 32768}, {"rand", 16384}, {"cells", 40000},
};
const Method kMethods[] = { M_LZSS, M_RANGE, M_HUFF, M_NOLZ };

/* Golden = {clen, fnv} per (method, case), generated from pre-refactor binaries.
 * Index: method*ncases + case, in the kMethods/kCases order above. */
struct Golden { long clen; uint64_t fnv; };
const Golden kGolden[] = {
    /* method, case -> {clen, fnv} */
    {486, 12925387944248396450ull}, /* lzss zero */
    {7770, 8099593264797660421ull}, /* lzss ramp */
    {3896, 7874796953322768307ull}, /* lzss skip */
    {2192, 1383172454275882339ull}, /* lzss rand */
    {4727, 7539271781283299980ull}, /* lzss cells */
    {38, 6590444694338662315ull}, /* range zero */
    {1513, 17144496192683404335ull}, /* range ramp */
    {2839, 7197658808919813550ull}, /* range skip */
    {2923, 17170055805909415554ull}, /* range rand */
    {169, 3490834877060286297ull}, /* range cells */
    {344, 11285551782132150157ull}, /* huff zero */
    {2473, 14687016240291852107ull}, /* huff ramp */
    {2577, 13545005584252710168ull}, /* huff skip */
    {1696, 6428909292415499835ull}, /* huff rand */
    {1400, 3906823867868506588ull}, /* huff cells */
    {799, 6512591621800714481ull}, /* nolz zero */
    {33567, 3287601318278214997ull}, /* nolz ramp */
    {5493, 1010087078155352378ull}, /* nolz skip */
    {16680, 1279357637425319436ull}, /* nolz rand */
    {11537, 9153423156888812555ull}, /* nolz cells */
};

} // namespace

int main(int argc, char **argv) {
    bool gen = (argc > 1 && !std::strcmp(argv[1], "--gen"));
    const int nc = (int)(sizeof(kCases) / sizeof(kCases[0]));
    const int nm = (int)(sizeof(kMethods) / sizeof(kMethods[0]));
    int fail = 0, idx = 0;
    if (gen) std::printf("    /* method, case -> {clen, fnv} */\n");
    for (int mi = 0; mi < nm; ++mi) {
        for (int ci = 0; ci < nc; ++ci, ++idx) {
            std::vector<uint8_t> in = gen_input(kCases[ci].kind, kCases[ci].n);
            std::vector<uint8_t> out;
            long c = compress(kMethods[mi], in.data(), (long)in.size(), out);
            if (c <= 0) { std::printf("FAIL: %s/%s compress returned %ld\n",
                          method_name(kMethods[mi]), kCases[ci].kind, c); fail = 1; continue; }
            uint64_t h = fnv1a(out.data(), c);
            if (gen) {
                std::printf("    {%ld, %lluull}, /* %s %s */\n", c,
                            (unsigned long long)h, method_name(kMethods[mi]), kCases[ci].kind);
            } else {
                const Golden &g = kGolden[idx];
                if (c != g.clen || h != g.fnv) {
                    std::printf("MISMATCH %s/%s: got {%ld, %lluull} want {%ld, %lluull}\n",
                        method_name(kMethods[mi]), kCases[ci].kind, c,
                        (unsigned long long)h, g.clen, (unsigned long long)g.fnv);
                    fail = 1;
                }
            }
        }
    }
    if (!gen && !fail) std::printf("coder_golden: OK\n");
    return fail;
}
