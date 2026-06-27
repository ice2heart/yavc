/* audio_dos.c - Sound Blaster output backend for the DOS player (DOS/4GW,
 * 32-bit protected mode, OpenWatcom wcl386).
 *
 * NOT part of the host CMake build. Compiled into the DOS player by
 * tools/build-dos.sh alongside backend_dos.c. Implements the same audio.h
 * interface the host CoreAudio backend does, so player.c is unchanged.
 *
 * How it works
 * ------------
 * The SB plays 8-bit unsigned PCM out of a single auto-init DMA buffer that the
 * 8237 controller cycles forever. We split that buffer into two halves and run
 * the classic double-buffer scheme: while the DMA plays half A, our IRQ handler
 * fills half B from the decoded-PCM ring, and vice-versa. One SB interrupt fires
 * per half-buffer; the ISR refills the half that just finished, acks the DSP and
 * the PIC, and bumps a played-sample counter the video loop paces against.
 *
 * DOS/4GW wrinkles (vs 16-bit real mode)
 * --------------------------------------
 *  - The 8237 DMA works in PHYSICAL addresses below 1 MB and cannot cross a
 *    64 KB page. Under DPMI we can't just pick an address: we ask DOS (via the
 *    DPMI "allocate DOS memory" call, int 0x31 ax=0x0100) for a real-mode block,
 *    which gives a paragraph-aligned buffer under 1 MB whose physical address ==
 *    segment<<4 (identity-mapped low memory). We over-allocate and align the
 *    buffer inside it so it never straddles a 64 KB boundary.
 *  - The ISR and every byte it touches must be PAGE-LOCKED so a DPMI host never
 *    pages them out while an interrupt is pending. We lock the ring, the counters
 *    and the code.
 *  - We hook the protected-mode interrupt vector for the SB IRQ (DPMI
 *    get/set PM vector, int 0x31 ax=0x0204/0x0205) and unmask it in the PIC.
 *
 * Sample format: IMA decodes to s16; the SB plays u8 easiest (8-bit DMA on
 * channel 1, no SB16 needed), so we downshift (s16>>8)^0x80. Quality is a
 * non-goal here. Mono only. */
#include "audio.h"

#include <conio.h>   /* inp/outp port I/O */
#include <dos.h>
#include <i86.h>
#include <stdlib.h>
#include <string.h>

/* ----- tunables ----- */
#define DMA_HALF   2048           /* u8 samples per half-buffer (~256 ms @ 8 kHz) */
#define DMA_TOTAL  (DMA_HALF * 2) /* full auto-init buffer (must be < 64 KB)      */
#define RING_SAMPS 32768          /* s16 staging ring the player fills (pow2)     */

/* ----- SB / hardware state ----- */
static int   g_base = 0x220;      /* DSP base port (from BLASTER)                 */
static int   g_irq  = 5;          /* SB IRQ                                       */
static int   g_dma  = 1;          /* 8-bit DMA channel                            */
static int   g_rate = 8000;
static int   g_running = 0;

/* DPMI-allocated low-memory DMA buffer. */
static unsigned char *g_dmabuf;   /* linear (== physical) base, identity-mapped  */
static unsigned long  g_dmaphys;  /* physical address for the 8237               */
static unsigned short g_dos_seg;  /* real-mode segment of the DOS block (to free)*/
static unsigned short g_dos_sel;  /* its protected-mode selector                 */

/* Double-buffer + sample clock, all touched by the ISR -> page-locked. */
static volatile int           g_half;        /* half the DMA is *playing* now     */
static volatile unsigned long g_played;      /* u8 samples handed to the speaker  */

/* s16 staging ring: player thread (main loop) writes, ISR reads + downconverts. */
static int16_t        g_ring[RING_SAMPS];
static volatile long  g_ring_head;           /* write cursor (main)               */
static volatile long  g_ring_tail;           /* read cursor (ISR)                 */
static volatile int   g_finished;            /* no more audio_submit coming       */

/* Saved PM interrupt vector (selector:offset), restored on shutdown. */
static unsigned short g_old_sel;
static unsigned long  g_old_off;

