// quantize.hpp - rgb24 frame -> termvideo v3 cells + color (C++23, offline).
#ifndef QUANTIZE_HPP
#define QUANTIZE_HPP

#include <cstdint>
#include <span>
#include <vector>

// Collapse a (cols*SUBW) x (rows*SUBH) rgb24 source into a per-cell 2x4 sub-pixel
// LUMA block. Source pixels inside a cell's sub-region are box-averaged into each
// of the TVID_MONO_SUBN sub-pixels. Returns cols*rows*TVID_MONO_SUBN bytes (8-bit
// luma, row-major within each cell, cells in raster order). This is what the
// mono quantizer and distortion metric consume.
std::vector<uint8_t> subpixel_frame(std::span<const uint8_t> rgb, int cols, int rows);

// Per-cell HUE for the color plane (TVID_FLAG_COLOR): box-average each cell's
// sub-pixel RGB region from the same (cols*SUBW) x (rows*SUBH) rgb24 source and
// snap to the nearest xterm-256 palette index. Returns cols*rows bytes (one hue
// index per cell). Brightness is carried by the cell's luma level at render time,
// so this is pure hue.
std::vector<uint8_t> cellcolor_frame(std::span<const uint8_t> rgb, int cols, int rows);

// Mono quantizer over a per-cell sub-pixel block (cols*rows*TVID_MONO_SUBN, from
// subpixel_frame): brute-force the best (luma level, glyph) per cell. Returns a
// cols*rows cell grid of packed mono bytes (the v3 cell byte: luma + glyph).
std::vector<uint8_t> quantize_mono(const std::vector<uint8_t> &sub,
                                   int cols, int rows);

#endif // QUANTIZE_HPP
