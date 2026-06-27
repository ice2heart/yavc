/* prof_dos.h - per-frame profiler + on-screen HUD for the DOS player.
 *
 * Purpose: find the choke point. A frame's wall time splits into three phases -
 *   decode  (codec/entropy unpack of the next frame into the framebuffer)
 *   blit    (backend_present: shadow-diff + writes to text VRAM)
 *   wait    (sync_wait: pacing to the audio clock / frame timer)
 * - plus the count of cells that actually changed (how much the blit had to do).
 * Whichever phase dominates is where optimisation pays off; without numbers the
 * "text vs graphics mode" question is a guess. This header makes them visible.
 *
 * Everything here is gated behind -DTVID_PROF and compiles to NOTHING in the
 * production build (mirrors the host -DTVID_PROBE convention). The shipped
 * PLAYER.EXE built by tools/build-dos.sh without TVID_PROF contains none of it.
 *
 * Timer: PIT channel 0 read back via the latch command. The BIOS programs it as
 * a ~18.2 Hz rate generator (reload 65536, counting DOWN at 1.193182 MHz), so the
 * latched 16-bit count gives ~838 ns resolution - fine enough to time a single
 * frame phase, where the BIOS 55 ms tick is useless. We accumulate a 32-bit
 * elapsed-tick total across the latch wraps by watching the down-counter roll.
 *
 * DOS-only (PIT ports, direct VRAM). Included by player.c and backend_dos.c only
 * when both __DOS__ and TVID_PROF are defined; the macros vanish otherwise. */
#ifndef PROF_DOS_H
#define PROF_DOS_H

#if defined(TVID_PROF) && defined(__DOS__)

/* PIT channel-0 frequency: 1.193182 MHz. One tick ~= 0.8381 us. */
#define PROF_PIT_HZ 1193182UL

/* Monotonic-ish elapsed-tick clock built on the down-counter. Each call returns
 * a 32-bit running total of PIT ticks since the first call, handling the 16-bit
 * wrap (which happens every ~55 ms). Single shared definition (backend_dos.c) so
 * the wrap accounting stays coherent across both translation units that time. */
unsigned long prof_now(void);

/* Convert a PIT-tick delta to microseconds. Macro (not a static fn) so the TU
 * that includes the header without using it draws no unreferenced-symbol noise. */
#define prof_us(ticks) ((unsigned long)(ticks) * 1000000UL / PROF_PIT_HZ)

/* ---- per-frame accumulators (filled by the hooks in player.c / backend_dos.c) ----
 * decode/blit are short (<<55 ms) so the fine PIT clock times them exactly.
 * The wait phase routinely exceeds the PIT's 55 ms wrap, so it is timed on the
 * coarse 18.2 Hz BIOS tick instead (prof_bios_us) and stored as microseconds. */
typedef struct {
    unsigned long t_decode;   /* PIT ticks spent decoding this frame   */
    unsigned long t_blit;     /* PIT ticks in backend_present          */
    unsigned long us_wait;    /* sync_wait time, microseconds (BIOS)   */
    int           cells;      /* cells changed this frame (blit work)  */
    unsigned long frames;     /* frames presented so far               */
    /* running sums for an exit summary (decode/blit in PIT ticks, wait in us) */
    unsigned long sum_decode, sum_blit, sum_wait_us;
    unsigned long sum_cells;
    unsigned long worst_frame_us; /* slowest active (decode+blit) frame */
} prof_state;

extern prof_state g_prof;
extern int g_prof_hud;        /* HUD visible? toggled by a key */

/* Read the BIOS 18.2 Hz tick at 0040:006C (coarse, ~55 ms, but never wraps over a
 * single wait). Returns elapsed microseconds since a captured start tick. */
unsigned long prof_bios_ticks(void);

/* Timing scope helpers: PROF_T0 captures a start tick into a local; PROF_ACC
 * adds the elapsed PIT ticks since that local into an accumulator field. */
#define PROF_DECL(var)   unsigned long var
#define PROF_T0(var)     do { (var) = prof_now(); } while (0)
#define PROF_ACC(field, var) do { g_prof.field += prof_now() - (var); } while (0)

/* Wait phase: capture a BIOS tick into var, then store the elapsed us. */
#define PROF_W0(var)     do { (var) = prof_bios_ticks(); } while (0)
#define PROF_WACC(var)   do { \
        g_prof.us_wait += (prof_bios_ticks() - (var)) * 54925UL; } while (0)

/* Headless measurement: TVID_PROFFRAMES=N caps playback at N frames so a scripted
 * DOSBox run terminates and prints the summary instead of playing for minutes.
 * Read once and cached; 0/unset means "no cap" (play to the end). Place at the
 * top of the playback loop body. */
int prof_frame_cap(void);
#define PROF_CAP(f)  do { int _c = prof_frame_cap(); \
        if (_c && (int)(f) >= _c) break; } while (0)

/* Draw the HUD onto the text VRAM top row(s). Implemented in backend_dos.c so it
 * shares the VRAM pointer + shadow handling. Called once per frame from player.c
 * after backend_present. */
void prof_hud_draw(void);
/* Fold this frame's accumulators into the running sums and reset per-frame ones.
 * Call once per presented frame (player.c). */
void prof_frame_end(void);
/* Print the exit summary to the post-shutdown text screen (stdout). */
void prof_summary(void);

#else  /* not profiling: every hook compiles away */

#define PROF_DECL(var)        ((void)0)
#define PROF_T0(var)          ((void)0)
#define PROF_ACC(field, var)  ((void)0)
#define PROF_W0(var)          ((void)0)
#define PROF_WACC(var)        ((void)0)
#define PROF_CAP(f)           ((void)0)
#define prof_hud_draw()       ((void)0)
#define prof_frame_end()      ((void)0)
#define prof_summary()        ((void)0)

#endif /* TVID_PROF && __DOS__ */

#endif /* PROF_DOS_H */
