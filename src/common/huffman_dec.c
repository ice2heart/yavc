/* huffman_dec.c - canonical static Huffman decode side (see huffman.h). Portable
 * C99, part of the player/decoder set. huff_build_codes lives here because the
 * decoder's fast-table builder needs it; the encoder (huffman_enc.cpp) also calls
 * it and links this file. The length-construction (huff_build_lengths) is
 * encoder-only and lives in huffman_enc.cpp. */
#include "huffman.h"

void huff_build_codes(const uint8_t *lengths, int nsym, uint16_t *codes) {
    uint16_t bl_count[HUFF_MAX_BITS + 1] = {0};
    for (int i = 0; i < nsym; ++i) {
        codes[i] = 0;
        if (lengths[i]) bl_count[lengths[i]]++;
    }
    /* Canonical first code per length. */
    uint16_t next_code[HUFF_MAX_BITS + 1] = {0};
    uint16_t code = 0;
    for (int bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
        code = (uint16_t)((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }
    for (int i = 0; i < nsym; ++i) {
        int len = lengths[i];
        if (len) codes[i] = next_code[len]++;
    }
}

void huff_decoder_init(huff_decoder *d, const uint8_t *lengths, int nsym) {
    d->nsym = nsym;
    for (int i = 0; i <= HUFF_MAX_BITS; ++i) d->count[i] = 0;
    for (int i = 0; i < nsym; ++i)
        if (lengths[i]) d->count[lengths[i]]++;

    /* Canonical first code + first sorted-symbol index per length. */
    uint16_t code = 0, sidx = 0;
    for (int bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
        code = (uint16_t)((code + d->count[bits - 1]) << 1);
        d->first_code[bits] = code;
        d->first_sym[bits] = sidx;
        sidx = (uint16_t)(sidx + d->count[bits]);
    }
    /* Symbols sorted by (length, symbol). */
    uint16_t offs[HUFF_MAX_BITS + 1];
    uint16_t acc = 0;
    for (int bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
        offs[bits] = acc; acc = (uint16_t)(acc + d->count[bits]);
    }
    for (int i = 0; i < nsym; ++i) {
        int len = lengths[i];
        if (len) d->sorted[offs[len]++] = (uint16_t)i;
    }

    /* Fast table: for every HUFF_DECODE_BITS prefix, find the symbol whose code is
     * a prefix. Build per-symbol codes and splat them across the table. */
    for (int i = 0; i < HUFF_DECODE_SIZE; ++i) { d->fast[i].sym = -1; d->fast[i].len = 0; }
    uint16_t codes[512];
    huff_build_codes(lengths, nsym, codes);
    for (int i = 0; i < nsym; ++i) {
        int len = lengths[i];
        if (!len || len > HUFF_DECODE_BITS) continue;
        /* Code occupies the top `len` bits of a HUFF_DECODE_BITS index; the low
         * (HUFF_DECODE_BITS-len) bits are don't-care -> splat all of them. */
        int shift = HUFF_DECODE_BITS - len;
        uint16_t base = (uint16_t)(codes[i] << shift);
        int span = 1 << shift;
        for (int j = 0; j < span; ++j) {
            d->fast[base + j].sym = (int16_t)i;
            d->fast[base + j].len = (uint8_t)len;
        }
    }
}
