// visualizer.cpp - codec process visualizer (host-only, TVID_VIZ).
//
// Drives the *real* v3 encode pipeline on an rgb24 frame stream (stdin, exactly
// the ffmpeg pipe tools/encode.sh feeds the encoder), captures the finished
// quadtree + per-plane byte accounting + the round-tripped framebuffer per frame
// into a VizTrace, then serves that trace as JSON alongside a small static web UI
// (civetweb) so you can step stage-by-stage and frame-by-frame through how the
// codec stores the video. The codec is the single source of truth: the trace is
// captured from the same Tree the serializer walks (see blockcoder.cpp's
// g_viz_capture hook), so the picture is exactly what would ship.
//
//   ffmpeg -i in.mp4 -vf "fps=10,scale=160:96" -pix_fmt rgb24 -f rawvideo - \
//     | visualizer --color --lambda 6 [--port 8080]
//
// then open the printed URL. The encode runs once at startup; the server then
// just answers /trace.json and the static assets.
#include "enc_stages.hpp"
#include "quantize.hpp"
#include "viz_trace.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "codec.h"
#include "tvid_format.h"
#include "range.h"
#include "lzss.h"
#include "entropy.h"
#include "civetweb.h"
}

// --- plane entropy auto-select (mirrors enc_stages.cpp::append_plane) ----------
// We only need the winning method + size for the global plane summary, so this
// recomputes the same candidates append_plane does and returns (method, coded).
static void best_plane(const std::vector<uint8_t> &plane, int *method, long *coded) {
    const long n = (long)plane.size();
    if (n == 0) { *method = 0; *coded = 0; return; }
    std::vector<uint8_t> lz(n + n / 16 + 64), hf(n + n / 16 + 1024),
        hf0(n + n / 16 + 1024), rc(n + n / 16 + 1024);
    long lc = lzss_compress(plane.data(), n, lz.data(), (long)lz.size());
    long hc = entropy_compress(plane.data(), n, hf.data(), (long)hf.size());
    long hc0 = entropy_compress_nolz(plane.data(), n, hf0.data(), (long)hf0.size());
    long rcc = range_compress(plane.data(), n, rc.data(), (long)rc.size());
    long best = n; int m = 0;
    if (lc > 0 && lc < best)  { best = lc;  m = 1; }
    if (hc > 0 && hc < best)  { best = hc;  m = 2; }
    if (hc0 > 0 && hc0 < best) { best = hc0; m = 2; }
    if (rcc > 0 && rcc < best) { best = rcc; m = 3; }
    *method = m; *coded = best;
}

