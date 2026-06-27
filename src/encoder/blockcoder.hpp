// blockcoder.hpp - v3 quadtree block-frame encoder (C++23, offline).
//
// Given the previously-shown cell grid and the new "ideal" cell grid + the
// per-cell 2x4 sub-pixel luma blocks, build the cheapest quadtree
// (rate + lambda*distortion) and serialize it to the v3 block bitstream. Pure
// encoder-side search; the decoder (codec.c) just replays the chosen tree. An
// optional per-cell color (hue) grid is mirrored into a parallel color plane.
#ifndef BLOCKCODER_HPP
#define BLOCKCODER_HPP

#include <cstdint>
#include <vector>

struct BlockCoderParams {
    int    cols, rows;
    // Rate-distortion knob: cost = bits + lambda * distortion. Higher lambda
    // spends more bytes chasing detail; lower lambda favors SKIP/SOLID.
    long   lambda;       // fixed-point, scaled by 256 internally
    // Block-level temporal hysteresis: distortion credit (in the same units as
    // the RD metric) subtracted from a SKIP leaf's cost, so a block that barely
    // changed prefers SKIP and stops shimmering. 0 = off.
    long   block_stable;
    // Motion search radius for the SHIFT sub-variant of SKIP (cells). 0 disables
    // SHIFT entirely (and the stream then carries no "moved" bit). Clamped to
    // TVID_SHIFT_MAX. When > 0, the encoder emits one "moved" bit per SKIP leaf.
    int    shift_range;
    // Per-cell 2x4 sub-pixel luma blocks (cols*rows*TVID_MONO_SUBN) the RD search
    // scores cells against. Set per frame by the caller. (A non-null `sub` arg to
    // the encode functions overrides this; both are the same buffer in practice.)
    const uint8_t *sub = nullptr;
};

// Byte-accounting for the encoder's --stats pass. All
// counts are in *bits* so structure (split/mode/selector/shift) and literal cell
// payloads are on one scale; the caller converts to bytes. Accumulates across
// frames when the same struct is passed to every blockcoder_encode call.
struct BlockStats {
    long split_bits = 0;     // quadtree split flags
    long mode_bits = 0;      // per-leaf mode tags (TVID_MODE_BITS each)
    long shift_bits = 0;     // SHIFT moved-bit + vector components
    long sel_bits = 0;       // PAL2 per-cell selector bits
    long cell_bits = 0;      // SOLID/RAW/PAL2-palette literal cell bytes (*8)
    // per-mode leaf counts, for "where do the bytes go" reporting
    long n_skip = 0, n_solid = 0, n_raw = 0, n_pal2 = 0;
};

// Encode one block frame. prev[] = cells currently on screen, ideal[] = cells we
// would like to show, sub[] = cols*rows*TVID_MONO_SUBN sub-pixel luma blocks for
// distortion (or null to use p.sub). Returns the serialized payload (may be empty
// if everything SKIPs). When stats is non-null, accumulates a structure-vs-literal
// bit breakdown into it.
std::vector<uint8_t> blockcoder_encode(const uint8_t *prev, const uint8_t *ideal,
                                       const uint8_t *sub,
                                       const BlockCoderParams &p,
                                       BlockStats *stats = nullptr);

// Split-stream variant (TVID_FLAG_SPLIT): returns this frame's *structure* bits
// (split/mode/selector/shift, byte-aligned) and *appends* this frame's cell bytes
// to cellplane (kept contiguous across frames). Same RD tree as the interleaved
// encode; only where cell bytes land differs. Decoder: codec_decode_block_split.
// modeplane (optional): pull per-leaf mode tags into their own byte plane
// (TVID_FLAG_MODEPLANE). palplane (optional): route SOLID/PAL2 palette cells into
// a separate plane from RAW cells (TVID_FLAG_CELLSPLIT); RAW cells stay in
// cellplane. color (optional) + colorplane (optional): when both non-null, append
// this frame's per-cell hue (xterm-256 index) to colorplane mirroring the
// cell-byte leaf structure (TVID_FLAG_COLOR). All are independent layout knobs
// over the same RD tree.
std::vector<uint8_t> blockcoder_encode_split(const uint8_t *prev,
                                             const uint8_t *ideal,
                                             const uint8_t *sub,
                                             const BlockCoderParams &p,
                                             std::vector<uint8_t> &cellplane,
                                             BlockStats *stats = nullptr,
                                             std::vector<uint8_t> *modeplane = nullptr,
                                             std::vector<uint8_t> *palplane = nullptr,
                                             const uint8_t *color = nullptr,
                                             std::vector<uint8_t> *colorplane = nullptr);

#endif // BLOCKCODER_HPP
