/* backend_dos.c - DOS text-mode (0xB800) backend for the player.
 *
 * NOT part of the host CMake build. The shipped DOS player is 32-bit DOS/4GW
 * (tools/build-dos.sh / -DTVID_DOS, OpenWatcom wcl386) so it can hold the whole
 * decoded audio track in flat memory; this file also still builds 16-bit real
 * mode (wcl -bt=dos -ml ...) for a video-only player. The two differ only in how
 * VRAM is addressed and which int call carries the BIOS register frame, guarded
 * by __386__ below.
 *
 * A text-mode cell is literally [char][attribute], which is exactly our cell
 * model: glyph -> char (CP437, ASCII subset of the ramp), color -> CGA attr. */
#include "backend.h"
#include "glyphset.h"
#include "tvid_format.h"
#include "xterm256.h"
#include "prof_dos.h"

#include <dos.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VRAM access + BIOS calls differ between 32-bit flat (DOS/4GW) and 16-bit real
 * mode. In flat mode physical text VRAM at 0xB8000 is a plain near pointer (the
 * default flat selector spans low memory) and BIOS ints go through int386 with
 * the 32-bit register frame; in real mode it is a far MK_FP pointer and int86. */
#ifdef __386__
  #define TVID_VRAM_PTR   ((unsigned char *)0xB8000)
  #define TVID_BIOS_INT   int386
#else
  #define TVID_VRAM_PTR   ((unsigned char far *)MK_FP(0xB800, 0x0000))
  #define TVID_BIOS_INT   int86
#endif

static int g_version;
static int g_color;            /* 1 = a per-cell xterm-256 hue plane accompanies frames */
static int g_cols, g_rows, g_ncells, g_ramp_len;
static char g_ramp[64];
/* Last-presented cells (shadow-diff). Sized to the actual grid at init, since
 * hi-res mono streams (e.g. 160x48) exceed the 80x24 TVID_CELLS default. */
static unsigned char *g_shadow;
static unsigned char *g_colshadow; /* last-presented hues (color only) */
#ifdef __386__
static unsigned char *g_vram;      /* flat near ptr to 0xB8000 */
#else
static unsigned char far *g_vram;  /* real-mode far ptr 0xB800:0000 */
#endif

/* Mono luma level -> CGA gray attribute: black / dark-gray / light-gray / white.
 * Indexed by the level scaled down to 0..3 (works for any TVID_MONO_LUMA_LEVELS;
 * with the shipped 4 levels it is a 1:1 map). */
static const unsigned char cga_gray4[4] = { 0, 8, 7, 15 };

/* In monochrome mode the cell's glyph nibble is a Braille 2x4 sub-cell ink mask
 * (glyphset.h). CP437 has no Braille, so the plain text path collapses each mask
 * to a single uniform shade block (tvid_mono_cp437) and loses all sub-cell
 * detail. Instead we redefine the VGA text font: glyph slot g draws the actual
 * 2x4 ink mask of pattern g, upscaled into the 8x16 character cell (each sub-cell
 * = a 4px-wide x 4px-tall block). Then backend_present writes the glyph index
 * straight to VRAM and the true Braille shape appears on real VGA hardware.
 *
 * Only built for the 32-bit DOS/4GW target (the shipped player); the 16-bit
 * video-only path keeps the CP437-shade fallback. The font upload uses BIOS
 * INT 10h AX=1110h (load user 8x16 font block), which needs the 32-bit int386
 * register frame for the ES:BP font pointer, hence __386__. */
#ifdef __386__
static unsigned char g_mono_font[256][16]; /* 8x16 glyph bitmaps, 1 bit/px */

/* Build the user font: glyph g = pattern g's 2x4 mask blown up to 8x16.
 * Cell is 8 wide (2 sub-cols x 4px) and 16 tall (4 sub-rows x 4px). MSB of each
 * font byte is the leftmost pixel. */
static void mono_build_font(void) {
    int g, sr, sc, py, px;
    unsigned char ink[TVID_MONO_SUBN];
    for (g = 0; g < 256; ++g) {
        tvid_mono_pattern_ink(tvid_mono_pattern(g), ink);
        for (sr = 0; sr < TVID_MONO_SUBH; ++sr) {       /* 4 sub-rows */
            for (py = 0; py < 4; ++py) {                /* 4 px tall each */
                unsigned char row = 0;
                for (sc = 0; sc < TVID_MONO_SUBW; ++sc) /* 2 sub-cols */
                    if (ink[sr * TVID_MONO_SUBW + sc])
                        for (px = 0; px < 4; ++px)       /* 4 px wide each */
                            row |= (unsigned char)(0x80u >> (sc * 4 + px));
                g_mono_font[g][sr * 4 + py] = row;
            }
        }
    }
}

