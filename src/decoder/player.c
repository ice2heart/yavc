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

/* Audio track + playback pacing. The ADPCM payload is read once from the file
 * tail and kept *compressed* resident; blocks are self-contained, so the player
 * decodes them ONE AT A TIME on demand in audio_pump (into a small scratch buffer)
 * instead of expanding the whole track to PCM up front. That keeps the resident
 * audio set at blob + one block (~4 KB) rather than the full decoded PCM (3 min @
 * 8 kHz = ~2.8 MB) -- the difference that lets long clips fit in the DOS DPMI
 * allocation. The video loop paces itself to the audio clock: before each frame it
 * tops up the backend buffer (decoding more blocks as needed) and waits until the
 * speaker has played past that frame's timestamp, keeping A/V in lockstep. The DOS
 * Sound Blaster path implements the same audio.h interface with auto-init DMA. */
typedef struct {
    int active;        /* audio present and the backend opened */
    int rate;          /* sample rate (Hz) */
    uint8_t *blob;     /* resident COMPRESSED ADPCM payload (the file tail) */
    long blob_len;     /* bytes in blob */
    long nsamples;     /* total samples the track will decode to */
    long ip;           /* blob read cursor: byte offset of the next block */
    long decoded;      /* samples decoded+submitted so far (== submitted) */
    long submitted;    /* samples handed to audio_submit (== decoded) */
    int16_t *scratch;  /* one block of PCM, reused per decode (ADPCM_BLOCK_SAMPLES) */
} audio_track;

/* Read the audio tail into at->blob (kept COMPRESSED resident). fp may be
 * positioned anywhere; the audio payload is the last audio_bytes of the file, and
 * the cursor is restored before returning so v4 segment streaming continues from
 * where it was. Blocks are decoded later, on demand, in audio_pump. Returns 0. */
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
    if (!blob) { fseek(fp, here, SEEK_SET); return 0; }
    if (fread(blob, 1, h->audio_bytes, fp) != h->audio_bytes) { free(blob); fseek(fp, here, SEEK_SET); return 0; }
    fseek(fp, here, SEEK_SET);

    /* One block of decode scratch, reused for every block (the backend copies it
     * into its own ring on submit, so we never hold more than one block of PCM). */
    int16_t *scratch = (int16_t *)malloc((size_t)ADPCM_BLOCK_SAMPLES * sizeof(int16_t));
    if (!scratch) { free(blob); return 0; }

    at->blob = blob;
    at->blob_len = (long)h->audio_bytes;
    at->nsamples = (long)h->audio_samples;
    at->scratch = scratch;
    at->ip = 0;
    at->decoded = 0;
    at->submitted = 0;
    at->rate = h->audio_rate;
    if (audio_init(at->rate, h->audio_channels ? h->audio_channels : 1) == 0)
        at->active = 1;         /* else: video still plays, just silent */
    return 0;
}

/* Decode and submit the next ADPCM block, advancing the blob cursor. Returns the
 * number of samples submitted (0 at end of track or on a malformed/short block,
 * which also stops further decoding by leaving ip at the bad position). */
static long audio_decode_next_block(audio_track *at) {
    if (at->decoded >= at->nsamples) return 0;
    long remain = at->nsamples - at->decoded;
    int n = remain > ADPCM_BLOCK_SAMPLES ? ADPCM_BLOCK_SAMPLES : (int)remain;
    long bb = adpcm_block_bytes(n);
    if (bb < 0 || at->ip + bb > at->blob_len) { at->nsamples = at->decoded; return 0; }
    int got = adpcm_decode_block(at->blob + at->ip, bb, at->scratch, n);
    if (got != n) { at->nsamples = at->decoded; return 0; } /* clamp track to what decoded */
    at->ip += bb;
    audio_submit(at->scratch, n);
    at->decoded += n;
    at->submitted += n;
    return n;
}

/* Top up the audio backend so it always has a comfortable lead over the speaker,
 * decoding ADPCM blocks on demand rather than submitting a pre-expanded track
 * (audio_submit blocks when its ring is full, which bounds how far ahead we run).
 * Called once per video frame. */
