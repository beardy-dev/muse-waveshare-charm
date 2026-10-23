#include "charm_notify.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

#include "charm_token.h"
#include "muse_chat.h"
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_voice.h"

static const char *TAG = "charm_notify";

/* The icon is drawn as a 3D token (charm_token.c); the button glyphs are flat, 12 px -> 48 px. */
#define ICON_PX CHARM_TOKEN_PX
_Static_assert(CHARM_ICON_N == CHARM_TOKEN_N, "notify icons and tokens must be the same size");
#define GLYPH_N 12
#define GLYPH_SCALE 4
#define GLYPH_PX (GLYPH_N * GLYPH_SCALE)
#define BUTTON_PX 96
#define THUMB_SCALE 3
#define THUMB_PX (CHARM_ICON_N * THUMB_SCALE)
#define INBOX_MAX 8

#define COLOR_YES 0x1f9a4a
#define COLOR_NO 0xc7304a
#define COLOR_TEXT 0xd8f2ff
#define COLOR_DIM 0x6f86b0
#define COLOR_ROW 0x0c1736
#define COLOR_ROW_WAITING 0x14285e

typedef enum {
    ST_WAITING = 0,
    ST_CONFIRMED,
    ST_DECLINED,
    ST_SEEN,
} state_t;

typedef struct {
    charm_notify_t n;
    state_t state;
    int64_t at_us;          /* when it last arrived */
    uint16_t thumb[THUMB_PX * THUMB_PX];
    lv_image_dsc_t dsc;
} entry_t;

/* Requests from the command task, applied by the UI task. */
typedef enum { OP_SHOW, OP_CLEAR } op_kind_t;
typedef struct {
    op_kind_t kind;
    charm_notify_t n;       /* OP_CLEAR uses n.id only ("" = whatever is open) */
} op_t;

static QueueHandle_t s_ops;

/* Ids in the inbox, mirrored for the command task's replies. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_known[INBOX_MAX][CHARM_ID_MAX];

/* UI-owned. The inbox, newest first, and the one open on the overlay (by id; "" = none). */
static entry_t *s_inbox;
static int s_count;
static char s_open_id[CHARM_ID_MAX];
static op_t *s_op;          /* scratch for receiving */
static bool s_inbox_dirty = true, s_inbox_visible;
static float s_inbox_refreshed;

static lv_obj_t *s_panel, *s_icon, *s_text, *s_yes, *s_no;
static lv_obj_t *s_list;
static uint16_t *s_icon_buf, *s_yes_buf, *s_no_buf;
static lv_image_dsc_t s_icon_dsc, s_yes_dsc, s_no_dsc;
static float s_shown_at;
static int s_icon_y;

static const char *const GLYPH_CHECK[GLYPH_N] = {
    "............",
    "...........#",
    "..........##",
    ".........##.",
    "........##..",
    "#......##...",
    "##....##....",
    ".##..##.....",
    "..####......",
    "...##.......",
    "............",
    "............",
};

static const char *const GLYPH_CROSS[GLYPH_N] = {
    "............",
    ".##......##.",
    ".###....###.",
    "..###..###..",
    "...######...",
    "....####....",
    "....####....",
    "...######...",
    "..###..###..",
    ".###....###.",
    ".##......##.",
    "............",
};

static bool alloc_buffers(void)
{
    if (s_inbox) {
        return true;
    }
    s_ops = xQueueCreateWithCaps(4, sizeof(op_t), MUSE_BIG_CAPS);
    s_op = heap_caps_calloc(1, sizeof(*s_op), MUSE_BIG_CAPS);
    s_icon_buf = heap_caps_malloc(ICON_PX * ICON_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_yes_buf = heap_caps_malloc(GLYPH_PX * GLYPH_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_no_buf = heap_caps_malloc(GLYPH_PX * GLYPH_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    entry_t *inbox = heap_caps_calloc(INBOX_MAX, sizeof(entry_t), MUSE_BIG_CAPS);
    if (!s_ops || !s_op || !s_icon_buf || !s_yes_buf || !s_no_buf || !inbox) {
        ESP_LOGE(TAG, "out of memory");
        return false;
    }
    s_inbox = inbox;
    return true;
}

static void dsc_init(lv_image_dsc_t *d, uint16_t *buf, int px)
{
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = px;
    d->header.h = px;
    d->header.stride = px * sizeof(uint16_t);
    d->data_size = (size_t)px * px * sizeof(uint16_t);
    d->data = (const uint8_t *)buf;
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xf8) << 8) | (((c >> 8) & 0xfc) << 3) | ((c & 0xff) >> 3));
}