/* Upload the 256-glyph 8x16 user font into VGA font memory (plane 2) by
 * programming the Sequencer + Graphics Controller directly. The BIOS
 * "load user font" call (AX=1110h) wants a real-mode ES:BP font pointer, which
 * is awkward from DOS/4GW flat model (BIOS runs in real mode, BP is a 16-bit
 * real-mode offset); writing the planes ourselves sidesteps that entirely and
 * is the standard DOS/4GW way. Font RAM is mapped at 0xA0000 while plane 2 is
 * selected. Each glyph occupies 32 bytes (16 used for 8x16); we write all 256. */
static void mono_load_font(void) {
    unsigned char *fontmem = (unsigned char *)0xA0000; /* flat, while plane 2 mapped */
    int g, y;

    /* --- enter font-load state --- */
    /* Sequencer: reset, plane-2 write mask, sequential addressing, no chain-4. */
    outp(0x3C4, 0x00); outp(0x3C5, 0x01);   /* SR0 = synchronous reset */
    outp(0x3C4, 0x02); outp(0x3C5, 0x04);   /* SR2 = write plane 2 only */
    outp(0x3C4, 0x04); outp(0x3C5, 0x07);   /* SR4 = sequential, ext mem, no chain */
    outp(0x3C4, 0x00); outp(0x3C5, 0x03);   /* SR0 = end reset */
    /* Graphics controller: map font window at A000, plane 2, read mode 0. */
    outp(0x3CE, 0x04); outp(0x3CF, 0x02);   /* GR4 = read plane 2 */
    outp(0x3CE, 0x05); outp(0x3CF, 0x00);   /* GR5 = write mode 0 */
    outp(0x3CE, 0x06); outp(0x3CF, 0x04);   /* GR6 = A0000-AFFFF, graphics off */

    for (g = 0; g < 256; ++g)
        for (y = 0; y < 16; ++y)
            fontmem[g * 32 + y] = g_mono_font[g][y];

    /* --- restore text state --- */
    outp(0x3C4, 0x00); outp(0x3C5, 0x01);
    outp(0x3C4, 0x02); outp(0x3C5, 0x03);   /* write planes 0+1 (char + attr) */
    outp(0x3C4, 0x04); outp(0x3C5, 0x03);   /* odd/even, chain on */
    outp(0x3C4, 0x00); outp(0x3C5, 0x03);
    outp(0x3CE, 0x04); outp(0x3CF, 0x00);   /* read plane 0 */
    outp(0x3CE, 0x05); outp(0x3CF, 0x10);   /* odd/even write mode */
    outp(0x3CE, 0x06); outp(0x3CF, 0x0E);   /* B8000-BFFFF text window */
}
/* ---------------------------------------------------------------------------
 * VGA Mode 13h graphics path (mono only, DOS/4GW flat).
 *
 * Default render for monochrome streams: instead of the custom-font text mode,
 * set 320x200x256 and paint each cell's 2x4 Braille ink mask as a small sprite
 * straight into the linear framebuffer at 0xA0000. Each lit sub-pixel is tinted
 * by the cell's gray level via a grayscale DAC, so the result has true per-pixel
 * control (vs text mode's one CGA attribute per whole cell). Set TVID_TEXT=1 to
 * force the old text path.
 *
 * Geometry is derived from the grid so both the normal 80x24 stream and hi-res
 * mono (e.g. 160x48) fit. Sub-pixel canvas = (cols*2) x (rows*4): 160x96 for
 * 80x24, 320x192 for 160x48. Per-sub-pixel integer scale = min(320/subw,
 * 200/subh) clamped >=1 -> 2x for the normal stream (320x192), 1x for hi-res
 * (320x192). The image is centered with letterbox borders. A cell (col,row)
 * occupies a (2*scale) x (4*scale) px rect; each sub-pixel a scale x scale block.
 *
 * 0xA0000 is a plain flat near pointer here, same as mono_load_font's font-RAM
 * window - the default flat selector spans low memory. */
#define GFX_W     320
#define GFX_H     200

static int g_gfx;                  /* 1 = Mode 13h graphics path active (mono) */
static unsigned char *g_lfb;       /* flat near ptr to the 0xA0000 framebuffer */
static int g_gfx_scale;            /* px per sub-pixel (integer, >=1)          */
static int g_gfx_xoff, g_gfx_yoff; /* top-left of the centered image, in px     */

