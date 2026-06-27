/* player.c - termvideo decoder/playback loop (portable C).
 *
 * Raw streams are read one frame at a time straight from the file (tiny RAM).
 * LZSS streams are decompressed whole into a buffer first, then played from
 * memory; the frame parser is identical either way via the `src` abstraction. */
#include "adpcm.h"
#include "audio.h"
#include "backend.h"
#include "codec.h"
#include "entropy.h"
#include "lzss.h"
#include "mode_rle.h"
#include "prof_dos.h"
#include "range.h"
#include "tvid_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __DOS__
#include <time.h>   /* nanosleep pacing on hosted POSIX; DOS spins on the clock */
#endif

static void die(const char *msg) {
    fprintf(stderr, "player: %s\n", msg);
    exit(1);
}

static uint16_t read_u16le(FILE *fp) {
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) die("truncated header");
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t read_u32le(FILE *fp) {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) die("truncated header");
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static int read_u8(FILE *fp) {
    int c = fgetc(fp);
    if (c == EOF) die("unexpected EOF");
    return c;
}

/* Decoded audio track + playback pacing. When the stream carries audio, the
 * whole ADPCM payload is read from the file tail and decoded once into PCM (3
 * min @ 8 kHz = ~2.8 MB, fine on the host). The video loop then paces itself to
 * the audio clock: before each frame it tops up the audio backend's buffer and
 * waits until the speaker has played past that frame's timestamp, keeping A/V in
 * lockstep. The DOS Sound Blaster path implements the same audio.h interface with
 * auto-init DMA; here we use whatever audio_*.c the host links. */
typedef struct {
    int active;        /* audio present and the backend opened */
    int rate;          /* sample rate (Hz) */
    int16_t *pcm;      /* decoded samples */
    long nsamples;     /* total decoded samples */
    long submitted;    /* samples already handed to audio_submit */
} audio_track;

/* Read and decode the audio tail into at->pcm. fp must be positioned anywhere;
 * the audio payload is the last audio_bytes of the file. Returns 0 on success. */
static int audio_load(audio_track *at, FILE *fp, const tvid_header *h) {
    memset(at, 0, sizeof(*at));
    if (!(h->flags & TVID_FLAG_AUDIO)) return 0;
    if (h->audio_codec != TVID_AUDIO_IMA_ADPCM) return 0; /* unknown: skip audio */

    long here = ftell(fp);
    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long end = ftell(fp);
    long start = end - (long)h->audio_bytes;
    if (start < 0 || fseek(fp, start, SEEK_SET) != 0) { fseek(fp, here, SEEK_SET); return 0; }

    uint8_t *blob = (uint8_t *)malloc((size_t)(h->audio_bytes ? h->audio_bytes : 1));
    if (!blob) return 0;
    if (fread(blob, 1, h->audio_bytes, fp) != h->audio_bytes) { free(blob); fseek(fp, here, SEEK_SET); return 0; }
    fseek(fp, here, SEEK_SET);

    long total = (long)h->audio_samples;
    int16_t *pcm = (int16_t *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int16_t));
    if (!pcm) { free(blob); return 0; }

    /* Walk the self-contained ADPCM blocks, same framing the encoder used. */
    long ip = 0, op = 0;
    while (op < total) {
        long remain = total - op;
        int n = remain > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)remain;
        long bb = adpcm_block_bytes(n);
        if (bb < 0 || ip + bb > (long)h->audio_bytes) break;
        int got = adpcm_decode_block(blob + ip, bb, pcm + op, n);
        if (got != n) break;
        ip += bb; op += n;
    }
    free(blob);

    at->pcm = pcm;
    at->nsamples = op;          /* what we actually decoded (op==total normally) */
    at->rate = h->audio_rate;
    at->submitted = 0;
    if (audio_init(at->rate, h->audio_channels ? h->audio_channels : 1) == 0)
        at->active = 1;         /* else: video still plays, just silent */
    return 0;
}

/* Top up the audio backend so it always has a comfortable lead over the speaker,
 * without submitting the whole track at once (audio_submit blocks when its ring
 * is full). Called once per video frame. */
static void audio_pump(audio_track *at) {
    if (!at->active) return;
    /* Keep ~0.5 s queued ahead of what's been played. */
    long target = audio_played_samples() + at->rate / 2;
    if (target > at->nsamples) target = at->nsamples;
    if (target > at->submitted) {
        audio_submit(at->pcm + at->submitted, target - at->submitted);
        at->submitted = target;
    }
}

