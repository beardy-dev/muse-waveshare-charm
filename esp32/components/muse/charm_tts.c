#include "charm_tts.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "muse_audio.h"
#include "muse_state.h"
#include "picotts.h"

static const char *TAG = "charm_tts";

/* Below the UI and everything else that matters: speech gets spare CPU only. */
#define TTS_PRIORITY 4
#define SAY_TIMEOUT_MS 60000

/*
 * Every voice PicoTTS has is female; a lower pitch is deeper, at some CPU.
 * Measured per second of speech (2026-10-23): 100% and 90% 0.79 s, 80-60%
 * 0.86-0.88 s. 90% is the deepest that costs nothing extra (Andrew's pick).
 */
static int s_pitch = 90;   /* percent; 100 is the voice as it is */

static bool s_ready;
static SemaphoreHandle_t s_done;
static volatile bool s_started;
static int64_t s_t0, s_first_us, s_last_us, s_write_us;
static size_t s_frames;

/* PicoTTS task: 16 kHz samples straight to the speaker, as made. */
static void on_samples(int16_t *buf, unsigned count)
{
    int64_t t = esp_timer_get_time();
    if (!s_started) {
        s_started = true;
        s_first_us = t - s_t0;
    }
    muse_state_set_level(muse_audio_level(buf, count));
    muse_audio_write(buf, count);
    s_last_us = esp_timer_get_time();
    s_write_us += s_last_us - t;
    s_frames += count;
}

/* PicoTTS task: idle again. It also goes idle once before anything is said. */
static void on_idle(void)
{
    if (s_started) {
        xSemaphoreGive(s_done);
    }
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: int %u/%u min %u, psram %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void charm_tts_set_pitch(int percent)
{
    s_pitch = percent < 50 ? 50 : percent > 200 ? 200 : percent;
    ESP_LOGI(TAG, "pitch %d%%", s_pitch);
}

bool charm_tts_say(const char *text)
{
    if (!s_ready) {
        log_heap("before engine");
        int64_t t = esp_timer_get_time();
        if (!s_done) {
            s_done = xSemaphoreCreateBinary();
        }
        if (!s_done || !picotts_init(TTS_PRIORITY, on_samples, -1)) {
            ESP_LOGE(TAG, "engine didn't start");
            return false;
        }
        picotts_set_idle_notify(on_idle);
        s_ready = true;
        ESP_LOGI(TAG, "engine up in %lld ms", (long long)((esp_timer_get_time() - t) / 1000));
        log_heap("engine up");
    }
    xSemaphoreTake(s_done, 0);   /* a stale one */
    s_started = false;
    s_frames = 0;
    s_write_us = 0;
    s_t0 = s_last_us = esp_timer_get_time();
    int pitch = s_pitch;
    if (pitch != 100) {
        char open[32];
        snprintf(open, sizeof(open), "<pitch level='%d'>", pitch);
        picotts_add(open, strlen(open));
    }
    picotts_add(text, strlen(text));
    if (pitch != 100) {
        picotts_add("</pitch>", 8);
    }
    picotts_add("", 1);   /* the \0 starts it */
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(SAY_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "no end of speech after %d s", SAY_TIMEOUT_MS / 1000);
    }
    muse_state_set_level(0);
    /* Making the audio, apart from waiting on the speaker: under the audio's
     * length means it keeps up with real time. */
    double audio_s = (double)s_frames / 16000;
    double synth_s = (double)(s_last_us - s_t0 - s_write_us) / 1e6;
    ESP_LOGI(TAG, "said %u chars at pitch %d: %.2f s of audio, first sound after %lld ms, synthesis %.2f s (%.1fx real time)",
             (unsigned)strlen(text), pitch, audio_s, (long long)(s_first_us / 1000), synth_s,
             synth_s > 0 ? audio_s / synth_s : 0.0);
    log_heap("after");
    return true;
}
