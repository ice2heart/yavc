/* audio.h - host audio output abstraction for the player (portable C).
 *
 * Mirrors backend.h for video: the player decodes ADPCM to s16 PCM and pushes it
 * here; the backend buffers and plays it, and reports how many samples have
 * actually reached the speaker so the video loop can pace itself to the audio
 * clock (A/V sync). Implementations: audio_coreaudio.c (macOS AudioQueue),
 * audio_null.c (no-op, video-only on non-Apple hosts), and audio_dos.c later
 * (Sound Blaster auto-init DMA). */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Start audio output at `rate` Hz, `channels` (1=mono). Returns 0 on success,
 * non-zero if audio is unavailable (caller falls back to silent playback). */
int audio_init(int rate, int channels);

/* Enqueue nsamples of interleaved s16 PCM for playback. May be called repeatedly
 * to stream; blocks only if the internal buffer is full. */
void audio_submit(const int16_t *pcm, long nsamples);

/* Total PCM samples that have been played out so far (monotonic). Used to derive
 * elapsed audio time for video pacing. Returns 0 before playback starts. */
long audio_played_samples(void);

/* Signal that no more samples will be submitted (lets the backend drain). */
void audio_finish(void);

/* Stop and release the audio device. */
void audio_shutdown(void);

#endif /* AUDIO_H */
