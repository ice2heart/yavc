/* bitstream.h - MSB-first bit reader/writer for the v2 block frame (portable C).
 *
 * Shared by the C++ encoder (writer) and the C decoder (reader). MSB-first so a
 * hand-dump of the bytes reads in emission order. No bounds checks on the hot
 * read path beyond a buffer end guard the caller sizes for; the encoder never
 * emits more than the decoder reads (round-trip test is the gate). */
#ifndef BITSTREAM_H
#define BITSTREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- reader ---- */
typedef struct {
    const uint8_t *buf;
    long len;      /* bytes available */
    long byte;     /* current byte index */
    int  bit;      /* next bit within buf[byte], 0 = MSB */
} bitreader;

static inline void br_init(bitreader *r, const uint8_t *buf, long len) {
    r->buf = buf; r->len = len; r->byte = 0; r->bit = 0;
}

/* Read one bit (0/1). Past end returns 0 (defensive). */
static inline int br_bit(bitreader *r) {
    if (r->byte >= r->len) return 0;
    int v = (r->buf[r->byte] >> (7 - r->bit)) & 1;
    if (++r->bit == 8) { r->bit = 0; r->byte++; }
    return v;
}

/* Read n bits MSB-first into an unsigned value (n <= 24). */
static inline uint32_t br_bits(bitreader *r, int n) {
    uint32_t v = 0;
    while (n--) v = (v << 1) | (uint32_t)br_bit(r);
    return v;
}

/* Read a whole byte (used for cell values; n may be unaligned). */
static inline uint8_t br_byte(bitreader *r) { return (uint8_t)br_bits(r, 8); }

/* ---- writer ---- */
typedef struct {
    uint8_t *buf;
    long cap;
    long byte;
    int  bit;      /* next bit position to fill, 0 = MSB */
    int  overflow; /* set if a write would exceed cap */
} bitwriter;

static inline void bw_init(bitwriter *w, uint8_t *buf, long cap) {
    w->buf = buf; w->cap = cap; w->byte = 0; w->bit = 0; w->overflow = 0;
    if (cap > 0) buf[0] = 0;
}

static inline void bw_bit(bitwriter *w, int v) {
    if (w->byte >= w->cap) { w->overflow = 1; return; }
    if (v) w->buf[w->byte] |= (uint8_t)(1 << (7 - w->bit));
    if (++w->bit == 8) {
        w->bit = 0;
        w->byte++;
        if (w->byte < w->cap) w->buf[w->byte] = 0;
    }
}

static inline void bw_bits(bitwriter *w, uint32_t v, int n) {
    while (n--) bw_bit(w, (int)((v >> n) & 1));
}

static inline void bw_byte(bitwriter *w, uint8_t b) { bw_bits(w, b, 8); }

/* Total bytes used (rounding up any partial final byte). */
static inline long bw_len(const bitwriter *w) {
    return w->byte + (w->bit ? 1 : 0);
}

#ifdef __cplusplus
}
#endif

#endif /* BITSTREAM_H */
