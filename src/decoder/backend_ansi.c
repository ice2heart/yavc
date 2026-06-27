/* backend_ansi.c - ANSI terminal backend for the player (portable C / POSIX). */
#include "backend.h"
#include "glyphset.h"
#include "tvid_format.h"
#include "xterm256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_cols, g_rows, g_ncells, g_ramp_len;
static int g_color;           /* 1 = a per-cell hue plane accompanies frames */
static char g_ramp[64];
static uint8_t *g_shadow;     /* last presented cells */
static uint8_t *g_colshadow;  /* last presented hues (color only) */
static char *g_out;           /* per-frame output buffer */
static size_t g_out_cap;

/* Terminal raw-mode state so backend_poll_quit can read single keypresses
 * (ESC / 'q') without the user pressing Enter. Saved/restored around playback. */
static struct termios g_termios_saved;
static int g_termios_active;  /* 1 if we put the tty in raw mode (must restore) */

void backend_init(int version, int has_color, int cols, int rows,
                  const char *ramp, int ramp_len) {
    (void)version; /* v3 only */
    g_color = has_color;
    g_cols = cols;
    g_rows = rows;
    g_ncells = cols * rows;
    g_ramp_len = ramp_len;
    memcpy(g_ramp, ramp, (size_t)ramp_len);

    g_shadow = (uint8_t *)malloc((size_t)g_ncells);
    memset(g_shadow, 0xFF, (size_t)g_ncells); /* force full first draw */
    g_colshadow = (uint8_t *)malloc((size_t)g_ncells);
    memset(g_colshadow, 0xFF, (size_t)g_ncells);

    /* Worst case per cell: cursor move + SGR + glyph. Color: ~24 B. Mono: a
     * truecolor fg SGR (ESC[38;2;v;v;vm ~19 B) + a 3-byte UTF-8 glyph + cursor
     * move ~ 32 B. Size for the larger. */
    g_out_cap = (size_t)g_ncells * 32 + 64;
    g_out = (char *)malloc(g_out_cap);

#ifdef TVID_PROBE
    if (getenv("TVID_DUMP")) { fflush(stdout); return; } /* dump mode: no ANSI setup */
#endif
    printf("\033[?25l\033[2J\033[H"); /* hide cursor, clear, home */
    fflush(stdout);

    /* Put stdin into non-canonical, no-echo mode so playback can be interrupted
     * by a single ESC/'q' keypress. Only if it's a real tty.
     *
     * VMIN=0/VTIME=0 already makes read() return immediately with 0 bytes when no
     * key is pending - that is all backend_poll_quit needs. We deliberately do
     * NOT set O_NONBLOCK on the fd: on a terminal stdin/stdout/stderr share one
     * open file description, and O_NONBLOCK is a property of that description, so
     * setting it via stdin also makes stdout non-blocking. A large frame write
     * (the ~6 KB keyframe, or any busy frame) then overflows the tty output
     * buffer, write() returns EAGAIN, and the tail of the frame is silently
     * dropped - half the screen never renders, with scattered artifacts. The
     * termios VMIN/VTIME settings give non-blocking reads without that fallout. */
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_termios_saved) == 0) {
        struct termios raw = g_termios_saved;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
            g_termios_active = 1;
    }
}

int backend_poll_quit(void) {
    int quit = 0;
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 0x1B || c == 'q' || c == 'Q') quit = 1;
    }
    return quit;
}

void backend_present(const uint8_t *fb, const uint8_t *colfb) {
#ifdef TVID_PROBE
    /* Frame-dump mode (compile -DTVID_PROBE, run with TVID_DUMP=1): write the raw
     * cols*rows CELL framebuffer to stdout (not the color plane) so two encodings
     * can be cmp'd for byte-identical *cell* decode -- the color plane is additive
     * and must not change the cells. TVID_DUMP=2 dumps the decoded HUE plane
     * (colfb) instead, to verify the color decode. See CLAUDE.md > Measurement
     * probes. */
    {
        const char *d = getenv("TVID_DUMP");
        if (d) {
            if (d[0] == '2' && colfb) fwrite(colfb, 1, (size_t)g_ncells, stdout);
            else fwrite(fb, 1, (size_t)g_ncells, stdout);
            return;
        }
    }
#endif
    size_t n = 0;
    int last_r = -1, last_g = -1, last_b = -1; /* skip redundant fg SGR */
    int expect_row = -1, expect_col = -1;      /* skip redundant cursor moves */
    const int color = g_color && colfb;

    for (int i = 0; i < g_ncells; ++i) {
        /* A cell needs a redraw if its shape/luma OR (with color) its hue changed. */
        if (fb[i] == g_shadow[i] && (!color || colfb[i] == g_colshadow[i])) continue;
        int row = i / g_cols, col = i % g_cols;

        if (row != expect_row || col != expect_col)
            n += (size_t)snprintf(g_out + n, g_out_cap - n,
                                  "\033[%d;%dH", row + 1, col + 1);
        /* Foreground = the cell's luma level, grayscale, or (with color) the hue
         * tinted by that luma. Truecolor SGR either way; last_* tracks the last
         * emitted triple to elide repeats. */
        int r, g, b;
        if (color)
            tvid_xterm256_rgb_dim(colfb[i], TVID_CELL_LUMA(fb[i]), &r, &g, &b);
        else
            r = g = b = tvid_mono_level_value(TVID_CELL_LUMA(fb[i]));
        if (r != last_r || g != last_g || b != last_b) {
            n += (size_t)snprintf(g_out + n, g_out_cap - n,
                                  "\033[38;2;%d;%d;%dm", r, g, b);
            last_r = r; last_g = g; last_b = b;
        }
        const char *gl = tvid_mono_glyph(TVID_CELL_MGLYPH(fb[i]))->utf8;
        while (*gl) g_out[n++] = *gl++;
        expect_row = row;
        expect_col = col + 1;
        g_shadow[i] = fb[i];
        if (color) g_colshadow[i] = colfb[i];
    }
    if (n) {
        fwrite(g_out, 1, n, stdout);
        fflush(stdout);
    }
}

void backend_wait_frame(int fps) {
#ifdef TVID_PROBE
    if (getenv("TVID_DUMP")) return; /* dump mode: no frame pacing */
#endif
    if (fps < 1) fps = 1;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000000L / fps;
    nanosleep(&ts, NULL);
}

void backend_shutdown(void) {
#ifdef TVID_PROBE
    if (getenv("TVID_DUMP")) { fflush(stdout); free(g_shadow); free(g_colshadow); free(g_out); return; }
#endif
    if (g_termios_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_termios_saved);
        g_termios_active = 0;
    }
    printf("\033[0m\033[%d;1H\033[?25h\n", g_rows + 1); /* reset, cursor back */
    fflush(stdout);
    free(g_shadow);
    free(g_colshadow);
    free(g_out);
}
