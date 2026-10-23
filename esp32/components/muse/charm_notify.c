#include "charm_notify.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

#define COLOR_YES 0x1f9a4a
#define COLOR_NO 0xc7304a
#define COLOR_TEXT 0xd8f2ff

/* Hand-off from the command task to the UI task. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static charm_notify_t *s_pending;      /* PSRAM; valid while s_has_pending */
static bool s_has_pending;
static bool s_clear_pending;
static char s_clear_id[CHARM_ID_MAX];
static char s_showing_id[CHARM_ID_MAX];  /* what's on screen, for clear and replace */

/* UI-owned. */
static charm_notify_t *s_cur;
static lv_obj_t *s_panel, *s_icon, *s_text, *s_yes, *s_no;
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
    if (s_cur) {
        return true;
    }
    s_pending = heap_caps_calloc(1, sizeof(*s_pending), MUSE_BIG_CAPS);
    s_cur = heap_caps_calloc(1, sizeof(*s_cur), MUSE_BIG_CAPS);
    s_icon_buf = heap_caps_malloc(ICON_PX * ICON_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_yes_buf = heap_caps_malloc(GLYPH_PX * GLYPH_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_no_buf = heap_caps_malloc(GLYPH_PX * GLYPH_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    if (!s_pending || !s_cur || !s_icon_buf || !s_yes_buf || !s_no_buf) {
        ESP_LOGE(TAG, "out of memory");
        return false;
    }
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

/* Nearest-neighbour blow-up, so the pixels stay chunky. */
static void scale_icon(const uint16_t *src, int n, int k, uint16_t *dst)
{
    int px = n * k;
    for (int y = 0; y < px; y++) {
        const uint16_t *row = &src[(y / k) * n];
        for (int x = 0; x < px; x++) {
            dst[y * px + x] = row[x / k];
        }
    }
}

static void stamp_glyph(const char *const *rows, uint16_t ink, uint16_t bg, uint16_t *dst)
{
    uint16_t small[GLYPH_N * GLYPH_N];
    for (int y = 0; y < GLYPH_N; y++) {
        for (int x = 0; x < GLYPH_N; x++) {
            small[y * GLYPH_N + x] = rows[y][x] == '#' ? ink : bg;
        }
    }
    scale_icon(small, GLYPH_N, GLYPH_SCALE, dst);
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xf8) << 8) | (((c >> 8) & 0xfc) << 3) | ((c & 0xff) >> 3));
}

static void answer(const char *what)
{
    /* An automatic message, so Muse doesn't mistake it for Andrew typing. */
    char *msg = malloc(256);
    if (msg) {
        snprintf(msg, 256,
                 "[charm notify] id=%s answer=%s (automatic message from Andrew's charm: "
                 "Andrew tapped %s on this notification)",
                 s_cur->id, what,
                 strcmp(what, "confirmed") == 0 ? "the green check"
                 : strcmp(what, "declined") == 0 ? "the red X" : "it to dismiss it");
        ESP_LOGI(TAG, "answered %s: %s", s_cur->id, what);
        muse_hatch_text_turn(msg);   /* frees it */
    }
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    portENTER_CRITICAL(&s_lock);
    s_showing_id[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

static void on_yes(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
    answer("confirmed");
}

static void on_no(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
    answer("declined");
}

static void on_panel(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
    if (s_cur->actions == CHARM_ACTIONS_NONE) {
        answer("seen");
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

static void build(void)
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
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_panel, on_panel, LV_EVENT_CLICKED, NULL);

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

static void present(float now)
{
    charm_token_prepare(s_cur->icon);
    charm_token_render(0, s_icon_buf);
    /* Same buffer, new pixels: re-setting the source makes LVGL redraw it. */
    lv_image_set_src(s_icon, NULL);
    lv_image_set_src(s_icon, &s_icon_dsc);
    lv_label_set_text(s_text, s_cur->text);

    bool yes = s_cur->actions != CHARM_ACTIONS_NONE;
    bool no = s_cur->actions == CHARM_ACTIONS_CONFIRM_DECLINE;
    lv_obj_set_flag(s_yes, LV_OBJ_FLAG_HIDDEN, !yes);
    lv_obj_set_flag(s_no, LV_OBJ_FLAG_HIDDEN, !no);
    int w = lv_display_get_horizontal_resolution(NULL);
    lv_obj_set_x(s_yes, no ? w / 2 - BUTTON_PX - 14 : (w - BUTTON_PX) / 2);

    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    s_shown_at = now;
    if (s_cur->chime) {
        muse_voice_request_chirp();
    }
    ESP_LOGI(TAG, "showing %s", s_cur->id);
}

bool charm_notify_show(const charm_notify_t *n, bool *replaced)
{
    if (!s_pending) {
        return false;   /* the UI hasn't run its first tick */
    }
    portENTER_CRITICAL(&s_lock);
    *replaced = s_showing_id[0] && strcmp(s_showing_id, n->id) == 0;
    memcpy(s_pending, n, sizeof(*n));
    s_pending->id[CHARM_ID_MAX - 1] = '\0';
    s_pending->text[CHARM_TEXT_MAX - 1] = '\0';
    s_has_pending = true;
    portEXIT_CRITICAL(&s_lock);
    /* Wake the screen so it's seen; the chirp is what gets attention. */
    muse_state_set_asleep(false);
    return true;
}

bool charm_notify_clear(const char *id)
{
    portENTER_CRITICAL(&s_lock);
    bool match = s_showing_id[0] && (!id || !id[0] || strcmp(id, s_showing_id) == 0);
    if (match) {
        strlcpy(s_clear_id, s_showing_id, sizeof(s_clear_id));
        s_clear_pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return match;
}

void charm_notify_tick(float now)
{
    if (!s_panel) {
        if (!alloc_buffers()) {
            return;
        }
        build();
    }

    bool fresh = false, clear = false;
    portENTER_CRITICAL(&s_lock);
    if (s_has_pending) {
        memcpy(s_cur, s_pending, sizeof(*s_cur));
        strlcpy(s_showing_id, s_cur->id, sizeof(s_showing_id));
        s_has_pending = false;
        fresh = true;
    } else if (s_clear_pending) {
        s_clear_pending = false;
        clear = strcmp(s_clear_id, s_showing_id) == 0;
        if (clear) {
            s_showing_id[0] = '\0';
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (fresh) {
        present(now);
    } else if (clear) {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "cleared");
    }
    if (!lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        /* It floats and rocks, like GUPPI. */
        float t = now - s_shown_at;
        charm_token_render(t, s_icon_buf);
        lv_obj_invalidate(s_icon);
        lv_obj_set_y(s_icon, s_icon_y + (int)lroundf(sinf(t * 1.8f) * 8.0f));
    }
}