/* Pace to the audio clock when audio is active: block until the speaker has
 * played up to (frame_index / fps) seconds; otherwise fall back to the backend's
 * timer. Pumps audio while waiting so the queue never starves. */
static void sync_wait(audio_track *at, uint32_t frame_index, int fps) {
    if (!at->active) { backend_wait_frame(fps); return; }
    long due = (long)((long long)frame_index * at->rate / (fps > 0 ? fps : 1));
    if (due > at->nsamples) due = at->nsamples;
    for (;;) {
        audio_pump(at);
        long played = audio_played_samples();
        if (played >= due) break;
        if (at->submitted >= at->nsamples && played >= at->nsamples)
            break; /* track ended */
#ifdef __DOS__
        /* The SB ISR drives the audio clock and refills the DMA itself; there is
         * no host scheduler to yield to and no syscall sleep. Spin on the played
         * counter (HLT keeps the CPU cool until the next interrupt). audio_pump
         * above keeps the s16 ring topped up for the ISR to consume. */
        __asm { hlt }
#else
        /* Sleep for (almost) the whole remaining gap in one shot instead of
         * busy-spinning a 125 us nap thousands of times per frame: that churned
         * the audio mutex and each nap overshot the scheduler tick, so frames
         * landed late and unevenly (the visible lag). Convert the sample gap to
         * nanoseconds, hold back a hair so we don't oversleep past `due`, then
         * loop once more to land precisely. */
        long gap = due - played;                 /* samples still to play */
        long ns = (long)((long long)gap * 1000000000LL / at->rate);
        ns -= ns / 8;                            /* wake ~12% early, re-check */
        if (ns < 200000L) ns = 200000L;          /* floor ~0.2 ms */
        struct timespec ts;
        ts.tv_sec = ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
        nanosleep(&ts, NULL);
#endif
    }
#ifdef TVID_PROBE
    /* TVID_FRAMELOG=1: print "frame <i> at <ms>" per presented frame so a sweep
     * can verify cadence (inter-frame ms should hover near 1000/fps, not bunch
     * into bursts). Cheap; behind the probe build only. */
    if (getenv("TVID_FRAMELOG")) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double ms = now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
        fprintf(stderr, "frame %u at %.1f (played=%ld due=%ld)\n",
                frame_index, ms, audio_played_samples(), due);
    }
#endif
}

static void audio_free(audio_track *at) {
    if (at->active) { audio_finish(); audio_shutdown(); }
    free(at->pcm);
    at->pcm = NULL;
    at->active = 0;
}

/* Byte source for the frame body: either the file (raw) or an in-memory buffer
 * (a decompressed LZSS stream). */
typedef struct {
    FILE *fp;            /* raw mode: read from here */
    const uint8_t *mem;  /* lzss mode: read from here */
    long pos, len;       /* lzss mode cursor */
} src;

static int src_u8(src *s) {
    if (s->mem) {
        if (s->pos >= s->len) die("unexpected end of stream");
        return s->mem[s->pos++];
    }
    return read_u8(s->fp);
}

static void src_read(src *s, uint8_t *dst, long n) {
    if (s->mem) {
        if (s->pos + n > s->len) die("truncated frame");
        memcpy(dst, s->mem + s->pos, (size_t)n);
        s->pos += n;
    } else if (fread(dst, 1, (size_t)n, s->fp) != (size_t)n) {
        die("truncated frame");
    }
}

/* Read one split-body plane chunk ([u8 method][u32 raw][u32 plen][payload]) from
 * fp and return the decompressed plane (malloc'd; caller frees). Sets *out_len. */
static uint8_t *read_plane(FILE *fp, long *out_len) {
    int method = read_u8(fp);
    uint32_t raw_len = read_u32le(fp);
    uint32_t plen = read_u32le(fp);
    uint8_t *payload = (uint8_t *)malloc(plen ? plen : 1);
    uint8_t *out = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (!payload || !out) die("out of memory");
    if (fread(payload, 1, plen, fp) != plen) die("truncated plane");
    long got;
    if (method == 0) { memcpy(out, payload, raw_len); got = raw_len; }
    else if (method == 1) got = lzss_decompress(payload, plen, out, (long)raw_len);
    else if (method == 2) got = entropy_decompress(payload, plen, out, (long)raw_len);
    else if (method == 3) got = range_decompress(payload, plen, out, (long)raw_len);
    else { die("bad plane method"); return NULL; }
    free(payload);
    if (got != (long)raw_len) die("plane decompress failed");
    *out_len = (long)raw_len;
    return out;
}

