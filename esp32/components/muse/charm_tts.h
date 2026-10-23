#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * On-device speech with PicoTTS (US English). The engine's 1.1 MB work area,
 * its task stack and its text queue are in PSRAM; the voice data is in the
 * app image. It starts on first use and stays up. Its task runs below the UI,
 * on spare CPU, and makes speech into a 2 s buffer that the reader drains.
 */

/* Starts saying text (up to ~1 KB). False if the engine won't start or is
 * still finishing the last one: then there's no speech for this text. */
bool charm_tts_begin(const char *text);

/* Up to max samples of the speech so far (16 kHz mono); 0 if none are ready. */
size_t charm_tts_read(int16_t *out, size_t max);

/* True once it's all said and read. */
bool charm_tts_done(void);

/* Stops the speech: what's left isn't made or read. */
void charm_tts_cancel(void);

/* Says text through the speaker; returns once it's said. Voice task only:
 * it writes the speaker. False if the engine won't start. */
bool charm_tts_say(const char *text);

/* Pitch for what's said next, in percent (50-200; 100 is the voice as it
 * is). Lower is deeper and costs some CPU. */
void charm_tts_set_pitch(int percent);

#ifdef __cplusplus
}
#endif
