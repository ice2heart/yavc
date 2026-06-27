// quantize.cpp - rgb24 frame -> termvideo v3 cells + color (C++23).
#include "quantize.hpp"

extern "C" {
#include "glyphset.h"
#include "mono_celldist.h"
#include "ramp.h"        /* rgb_luma */
#include "tvid_format.h"
#include "xterm256.h"
}

std::vector<uint8_t> subpixel_frame(std::span<const uint8_t> rgb,
                                    int cols, int rows) {
    // Source is (cols*SUBW) x (rows*SUBH) rgb24; box-average each sub-pixel's
    // source region (here 1 source pixel per sub-pixel) into one 8-bit luma.
    const int sw = cols * TVID_MONO_SUBW;
    std::vector<uint8_t> sub((size_t)cols * rows * TVID_MONO_SUBN);
    for (int cy = 0; cy < rows; ++cy)
        for (int cx = 0; cx < cols; ++cx) {
            size_t base = ((size_t)cy * cols + cx) * TVID_MONO_SUBN;
            for (int sy = 0; sy < TVID_MONO_SUBH; ++sy)
                for (int sx = 0; sx < TVID_MONO_SUBW; ++sx) {
                    int px = cx * TVID_MONO_SUBW + sx;
                    int py = cy * TVID_MONO_SUBH + sy;
                    size_t idx = ((size_t)py * sw + px) * 3;
                    int l = rgb_luma(rgb[idx], rgb[idx + 1], rgb[idx + 2]);
                    sub[base + sy * TVID_MONO_SUBW + sx] = (uint8_t)l;
                }
        }
    return sub;
}

std::vector<uint8_t> cellcolor_frame(std::span<const uint8_t> rgb,
                                     int cols, int rows) {
    // Same source layout as subpixel_frame ((cols*SUBW) x (rows*SUBH) rgb24): box-
    // average all of a cell's sub-pixels into one RGB, then snap to the nearest
    // xterm-256 index. One hue byte per cell; brightness comes from the cell's luma
    // level at render (xterm256.h tvid_xterm256_rgb_dim), so we average raw hue
    // here and let luma carry brightness.
    const int sw = cols * TVID_MONO_SUBW;
    std::vector<uint8_t> color((size_t)cols * rows);
    for (int cy = 0; cy < rows; ++cy)
        for (int cx = 0; cx < cols; ++cx) {
            long r = 0, g = 0, b = 0;
            for (int sy = 0; sy < TVID_MONO_SUBH; ++sy)
                for (int sx = 0; sx < TVID_MONO_SUBW; ++sx) {
                    int px = cx * TVID_MONO_SUBW + sx;
                    int py = cy * TVID_MONO_SUBH + sy;
                    size_t idx = ((size_t)py * sw + px) * 3;
                    r += rgb[idx]; g += rgb[idx + 1]; b += rgb[idx + 2];
                }
            int n = TVID_MONO_SUBN;
            color[(size_t)cy * cols + cx] =
                tvid_xterm256_nearest((int)(r / n), (int)(g / n), (int)(b / n));
        }
    return color;
}

std::vector<uint8_t> quantize_mono(const std::vector<uint8_t> &sub,
                                   int cols, int rows) {
    std::vector<uint8_t> cells((size_t)cols * rows);
    for (int c = 0; c < cols * rows; ++c)
        cells[c] = tvid_mono_quantize_joint(&sub[(size_t)c * TVID_MONO_SUBN]);
    return cells;
}
