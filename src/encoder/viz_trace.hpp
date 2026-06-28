// viz_trace.hpp - codec-visualizer trace model (host-only, behind TVID_VIZ).
//
// The visualizer target (src/visualizer/) drives the real encode pipeline and
// captures, per frame, the finished quadtree the serializer walks plus the
// per-plane byte accounting. The capture hook lives in blockcoder.cpp behind
// `#if defined(TVID_VIZ)`; it appends VizLeaf records to whatever VizFrame the
// visualizer points `g_viz_capture` at. None of this compiles into the shipped
// encoder/player (TVID_VIZ is defined only on the visualizer target), exactly
// like the -DTVID_PROBE byte-accounting scaffolding.
//
// The data is flat and small, so to_json() is a hand-rolled string builder (no
// JSON dependency). The browser fetches one /trace.json and renders it.
#ifndef VIZ_TRACE_HPP
#define VIZ_TRACE_HPP

#if defined(TVID_VIZ)

#include <cstdint>
#include <string>
#include <vector>

// One quadtree leaf as the serializer emitted it: position, size, chosen mode
// and the exact RD numbers the search settled on (not recomputed). Payload cells
// are recorded so the UI can show what each region actually stores.
struct VizLeaf {
    int     x = 0, y = 0, s = 0;   // grid position and edge length (cells)
    uint8_t mode = 0;              // TVID_MODE_SKIP/SOLID/RAW/PAL2
    long    rd = 0;                // lambda*bits + distortion (the RD cost kept)
    bool    shift = false;         // SKIP sub-variant: motion copy
    int     mvx = 0, mvy = 0;      // SHIFT vector (cells), valid when shift
    uint8_t solid = 0;             // SOLID cell byte
    uint8_t pal0 = 0, pal1 = 0;    // PAL2 palette cell bytes

    // Exact bytes this leaf contributed to each plane, captured around its
    // serialization (no recomputation). The structure plane is a bitstream, so
    // it is recorded as the absolute start bit offset within the frame's
    // structure chunk plus the leaf's own bits as MSB-first '0'/'1' chars; the
    // cell and color planes are byte-aligned, so they are the literal bytes
    // appended. Lets the UI show the wire representation of a selected region at
    // each stage.
    long              struct_bit0 = 0;   // start bit offset in the frame's struct bits
    std::string       struct_bits;       // this leaf's bits, MSB-first '0'/'1'
    std::vector<uint8_t> cell_bytes;     // cell-plane bytes (SOLID=1, PAL2=2, RAW=s*s)
    std::vector<uint8_t> color_bytes;    // color-plane bytes (when --color)
};

// Per-plane byte length contributed by one frame (pre-entropy, raw plane bytes).
struct VizPlaneBytes {
    long structure = 0;  // this frame's structure-bit chunk (incl [u16 len])
    long cell = 0;       // cell bytes appended this frame
    long color = 0;      // color (hue) bytes appended this frame, 0 if no --color
};

// One frame's full trace: every leaf, the mode tally, the byte breakdown, and
// the decoded framebuffer (cell + hue) the round-trip produced for this frame so
// the decode stage can render exactly what the player would show.
struct VizFrame {
    bool                  keyframe = false;
    std::vector<VizLeaf>  leaves;             // empty for the keyframe
    long n_skip = 0, n_solid = 0, n_raw = 0, n_pal2 = 0;
    VizPlaneBytes         bytes;
    std::vector<uint8_t>  fb;                 // decoded cells (cols*rows)
    std::vector<uint8_t>  colfb;              // decoded hues (cols*rows) or empty
};

// Whole-clip trace + the metadata the UI needs to lay out the grid and report
// the global plane sizes / entropy methods (filled by the visualizer once the
// planes are entropy-coded with the real auto-select).
struct VizTrace {
    int  cols = 0, rows = 0, fps = 0;
    int  lambda = 0, shift = 0;
    bool color = false;
    std::vector<VizFrame> frames;

    // Global plane sizes after the real per-plane entropy auto-select, so the UI
    // can show "raw -> coded (method)" per plane. Method is the TVID coder id
    // (0 raw, 1 lzss, 2 huffman, 3 range); -1 = plane absent.
    struct PlaneSummary {
        const char *name = "";
        long raw_bytes = 0, coded_bytes = 0;
        int  method = -1;
    };
    std::vector<PlaneSummary> planes;
    long total_bytes = 0;   // header + body (+ audio if embedded)

    // The trace is served in two pieces so the browser never holds the whole
    // thing (per-frame leaves + framebuffers + per-region wire bytes are large):
    //   index_json()    -> metadata + per-frame light summary, no heavy arrays.
    //   frame_json(i)    -> one frame's leaves/fb/colfb, fetched on demand.
    std::string index_json() const;
    std::string frame_json(size_t i) const;
};

// The capture sink the blockcoder hook appends leaves to. The visualizer sets
// this to the current frame's leaf vector before the canonical split encode and
// clears it (null) afterward, so only the canonical pass is recorded (the
// nomode/raster passes run with it null and capture nothing).
extern std::vector<VizLeaf> *g_viz_capture;

#endif // TVID_VIZ
#endif // VIZ_TRACE_HPP
