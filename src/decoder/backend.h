/* backend.h - output abstraction for the player (portable C).
 *
 * A backend keeps its own shadow framebuffer; backend_present() diffs the new
 * frame against it and emits only the cells that changed. This keeps both the
 * ANSI path (fewer escape bytes) and the DOS path (fewer 0xB800 writes) fast. */
#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>

/* Initialize output. `version` must be TVID_VERSION (3): each cell draws a
 * built-in 2x4 sub-cell glyph (glyphset.h) at one of a few brightness levels.
 * `has_color` selects whether a per-cell xterm-256 hue plane accompanies frames
 * (TVID_FLAG_COLOR): when set, the cell's lit sub-pixels are tinted by the hue
 * (scaled by luma); when clear, they render grayscale. ramp[]/ramp_len is the
 * (now informational) header ramp field. cols*rows is the grid size. */
void backend_init(int version, int has_color, int cols, int rows,
                  const char *ramp, int ramp_len);

/* Draw the ncells-byte cell framebuffer, emitting only changed cells. `colfb` is
 * the parallel per-cell hue framebuffer (cols*rows xterm-256 indices) when the
 * stream carries color; pass NULL for a grayscale stream. */
void backend_present(const uint8_t *fb, const uint8_t *colfb);

/* Pace playback to fps (sleep until the next frame is due). */
void backend_wait_frame(int fps);

/* Poll for a user quit request (non-blocking). Returns nonzero if the user has
 * asked to stop playback (ESC, or 'q' on the host terminal). The player checks
 * this once per frame and exits cleanly when it fires. */
int backend_poll_quit(void);

/* Restore terminal/screen state. */
void backend_shutdown(void);

#endif /* BACKEND_H */