/* Set 320x200x256 linear (BIOS INT 10h AX=0013h). */
static void gfx_set_mode13(void) {
    union REGS r;
    r.h.ah = 0x00; r.h.al = 0x13; TVID_BIOS_INT(0x10, &r, &r);
}

/* Program the 256-entry DAC. Two layouts depending on g_color:
 *  - grayscale (no color plane): slot lvl = the 8-bit gray for luma level lvl,
 *    so the per-pixel index written == the cell's luma (slots above the levels
 *    stay black). A level-0 lit dot is invisible, matching the model.
 *  - color: slot N = xterm-256 palette color N (the same RGBs the ANSI backend
 *    emits as ESC[38;5;Nm), so the per-pixel index written == the cell's hue.
 *    Brightness then comes from the lit-dot coverage of the Braille mask (the
 *    luma level gates which sub-pixels are lit; see gfx_present_mono). All 8-bit
 *    RGB channels are scaled to the DAC's 6-bit (>>2). */
static void gfx_set_palette(void) {
    int i;
    outp(0x3C8, 0);                 /* start writing DAC entries at index 0 */
    if (g_color) {
        for (i = 0; i < 256; ++i) {
            int r, g, b;
            tvid_xterm256_rgb(i, &r, &g, &b);
            outp(0x3C9, (unsigned char)(r >> 2));
            outp(0x3C9, (unsigned char)(g >> 2));
            outp(0x3C9, (unsigned char)(b >> 2));
        }
    } else {
        for (i = 0; i < 256; ++i) {
            unsigned char g6 = (i < TVID_MONO_LUMA_LEVELS)
                ? (unsigned char)(tvid_mono_level_value(i) >> 2) : 0;
            outp(0x3C9, g6);        /* R */
            outp(0x3C9, g6);        /* G */
            outp(0x3C9, g6);        /* B */
        }
    }
}

/* Blit the changed cells of fb into the linear framebuffer. Same shadow-diff as
 * the text path; each changed cell paints its 4x8 pixel rect. The per-lit-dot
 * pixel index is the cell's luma (grayscale DAC) or, with color, its xterm-256
 * hue (color DAC); brightness in the color path comes from how many dots the
 * luma-driven glyph lights. colfb is the parallel hue plane (NULL = grayscale). */
static void gfx_present_mono(const unsigned char *fb, const unsigned char *colfb) {
    const int s = g_gfx_scale;
    const int cellw = TVID_MONO_SUBW * s;          /* cell px width  */
    const int cellh = TVID_MONO_SUBH * s;          /* cell px height */
    const int color = g_color && colfb;
    int i;
    for (i = 0; i < g_ncells; ++i) {
        int col, row, lvl, sr, sc, px, py;
        unsigned char ink[TVID_MONO_SUBN];
        unsigned char index;   /* DAC index for a lit dot: luma or hue */
        unsigned char *p;
        /* Redraw when shape/luma OR (color) the hue changed. */
        if (fb[i] == g_shadow[i] && (!color || colfb[i] == g_colshadow[i])) continue;
#if defined(TVID_PROF) && defined(__DOS__)
        g_prof.cells++;
#endif
        col = i % g_cols;
        row = i / g_cols;
        lvl = TVID_CELL_LUMA(fb[i]);
        index = color ? colfb[i] : (unsigned char)lvl;
        tvid_mono_pattern_ink(tvid_mono_pattern(TVID_CELL_MGLYPH(fb[i])), ink);

        /* top-left of this cell's rect inside the centered image */
        p = g_lfb + (g_gfx_yoff + row * cellh) * GFX_W + (g_gfx_xoff + col * cellw);
        for (sr = 0; sr < TVID_MONO_SUBH; ++sr) {       /* 4 sub-rows */
            for (sc = 0; sc < TVID_MONO_SUBW; ++sc) {   /* 2 sub-cols */
                unsigned char v = ink[sr * TVID_MONO_SUBW + sc] ? index : 0;
                unsigned char *q = p + (sr * s) * GFX_W + sc * s; /* scale x scale block */
                for (py = 0; py < s; ++py)
                    for (px = 0; px < s; ++px)
                        q[py * GFX_W + px] = v;
            }
        }
        g_shadow[i] = fb[i];
        if (color) g_colshadow[i] = colfb[i];
    }
}
#endif /* __386__ */

