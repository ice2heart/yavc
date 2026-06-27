/* huffman_enc.cpp - canonical static Huffman code-length construction (encoder
 * side, C++). Builds a Huffman tree by repeated min-merge, reads off code
 * lengths, then enforces the HUFF_MAX_BITS limit with the standard overflow-
 * redistribution fix. Alphabets are small (<= 512), so an O(nsym^2) merge is fine
 * offline. The decoder-side fast-table builder + huff_build_codes live in
 * huffman_dec.c (the player needs them). Declared extern "C" (via huffman.h). */
#include "huffman.h"

int huff_build_lengths(const uint32_t *freq, int nsym, uint8_t *lengths) {
    for (int i = 0; i < nsym; ++i) lengths[i] = 0;

    /* Collect used symbols. */
    int used = 0;
    for (int i = 0; i < nsym; ++i) if (freq[i]) used++;
    if (used == 0) return 0;
    if (used == 1) {
        /* A single symbol still needs a 1-bit code so the stream is decodable. */
        for (int i = 0; i < nsym; ++i) if (freq[i]) { lengths[i] = 1; break; }
        return 1;
    }

    /* Node pool: leaves first, then internal nodes. */
    /* weight/parent over up to 2*nsym nodes. */
    enum { MAXN = 2 * 512 };
    long w[MAXN];
    int parent[MAXN];
    int leaf_of[512]; /* node index per symbol */
    int nn = 0;
    for (int i = 0; i < nsym; ++i) {
        if (freq[i]) { w[nn] = (long)freq[i]; parent[nn] = -1; leaf_of[i] = nn; nn++; }
        else leaf_of[i] = -1;
    }
    int nleaves = nn;

    /* Repeatedly merge the two smallest live roots. */
    int live = nleaves;
    while (live > 1) {
        int a = -1, b = -1;
        for (int i = 0; i < nn; ++i) {
            if (parent[i] != -1) continue;
            if (a == -1 || w[i] < w[a]) { b = a; a = i; }
            else if (b == -1 || w[i] < w[b]) { b = i; }
        }
        int p = nn++;
        w[p] = w[a] + w[b];
        parent[p] = -1;
        parent[a] = p; parent[b] = p;
        live--; /* two roots become one */
    }

    /* Depth of each leaf = code length. */
    for (int i = 0; i < nsym; ++i) {
        if (leaf_of[i] < 0) continue;
        int len = 0, c = leaf_of[i];
        while (parent[c] != -1) { c = parent[c]; len++; }
        if (len < 1) len = 1;
        lengths[i] = (uint8_t)(len > 255 ? 255 : len);
    }

    /* Enforce HUFF_MAX_BITS. Count lengths, push overflow down using the
     * canonical Kraft-redistribution. */
    for (;;) {
        int over = 0;
        for (int i = 0; i < nsym; ++i) if (lengths[i] > HUFF_MAX_BITS) over = 1;
        if (!over) break;
        /* Find a symbol over the limit and the longest symbol under it; lengthen
         * the under one and shorten the over one (keeps the tree valid). This is a
         * simple convergent fixup adequate for these small alphabets. */
        int hi = -1; /* over-limit symbol */
        for (int i = 0; i < nsym; ++i)
            if (lengths[i] > HUFF_MAX_BITS && (hi < 0 || lengths[i] > lengths[hi])) hi = i;
        int lo = -1; /* symbol with the shortest length >0 that we can extend */
        for (int i = 0; i < nsym; ++i)
            if (lengths[i] > 0 && lengths[i] < HUFF_MAX_BITS &&
                (lo < 0 || lengths[i] < lengths[lo])) lo = i;
        if (lo < 0) { lengths[hi] = HUFF_MAX_BITS; continue; }
        lengths[hi]--; lengths[lo]++;
    }

    int nz = 0;
    for (int i = 0; i < nsym; ++i) if (lengths[i]) nz++;
    return nz;
}
