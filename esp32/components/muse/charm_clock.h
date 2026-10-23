#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Wall-clock time for the charm's status row: SNTP over Wi-Fi, in Andrew's
 * time zone (US Central). The Muse firmware keeps no clock of its own.
 */

/* UI task, a few times a second: starts SNTP once Wi-Fi is up. */
void charm_clock_tick(void);

/* "HH:MM" (24 h) into out; false until the clock has synced. */
bool charm_clock_hhmm(char *out, size_t cap);