#if defined(TVID_PROF) && defined(__DOS__)
static void prof_toggle(void);   /* defined below; toggles the timing HUD */
#endif

/* Far peek of the BIOS 18.2Hz tick counter at 0040:006C. */
static unsigned long bios_ticks(void) {
    unsigned long far *t = (unsigned long far *)MK_FP(0x0040, 0x006C);
    return *t;
}

void backend_init(int version, int has_color, int cols, int rows,
                  const char *ramp, int ramp_len) {
    union REGS r;
    g_version = version;
    g_color = has_color;
    g_cols = cols; g_rows = rows; g_ncells = cols * rows;
    g_ramp_len = ramp_len;
    memcpy(g_ramp, ramp, (size_t)ramp_len);
    /* Grid is dynamic (hi-res mono can be 160x48), so size the shadow to it. */
    g_shadow = (unsigned char *)malloc((size_t)g_ncells);
    memset(g_shadow, 0xFF, (size_t)g_ncells); /* force full first draw */
    g_colshadow = (unsigned char *)malloc((size_t)g_ncells);
    memset(g_colshadow, 0xFF, (size_t)g_ncells);
    g_vram = TVID_VRAM_PTR;

#ifdef __386__
    /* v3 streams render in Mode 13h graphics by default (real per-pixel sub-cell
     * sprites + a smooth DAC: grayscale, or the xterm-256 palette when the stream
     * carries color). TVID_TEXT=1 forces the old text path (grayscale only). */
    g_gfx = (getenv("TVID_TEXT") == NULL);
    if (g_gfx) {
        int subw = g_cols * TVID_MONO_SUBW;   /* sub-pixel canvas */
        int subh = g_rows * TVID_MONO_SUBH;
        int sw = subw > 0 ? GFX_W / subw : 1;
        int sh = subh > 0 ? GFX_H / subh : 1;
        g_gfx_scale = sw < sh ? sw : sh;      /* largest integer scale that fits */
        if (g_gfx_scale < 1) g_gfx_scale = 1; /* oversized grid: clip rather than 0 */
        g_gfx_xoff = (GFX_W - subw * g_gfx_scale) / 2;
        g_gfx_yoff = (GFX_H - subh * g_gfx_scale) / 2;
        if (g_gfx_xoff < 0) g_gfx_xoff = 0;
        if (g_gfx_yoff < 0) g_gfx_yoff = 0;
        g_lfb = (unsigned char *)0xA0000;
        gfx_set_mode13();
        gfx_set_palette();
        return; /* no text-mode set, no font upload, no cursor call */
    }
#endif

    r.h.ah = 0x00; r.h.al = 0x03; TVID_BIOS_INT(0x10, &r, &r); /* 80x25 text */
    r.h.ah = 0x01; r.w.cx = 0x2000; TVID_BIOS_INT(0x10, &r, &r); /* hide cursor */

#ifdef __386__
    /* Text-mode (TVID_TEXT=1 fallback): install the Braille sub-cell font so glyph
     * slots render the real 2x4 ink masks the codec encoded, not flat shade blocks.
     * Text mode is grayscale only; color needs the Mode 13h graphics path. */
    if (!getenv("TVID_NOFONT")) {
        mono_build_font();
        mono_load_font();
    }
#endif
}

void backend_present(const unsigned char *fb, const unsigned char *colfb) {
    int i;
#ifdef __386__
    if (g_gfx) { gfx_present_mono(fb, colfb); return; }
#endif
    /* Text-mode fallback (TVID_TEXT=1): grayscale Braille shapes, no color. */
    (void)colfb;
    for (i = 0; i < g_ncells; ++i) {
        unsigned off;
        int lvl, g4;
        if (fb[i] == g_shadow[i]) continue;
#if defined(TVID_PROF) && defined(__DOS__)
        g_prof.cells++;
#endif
        off = (unsigned)i << 1; /* 2 bytes per cell */
        lvl = TVID_CELL_LUMA(fb[i]);
        /* scale the level to 0..3 for the 4-entry gray attr map */
        g4 = (TVID_MONO_LUMA_LEVELS <= 1) ? 3 : lvl * 3 / (TVID_MONO_LUMA_LEVELS - 1);
#ifdef __386__
        /* Custom Braille font is loaded: the glyph index *is* the font slot, so
         * the real 2x4 sub-cell shape draws (4 gray levels). */
        g_vram[off]     = TVID_CELL_MGLYPH(fb[i]);
#else
        /* 16-bit fallback: no custom font, collapse to a CP437 shade. */
        g_vram[off]     = tvid_mono_glyph(TVID_CELL_MGLYPH(fb[i]))->cp437;
#endif
        g_vram[off + 1] = cga_gray4[g4];
        g_shadow[i] = fb[i];
    }
}