/* Nearest-neighbour blow-up, so the pixels stay chunky; transparent (0) becomes bg. */
static void scale_icon(const uint16_t *src, int n, int k, uint16_t bg, uint16_t *dst)
{
    int px = n * k;
    for (int y = 0; y < px; y++) {
        const uint16_t *row = &src[(y / k) * n];
        for (int x = 0; x < px; x++) {
            uint16_t c = row[x / k];
            dst[y * px + x] = c ? c : bg;
        }
    }
}

static void stamp_glyph(const char *const *rows, uint16_t ink, uint16_t bg, uint16_t *dst)
{
    uint16_t small[GLYPH_N * GLYPH_N];
    for (int y = 0; y < GLYPH_N; y++) {
        for (int x = 0; x < GLYPH_N; x++) {
            small[y * GLYPH_N + x] = rows[y][x] == '#' ? ink : 0;
        }
    }
    scale_icon(small, GLYPH_N, GLYPH_SCALE, bg, dst);
}

/* ---- the inbox store (UI task) ---- */

static void sync_known(void)
{
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < INBOX_MAX; i++) {
        strlcpy(s_known[i], i < s_count ? s_inbox[i].n.id : "", CHARM_ID_MAX);
    }
    portEXIT_CRITICAL(&s_lock);
    s_inbox_dirty = true;
}

static int find(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_inbox[i].n.id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static void remove_at(int i)
{
    memmove(&s_inbox[i], &s_inbox[i + 1], (size_t)(s_count - i - 1) * sizeof(entry_t));
    s_count--;
}

/* A new or repeated notification goes to the top, waiting again. */
static entry_t *upsert(const charm_notify_t *n)
{
    int i = find(n->id);
    if (i >= 0) {
        remove_at(i);
    } else if (s_count == INBOX_MAX) {
        s_count--;   /* the oldest falls off */
    }
    memmove(&s_inbox[1], &s_inbox[0], (size_t)s_count * sizeof(entry_t));
    s_count++;
    entry_t *e = &s_inbox[0];
    e->n = *n;
    e->state = ST_WAITING;
    e->at_us = esp_timer_get_time();
    scale_icon(n->icon, CHARM_ICON_N, THUMB_SCALE, rgb565(COLOR_ROW_WAITING), e->thumb);
    sync_known();
    return e;
}

/* ---- the overlay ---- */

static void hide_overlay(void)
{
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_open_id[0] = '\0';
}

static void open_overlay(const entry_t *e, bool chime, float now)
{
    strlcpy(s_open_id, e->n.id, sizeof(s_open_id));
    charm_token_prepare(e->n.icon);
    charm_token_render(0, s_icon_buf);
    /* Same buffer, new pixels: re-setting the source makes LVGL redraw it. */
    lv_image_set_src(s_icon, NULL);
    lv_image_set_src(s_icon, &s_icon_dsc);
    lv_label_set_text(s_text, e->n.text);

    bool yes = e->n.actions != CHARM_ACTIONS_NONE;
    bool no = e->n.actions == CHARM_ACTIONS_CONFIRM_DECLINE;
    lv_obj_set_flag(s_yes, LV_OBJ_FLAG_HIDDEN, !yes);
    lv_obj_set_flag(s_no, LV_OBJ_FLAG_HIDDEN, !no);
    int w = lv_display_get_horizontal_resolution(NULL);
    lv_obj_set_x(s_yes, no ? w / 2 - BUTTON_PX - 14 : (w - BUTTON_PX) / 2);

    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    s_shown_at = now;
    if (chime && e->n.chime) {
        muse_voice_request_chirp();
    }
    ESP_LOGI(TAG, "showing %s", e->n.id);
}

static void answer(state_t st)
{
    int i = find(s_open_id);
    if (i < 0) {
        hide_overlay();
        return;
    }
    entry_t *e = &s_inbox[i];
    e->state = st;
    const char *what = st == ST_CONFIRMED ? "confirmed" : st == ST_DECLINED ? "declined" : "seen";
    /* An automatic message, so Muse doesn't mistake it for Andrew typing. */
    char *msg = malloc(256);
    if (msg) {
        snprintf(msg, 256,
                 "[charm notify] id=%s answer=%s (automatic message from Andrew's charm: "
                 "Andrew tapped %s on this notification)",
                 e->n.id, what,
                 st == ST_CONFIRMED ? "the green check" : st == ST_DECLINED ? "the red X" : "it to dismiss it");
        ESP_LOGI(TAG, "answered %s: %s", e->n.id, what);
        muse_hatch_text_turn(msg);   /* frees it */
    }
    s_inbox_dirty = true;
    hide_overlay();
}

static void on_yes(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
    answer(ST_CONFIRMED);
}

static void on_no(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
    answer(ST_DECLINED);
}

static void on_panel(lv_event_t *e)
{
    muse_state_poke();
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        /* Swiped away: not now. It stays waiting in the inbox; Muse isn't told. */
        lv_indev_wait_release(lv_indev_active());
        ESP_LOGI(TAG, "set aside %s", s_open_id);
        hide_overlay();
        return;
    }
    int i = find(s_open_id);
    if (i >= 0 && s_inbox[i].n.actions == CHARM_ACTIONS_NONE) {
        answer(ST_SEEN);
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, uint32_t color, lv_image_dsc_t *glyph, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BUTTON_PX, BUTTON_PX);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *img = lv_image_create(b);
    lv_image_set_src(img, glyph);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static void build_overlay(void)
{
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);

    stamp_glyph(GLYPH_CHECK, 0xffff, rgb565(COLOR_YES), s_yes_buf);
    stamp_glyph(GLYPH_CROSS, 0xffff, rgb565(COLOR_NO), s_no_buf);
    dsc_init(&s_icon_dsc, s_icon_buf, ICON_PX);
    dsc_init(&s_yes_dsc, s_yes_buf, GLYPH_PX);
    dsc_init(&s_no_dsc, s_no_buf, GLYPH_PX);

    /* Over the avatar and the screen's own overlays; black, and it swallows taps. */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, w, h);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    /* Swipes stop here (LVGL hands a gesture to the first parent that doesn't bubble it). */
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_panel, on_panel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_panel, on_panel, LV_EVENT_GESTURE, NULL);

    s_icon_y = h / 2 - ICON_PX / 2 - 70;
    s_icon = lv_image_create(s_panel);
    lv_image_set_src(s_icon, &s_icon_dsc);
    lv_obj_set_pos(s_icon, (w - ICON_PX) / 2, s_icon_y);
    lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_CLICKABLE);

    s_text = lv_label_create(s_panel);
    lv_obj_set_width(s_text, w - 140);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_text, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, s_icon_y + ICON_PX + 22);
    lv_obj_remove_flag(s_text, LV_OBJ_FLAG_CLICKABLE);

    int by = h / 2 + 95;
    s_yes = make_button(s_panel, COLOR_YES, &s_yes_dsc, on_yes);
    lv_obj_set_pos(s_yes, w / 2 - BUTTON_PX - 14, by);
    s_no = make_button(s_panel, COLOR_NO, &s_no_dsc, on_no);
    lv_obj_set_pos(s_no, w / 2 + 14, by);
}