static void audio_pump(audio_track *at) {
    if (!at->active) return;
    /* Keep ~0.5 s queued ahead of what's been played. Decode whole blocks until we
     * pass that mark (or run out); a block is ~0.25 s so this is 1-3 blocks/frame. */
    long target = audio_played_samples() + at->rate / 2;
    if (target > at->nsamples) target = at->nsamples;
    while (at->submitted < target) {
        if (audio_decode_next_block(at) == 0) break; /* end of track */
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
    free(at->blob);
    free(at->scratch);
    at->blob = NULL;
    at->scratch = NULL;
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

/* One segment's worth of decompressed planes + the cursors into them. For v3 the
 * "segment" is the whole movie (loaded once); for v4 it is seg_frames frames and
 * gets refilled at each boundary. The structure plane here EXCLUDES the leading
 * keyframe-vs-carry byte (seg_load strips it into `is_key`); `sp`/`cell_pos`/...
 * are reset to 0 by seg_load, and the caller seeds the keyframe-derived cursors. */
typedef struct {
    uint8_t *structp; long struct_len; long sp;
    uint8_t *modep;   long mode_len;   long mode_pos;
    uint8_t *cellp;   long cell_len;   long cell_pos;
    uint8_t *palp;    long pal_len;    long pal_pos;
    uint8_t *colorp;  long color_len;  long color_pos;
    int is_key;       /* segment starts with a fresh keyframe (vs CARRY) */
    int color_failed; /* color chunk present but couldn't be held (grayscale) */
    int step;         /* prefetch state machine cursor (SEG_STEP_*); LOADED when done */
} plane_seg;

/* Per-plane prefetch steps, in body order. seg_load_step does exactly one of
 * these per call so the work of pulling a segment off disk can be spread one
 * plane per frame instead of stalling the whole boundary frame (see the
 * double-buffered prefetch in the v4 SPLIT loop). The mode/pal/color steps are
 * skipped when their flag is off; the machine advances to SEG_STEP_LOADED. */
enum {
    SEG_STEP_STRUCT = 0,
    SEG_STEP_MODE,
    SEG_STEP_CELL,
    SEG_STEP_PAL,
    SEG_STEP_COLOR,
    SEG_STEP_LOADED
};

static void seg_free(plane_seg *s) {
    free(s->structp); free(s->modep); free(s->cellp);
    free(s->palp); free(s->colorp);
    s->structp = s->modep = s->cellp = s->palp = s->colorp = NULL;
}

/* Read one segment of every active plane from fp, in body order. `segmented` v4
 * structure chunks carry a leading keyframe/carry byte; v3 has none (treated as
 * KEY). `want_color` is the color-active flag; on a failed color allocation the
 * chunk is still consumed and color_failed is set. */
/* Reset a holder to "empty, about to load segment 0 of its steps". */
static void seg_load_begin(plane_seg *s) {
    memset(s, 0, sizeof(*s));
    s->is_key = 1;
    s->step = SEG_STEP_STRUCT;
}

/* Advance the prefetch state machine by ONE plane (one read_plane + decompress).
 * Skips inactive planes and stops at SEG_STEP_LOADED. Returns 1 once the whole
 * segment is resident, 0 while more steps remain. Each call reads at most one
 * chunk from fp, so calling it once per frame spreads a segment's I/O+decompress
 * across the preceding segment's frames instead of one boundary-frame stall. */
static int seg_load_step(plane_seg *s, FILE *fp, const tvid_header *h,
                         int segmented, int want_color) {
    switch (s->step) {
    case SEG_STEP_STRUCT:
        s->structp = read_plane(fp, &s->struct_len);
        if (segmented) {
            if (s->struct_len < 1) die("v4 structure segment missing keyframe byte");
            s->is_key = (s->structp[0] == TVID_SEG_KEY);
            /* Drop the leading flag byte so sp indexing matches the v3 layout. */
            memmove(s->structp, s->structp + 1, (size_t)(s->struct_len - 1));
            s->struct_len -= 1;
        }
        s->step = SEG_STEP_MODE;
        return 0;
    case SEG_STEP_MODE:
        if (h->flags & TVID_FLAG_MODEPLANE) {
            s->modep = read_plane(fp, &s->mode_len);
            if (h->flags & TVID_FLAG_MODERLE) {
                long exp_len = mode_rle_decoded_len(s->modep, s->mode_len);
                if (exp_len < 0) die("malformed mode RLE plane");
                uint8_t *exp = (uint8_t *)malloc(exp_len ? exp_len : 1);
                if (!exp) die("out of memory");
                if (mode_rle_decode(s->modep, s->mode_len, exp) != exp_len)
                    die("malformed mode RLE plane");
                free(s->modep); s->modep = exp; s->mode_len = exp_len;
            }
            s->step = SEG_STEP_CELL;
            return 0;
        }
        s->step = SEG_STEP_CELL;
        /* fall through: no separate frame spent on an absent mode plane */
    case SEG_STEP_CELL:
        s->cellp = read_plane(fp, &s->cell_len);
        s->step = SEG_STEP_PAL;
        return 0;
    case SEG_STEP_PAL:
        if (h->flags & TVID_FLAG_CELLSPLIT) {
            s->palp = read_plane(fp, &s->pal_len);
            s->step = SEG_STEP_COLOR;
            return 0;
        }
        s->step = SEG_STEP_COLOR;
        /* fall through */
    case SEG_STEP_COLOR:
        if (want_color) {
            s->colorp = read_plane_opt(fp, &s->color_len);
            if (!s->colorp) s->color_failed = 1; /* consumed but unheld: grayscale */
        }
        s->step = SEG_STEP_LOADED;
        return 1;
    default:
        return 1;
    }
}

/* Synchronous full load: drain the step machine. Used for segment 0 (must be
 * resident before the first present) and for v3 (whole movie in one segment). */
static void seg_load(plane_seg *s, FILE *fp, const tvid_header *h,
                     int segmented, int want_color) {
    seg_load_begin(s);
    while (!seg_load_step(s, fp, h, segmented, want_color))
        ;
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
    /* v3 and v4 share the codec: each cell is a 2x4 sub-cell glyph at a luma level,
     * with an optional parallel xterm-256 color plane (TVID_FLAG_COLOR). v4 differs
     * only in that its SPLIT body is segmented (streamable) and the header carries a
     * [u16 seg_frames] field. v2 (the retired flat-color model) has no decode path. */
    if (h.version == TVID_VERSION_V2)
        die("v2 streams are no longer supported (re-encode as v3/v4)");
    if (h.version != TVID_VERSION_V3 && h.version != TVID_VERSION)
        die("unsupported version");
    const int segmented = (h.version == TVID_VERSION); /* v4 */
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
    h.seg_frames = 0;
    if (segmented) {
        h.seg_frames = read_u16le(fp); /* v4: frames per segment */
        if (h.seg_frames == 0) die("v4 stream has seg_frames == 0");
        if (!(h.flags & TVID_FLAG_SPLIT)) die("v4 stream is not SPLIT");
    }
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
        /* Plane order in the body: structure, [mode plane if MODEPLANE], then
         * cells (combined, or raster+palette if CELLSPLIT), [color if COLOR]. v3
         * stores one chunk per plane over the whole movie; v4 stores one chunk per
         * plane per segment (segment-major). The decode loop is identical -- it
         * indexes the *current* segment's buffers and, for v4, refills at the
         * boundary (seg_load) and resets the cursors. For v3 the whole movie is one
         * segment (seg_frames == frame_count, always keyframe-led). */
        const uint32_t seg_frames = segmented ? h.seg_frames : h.frame_count;

        plane_seg seg;
        seg_load(&seg, fp, &h, segmented, has_color);
        int has_color_play = has_color && !seg.color_failed;

        /* Per-cell hue framebuffer (persists across segments and frames like fb;
         * SKIP/CARRY keep the previous hue). Seeded from the keyframe hues. */
        uint8_t *colfb = NULL;
        if (has_color_play) {
            if (seg.color_len < ncells) die("split color plane too short for keyframe");
            colfb = (uint8_t *)malloc((size_t)ncells);
            if (!colfb) { has_color_play = 0; } /* fb alloc failed: grayscale */
        }

        audio_track at;
        audio_load(&at, fp, &h); /* reads the audio tail; for v4 this seeks past
                                  * the segments already consumed via ftell. */
        /* fp is kept open for v4 to stream later segments; closed for v3. */
        if (!segmented) { fclose(fp); fp = NULL; }

        /* Keyframe of segment 0 = first ncells bytes of the cell/raster plane. */
        if (seg.cell_len < ncells) die("split cell plane too short for keyframe");
        memcpy(fb, seg.cellp, (size_t)ncells);
        seg.cell_pos = ncells;
        if (has_color_play) memcpy(colfb, seg.colorp, (size_t)ncells);
        seg.color_pos = has_color_play ? ncells : 0;

        backend_init(h.version, has_color_play, h.cols, h.rows, h.ramp, h.ramp_len);
        backend_present(fb, colfb);
        sync_wait(&at, 1, h.fps);

        /* v4 prefetch: pull the NEXT segment off disk one plane per frame during
         * the current segment, so the boundary is a pointer swap instead of a
         * full read+decompress stall in a single frame slot. fp sits at the start
         * of segment 1 here (audio_load restored the cursor). `next_ready` flips
         * when seg_load_step finishes; `next_started` guards beginning a load only
         * when there is another segment left to read. */
        const uint32_t nsegments = segmented
            ? (h.frame_count + seg_frames - 1) / seg_frames : 1;
        plane_seg next;
        seg_load_begin(&next);
        int next_started = 0, next_ready = 0;
        uint32_t prefetch_seg = 1; /* index of the segment `next` is loading */
        if (segmented && nsegments > 1) { next_started = 1; }

        for (uint32_t f = 1; f < h.frame_count; ++f) {
            PROF_DECL(pt);
            PROF_CAP(f);
            if (backend_poll_quit()) break;
            /* Prefetch one plane of the next segment this frame (amortizes the
             * boundary I/O+decompress across the segment instead of stalling one
             * frame). Cheap no-op once the next segment is fully resident. */
            if (next_started && !next_ready)
                next_ready = seg_load_step(&next, fp, &h, segmented,
                                           has_color_play);
            /* v4 segment boundary: free the old segment, load the next, reset the
             * plane cursors. A KEY segment re-seeds fb/colfb from its keyframe; a
             * CARRY segment continues from the persisted fb/colfb. */
            if (segmented && f % seg_frames == 0) {
                /* Finish the prefetch if it hasn't completed (tiny segments leave
                 * too few frames to spread it); then swap next -> seg. */
                if (next_started)
                    while (!next_ready)
                        next_ready = seg_load_step(&next, fp, &h, segmented,
                                                   has_color_play);
                seg_free(&seg);
                seg = next;            /* pointer swap: no read/decompress here */
                if (has_color_play && seg.color_failed)
                    die("v4 color segment unexpectedly unheld");
                /* Begin prefetching the following segment, if any. */
                prefetch_seg++;
                if (prefetch_seg < nsegments) {
                    seg_load_begin(&next);
                    next_started = 1; next_ready = 0;
                } else {
                    next_started = 0; next_ready = 1;
                }
                if (seg.is_key) {
                    if (seg.cell_len < ncells) die("v4 keyframe segment too short");
                    memcpy(fb, seg.cellp, (size_t)ncells);
                    seg.cell_pos = ncells;
                    if (has_color_play) {
                        if (seg.color_len < ncells) die("v4 keyframe color too short");
                        memcpy(colfb, seg.colorp, (size_t)ncells);
                        seg.color_pos = ncells;
                    }
                    /* The keyframe IS this segment's frame f; present it and move
                     * on (it is not a block frame in the structure plane). */
                    if (prevfb) memcpy(prevfb, fb, (size_t)ncells);
                    backend_present(fb, colfb);
                    prof_hud_draw();
                    sync_wait(&at, f + 1, h.fps);
                    prof_frame_end();
                    continue;
                }
                /* CARRY: cursors already 0 from seg_load; fb/colfb persist. */
            }
            if (seg.sp + 2 > seg.struct_len) die("truncated split structure plane");
            int len = seg.structp[seg.sp] | (seg.structp[seg.sp + 1] << 8);
            seg.sp += 2;
            if (seg.sp + len > seg.struct_len) die("truncated split structure frame");
            if (prevfb) memcpy(prevfb, fb, (size_t)ncells);
            PROF_T0(pt);
            if (codec_decode_block_split(
                    seg.structp + seg.sp, len, seg.cellp, seg.cell_len, &seg.cell_pos,
                    seg.modep, seg.mode_len, &seg.mode_pos,
                    seg.palp, seg.pal_len, &seg.pal_pos,
                    has_color_play ? seg.colorp : NULL,
                    has_color_play ? seg.color_len : 0, &seg.color_pos,
                    fb, colfb, prevfb ? prevfb : fb, h.cols, h.rows, caps) != 0)
                die("malformed split frame");
            PROF_ACC(t_decode, pt);
            seg.sp += len;
            PROF_T0(pt); backend_present(fb, colfb);    PROF_ACC(t_blit, pt);
            prof_hud_draw();
            PROF_W0(pt); sync_wait(&at, f + 1, h.fps); PROF_WACC(pt);
            prof_frame_end();
        }

        backend_shutdown();
        prof_summary();
        audio_free(&at);
        if (fp) fclose(fp);
        seg_free(&seg);
        seg_free(&next);   /* may hold a partially-prefetched segment */
        free(colfb);
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