/* ---------- low-level port helpers ---------- */
#define DSP_RESET   (g_base + 0x6)
#define DSP_READ    (g_base + 0xA)
#define DSP_WRITE   (g_base + 0xC)   /* bit7=1 -> busy on read */
#define DSP_RSTAT   (g_base + 0xE)   /* read-data status; read to ack 8-bit IRQ */

static void dsp_write(int v) {
    while (inp(DSP_WRITE) & 0x80) { /* wait until DSP ready for a command */ }
    outp(DSP_WRITE, v);
}

/* ---------- BLASTER env parse ---------- */
static void parse_blaster(void) {
    const char *b = getenv("BLASTER");
    if (!b) return;                 /* keep A220 I5 D1 defaults */
    while (*b) {
        int c = *b++;
        if (c == 'A' || c == 'a') g_base = (int)strtol(b, NULL, 16);
        else if (c == 'I' || c == 'i') g_irq = (int)strtol(b, NULL, 10);
        else if (c == 'D' || c == 'd') g_dma = (int)strtol(b, NULL, 10);
        while (*b && *b != ' ') b++;
        while (*b == ' ') b++;
    }
}

/* ---------- DPMI: low DOS memory for the DMA buffer ---------- */
/* Allocate `paras` paragraphs (16 B) of real-mode-addressable memory. Returns the
 * linear base (identity-mapped, == physical), fills seg/sel for freeing. */
static unsigned char *dpmi_alloc_dos(unsigned paras,
                                     unsigned short *seg, unsigned short *sel) {
    union REGS r;
    r.w.ax = 0x0100;        /* DPMI allocate DOS memory block */
    r.w.bx = paras;
    int386(0x31, &r, &r);
    if (r.w.cflag) return NULL;
    *seg = r.w.ax;          /* real-mode segment */
    *sel = r.w.dx;          /* PM selector for the same memory */
    return (unsigned char *)((unsigned long)r.w.ax << 4); /* linear base */
}

static void dpmi_free_dos(unsigned short sel) {
    union REGS r;
    r.w.ax = 0x0101;
    r.w.dx = sel;
    int386(0x31, &r, &r);
}

/* Lock a linear region so the DPMI host won't page it out under interrupt. */
static void dpmi_lock(void *addr, unsigned long len) {
    union REGS r;
    unsigned long lin = (unsigned long)addr;
    r.w.ax = 0x0600;                 /* lock linear region */
    r.w.bx = (unsigned short)(lin >> 16);
    r.w.cx = (unsigned short)(lin & 0xFFFF);
    r.w.si = (unsigned short)(len >> 16);
    r.w.di = (unsigned short)(len & 0xFFFF);
    int386(0x31, &r, &r);
}

/* Get a protected-mode interrupt vector. DPMI returns the handler selector in CX
 * and its 32-bit offset in EDX (we are a 32-bit client). */
static void dpmi_get_pm_vector(int vec, unsigned short *sel, unsigned long *off) {
    union REGS r;
    r.x.eax = 0x0204; r.h.bl = (unsigned char)vec;
    int386(0x31, &r, &r);
    *sel = r.w.cx;
    *off = r.x.edx;
}

/* Set a protected-mode interrupt vector to sel:off. */
static void dpmi_set_pm_vector(int vec, unsigned short sel, unsigned long off) {
    union REGS r;
    r.x.eax = 0x0205; r.h.bl = (unsigned char)vec;
    r.w.cx = sel;
    r.x.edx = off;
    int386(0x31, &r, &r);
}

/* Our own code selector (CS), needed when installing a PM handler. */
extern unsigned short get_cs(void);
#pragma aux get_cs = "mov ax, cs" value [ax] modify [ax];

/* ---------- 8237 DMA programming ---------- */
/* 8-bit channels 0..3: page/addr/count port maps. */
static const unsigned char dma_page_port[4] = { 0x87, 0x83, 0x81, 0x82 };
static const unsigned char dma_addr_port[4] = { 0x00, 0x02, 0x04, 0x06 };
static const unsigned char dma_cnt_port[4]  = { 0x01, 0x03, 0x05, 0x07 };