/* ---- the inbox page ---- */

static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    muse_state_poke();
    if (i < s_count && s_inbox[i].state == ST_WAITING) {
        open_overlay(&s_inbox[i], false, (float)esp_timer_get_time() / 1e6f);
    }
}

static void on_clear_all(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
    ESP_LOGI(TAG, "inbox cleared (%d)", s_count);
    s_count = 0;
    hide_overlay();
    sync_known();
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void age(int64_t at_us, char *out, size_t cap)
{
    int64_t s = (esp_timer_get_time() - at_us) / 1000000;
    if (s < 60) snprintf(out, cap, "NOW");
    else if (s < 3600) snprintf(out, cap, "%dM AGO", (int)(s / 60));
    else snprintf(out, cap, "%dH AGO", (int)(s / 3600));
}

static void rebuild_inbox(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);
    if (!s_count) {
        lv_obj_t *l = make_label(s_list, "NOTHING HERE", COLOR_DIM);
        lv_obj_set_style_pad_top(l, 60, 0);
        return;
    }
    for (int i = 0; i < s_count; i++) {
        entry_t *e = &s_inbox[i];
        bool waiting = e->state == ST_WAITING;
        /* Thumbnails sit on the row's colour, which shows the state. */
        uint16_t bg = rgb565(waiting ? COLOR_ROW_WAITING : COLOR_ROW);
        scale_icon(e->n.icon, CHARM_ICON_N, THUMB_SCALE, bg, e->thumb);
        dsc_init(&e->dsc, e->thumb, THUMB_PX);

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), THUMB_PX + 16);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(waiting ? COLOR_ROW_WAITING : COLOR_ROW), 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *img = lv_image_create(row);
        lv_image_set_src(img, &e->dsc);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *col = lv_obj_create(row);
        lv_obj_remove_style_all(col);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 4, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *t = make_label(col, e->n.text[0] ? e->n.text : e->n.id, COLOR_TEXT);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

        char when[16], status[40];
        age(e->at_us, when, sizeof(when));
        static const char *const NAMES[] = { "TAP TO ANSWER", "CONFIRMED", "DECLINED", "SEEN" };
        snprintf(status, sizeof(status), "%s - %s", NAMES[e->state], when);
        make_label(col, status, waiting ? 0x9fe6ff : COLOR_DIM);
    }

    lv_obj_t *clear = lv_obj_create(s_list);
    lv_obj_remove_style_all(clear);
    lv_obj_set_size(clear, 170, 48);
    lv_obj_set_style_radius(clear, 24, 0);
    lv_obj_set_style_bg_opa(clear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0x2a1430), 0);
    lv_obj_add_flag(clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clear, on_clear_all, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = make_label(clear, "CLEAR ALL", 0xff9cc8);
    lv_obj_center(cl);
}

