#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Charm notifications: Muse calls notify.show to put a pixel-art icon, a line
 * of text and optional answer buttons over the avatar, with a chirp. A tap
 * sends the answer back to Muse as a typed turn, so it can arrive long after
 * the command returned.
 */

#define CHARM_ICON_N 16           /* icons are CHARM_ICON_N x CHARM_ICON_N */
#define CHARM_ID_MAX 48
#define CHARM_TEXT_MAX 64

typedef enum {
    CHARM_ACTIONS_CONFIRM_DECLINE = 0,
    CHARM_ACTIONS_CONFIRM,
    CHARM_ACTIONS_NONE,
} charm_actions_t;

/* RGB565 per pixel; transparent pixels are 0 (black, the panel's colour). */
typedef struct {
    char id[CHARM_ID_MAX];
    char text[CHARM_TEXT_MAX];
    uint16_t icon[CHARM_ICON_N * CHARM_ICON_N];
    charm_actions_t actions;
    bool chime;
} charm_notify_t;

/*
 * Any task: queues the notification (copied) for the UI and wakes the screen.
 * False if the UI isn't up yet. *replaced: one with the same id was showing.
 */
bool charm_notify_show(const charm_notify_t *n, bool *replaced);

/* Any task: clears the notification with this id, or any one if id is NULL or empty. Returns true if one was cleared. */
bool charm_notify_clear(const char *id);

/* UI task, display lock held, once per frame. */
void charm_notify_tick(float now);

/* UI task: builds the inbox page (past notifications) into a tile. */
struct _lv_obj_t;
void charm_notify_build_inbox(struct _lv_obj_t *tile);

/* UI task: the inbox page came on or went off screen. */
void charm_notify_inbox_visible(bool visible);