static void program_dma(unsigned long phys, unsigned len) {
    int ch = g_dma & 3;
    unsigned mask = 0x04 | ch;       /* set mask bit for this channel */
    unsigned mode = 0x58 | ch;       /* single-channel, auto-init, read, ch */
    unsigned off  = (unsigned)(phys & 0xFFFF);
    unsigned page = (unsigned)((phys >> 16) & 0xFF);
    unsigned cnt  = len - 1;

    outp(0x0A, mask);                /* mask channel                          */
    outp(0x0C, 0x00);                /* clear flip-flop                       */
    outp(0x0B, mode);                /* mode: auto-init, single, read         */
    outp(dma_addr_port[ch], off & 0xFF);
    outp(dma_addr_port[ch], (off >> 8) & 0xFF);
    outp(dma_page_port[ch], page);
    outp(dma_cnt_port[ch], cnt & 0xFF);
    outp(dma_cnt_port[ch], (cnt >> 8) & 0xFF);
    outp(0x0A, ch);                  /* unmask channel                        */
}

/* ---------- ring -> u8 DMA half fill (called from ISR) ---------- */
static void fill_half(int half) {
    unsigned char *dst = g_dmabuf + (half ? DMA_HALF : 0);
    int i;
    for (i = 0; i < DMA_HALF; ++i) {
        int16_t s;
        if (g_ring_tail != g_ring_head) {
            s = g_ring[g_ring_tail & (RING_SAMPS - 1)];
            g_ring_tail++;
        } else {
            s = 0;                   /* underrun / end of stream -> silence */
        }
        dst[i] = (unsigned char)(((s >> 8) & 0xFF) ^ 0x80); /* s16 -> u8 */
    }
}

/* ---------- the SB interrupt handler ---------- */
static void __interrupt sb_isr(void) {
    /* The DMA is now playing the half opposite the one that just finished.
     * Refill the finished half and advance the clock by one half-buffer. */
    int finished = g_half;           /* the half that just drained */
    fill_half(finished);
    g_half = finished ^ 1;
    g_played += DMA_HALF;

    inp(DSP_RSTAT);                  /* ack DSP 8-bit DMA interrupt */
    if (g_irq >= 8) outp(0xA0, 0x20);/* ack slave PIC */
    outp(0x20, 0x20);                /* ack master PIC */
}

/* ---------- PIC mask helpers ---------- */
static void pic_unmask(int irq) {
    if (irq < 8) outp(0x21, inp(0x21) & ~(1 << irq));
    else { outp(0x21, inp(0x21) & ~(1 << 2)); /* cascade */
           outp(0xA1, inp(0xA1) & ~(1 << (irq - 8))); }
}
static void pic_mask(int irq) {
    if (irq < 8) outp(0x21, inp(0x21) | (1 << irq));
    else outp(0xA1, inp(0xA1) | (1 << (irq - 8)));
}

/* IRQ -> interrupt vector number. Master PIC IRQ0-7 -> 0x08..0x0F,
 * slave IRQ8-15 -> 0x70..0x77. */
static int irq_vector(int irq) {
    return irq < 8 ? 0x08 + irq : 0x70 + (irq - 8);
}

/* ---------- DSP init ---------- */
static int dsp_reset(void) {
    int i;
    outp(DSP_RESET, 1);
    for (i = 0; i < 100; ++i) inp(DSP_RESET);   /* >3 us */
    outp(DSP_RESET, 0);
    for (i = 0; i < 5000; ++i)
        if (inp(DSP_RSTAT) & 0x80)               /* read-data available */
            if (inp(DSP_READ) == 0xAA) return 0;  /* DSP ready signature */
    return 1;                                    /* no SB at this base */
}