void charm_notify_build_inbox(struct _lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);

    lv_obj_t *title = make_label(tile, "NOTIFICATIONS", COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    /* A column that scrolls inside the round screen's safe middle. */
    s_list = lv_obj_create(tile);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, w - 130, h - 150);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_style_pad_bottom(s_list, 40, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    s_inbox_dirty = true;
}

void charm_notify_inbox_visible(bool visible)
{
    s_inbox_visible = visible;
    if (visible) {
        s_inbox_dirty = true;   /* fresh ages */
    }
}

/* ---- any task ---- */

static bool known(const char *id)
{
    bool hit = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < INBOX_MAX && !hit; i++) {
        hit = s_known[i][0] && strcmp(s_known[i], id) == 0;
    }
    portEXIT_CRITICAL(&s_lock);
    return hit;
}

bool charm_notify_show(const charm_notify_t *n, bool *replaced)
{
    if (!s_ops) {
        return false;   /* the UI hasn't run its first tick */
    }
    *replaced = known(n->id);
    op_t *op = heap_caps_malloc(sizeof(*op), MUSE_BIG_CAPS);
    if (!op) {
        return false;
    }
    op->kind = OP_SHOW;
    op->n = *n;
    op->n.id[CHARM_ID_MAX - 1] = '\0';
    op->n.text[CHARM_TEXT_MAX - 1] = '\0';
    bool ok = xQueueSend(s_ops, op, pdMS_TO_TICKS(200)) == pdTRUE;
    free(op);
    /* Wake the screen so it's seen; the chirp is what gets attention. */
    muse_state_set_asleep(false);
    return ok;
}

bool charm_notify_clear(const char *id)
{
    if (!s_ops) {
        return false;
    }
    bool hit = id && id[0] ? known(id) : true;
    op_t *op = heap_caps_calloc(1, sizeof(*op), MUSE_BIG_CAPS);
    if (!op) {
        return false;
    }
    op->kind = OP_CLEAR;
    strlcpy(op->n.id, id ? id : "", CHARM_ID_MAX);
    xQueueSend(s_ops, op, pdMS_TO_TICKS(200));
    free(op);
    return hit;
}

/* ---- UI task, every frame ---- */

void charm_notify_tick(float now)
{
    if (!s_panel) {
        if (!alloc_buffers()) {
            return;
        }
        build_overlay();
    }

    while (xQueueReceive(s_ops, s_op, 0) == pdTRUE) {
        if (s_op->kind == OP_SHOW) {
            entry_t *e = upsert(&s_op->n);
            open_overlay(e, true, now);
        } else {
            const char *id = s_op->n.id[0] ? s_op->n.id : s_open_id;
            int i = find(id);
            if (strcmp(id, s_open_id) == 0) {
                hide_overlay();
            }
            if (i >= 0) {
                remove_at(i);   /* Muse says it no longer applies */
                sync_known();
            }
            ESP_LOGI(TAG, "cleared %s", id);
        }
    }

    if (s_inbox_visible && now - s_inbox_refreshed > 30.0f) {
        s_inbox_dirty = true;
    }
    if (s_inbox_dirty) {
        rebuild_inbox();
        s_inbox_dirty = false;
        s_inbox_refreshed = now;
    }

    if (!lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        /* It floats and rocks, like GUPPI. */
        float t = now - s_shown_at;
        charm_token_render(t, s_icon_buf);
        lv_obj_invalidate(s_icon);
        lv_obj_set_y(s_icon, s_icon_y + (int)lroundf(sinf(t * 1.8f) * 8.0f));
    }
}
