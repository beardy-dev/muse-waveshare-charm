#include "charm_clock.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "muse_wifi.h"

static const char *TAG = "charm_clock";

/* US Central, with daylight saving (second Sunday of March to first Sunday of November). */
#define CHARM_TZ "CST6CDT,M3.2.0,M11.1.0"

static bool s_started;

void charm_clock_tick(void)
{
    if (s_started || !muse_wifi_connected()) {
        return;
    }
    setenv("TZ", CHARM_TZ, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.wait_for_sync = false;   /* nobody blocks on it; the label just waits */
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "SNTP started");
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        s_started = true;   /* don't retry every tick */
    }
}

bool charm_clock_hhmm(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 2025 - 1900) {
        return false;   /* not synced: the clock still thinks it's 1970 */
    }
    strftime(out, cap, "%H:%M", &tm);
    return true;
}