// --- the encode + capture pass ------------------------------------------------
// A faithful copy of pass2_encode's canonical split path, instrumented to record
// the trace. We do not call pass2_encode itself because it runs three encode
// passes per frame (canonical/nomode/raster) and we want capture only on the
// canonical one; running just that pass here keeps the bytestream identical (same
// RD tree, same round-trip) while giving us clean per-frame hooks.
static void run_encode(const EncoderConfig &cfg, EncoderState &st, VizTrace &tr) {
    tr.cols = cfg.cols; tr.rows = cfg.rows; tr.fps = cfg.fps;
    tr.lambda = cfg.lambda; tr.shift = cfg.shift; tr.color = cfg.color;

    const int caps = cfg.shift > 0 ? TVID_CAP_SHIFT : 0;
    auto quantize = [&](const std::vector<uint8_t> &avg) {
        return quantize_mono(avg, cfg.cols, cfg.rows);
    };

    std::vector<uint8_t> shown = quantize(st.targets[0]);   // decoder's current cells
    st.cell_plane.insert(st.cell_plane.end(), shown.begin(), shown.end());
    if (cfg.color)
        st.color_plane.insert(st.color_plane.end(),
                              st.ctargets[0].begin(), st.ctargets[0].end());

    // Frame 0: the keyframe. No leaves; the decoded fb is the keyframe cells.
    {
        VizFrame kf;
        kf.keyframe = true;
        kf.bytes.cell = (long)shown.size();
        kf.bytes.color = cfg.color ? (long)st.ctargets[0].size() : 0;
        kf.fb = shown;
        if (cfg.color) kf.colfb = st.ctargets[0];
        tr.frames.push_back(std::move(kf));
    }

    BlockCoderParams bp;
    bp.cols = cfg.cols; bp.rows = cfg.rows;
    bp.lambda = cfg.lambda; bp.block_stable = (long)cfg.block_stable;
    bp.shift_range = cfg.shift;
    bp.split_lookahead = cfg.split_lookahead; // split-coarsening lever B (default 2);
                                              // future_sub set per frame below

    long cell_pos = (long)st.cell_plane.size();
    long color_pos = (long)st.color_plane.size();
    std::vector<uint8_t> shown_color;
    if (cfg.color) shown_color = st.ctargets[0];
    std::vector<uint8_t> prev_snapshot(shown.size());

    for (size_t f = 1; f < st.targets.size(); ++f) {
        std::vector<uint8_t> ideal = quantize(st.targets[f]);
        prev_snapshot = shown;
        bp.sub = st.targets[f].data();
        // Split lookahead (lever B): mirror pass2_encode -- hand the coder the next
        // split_lookahead frames' sub-pixel blocks so the captured tree matches the
        // real encoder's. No-op when split_lookahead == 0.
        std::vector<const uint8_t *> fut_sub;
        if (cfg.split_lookahead > 0) {
            for (int k = 1; k <= cfg.split_lookahead && f + (size_t)k < st.targets.size(); ++k)
                fut_sub.push_back(st.targets[f + k].data());
        }
        bp.future_sub = fut_sub.empty() ? nullptr : fut_sub.data();
        bp.future_ideal = nullptr;
        bp.future_n = (int)fut_sub.size();
        const uint8_t *frame_color = cfg.color ? st.ctargets[f].data() : nullptr;

        VizFrame vf;
        const size_t cell_before = st.cell_plane.size();
        const size_t color_before = st.color_plane.size();

        // Canonical split encode, with leaf capture into this frame's record.
        g_viz_capture = &vf.leaves;
        std::vector<uint8_t> sbits = blockcoder_encode_split(
            shown.data(), ideal.data(), st.targets[f].data(), bp, st.cell_plane,
            nullptr, nullptr, nullptr,
            frame_color, cfg.color ? &st.color_plane : nullptr);
        g_viz_capture = nullptr;
        if (sbits.size() > 0xFFFF) enc_die("split structure frame exceeds u16 length");

        // Round-trip exactly as the player will, threading the cell/color cursors.
        long np = cell_pos;
        long dummy_mode = 0, dummy_pal = 0;
        if (codec_decode_block_split(
                sbits.data(), (long)sbits.size(),
                st.cell_plane.data(), (long)st.cell_plane.size(), &np,
                nullptr, 0, &dummy_mode,
                nullptr, 0, &dummy_pal,
                cfg.color ? st.color_plane.data() : nullptr,
                cfg.color ? (long)st.color_plane.size() : 0, &color_pos,
                shown.data(), cfg.color ? shown_color.data() : nullptr,
                prev_snapshot.data(), cfg.cols, cfg.rows, caps) != 0)
            enc_die("internal: split frame failed to round-trip");
        cell_pos = np;

        // Accumulate the structure plane just like pass2 (incl the [u16 len]).
        st.struct_stream.push_back((uint8_t)(sbits.size() & 0xFF));
        st.struct_stream.push_back((uint8_t)((sbits.size() >> 8) & 0xFF));
        st.struct_stream.insert(st.struct_stream.end(), sbits.begin(), sbits.end());

        // Per-frame byte breakdown (pre-entropy, raw plane bytes).
        vf.bytes.structure = 2 + (long)sbits.size();
        vf.bytes.cell = (long)(st.cell_plane.size() - cell_before);
        vf.bytes.color = (long)(st.color_plane.size() - color_before);
        for (const VizLeaf &l : vf.leaves) {
            switch (l.mode) {
            case TVID_MODE_SKIP:  vf.n_skip++;  break;
            case TVID_MODE_SOLID: vf.n_solid++; break;
            case TVID_MODE_RAW:   vf.n_raw++;   break;
            case TVID_MODE_PAL2:  vf.n_pal2++;  break;
            }
        }
        vf.fb = shown;
        if (cfg.color) vf.colfb = shown_color;
        tr.frames.push_back(std::move(vf));
    }

    // Global plane summary through the real entropy auto-select.
    auto add_plane = [&](const char *name, const std::vector<uint8_t> &p) {
        VizTrace::PlaneSummary s; s.name = name; s.raw_bytes = (long)p.size();
        if (p.empty()) { s.method = -1; s.coded_bytes = 0; }
        else best_plane(p, &s.method, &s.coded_bytes);
        tr.planes.push_back(s);
    };
    add_plane("structure", st.struct_stream);
    add_plane("cell", st.cell_plane);
    if (cfg.color) add_plane("color", st.color_plane);
    for (const auto &p : tr.planes) tr.total_bytes += p.coded_bytes;
}

// --- civetweb handlers --------------------------------------------------------
// The whole trace is large (per-frame leaves + framebuffers + per-region wire
// bytes), so we never serialize it all at once. We keep the captured trace alive
// and serve a small index (/trace.json) plus per-frame chunks (/frame/N.json)
// generated on demand, so the browser only ever holds one frame's heavy data.
static const VizTrace *g_trace = nullptr;
static std::string g_index_json;
static std::string g_webroot;

