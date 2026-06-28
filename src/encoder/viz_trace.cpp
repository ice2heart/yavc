// viz_trace.cpp - JSON serialization of the codec visualizer trace (TVID_VIZ).
#if defined(TVID_VIZ)

#include "viz_trace.hpp"

#include <cstdio>

extern "C" {
#include "tvid_format.h"
}

std::vector<VizLeaf> *g_viz_capture = nullptr;

namespace {

void put_u8_array(std::string &o, const std::vector<uint8_t> &v) {
    o += '[';
    char buf[8];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ',';
        std::snprintf(buf, sizeof buf, "%u", (unsigned)v[i]);
        o += buf;
    }
    o += ']';
}

void put_kv(std::string &o, const char *k, long v, bool comma = true) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "\"%s\":%ld", k, v);
    o += buf;
    if (comma) o += ',';
}

const char *mode_name(uint8_t m, bool shift) {
    switch (m) {
    case TVID_MODE_SKIP:  return shift ? "SHIFT" : "SKIP";
    case TVID_MODE_SOLID: return "SOLID";
    case TVID_MODE_RAW:   return "RAW";
    case TVID_MODE_PAL2:  return "PAL2";
    }
    return "?";
}

// One frame's light summary (no leaves / framebuffer / wire bytes) for the index.
void put_frame_summary(std::string &o, const VizFrame &fr) {
    char buf[128];
    o += '{';
    std::snprintf(buf, sizeof buf, "\"keyframe\":%s,",
                  fr.keyframe ? "true" : "false");
    o += buf;
    put_kv(o, "n_skip", fr.n_skip);
    put_kv(o, "n_solid", fr.n_solid);
    put_kv(o, "n_raw", fr.n_raw);
    put_kv(o, "n_pal2", fr.n_pal2);
    std::snprintf(buf, sizeof buf,
        "\"bytes\":{\"structure\":%ld,\"cell\":%ld,\"color\":%ld}",
        fr.bytes.structure, fr.bytes.cell, fr.bytes.color);
    o += buf;
    o += '}';
}

} // namespace

// Index: metadata + global plane summary + a light per-frame summary. No leaves,
// no framebuffers, no per-region wire bytes -- those come from frame_json(i). The
// browser holds only this small object plus whatever single frame it is viewing.
std::string VizTrace::index_json() const {
    std::string o;
    o.reserve(1 << 14);
    char buf[128];

    o += '{';
    put_kv(o, "cols", cols);
    put_kv(o, "rows", rows);
    put_kv(o, "fps", fps);
    put_kv(o, "lambda", lambda);
    put_kv(o, "shift", shift);
    std::snprintf(buf, sizeof buf, "\"color\":%s,", color ? "true" : "false");
    o += buf;
    put_kv(o, "total_bytes", total_bytes);
    put_kv(o, "nframes", (long)frames.size());

    o += "\"planes\":[";
    for (size_t i = 0; i < planes.size(); ++i) {
        const PlaneSummary &p = planes[i];
        if (i) o += ',';
        std::snprintf(buf, sizeof buf,
            "{\"name\":\"%s\",\"raw\":%ld,\"coded\":%ld,\"method\":%d}",
            p.name, p.raw_bytes, p.coded_bytes, p.method);
        o += buf;
    }
    o += "],";

    o += "\"frames\":[";
    for (size_t f = 0; f < frames.size(); ++f) {
        if (f) o += ',';
        put_frame_summary(o, frames[f]);
    }
    o += "]}";
    return o;
}

// One frame's heavy data: its leaves (with per-region wire bytes) and the
// decoded framebuffer(s). Fetched on demand as the user steps to a frame.
std::string VizTrace::frame_json(size_t f) const {
    std::string o;
    if (f >= frames.size()) return std::string("{}");
    const VizFrame &fr = frames[f];
    o.reserve(1 << 18);
    char buf[128];

    o += '{';
    std::snprintf(buf, sizeof buf, "\"keyframe\":%s,",
                  fr.keyframe ? "true" : "false");
    o += buf;

    o += "\"leaves\":[";
    for (size_t i = 0; i < fr.leaves.size(); ++i) {
        const VizLeaf &l = fr.leaves[i];
        if (i) o += ',';
        std::snprintf(buf, sizeof buf,
            "{\"x\":%d,\"y\":%d,\"s\":%d,\"mode\":\"%s\",\"rd\":%ld",
            l.x, l.y, l.s, mode_name(l.mode, l.shift), l.rd);
        o += buf;
        if (l.mode == TVID_MODE_SKIP && l.shift) {
            std::snprintf(buf, sizeof buf, ",\"mvx\":%d,\"mvy\":%d", l.mvx, l.mvy);
            o += buf;
        } else if (l.mode == TVID_MODE_SOLID) {
            std::snprintf(buf, sizeof buf,
                ",\"cell\":%u,\"luma\":%u,\"glyph\":%u", (unsigned)l.solid,
                (unsigned)TVID_CELL_LUMA(l.solid),
                (unsigned)TVID_CELL_MGLYPH(l.solid));
            o += buf;
        } else if (l.mode == TVID_MODE_PAL2) {
            std::snprintf(buf, sizeof buf, ",\"pal0\":%u,\"pal1\":%u",
                          (unsigned)l.pal0, (unsigned)l.pal1);
            o += buf;
        }
        // Exact wire representation of this region per plane: the structure bits
        // it wrote (with their start offset), and the cell/color bytes.
        std::snprintf(buf, sizeof buf, ",\"sbit0\":%ld,\"sbits\":\"", l.struct_bit0);
        o += buf;
        o += l.struct_bits;       // already only '0'/'1' chars
        o += "\",\"cbytes\":";
        put_u8_array(o, l.cell_bytes);
        o += ",\"clbytes\":";
        put_u8_array(o, l.color_bytes);
        o += '}';
    }
    o += "],";

    o += "\"fb\":";
    put_u8_array(o, fr.fb);
    if (!fr.colfb.empty()) {
        o += ",\"colfb\":";
        put_u8_array(o, fr.colfb);
    }
    o += '}';
    return o;
}

#endif // TVID_VIZ
