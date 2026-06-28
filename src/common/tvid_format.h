/* tvid_format.h - termvideo container format v3 (portable C, shared encoder/decoder).
 *
 * v3 is a block-based codec: each frame after the keyframe is partitioned into a
 * quadtree of square cell regions, and every leaf carries a small "mode" tag
 * chosen offline by the encoder's rate-distortion search. The decoder only reads
 * split bits and a mode switch of cheap fills/copies - O(cells), table-driven,
 * no search, no floating point, no per-frame allocation (see doc/compression.md).
 *
 * The cell byte encodes a 2x4 sub-cell SHAPE glyph + a brightness LUMA level (see
 * the cell-byte macros below and glyphset.h). v3 optionally carries a parallel
 * COLOR plane (TVID_FLAG_COLOR): one xterm-256 hue index per cell, rendered on top
 * of the luma/glyph so the same shape gains color (see xterm256.h, doc/compression.md).
 *
 * v2 (the old flat ANSI-16 color + ASCII ramp cell model) is retired: v3 with the
 * color plane is a strict superset (sub-cell shape AND 256 colors). The decoder
 * rejects a v2 version byte; there is no v2 decode path.
 */
#ifndef TVID_FORMAT_H
#define TVID_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TVID_MAGIC0 'T'
#define TVID_MAGIC1 'V'
#define TVID_MAGIC2 'I'
#define TVID_MAGIC3 'D'
#define TVID_VERSION 4      /* current format: v3 codec, but the SPLIT body is segmented
                             * (streamable) and the header carries [u16 seg_frames] */
#define TVID_VERSION_V3 3   /* whole-resident SPLIT body; still decoded unchanged */
#define TVID_VERSION_V2 2   /* retired flat-color model; rejected on read, never written */

/* Segmentation (v4). A v4 SPLIT body emits each plane as a sequence of per-segment
 * chunks (segment-major: structure, [mode], cell(/raster+palette), [color] for
 * segment 0, then again for segment 1, ...), each chunk in the same on-disk shape
 * as a v3 whole-plane chunk. The player only holds the current segment of every
 * plane resident, so the resident set is bounded by seg_frames x planes instead of
 * the whole movie (see doc/v3-streaming.md). seg_frames is stored in the header
 * (present only when version >= 4) so the encoder can trade RAM vs ratio.
 *
 * Each segment after segment 0 begins with a keyframe-vs-carry byte at the front
 * of its structure chunk (see TVID_SEG_KEY/TVID_SEG_CARRY): a keyframe segment is
 * independently decodable (its cell/color chunk starts with cols*rows keyframe
 * bytes), a carry segment continues from the previous segment's framebuffer. */
#define TVID_SEG_DEFAULT 64 /* frames per segment unless the encoder overrides it */
#define TVID_SEG_CARRY 0    /* segment continues from the previous fb (no keyframe) */
#define TVID_SEG_KEY   1    /* segment starts with a fresh keyframe (independently decodable) */

/* Grid is fixed at 80x24 cells for the floppy target, but stored in the header
 * so the decoder never hardcodes it. */
#define TVID_COLS 80
#define TVID_ROWS 24
#define TVID_CELLS (TVID_COLS * TVID_ROWS)

/* Quadtree partition. The frame is tiled into square superblocks of
 * TVID_SB cells on a side; each is the root of an independent quadtree that
 * splits down to 1x1 leaves. 80x24 is covered exactly by 8x8 superblocks
 * (10 across x 3 down = 30 superblocks). One bit per internal node says
 * "split"; a leaf carries a mode tag + payload. */
#define TVID_SB 8

/* Header flags. */
#define TVID_FLAG_LZSS 0x01        /* body is [u32le raw_len][whole-stream LZSS] */
#define TVID_FLAG_AUDIO 0x02       /* file carries an audio track: an audio
                                    * sub-header follows the ramp in the file
                                    * header, and an IMA-ADPCM payload is appended
                                    * after the whole video body (see below). Was
                                    * the never-emitted PERIODIC_KF bit. */
#define TVID_FLAG_SHIFT 0x04       /* SKIP leaves carry a motion-vector sub-variant */
#define TVID_FLAG_HUFF 0x08        /* (whole-stream, i.e. NOT SPLIT) body is
                                    * [u32le raw_len][LZSS+Huffman entropy]. With
                                    * SPLIT this bit instead means TVID_FLAG_COLOR
                                    * (the two never co-occur): SPLIT uses per-plane
                                    * auto-select, so whole-stream HUFF is impossible
                                    * there, freeing 0x08 for the color plane. */
#define TVID_FLAG_COLOR 0x08       /* (with SPLIT only) a parallel COLOR plane
                                    * follows the cell plane(s): one xterm-256 hue
                                    * index per cell, mirroring the cell plane's
                                    * leaf structure (SOLID=1 color, RAW=per-cell,
                                    * PAL2=2 colors + the same selector bits, SKIP
                                    * keeps the previous color). The decoder paints
                                    * the cell's luma/glyph shape in this hue (see
                                    * xterm256.h). Auto-set when the encoder is asked
                                    * to add color (--color). v3 only. */