static std::string slurp(const std::string &path) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, n);
    std::fclose(fp);
    return s;
}

static void send(struct mg_connection *c, const char *ctype, const std::string &body) {
    mg_printf(c,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n\r\n",
        ctype, body.size());
    mg_write(c, body.data(), body.size());
}

static int handle_trace(struct mg_connection *c, void *) {
    send(c, "application/json", g_index_json);
    return 200;
}

// /frame/N.json -> that frame's heavy data, serialized on demand.
static int handle_frame(struct mg_connection *c, void *) {
    const struct mg_request_info *ri = mg_get_request_info(c);
    const char *uri = ri->local_uri ? ri->local_uri : "";
    long n = -1;
    if (std::sscanf(uri, "/frame/%ld", &n) != 1 || n < 0 || !g_trace ||
        (size_t)n >= g_trace->frames.size()) {
        mg_printf(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        return 404;
    }
    send(c, "application/json", g_trace->frame_json((size_t)n));
    return 200;
}

// Serve a static asset from the webroot (index.html, app.js, app.css). Maps "/"
// to index.html. Path is fixed to the known asset set, so no traversal risk.
static int handle_static(struct mg_connection *c, void *) {
    const struct mg_request_info *ri = mg_get_request_info(c);
    std::string uri = ri->local_uri ? ri->local_uri : "/";
    const char *fname = "index.html"; const char *ctype = "text/html";
    if (uri == "/" || uri == "/index.html") { fname = "index.html"; ctype = "text/html"; }
    else if (uri == "/app.js")  { fname = "app.js";  ctype = "application/javascript"; }
    else if (uri == "/app.css") { fname = "app.css"; ctype = "text/css"; }
    else { mg_printf(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); return 404; }
    std::string body = slurp(g_webroot + "/" + fname);
    if (body.empty()) {
        mg_printf(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        return 404;
    }
    send(c, ctype, body);
    return 200;
}

int main(int argc, char **argv) {
    // Reuse the encoder's arg parser for the codec knobs (--color/--lambda/...);
    // pull out our own --port and --webroot first.
    std::string port = "8080";
    g_webroot = "src/visualizer/web";
    std::vector<char *> passthru;
    passthru.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
        else if (!std::strcmp(argv[i], "--webroot") && i + 1 < argc) g_webroot = argv[++i];
        else passthru.push_back(argv[i]);
    }
    // The visualizer always uses the split body (the shipped layout it visualizes).
    // parse_args requires --out (the encoder writes a file there); the visualizer
    // never writes one, so satisfy the check with a discard path.
    static char split_flag[] = "--split";
    static char out_flag[] = "--out";
    static char out_path[] = "/dev/null";
    passthru.push_back(split_flag);
    passthru.push_back(out_flag);
    passthru.push_back(out_path);

    EncoderConfig cfg = parse_args((int)passthru.size(), passthru.data());
    cfg.split = true;
    cfg.stats = true;

    EncoderState st;
    std::fprintf(stderr, "visualizer: reading rgb24 frames from stdin...\n");
    pass1a_read_frames(cfg, st);
    pass1b_hysteresis(cfg, st);

    static VizTrace tr;
    run_encode(cfg, st, tr);
    g_trace = &tr;
    g_index_json = tr.index_json();
    std::fprintf(stderr,
        "visualizer: captured %zu frames; index %zu B, frames served on demand\n",
        tr.frames.size(), g_index_json.size());

    const char *opts[] = {
        "listening_ports", port.c_str(),
        "num_threads", "2",
        nullptr };
    struct mg_callbacks cb;
    std::memset(&cb, 0, sizeof cb);
    struct mg_context *ctx = mg_start(&cb, nullptr, opts);
    if (!ctx) { std::fprintf(stderr, "visualizer: failed to start server\n"); return 1; }
    mg_set_request_handler(ctx, "/trace.json", handle_trace, nullptr);
    mg_set_request_handler(ctx, "/frame/", handle_frame, nullptr);
    mg_set_request_handler(ctx, "/", handle_static, nullptr);

    std::fprintf(stderr,
        "visualizer: serving on http://localhost:%s/  (webroot: %s)\n"
        "visualizer: Ctrl-C to stop.\n", port.c_str(), g_webroot.c_str());
    for (;;) {
#if defined(_WIN32)
        Sleep(1000);
#else
        struct timespec ts{1, 0};
        nanosleep(&ts, nullptr);
#endif
    }
    mg_stop(ctx);
    return 0;
}