/* Like read_plane, but never aborts on an out-of-memory output buffer: it always
 * CONSUMES the chunk from fp (so the file cursor stays correct), and returns NULL
 * if the decompressed plane can't be held. Used for the optional color plane so a
 * RAM-starved target degrades to grayscale instead of failing playback. */
static uint8_t *read_plane_opt(FILE *fp, long *out_len) {
    int method = read_u8(fp);
    uint32_t raw_len = read_u32le(fp);
    uint32_t plen = read_u32le(fp);
    uint8_t *payload = (uint8_t *)malloc(plen ? plen : 1);
    uint8_t *out = (uint8_t *)malloc(raw_len ? raw_len : 1);
    long got;
    if (!payload) { free(out); die("out of memory"); }
    if (fread(payload, 1, plen, fp) != plen) die("truncated plane");
    if (!out) { free(payload); *out_len = 0; return NULL; } /* skip: grayscale */
    if (method == 0) { memcpy(out, payload, raw_len); got = raw_len; }
    else if (method == 1) got = lzss_decompress(payload, plen, out, (long)raw_len);
    else if (method == 2) got = entropy_decompress(payload, plen, out, (long)raw_len);
    else if (method == 3) got = range_decompress(payload, plen, out, (long)raw_len);
    else { die("bad plane method"); return NULL; }
    free(payload);
    if (got != (long)raw_len) die("plane decompress failed");
    *out_len = (long)raw_len;
    return out;
}