/* ---------- audio.h interface ---------- */
int audio_init(int rate, int channels) {
    unsigned vec;
    (void)channels;                  /* mono only */
    g_rate = rate > 0 ? rate : 8000;

    if (getenv("TVID_NOAUDIO")) return 1; /* debug: silent video, skip all SB/DMA/IRQ setup */

    parse_blaster();
    if (dsp_reset() != 0) return 1;  /* fall back to silent video */

    /* Allocate a DMA buffer in low memory, aligned so it can't cross 64 KB.
     * Over-allocate by one buffer's worth and pick an aligned start. */
    {
        unsigned paras = (DMA_TOTAL * 2 + 15) / 16;
        unsigned char *raw = dpmi_alloc_dos(paras, &g_dos_seg, &g_dos_sel);
        unsigned long base, aligned;
        if (!raw) return 1;
        base = (unsigned long)raw;
        aligned = (base + DMA_TOTAL - 1) & ~((unsigned long)DMA_TOTAL - 1);
        /* If aligning pushed us across a 64 KB boundary relative to base, the
         * over-allocation guarantees room; verify the whole buffer fits one page. */
        if ((aligned & 0xFFFF) + DMA_TOTAL > 0x10000)
            aligned = (aligned + 0x10000) & ~0xFFFFUL; /* next 64 KB page */
        g_dmabuf = (unsigned char *)aligned;
        g_dmaphys = aligned;         /* identity-mapped low memory */
    }
    memset(g_dmabuf, 0x80, DMA_TOTAL); /* u8 silence */

    /* Page-lock everything the ISR touches. */
    dpmi_lock((void *)sb_isr, 4096);
    dpmi_lock(g_dmabuf, DMA_TOTAL);
    dpmi_lock((void *)g_ring, sizeof(g_ring));
    dpmi_lock((void *)&g_played, sizeof(g_played));
    dpmi_lock((void *)&g_half, sizeof(g_half));
    dpmi_lock((void *)&g_ring_head, sizeof(g_ring_head));
    dpmi_lock((void *)&g_ring_tail, sizeof(g_ring_tail));

    g_half = 0; g_played = 0;
    g_ring_head = g_ring_tail = 0;
    g_finished = 0;

    /* Hook the PM interrupt vector for the SB IRQ: save the old one, install
     * ours (our CS selector + the flat 32-bit address of sb_isr). */
    vec = irq_vector(g_irq);
    dpmi_get_pm_vector(vec, &g_old_sel, &g_old_off);
    dpmi_set_pm_vector(vec, get_cs(), (unsigned long)sb_isr);

    pic_unmask(g_irq);

    /* Program the 8237 for the full auto-init buffer, then tell the DSP to play
     * a half-buffer's worth of bytes per DMA cycle (interrupt every DMA_HALF). */
    program_dma(g_dmaphys, DMA_TOTAL);

    /* Set sample rate via the time-constant (good enough for SB/SBPro). */
    {
        int tc = 256 - (1000000 / g_rate);
        if (tc < 0) tc = 0; if (tc > 255) tc = 255;
        dsp_write(0x40);             /* set time constant */
        dsp_write(tc);
    }
    dsp_write(0xD1);                 /* turn speaker on */

    /* 8-bit single-cycle auto-init output (DSP cmd 0x1C); block size = half. */
    dsp_write(0x48);                 /* set DMA block size */
    dsp_write((DMA_HALF - 1) & 0xFF);
    dsp_write(((DMA_HALF - 1) >> 8) & 0xFF);
    dsp_write(0x1C);                 /* auto-init 8-bit DMA output */

    g_running = 1;
    return 0;
}

void audio_submit(const int16_t *pcm, long n) {
    long i;
    if (!g_running) return;
    for (i = 0; i < n; ++i) {
        long next = (g_ring_head + 1);
        /* Wait for the ISR to make room (it drains as the DMA plays). */
        while (next - g_ring_tail > RING_SAMPS) { /* spin; main loop only */ }
        g_ring[g_ring_head & (RING_SAMPS - 1)] = pcm[i];
        g_ring_head = next;
    }
}

long audio_played_samples(void) {
    return (long)g_played;
}

void audio_finish(void) {
    g_finished = 1;
}

void audio_shutdown(void) {
    if (!g_running) return;
    g_running = 0;

    dsp_write(0xDA);                 /* exit auto-init 8-bit DMA */
    dsp_write(0xD3);                 /* speaker off */
    outp(0x0A, 0x04 | (g_dma & 3));  /* mask the DMA channel */

    pic_mask(g_irq);
    dpmi_set_pm_vector(irq_vector(g_irq), g_old_sel, g_old_off); /* restore */

    if (g_dos_sel) dpmi_free_dos(g_dos_sel);
    g_dmabuf = NULL;
}