#define TVID_FLAG_SPLIT 0x10       /* block frames split into structure + cell planes
                                    * (each plane independently entropy-coded). Body
                                    * is a sequence of plane chunks, each
                                    *   [u8 method][u32 raw_len][u32 payload_len][bytes]
                                    * method 0=raw 1=lzss 2=lzss+huff
                                    * 3=adaptive order-1 range (range.c). Plane order:
                                    *   structure, [mode if MODEPLANE],
                                    *   cell (or raster+palette if CELLSPLIT),
                                    *   [color if TVID_FLAG_COLOR].
                                    * struct plane = per-frame [u16 len][len bytes];
                                    * cell plane = keyframe cells then leaf cells;
                                    * color plane = keyframe colors then leaf colors,
                                    *   built by the identical leaf walk as cells. */
#define TVID_FLAG_MODEPLANE 0x20   /* (with SPLIT) per-leaf mode tags live in their
                                    * own byte plane (1 byte/leaf) between the
                                    * structure and cell planes, instead of inline in
                                    * the structure bits. Auto-selected by the encoder
                                    * when it compresses smaller (color video). */
#define TVID_FLAG_MODERLE 0x80     /* (with SPLIT+MODEPLANE) the mode-tag plane is
                                    * zero-run-length-encoded before entropy coding:
                                    * each byte is literal, and after a 0 (SKIP) byte
                                    * comes a count byte of additional consecutive
                                    * zeros (0..255, longer runs split). The decoder
                                    * expands it once into RAM after decompression.
                                    * Auto-selected when it compresses smaller. */
#define TVID_FLAG_CELLSPLIT 0x40   /* (with SPLIT) the cell bytes are carried in TWO
                                    * planes by leaf-mode class instead of one: a
                                    * raster plane (keyframe cells then RAW-leaf cells)
                                    * and a palette plane (SOLID + PAL2 cells), each
                                    * entropy-coded with its own table. RAW reads the
                                    * raster cursor, SOLID/PAL2 the palette cursor.
                                    * Plane order: structure, [mode if MODEPLANE],
                                    * raster, palette. Auto-selected when smaller. */

/* SHIFT motion vectors. A SKIP leaf, when TVID_FLAG_SHIFT is set, is followed by
 * one "moved" bit; if set, two TVID_SHIFT_BITS-wide biased components select an
 * offset (dx,dy) and the leaf is copied from the *previous* frame at that offset
 * (sources clamped to the grid). dx==dy==0 is just classic SKIP, so the encoder
 * never emits a zero motion vector - it codes moved=0 instead. */
#define TVID_SHIFT_BITS 4          /* bits per motion-vector component */
#define TVID_SHIFT_BIAS 8          /* stored = component + bias (covers -8..+7) */
#define TVID_SHIFT_MIN  (-8)
#define TVID_SHIFT_MAX  (7)

/* Decoder capability bits: which optional leaf fields are present in the stream.
 * Derived from the header flags and passed to the decoder so the bitstream
 * parser reads exactly the bits the encoder wrote. */
#define TVID_CAP_SHIFT  TVID_FLAG_SHIFT

/* Frame stream layout (after the header):
 *   keyframe:    [TVID_FRAME_KEY]   [cols*rows raw cell bytes]
 *   block frame: [TVID_FRAME_BLOCK] [u16le payload_len][payload_len bytes]
 * The u16 length lets the decoder stream one frame at a time into a fixed,
 * tiny buffer (no whole-file load) on RAM-starved targets. */
#define TVID_FRAME_KEY   0
#define TVID_FRAME_BLOCK 2 /* v2 quadtree block frame (1 is the retired v1 delta) */

/* Quadtree leaf modes (3 bits in the block bitstream). The decoder is a switch
 * over these; all the cleverness (which mode, which colors, where to split) is
 * encoder-side and leaves no trace but the chosen bytes. */
#define TVID_MODE_SKIP  0 /* leaf unchanged from prev frame (no payload)        */
#define TVID_MODE_SOLID 1 /* one cell byte fills the leaf                       */
#define TVID_MODE_RAW   2 /* w*h literal cell bytes                             */
#define TVID_MODE_PAL2  3 /* 2 cell bytes + (w*h)-bit mask: per-cell 1-of-2     */
#define TVID_MODE_BITS  2 /* bits used to code a mode (values 0..3)             */

/* The cell byte (v3): brightness + a sub-cell shape glyph. The byte packs a luma
 * level in the high TVID_MONO_LUMA_BITS bits (decoder paints it as a grayscale
 * foreground level, or - with the color plane - scales the cell's hue by it) and a
 * glyph index in the low TVID_MONO_GLYPH_BITS bits (a Braille dot pattern giving
 * the exact 2x4 sub-cell ink mask; see glyphset.h). The block layer (codec.c,
 * blockcoder) is byte-opaque and shared unchanged. Color, when present, is a
 * SEPARATE plane (TVID_FLAG_COLOR, xterm256.h) - it does not touch this byte.
 *
 * The split between luma and glyph bits is a compile-time knob so the -DTVID_PROBE
 * build can sweep candidates (2+6, 0+8, 4+4, 3+5) before the format is committed.
 * The shipped default is 2 luma + 6 glyph. Always 8 bits total. */