void backend_wait_frame(int fps) {
    /* BIOS tick is 18.2Hz; wait ceil(18.2/fps) ticks (coarse but dependency-free). */
    unsigned long wait = (182UL / (fps > 0 ? fps : 1) + 5) / 10;
    unsigned long start;
    if (wait < 1) wait = 1;
    start = bios_ticks();
    while (bios_ticks() - start < wait) { /* spin */ }
}

int backend_poll_quit(void) {
    /* Non-blocking: only touch the DSP-free keyboard buffer. ESC (0x1B) quits;
     * drain any other pending key so the buffer can't fill and beep. kbhit()/
     * getch() are real-mode-safe conio (DOS/4GW maps them through). */
    int quit = 0;
    while (kbhit()) {
        int k = getch();
        if (k == 0x1B) quit = 1;
#if defined(TVID_PROF) && defined(__DOS__)
        else if (k == 'd' || k == 'D') prof_toggle(); /* toggle the timing HUD */
#endif
    }
    return quit;
}

void backend_shutdown(void) {
    union REGS r;
    r.h.ah = 0x00; r.h.al = 0x03; TVID_BIOS_INT(0x10, &r, &r); /* reset text mode */
    free(g_shadow);
    g_shadow = NULL;
    free(g_colshadow);
    g_colshadow = NULL;
}

/* ---------------------------------------------------------------------------
 * Profiler / HUD (only when -DTVID_PROF). Shares the VRAM pointer and shadow so
 * the overlay is a direct text-cell poke (cheap) and the cells it covers are
 * forced dirty so the video redraws under the HUD the moment it is toggled off.
 * ------------------------------------------------------------------------- */
#if defined(TVID_PROF) && defined(__DOS__)
prof_state g_prof;
int        g_prof_hud = 1;   /* HUD on by default in a profiling build */

/* Read PIT channel 0's current 16-bit down-counter via the latch command.
 * Counter counts DOWN from 65536 at 1.193182 MHz; the caller diffs successive
 * reads to get elapsed ticks (~838 ns each). */
static unsigned prof_pit_read(void) {
    unsigned lo, hi;
    outp(0x43, 0x00);            /* latch channel 0 */
    lo = (unsigned)inp(0x40);
    hi = (unsigned)inp(0x40);
    return (hi << 8) | lo;
}

/* BIOS 18.2 Hz tick count for the coarse wait-phase timer (declared prof_dos.h).
 * Reuses the file's existing bios_ticks() reader. */
unsigned long prof_bios_ticks(void) { return bios_ticks(); }

/* TVID_PROFFRAMES cap (declared prof_dos.h), parsed once. */
int prof_frame_cap(void) {
    static int cap = -1;
    if (cap < 0) { const char *e = getenv("TVID_PROFFRAMES"); cap = e ? atoi(e) : 0; }
    return cap;
}

/* The one shared elapsed-tick clock (declared in prof_dos.h). */
unsigned long prof_now(void) {
    static unsigned long total = 0;
    static unsigned last = 0;     /* previous raw down-counter */
    static int init = 0;
    unsigned cur = prof_pit_read();
    if (!init) { last = cur; init = 1; return 0; }
    total += (unsigned long)((last - cur) & 0xFFFF); /* down-counter diff */
    last = cur;
    return total;
}

/* Poke an ASCII string into VRAM at (row,col) with attribute `attr`, and mark
 * those shadow cells dirty so the underlying video repaints there next frame. */
static void prof_puts(int row, int col, const char *s, unsigned char attr) {
    int c = col;
    while (*s && c < g_cols) {
        unsigned idx = (unsigned)(row * g_cols + c);
        unsigned off = idx << 1;
        g_vram[off]     = (unsigned char)*s++;
        g_vram[off + 1] = attr;
        if ((int)idx < g_ncells) g_shadow[idx] = 0xFF; /* force redraw later */
        c++;
    }
}

/* Tiny unsigned-to-decimal, right-justified into buf[width] (space-padded), NUL
 * at buf[width]. If the value needs more than `width` digits, buf[0] becomes '+'
 * to flag the overflow rather than print garbage. */