int main(int argc, char **argv) {
    if (argc < 2) die("usage: player <file.tvid>");
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) die("cannot open input file");

    /* Header. */
    uint8_t magic[4];
    if (fread(magic, 1, 4, fp) != 4) die("truncated file");
    if (magic[0] != TVID_MAGIC0 || magic[1] != TVID_MAGIC1 ||
        magic[2] != TVID_MAGIC2 || magic[3] != TVID_MAGIC3)
        die("bad magic (not a .tvid file)");

    tvid_header h;
    h.version = (uint8_t)read_u8(fp);
    /* v3 is the only format: each cell is a 2x4 sub-cell glyph at a luma level,
     * with an optional parallel xterm-256 color plane (TVID_FLAG_COLOR). v2 (the
     * retired flat-color model) has no decode path. */
    if (h.version == TVID_VERSION_V2)
        die("v2 streams are no longer supported (re-encode as v3)");
    if (h.version != TVID_VERSION)
        die("unsupported version");
    h.flags = (uint8_t)read_u8(fp);
    /* Refuse any flag this build doesn't understand. 0x08 is HUFF (whole-stream)
     * or, with SPLIT, COLOR -- both are in the accepted set. */
    if (h.flags & ~(uint8_t)(TVID_FLAG_LZSS | TVID_FLAG_AUDIO |
                             TVID_FLAG_SHIFT | TVID_FLAG_HUFF | TVID_FLAG_SPLIT |
                             TVID_FLAG_MODEPLANE | TVID_FLAG_CELLSPLIT |
                             TVID_FLAG_MODERLE | TVID_FLAG_COLOR))
        die("stream uses unsupported feature flags (newer codec?)");
    /* 0x08 means whole-stream HUFF only when NOT split; with SPLIT it is COLOR. */
    const int has_color = (h.flags & TVID_FLAG_SPLIT) && (h.flags & TVID_FLAG_COLOR);
    const int whole_stream_huff = !(h.flags & TVID_FLAG_SPLIT) && (h.flags & TVID_FLAG_HUFF);
    if ((h.flags & TVID_FLAG_LZSS) && whole_stream_huff)
        die("stream sets both LZSS and HUFF (mutually exclusive)");
    h.cols = (uint8_t)read_u8(fp);
    h.rows = (uint8_t)read_u8(fp);
    h.fps = (uint8_t)read_u8(fp);
    h.frame_count = read_u32le(fp);
    h.ramp_len = (uint8_t)read_u8(fp);
    if (h.ramp_len == 0 || h.ramp_len > sizeof(h.ramp)) die("bad ramp length");
    if (fread(h.ramp, 1, h.ramp_len, fp) != h.ramp_len) die("truncated ramp");

    /* Audio sub-header (only when TVID_FLAG_AUDIO): follows the ramp in the file
     * header; the ADPCM payload itself sits at the very end of the file. */
    h.audio_codec = h.audio_channels = 0;
    h.audio_rate = 0; h.audio_samples = h.audio_bytes = 0;
    if (h.flags & TVID_FLAG_AUDIO) {
        h.audio_codec = (uint8_t)read_u8(fp);
        h.audio_channels = (uint8_t)read_u8(fp);
        h.audio_rate = read_u16le(fp);
        h.audio_samples = read_u32le(fp);
        h.audio_bytes = read_u32le(fp);
    }

    const int ncells = h.cols * h.rows;
    uint8_t *fb = (uint8_t *)malloc((size_t)ncells);
    uint8_t *tok = (uint8_t *)malloc((size_t)ncells * 2 + 16);
    if (!fb || !tok) die("out of memory");

    /* SHIFT leaves read a clamped motion offset from the *previous* frame, which
     * may already be partly overwritten in fb; keep a pristine snapshot to read
     * from. Allocated only when the stream uses SHIFT - strict streams pay zero. */
    const int caps = (h.flags & TVID_FLAG_SHIFT) ? TVID_CAP_SHIFT : 0;
    uint8_t *prevfb = NULL;
    if (caps & TVID_CAP_SHIFT) {
        prevfb = (uint8_t *)malloc((size_t)ncells);
        if (!prevfb) die("out of memory");
    }

    /* Split-stream playback: two independently entropy-coded planes (structure
     * bits + cell bytes). Decompress both, then walk frames reading one
     * byte-aligned structure slice per frame and threading the cell cursor. */
    if (h.flags & TVID_FLAG_SPLIT) {
        /* Plane order in the body: structure, [mode plane if MODEPLANE],
         * then cells: one combined plane, or (CELLSPLIT) a raster plane
         * (keyframe + RAW leaves) followed by a palette plane (SOLID/PAL2). */
        long struct_len = 0, mode_len = 0, cell_len = 0, pal_len = 0, color_len = 0;
        uint8_t *struct_plane = read_plane(fp, &struct_len);
        uint8_t *mode_plane = NULL;
        if (h.flags & TVID_FLAG_MODEPLANE) {
            mode_plane = read_plane(fp, &mode_len);
            if (h.flags & TVID_FLAG_MODERLE) {
                long exp_len = mode_rle_decoded_len(mode_plane, mode_len);
                if (exp_len < 0) die("malformed mode RLE plane");
                uint8_t *exp = (uint8_t *)malloc(exp_len ? exp_len : 1);
                if (!exp) die("out of memory");
                if (mode_rle_decode(mode_plane, mode_len, exp) != exp_len)
                    die("malformed mode RLE plane");
                free(mode_plane);
                mode_plane = exp; mode_len = exp_len;
            }
        }
        uint8_t *cell_plane = read_plane(fp, &cell_len);
        uint8_t *pal_plane = NULL;
        if (h.flags & TVID_FLAG_CELLSPLIT) pal_plane = read_plane(fp, &pal_len);
        /* Color plane last (TVID_FLAG_COLOR): per-cell xterm-256 hue indices. The
         * whole plane decompresses resident, like the cell plane; on a RAM-starved
         * target (e.g. DOS/4GW with hi-res + audio) it may not fit. Decode it
         * non-fatally: if the color plane or its hue framebuffer can't be
         * allocated, drop to grayscale rather than aborting the whole playback. */
        int color_active = has_color;
        uint8_t *color_plane = NULL;
        uint8_t *colfb = NULL;
        if (color_active) {
            color_plane = read_plane_opt(fp, &color_len);
            if (!color_plane) color_active = 0; /* couldn't hold it: grayscale */
        }
        audio_track at;
        audio_load(&at, fp, &h); /* reads the audio tail before we close fp */
        fclose(fp);

        /* Per-cell hue framebuffer (persists across frames like fb; SKIP keeps its
         * previous hue). Seeded from the keyframe hues. NULL when grayscale. */
        if (color_active) {
            if (color_len < ncells) die("split color plane too short for keyframe");
            colfb = (uint8_t *)malloc((size_t)ncells);
            if (colfb) {
                memcpy(colfb, color_plane, (size_t)ncells);
            } else {
                free(color_plane); color_plane = NULL; color_active = 0;
            }
        }
        const int has_color_play = color_active; /* what the backend will render */

        /* Keyframe = first ncells bytes of the (raster/combined) cell plane. */
        if (cell_len < ncells) die("split cell plane too short for keyframe");
        memcpy(fb, cell_plane, (size_t)ncells);
        long cell_pos = ncells, mode_pos = 0, pal_pos = 0;
        long color_pos = has_color_play ? ncells : 0;
        long sp = 0;

        backend_init(h.version, has_color_play, h.cols, h.rows, h.ramp, h.ramp_len);
        backend_present(fb, colfb);
        sync_wait(&at, 1, h.fps);

        for (uint32_t f = 1; f < h.frame_count; ++f) {
            PROF_DECL(pt);
            PROF_CAP(f);
            if (backend_poll_quit()) break;
            if (sp + 2 > struct_len) die("truncated split structure plane");
            int len = struct_plane[sp] | (struct_plane[sp + 1] << 8);
            sp += 2;
            if (sp + len > struct_len) die("truncated split structure frame");
            if (prevfb) memcpy(prevfb, fb, (size_t)ncells);
            PROF_T0(pt);
            if (codec_decode_block_split(
                    struct_plane + sp, len, cell_plane, cell_len, &cell_pos,
                    mode_plane, mode_len, &mode_pos,
                    pal_plane, pal_len, &pal_pos,
                    color_plane, color_len, &color_pos,
                    fb, colfb, prevfb ? prevfb : fb, h.cols, h.rows, caps) != 0)
                die("malformed split frame");
            PROF_ACC(t_decode, pt);
            sp += len;
            PROF_T0(pt); backend_present(fb, colfb);    PROF_ACC(t_blit, pt);
            prof_hud_draw();
            PROF_W0(pt); sync_wait(&at, f + 1, h.fps); PROF_WACC(pt);
            prof_frame_end();
        }

        backend_shutdown();
        prof_summary();
        audio_free(&at);
        free(struct_plane); free(mode_plane); free(cell_plane); free(pal_plane);
        free(color_plane); free(colfb);
        free(fb); free(tok); free(prevfb);
        return 0;
    }

    /* Load the audio tail (if any) before we start consuming the body. */
    audio_track at;
    audio_load(&at, fp, &h);

    /* Set up the frame-body source. */
    src s;
    s.fp = fp;
    s.mem = NULL;
    s.pos = s.len = 0;
    uint8_t *decomp = NULL;
    if (h.flags & (TVID_FLAG_LZSS | TVID_FLAG_HUFF)) {
        uint32_t raw_len = read_u32le(fp);
        long here = ftell(fp);
        if (fseek(fp, 0, SEEK_END) != 0) die("file not seekable");
        long end = ftell(fp);
        if (fseek(fp, here, SEEK_SET) != 0) die("file not seekable");
        /* The compressed stream ends where the audio tail begins. */
        long clen = end - here - (long)h.audio_bytes;
        if (clen < 0) die("audio sub-header inconsistent with file size");
        uint8_t *comp = (uint8_t *)malloc((size_t)clen);
        decomp = (uint8_t *)malloc((size_t)raw_len);
        if (!comp || !decomp) die("out of memory");
        if (fread(comp, 1, (size_t)clen, fp) != (size_t)clen)
            die("truncated compressed stream");
        long got = (h.flags & TVID_FLAG_HUFF)
                       ? entropy_decompress(comp, clen, decomp, (long)raw_len)
                       : lzss_decompress(comp, clen, decomp, (long)raw_len);
        free(comp);
        if (got != (long)raw_len) die("decompress failed");
        s.mem = decomp;
        s.len = (long)raw_len;
        s.fp = NULL;
    }

    backend_init(h.version, 0, h.cols, h.rows, h.ramp, h.ramp_len);

    for (uint32_t f = 0; f < h.frame_count; ++f) {
        PROF_DECL(pt);
        PROF_CAP(f);
        if (backend_poll_quit()) break;
        PROF_T0(pt);
        int type = src_u8(&s);
        if (type == TVID_FRAME_KEY) {
            src_read(&s, fb, ncells);
        } else if (type == TVID_FRAME_BLOCK) {
            int lo = src_u8(&s), hi = src_u8(&s);
            int len = lo | (hi << 8);
            src_read(&s, tok, len);
            if (prevfb) {
                memcpy(prevfb, fb, (size_t)ncells);
                if (codec_decode_block_ref(tok, len, fb, prevfb,
                                           h.cols, h.rows, caps) != 0)
                    die("malformed block frame");
            } else if (codec_decode_block(tok, len, fb, h.cols, h.rows) != 0) {
                die("malformed block frame");
            }
        } else {
            die("unknown frame type");
        }
        PROF_ACC(t_decode, pt);
        PROF_T0(pt); backend_present(fb, NULL);    PROF_ACC(t_blit, pt);
        prof_hud_draw();
        PROF_W0(pt); sync_wait(&at, f + 1, h.fps); PROF_WACC(pt);
        prof_frame_end();
    }

    backend_shutdown();
    prof_summary();
    audio_free(&at);
    free(fb);
    free(tok);
    free(prevfb);
    free(decomp);
    fclose(fp);
    return 0;
}
