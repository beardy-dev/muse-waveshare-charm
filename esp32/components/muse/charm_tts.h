#pragma once

#include <stdbool.h>

/*
 * On-device speech with PicoTTS (spike, US English). The engine's 1.1 MB
 * work area and its task stack are in PSRAM; the voice data is in the app
 * image. It starts on first use and stays up.
 */

/* Says text through the speaker; returns once it's said. Voice task only:
 * it writes the speaker. Logs RAM and timing. False if the engine won't start. */
bool charm_tts_say(const char *text);

/* Pitch for what's said next, in percent (50-200; 100 is the voice as it
 * is). Lower is deeper and costs some CPU. */
void charm_tts_set_pitch(int percent);
