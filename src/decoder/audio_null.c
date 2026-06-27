/* audio_null.c - no-op audio backend (non-Apple hosts). The player still decodes
 * and paces video; audio is simply dropped. audio_played_samples() returns 0 so
 * the player falls back to its timer-based video pacing. */
#include "audio.h"

int audio_init(int rate, int channels) { (void)rate; (void)channels; return 1; }
void audio_submit(const int16_t *pcm, long nsamples) { (void)pcm; (void)nsamples; }
long audio_played_samples(void) { return 0; }
void audio_finish(void) {}
void audio_shutdown(void) {}
