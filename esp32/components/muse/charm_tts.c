#include "charm_tts.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_state.h"
#include "picotts.h"

static const char *TAG = "charm_tts";

/* Below the UI and everything else that matters: speech gets spare CPU only. */
#define TTS_PRIORITY 4
#define BUF_BYTES (16000 * 2 * 2)   /* 2 s of speech made ahead of the reader */
#define SAY_TIMEOUT_US (60LL * 1000000)

/*
 * Every voice PicoTTS has is female; a lower pitch is deeper, at some CPU.
 * Measured per second of speech (2026-10-23): 100% and 90% 0.79 s, 80-60%
 * 0.86-0.88 s. 90% is the deepest that costs nothing extra (Andrew's pick).
 */
static int s_pitch = 90;   /* percent; 100 is the voice as it is */

static bool s_ready;
static StreamBufferHandle_t s_buf;
static volatile bool s_busy;      /* from begin until the engine is idle again */
static volatile bool s_discard;   /* cancelled: drop what it still makes */
static volatile bool s_cancelled; /* until the next begin: nothing more to read */
static volatile bool s_started;
static int64_t s_t0, s_first_us;
static volatile size_t s_frames;
static size_t s_chars;

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: int %u/%u min %u, psram %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* PicoTTS task: into the buffer, waiting while it's full (the reader sets the pace). */
static void on_samples(int16_t *buf, unsigned count)
{
    if (!s_started) {
        s_started = true;
        s_first_us = esp_timer_get_time() - s_t0;
    }
    size_t bytes = count * sizeof(int16_t), off = 0;
    while (off < bytes && !s_discard) {
        off += xStreamBufferSend(s_buf, (const uint8_t *)buf + off, bytes - off, pdMS_TO_TICKS(50));
    }
    s_frames += count;
}

/* PicoTTS task: idle again, after speech or an abort (and once after starting). */
static void on_idle(void)
{
    if (s_busy) {
        ESP_LOGI(TAG, "%s %u chars: %.2f s of speech, first after %lld ms", s_discard ? "stopped" : "said",
                 (unsigned)s_chars, (double)s_frames / 16000, (long long)(s_first_us / 1000));
    }
    s_discard = false;
    s_busy = false;
}

static bool engine_up(void)
{
    if (s_ready) {
        return true;
    }
    log_heap("before engine");
    int64_t t = esp_timer_get_time();
    if (!s_buf) {
        s_buf = xStreamBufferCreateWithCaps(BUF_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_buf || !picotts_init(TTS_PRIORITY, on_samples, -1)) {
        ESP_LOGE(TAG, "engine didn't start");
        return false;
    }
    picotts_set_idle_notify(on_idle);
    s_ready = true;
    ESP_LOGI(TAG, "engine up in %lld ms", (long long)((esp_timer_get_time() - t) / 1000));
    log_heap("engine up");
    return true;
}

bool charm_tts_begin(const char *text)
{
    if (!engine_up()) {
        return false;
    }
    if (s_busy) {
        ESP_LOGW(TAG, "still finishing the last one: no speech for this");
        return false;
    }
    xStreamBufferReset(s_buf);   /* the engine is idle: nothing's writing */
    s_discard = s_cancelled = s_started = false;
    s_frames = 0;
    s_chars = strlen(text);
    s_t0 = esp_timer_get_time();
    s_busy = true;
    /* The queue holds a whole reply, so none of this waits. */
    int pitch = s_pitch;
    if (pitch != 100) {
        char open[32];
        snprintf(open, sizeof(open), "<pitch level='%d'>", pitch);
        picotts_add(open, strlen(open));
    }
    picotts_add(text, s_chars);
    if (pitch != 100) {
        picotts_add("</pitch>", 8);
    }
    picotts_add("", 1);   /* the \0 starts it */
    return true;
}

size_t charm_tts_read(int16_t *out, size_t max)
{
    if (!s_ready || s_cancelled) {
        return 0;
    }
    return xStreamBufferReceive(s_buf, out, max * sizeof(int16_t), 0) / sizeof(int16_t);
}

bool charm_tts_done(void)
{
    return !s_busy && (s_cancelled || !s_buf || xStreamBufferIsEmpty(s_buf));
}

void charm_tts_cancel(void)
{
    s_cancelled = true;
    if (s_busy && !s_discard) {
        s_discard = true;
        picotts_abort();
    }
}

void charm_tts_set_pitch(int percent)
{
    s_pitch = percent < 50 ? 50 : percent > 200 ? 200 : percent;
    ESP_LOGI(TAG, "pitch %d%%", s_pitch);
}

bool charm_tts_say(const char *text)
{
    if (!charm_tts_begin(text)) {
        return false;
    }
    static int16_t buf[256];
    int64_t give_up = esp_timer_get_time() + SAY_TIMEOUT_US;
    while (!charm_tts_done() && esp_timer_get_time() < give_up) {
        size_t n = charm_tts_read(buf, 256);
        if (n) {
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    muse_state_set_level(0);
    log_heap("after");
    return true;
}