static void prof_utoa(unsigned long v, char *buf, int width) {
    int i;
    buf[width] = '\0';
    for (i = width - 1; i >= 0; --i) {
        if (v == 0 && i < width - 1) { buf[i] = ' '; }   /* pad above the number */
        else { buf[i] = (char)('0' + (int)(v % 10)); v /= 10; }
    }
    if (v != 0) buf[0] = '+';                            /* didn't fit in width */
}

void prof_hud_draw(void) {
    char line[81];
    char nbuf[12];
    unsigned long dec_us, blt_us, wt_us, tot_us, fps10;
    int p;

    if (!g_prof_hud) return;
#ifdef __386__
    /* The HUD pokes text VRAM (0xB8000); in Mode 13h that is the graphics
     * framebuffer, so the text overlay would corrupt the picture. Skip the
     * on-screen HUD in gfx mode - prof_summary() still prints to stdout after
     * shutdown returns to text mode. */
    if (g_gfx) return;
#endif

    dec_us = prof_us(g_prof.t_decode);
    blt_us = prof_us(g_prof.t_blit);
    wt_us  = g_prof.us_wait;
    tot_us = dec_us + blt_us + wt_us;
    /* fps*10 from the active (non-wait) work + wait, i.e. real frame period. */
    fps10 = tot_us ? (10000000UL / tot_us) : 0;

    /* Line 0: "F<frames> fps<xx.x> dec<us> blt<us> wt<us> chg<cells>"
     * Built field by field to avoid sprintf bloat in the DOS binary. */
    for (p = 0; p < 80; ++p) line[p] = ' ';
    line[80] = '\0';

    line[0] = 'F';
    prof_utoa(g_prof.frames, nbuf, 5);  memcpy(line + 1, nbuf, 5);
    memcpy(line + 7, "fps", 3);
    prof_utoa(fps10 / 10, nbuf, 3);     memcpy(line + 10, nbuf, 3);
    line[13] = '.';
    nbuf[0] = (char)('0' + (fps10 % 10)); line[14] = nbuf[0];
    memcpy(line + 16, "dec", 3);
    prof_utoa(dec_us, nbuf, 5);         memcpy(line + 19, nbuf, 5);
    memcpy(line + 25, "blt", 3);
    prof_utoa(blt_us, nbuf, 5);         memcpy(line + 28, nbuf, 5);
    memcpy(line + 34, "wt", 2);
    prof_utoa(wt_us, nbuf, 6);          memcpy(line + 36, nbuf, 6);
    memcpy(line + 43, "chg", 3);
    prof_utoa((unsigned long)g_prof.cells, nbuf, 4); memcpy(line + 46, nbuf, 4);
    line[51] = '\0';

    prof_puts(0, 0, line, 0x1F); /* white on blue */
}

void prof_frame_end(void) {
    unsigned long active;
    g_prof.frames++;
    g_prof.sum_decode  += g_prof.t_decode;
    g_prof.sum_blit    += g_prof.t_blit;
    g_prof.sum_wait_us += g_prof.us_wait;
    g_prof.sum_cells   += (unsigned long)g_prof.cells;
    active = prof_us(g_prof.t_decode + g_prof.t_blit); /* the part we control */
    if (active > g_prof.worst_frame_us) g_prof.worst_frame_us = active;
    /* reset per-frame */
    g_prof.t_decode = g_prof.t_blit = 0;
    g_prof.us_wait = 0;
    g_prof.cells = 0;
}

void prof_summary(void) {
    unsigned long n = g_prof.frames ? g_prof.frames : 1;
    unsigned long active;
    printf("\n--- TVID_PROF summary (%lu frames) ---\n", g_prof.frames);
    printf("avg decode : %lu us/frame\n", prof_us(g_prof.sum_decode) / n);
    printf("avg blit   : %lu us/frame\n", prof_us(g_prof.sum_blit) / n);
    printf("avg wait   : %lu us/frame (coarse, BIOS tick)\n",
           g_prof.sum_wait_us / n);
    printf("avg cells  : %lu changed/frame\n", g_prof.sum_cells / n);
    printf("worst active: %lu us (decode+blit, excl. wait)\n",
           g_prof.worst_frame_us);
    active = prof_us(g_prof.sum_decode + g_prof.sum_blit) / n;
    printf("avg active : %lu us/frame -> ceiling ~%lu fps (no wait)\n",
           active, active ? 1000000UL / active : 0);
}

/* Toggle the HUD; called from backend_poll_quit on the 'd' key. */
static void prof_toggle(void) { g_prof_hud = !g_prof_hud; }
#endif /* TVID_PROF && __DOS__ */
