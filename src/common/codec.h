/* codec.h - v2 quadtree block-frame decoder (portable C, shared).
 *
 * The encoder (C++, offline) builds the block bitstream with a rate-distortion
 * search; this is only the decode half that ships on the floppy. */
#ifndef CODEC_H
#define CODEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Apply a v2 block-frame payload (in[0..len)) to the framebuffer fb[] in place
 * (cols*rows cells). SKIP leaves keep the previous contents, so fb must hold the
 * previously decoded frame on entry. Returns 0 on success, -1 on malformed input. */
int codec_decode_block(const uint8_t *in, long len, uint8_t *fb,
                       int cols, int rows);

/* Reference-aware decode. `prev` is a pristine snapshot of the previous frame's
 * cells (cols*rows), read by motion-vector (SHIFT) leaves whose source cells may
 * already have been overwritten in `fb` this frame. `caps` is an OR of TVID_CAP_*
 * bits telling the parser which optional leaf fields are present. Pass prev==fb
 * and caps==0 to get the legacy in-place behavior (no SHIFT). */
int codec_decode_block_ref(const uint8_t *in, long len, uint8_t *fb,
                           const uint8_t *prev, int cols, int rows, int caps);

/* Split-stream decode (TVID_FLAG_SPLIT): one frame's structure bits in
 * sbits[0..sbits_len) (byte-aligned, self-delimiting via the quadtree walk), the
 * cell-byte plane in cells[0..cells_len), and (with TVID_FLAG_MODEPLANE) the
 * per-leaf mode-tag plane in modes[0..modes_len); pass modes==NULL for the
 * 2-plane layout (mode tags inline in sbits). With TVID_FLAG_CELLSPLIT the cells
 * are carried in two planes by leaf-mode class: `cells` is the raster plane
 * (keyframe + RAW leaves) and `pals` is the palette plane (SOLID + PAL2); pass
 * pals==NULL to read SOLID/PAL2 from the single `cells` plane. With
 * TVID_FLAG_COLOR a per-cell xterm-256 hue plane `color` is decoded into `colfb`
 * (cols*rows) mirroring the cell-byte leaf structure; pass color==NULL and
 * colfb==NULL to disable. *cells_pos, *modes_pos, *pals_pos, *color_pos are the
 * shared cursors threaded frame to frame (updated in place). Returns 0 on
 * success, -1 on malformed input. */
int codec_decode_block_split(const uint8_t *sbits, long sbits_len,
                             const uint8_t *cells, long cells_len, long *cells_pos,
                             const uint8_t *modes, long modes_len, long *modes_pos,
                             const uint8_t *pals, long pals_len, long *pals_pos,
                             const uint8_t *color, long color_len, long *color_pos,
                             uint8_t *fb, uint8_t *colfb, const uint8_t *prev,
                             int cols, int rows, int caps);

#ifdef __cplusplus
}
#endif

#endif /* CODEC_H */