#ifndef TVID_MONO_LUMA_BITS
#define TVID_MONO_LUMA_BITS 2
#endif
#define TVID_MONO_GLYPH_BITS (8 - TVID_MONO_LUMA_BITS)
#define TVID_MONO_LUMA_LEVELS (1 << TVID_MONO_LUMA_BITS)
#define TVID_MONO_GLYPH_COUNT (1 << TVID_MONO_GLYPH_BITS)
#define TVID_MONO_CELL(luma, glyph) \
    ((uint8_t)(((luma) << TVID_MONO_GLYPH_BITS) | \
               ((glyph) & (TVID_MONO_GLYPH_COUNT - 1))))
#define TVID_CELL_LUMA(c)   ((uint8_t)((c) >> TVID_MONO_GLYPH_BITS))
#define TVID_CELL_MGLYPH(c) ((uint8_t)((c) & (TVID_MONO_GLYPH_COUNT - 1)))

/* Audio track (when TVID_FLAG_AUDIO is set). The video stays the primary stream;
 * audio is a side-channel the decoder plays in lockstep (and paces video to).
 *
 * File layout with audio:
 *   [header][audio sub-header][video body ...][audio payload]
 * The audio sub-header sits right after the ramp in the file header; the ADPCM
 * payload is appended after the entire video body (so the front-to-back video
 * parser is untouched - the player seeks to filesize - audio_bytes to find it,
 * and a strict video-only player simply never reads past the body).
 *
 * Audio sub-header (little-endian), present iff TVID_FLAG_AUDIO:
 *   [u8  audio_codec]     1 = IMA ADPCM (TVID_AUDIO_IMA_ADPCM)
 *                         2 = entropy-coded IMA ADPCM (TVID_AUDIO_IMA_ADPCM_ENT)
 *   [u8  audio_channels]  1 = mono (only value today)
 *   [u16 audio_rate]      sample rate in Hz (e.g. 8000)
 *   [u32 audio_samples]   total PCM sample count (exact stream end + A/V sync)
 *   [u32 audio_bytes]     length of the audio payload appended after the body
 *
 * Codec 1: the payload is a sequence of self-contained IMA blocks (see adpcm.h):
 * the decoder can start at any block boundary, which the DOS auto-init DMA path
 * relies on.
 *
 * Codec 2: same IMA blocks, but each block (or fixed group of blocks) is wrapped
 * in a self-describing entropy chunk so the raw ADPCM nibbles - which carry no
 * entropy stage of their own - are range-coded losslessly (~10-16% smaller; the
 * audio tail is ~90% of file bytes). Chunk shape mirrors the split-body plane
 * chunk: [u8 method][u32le adpcm_len][u32le payload_len][payload], method 0 =
 * stored raw block bytes, 3 = range-coded (range.h). The decoder range-decodes a
 * chunk back to the EXACT codec-1 ADPCM block bytes, then runs the unchanged
 * adpcm_decode_block - so decoded PCM is bit-identical to codec 1. Per-chunk
 * restart points preserve the DOS block-streaming / DMA self-heal property.
 * A strict codec-1-only player rejects audio_codec 2 and plays silent. */
#define TVID_AUDIO_IMA_ADPCM 1
#define TVID_AUDIO_IMA_ADPCM_ENT 2
/* ADPCM blocks grouped per entropy chunk under codec 2. The adaptive range coder
 * needs a long warmup, so per-reset tax is steep at small group sizes (probe
 * sweep on vi/sat: K=4 -> ~96%, K=64 -> ~92%, K=256 -> ~89%, K=1024 -> ~87% ==
 * whole-stream). 256 blocks (~64 s @ 8 kHz) sits at the knee: within ~2% of the
 * whole-stream floor while bounding the decompressed group to 256*1 KB = 256 KB
 * resident -- far under the 2.8 MB whole-PCM the on-demand path exists to avoid,
 * and a clean DOS restart point every ~64 s. */
#define TVID_AUDIO_ENT_GROUP 256
#define TVID_AUDIO_SUBHEADER_BYTES 12 /* 1+1+2+4+4 */

/* On-disk header. Written/read field-by-field (no struct packing reliance). */
typedef struct {
    uint8_t  version;
    uint8_t  flags;
    uint8_t  cols;
    uint8_t  rows;
    uint8_t  fps;
    uint32_t frame_count;
    uint16_t seg_frames; /* (v4 only) frames per segment; 0 for v3. On the wire it
                          * follows frame_count when version >= 4. */
    uint8_t  ramp_len;
    char     ramp[64]; /* ASCII glyph ramp, dark->light; ramp_len entries used */
    /* Audio sub-header (valid only when flags & TVID_FLAG_AUDIO). */
    uint8_t  audio_codec;
    uint8_t  audio_channels;
    uint16_t audio_rate;
    uint32_t audio_samples;
    uint32_t audio_bytes;
} tvid_header;

#ifdef __cplusplus
}
#endif

#endif /* TVID_FORMAT_H */
