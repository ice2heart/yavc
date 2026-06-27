/* audio_coreaudio.c - macOS host audio backend (AudioToolbox AudioQueue).
 *
 * A few output buffers cycle through a callback that pulls s16 PCM from a ring
 * buffer the player fills via audio_submit(). The played-sample counter is
 * advanced as buffers are handed to the queue, giving the video loop an audio
 * clock to pace against. This is the host verification path; the DOS Sound
 * Blaster backend (audio_dos.c) implements the same audio.h interface with
 * auto-init DMA. */
#include "audio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <string.h>

#define NUM_BUFFERS 4
#define BUFFER_FRAMES 2048           /* s16 samples per queue buffer */
#define RING_FRAMES (BUFFER_FRAMES * 16) /* host-side staging ring */

static AudioQueueRef g_queue;
static AudioQueueBufferRef g_buffers[NUM_BUFFERS];
static int g_channels = 1;
static int g_running = 0;

/* Ring buffer of s16 samples, filled by audio_submit (player thread), drained by
 * the AudioQueue callback (CoreAudio thread). Guarded by a mutex + condvar. */
static int16_t g_ring[RING_FRAMES];
static long g_ring_head, g_ring_tail; /* sample indices, mod RING_FRAMES */
static long g_ring_count;             /* samples currently queued */
static int g_finished;                /* no more audio_submit calls coming */
static long g_played;                 /* samples handed to the device */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_space = PTHREAD_COND_INITIALIZER; /* room to write */

static void fill_buffer(AudioQueueBufferRef buf) {
    int16_t *dst = (int16_t *)buf->mAudioData;
    UInt32 want = BUFFER_FRAMES;
    UInt32 got = 0;

    pthread_mutex_lock(&g_lock);
    while (got < want && g_ring_count > 0) {
        dst[got++] = g_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1) % RING_FRAMES;
        g_ring_count--;
    }
    int finished = g_finished;
    g_played += got;
    pthread_cond_signal(&g_space);
    pthread_mutex_unlock(&g_lock);

    /* Pad a partial buffer with silence (underrun or end-of-stream tail). */
    while (got < want) dst[got++] = 0;
    buf->mAudioDataByteSize = want * sizeof(int16_t);

    (void)finished; /* we keep enqueuing silence so the clock keeps advancing */
}

static void queue_cb(void *userData, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)userData;
    if (!g_running) return;
    fill_buffer(buf);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

int audio_init(int rate, int channels) {
    g_channels = channels < 1 ? 1 : channels;

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = (Float64)rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger |
                       kLinearPCMFormatFlagIsPacked;
    fmt.mBitsPerChannel = 16;
    fmt.mChannelsPerFrame = (UInt32)g_channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = (UInt32)(2 * g_channels);
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    if (AudioQueueNewOutput(&fmt, queue_cb, NULL, NULL, NULL, 0, &g_queue) != 0)
        return 1;

    g_running = 1;
    g_ring_head = g_ring_tail = g_ring_count = 0;
    g_finished = 0;
    g_played = 0;

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (AudioQueueAllocateBuffer(g_queue, BUFFER_FRAMES * sizeof(int16_t),
                                     &g_buffers[i]) != 0)
            return 1;
        fill_buffer(g_buffers[i]); /* prime with whatever's queued (likely silence) */
        AudioQueueEnqueueBuffer(g_queue, g_buffers[i], 0, NULL);
    }
    return AudioQueueStart(g_queue, NULL) == 0 ? 0 : 1;
}

void audio_submit(const int16_t *pcm, long nsamples) {
    long i = 0;
    pthread_mutex_lock(&g_lock);
    while (i < nsamples) {
        while (g_ring_count >= RING_FRAMES)
            pthread_cond_wait(&g_space, &g_lock);
        while (i < nsamples && g_ring_count < RING_FRAMES) {
            g_ring[g_ring_head] = pcm[i++];
            g_ring_head = (g_ring_head + 1) % RING_FRAMES;
            g_ring_count++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

long audio_played_samples(void) {
    /* Use the queue's sample-accurate playback timeline rather than the
     * buffer-fill counter g_played. g_played jumps forward 2048 samples at a
     * time (one whole buffer, ~256 ms @ 8 kHz) the instant the callback refills
     * a buffer — it does NOT track the speaker between refills. Pacing video off
     * it made frames present in 256 ms bursts (15 fps looked like ~4). The queue
     * timeline (mSampleTime) advances continuously with the hardware clock. */
    if (g_running) {
        AudioTimeStamp ts;
        memset(&ts, 0, sizeof(ts));
        if (AudioQueueGetCurrentTime(g_queue, NULL, &ts, NULL) == 0 &&
            (ts.mFlags & kAudioTimeStampSampleTimeValid)) {
            long s = (long)ts.mSampleTime;
            return s < 0 ? 0 : s;
        }
    }
    pthread_mutex_lock(&g_lock);
    long p = g_played;
    pthread_mutex_unlock(&g_lock);
    return p;
}

void audio_finish(void) {
    pthread_mutex_lock(&g_lock);
    g_finished = 1;
    pthread_mutex_unlock(&g_lock);
}

void audio_shutdown(void) {
    if (!g_running) return;
    g_running = 0;
    AudioQueueStop(g_queue, true);
    for (int i = 0; i < NUM_BUFFERS; ++i)
        if (g_buffers[i]) AudioQueueFreeBuffer(g_queue, g_buffers[i]);
    AudioQueueDispose(g_queue, true);
    g_queue = NULL;
}
