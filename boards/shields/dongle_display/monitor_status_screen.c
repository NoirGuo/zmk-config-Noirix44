/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/monitor_status.h>

#include "custom_status_screen.h"

/*
 * ST7789V 280x240 layout (landscape, 1.69" 240x280 rounded-corner panel
 * rotated 90° via mdac=0x60 in the overlay):
 *
 *   WPM 42                      U B2
 *             BASE
 *              H!@
 *            (mac modifier icons)
 *   L 85%      M 62%      R 18%
 *  [====]     [=====]      [==]
 *
 * Central area is three rows: layer name / last typed chars (letters, digits
 * and US-ANSI symbols) / mac modifier icons (CTRL SHIFT ALT GUI).
 *
 * Left bar   : left half keyboard battery
 * Middle bar : monitor (dongle) own battery
 * Right bar  : right half keyboard battery
 *
 * Bar fill grows left-to-right with the battery level (52px full scale).
 * Black background, white text. A ~30px safe margin is kept on every side so
 * the content stays inside the visible area of the rounded-corner panel.
 */

#define SCREEN_MARGIN 30

static lv_obj_t *screen;
static lv_obj_t *wpm;
static lv_obj_t *connection;
static lv_obj_t *layer;          /* Row 1: layer name (WAITING on link loss, unchanged) */
static lv_obj_t *typed;          /* Row 2: recently typed chars (letters/symbols) */
static lv_obj_t *img_ctrl;
static lv_obj_t *img_shift;
static lv_obj_t *img_alt;
static lv_obj_t *img_cmd;
static lv_obj_t *left_battery;   /* battery % value label (inside its group) */
static lv_obj_t *mid_battery;
static lv_obj_t *right_battery;
static lv_obj_t *left_grp;       /* battery text group: prefix L + value */
static lv_obj_t *mid_grp;        /* prefix M + value */
static lv_obj_t *right_grp;      /* prefix R + value */
static lv_obj_t *left_pre;
static lv_obj_t *mid_pre;
static lv_obj_t *right_pre;
static lv_obj_t *left_bar;
static lv_obj_t *mid_bar;
static lv_obj_t *right_bar;
static lv_obj_t *left_fill;
static lv_obj_t *mid_fill;
static lv_obj_t *right_fill;
static bool ready;

/* Mac-style modifier icons (bitmaps from widgets/modifiers_sym.c). */
LV_IMG_DECLARE(control_icon);
LV_IMG_DECLARE(shift_icon);
LV_IMG_DECLARE(alt_icon);
LV_IMG_DECLARE(cmd_icon);

/* Modifier mask bits used by the status advertisement. */
#define MOD_CTRL 0x11
#define MOD_SHIFT 0x22
#define MOD_ALT 0x44
#define MOD_GUI 0x88

/* Battery bar fill colors (level thresholds). */
#define BAR_GREEN 0x4CAF50
#define BAR_YELLOW 0xFFD600
#define BAR_RED 0xFF1744

/* Battery text prefix colors: L blue / M white / R orange. */
#define PRE_L_COLOR 0x2196F3
#define PRE_R_COLOR 0xFF9800

static void clean_obj(lv_obj_t *obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static lv_obj_t *make_modifier_img(const lv_img_dsc_t *dsc) {
    lv_obj_t *img = lv_img_create(screen);
    lv_img_set_src(img, dsc);
    lv_img_set_zoom(img, 384); /* 14px icon -> ~21px (1.5x) */
    return img;
}

static void configure_bar(lv_obj_t **track, lv_obj_t **fill) {
    *track = lv_obj_create(screen);
    clean_obj(*track);
    lv_obj_set_size(*track, 56, 12);
    lv_obj_set_style_border_width(*track, 2, 0);
    lv_obj_set_style_border_color(*track, lv_color_white(), 0);
    lv_obj_set_style_bg_color(*track, lv_color_black(), 0);

    *fill = lv_obj_create(*track);
    clean_obj(*fill);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    lv_obj_align(*fill, LV_ALIGN_LEFT_MID, 2, 0);
}

/* Battery text group: colored prefix (L blue / M white / R orange) on the
 * left, white "NN%" value on the right. The returned group is aligned over
 * its bar; the value keeps using set_battery_text()/set_bar(). */
static lv_obj_t *make_battery_group(lv_obj_t **pre, lv_obj_t **val,
                                    const char *prefix, lv_color_t color) {
    lv_obj_t *grp = lv_obj_create(screen);
    clean_obj(grp);
    lv_obj_set_size(grp, 62, 24);

    *pre = lv_label_create(grp);
    *val = lv_label_create(grp);
    clean_obj(*pre);
    clean_obj(*val);
    lv_obj_set_style_text_font(*pre, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_font(*val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(*pre, color, 0);
    lv_obj_set_style_text_align(*val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(*pre, prefix);
    lv_obj_align(*pre, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(*val, LV_ALIGN_RIGHT_MID, 0, 0);
    return grp;
}

static void set_bar_color(lv_obj_t *fill, uint8_t level) {
    lv_color_t color;

    if (level > 30) {
        color = lv_color_hex(BAR_GREEN);
    } else if (level > 10) {
        color = lv_color_hex(BAR_YELLOW);
    } else {
        color = lv_color_hex(BAR_RED);
    }
    lv_obj_set_style_bg_color(fill, color, 0);
}

static void set_bar(lv_obj_t *fill, uint8_t level) {
    level = MIN(level, 100);
    set_bar_color(fill, level);
    lv_obj_set_size(fill, MAX(1, (52 * level) / 100), 8);
}

static void set_battery_text(lv_obj_t *label, uint8_t level) {
    char text[8];
    if (level > 100) {
        lv_label_set_text(label, "--%");
        return;
    }
    snprintf(text, sizeof(text), "%u%%", level);
    lv_label_set_text(label, text);
}

static void update_screen(struct k_work *work) {
    ARG_UNUSED(work);
    if (!ready) {
        return;
    }

    struct zmk_monitor_status status;
    zmk_monitor_status_snapshot(&status);
    bool alive = status.present && (k_uptime_get_32() - status.last_seen_ms) < 15000U;
    char text[24];

    if (alive) {
        snprintf(text, sizeof(text), "WPM %u", status.wpm);
        lv_label_set_text(wpm, text);

        if (status.ble_connected) {
            snprintf(text, sizeof(text), "%c B%u", status.usb_ready ? 'U' : '-', status.profile);
        } else {
            snprintf(text, sizeof(text), "%c -", status.usb_ready ? 'U' : '-');
        }
        lv_label_set_text(connection, text);

        /* Row 1: layer name. */
        if (status.layer_name[0] != '\0') {
            snprintf(text, sizeof(text), "%s", status.layer_name);
        } else {
            snprintf(text, sizeof(text), "LAYER %u", status.layer);
        }
        lv_label_set_text(layer, text);
        lv_obj_align(layer, LV_ALIGN_CENTER, 0, -55);

        /* Row 2: recently typed chars (letters, digits, US-ANSI symbols). */
        lv_label_set_text(typed, status.typed_keys);
        lv_obj_align(typed, LV_ALIGN_CENTER, 0, -26);

        lv_obj_set_style_opa(img_ctrl, (status.modifiers & MOD_CTRL) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(img_shift, (status.modifiers & MOD_SHIFT) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(img_alt, (status.modifiers & MOD_ALT) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(img_cmd, (status.modifiers & MOD_GUI) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

        set_battery_text(left_battery, status.left_battery);
        set_battery_text(mid_battery, zmk_battery_state_of_charge());
        set_battery_text(right_battery, status.right_battery);
        set_bar(left_fill, status.left_battery);
        set_bar(mid_fill, zmk_battery_state_of_charge());
        set_bar(right_fill, status.right_battery);
    } else {
        lv_label_set_text(wpm, "WPM --");
        lv_label_set_text(connection, "-- --");
        /* Link loss: WAITING stays on the layer row, unchanged. */
        lv_label_set_text(layer, "WAITING");
        lv_label_set_text(typed, "");
        lv_obj_align(typed, LV_ALIGN_CENTER, 0, -26);
        lv_obj_set_style_opa(img_ctrl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(img_shift, LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(img_alt, LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(img_cmd, LV_OPA_TRANSP, 0);
        lv_label_set_text(left_battery, "--%");
        set_battery_text(mid_battery, zmk_battery_state_of_charge());
        lv_label_set_text(right_battery, "--%");
        set_bar(left_fill, 0);
        set_bar(mid_fill, zmk_battery_state_of_charge());
        set_bar(right_fill, 0);
    }
}

K_WORK_DEFINE(screen_update_work, update_screen);

void zmk_monitor_status_changed(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &screen_update_work);
    }
}

void zmk_display_settings_runtime_changed(void) {
    zmk_monitor_status_changed();
}

/* Keep the middle (monitor own battery) bar live: refresh when the battery
 * module publishes a new state-of-charge (every CONFIG_ZMK_BATTERY_REPORT_INTERVAL). */
static int battery_event_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    zmk_monitor_status_changed();
    return 0;
}
ZMK_LISTENER(monitor_battery, battery_event_listener);
ZMK_SUBSCRIPTION(monitor_battery, zmk_battery_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_coord_t hor = lv_disp_get_hor_res(NULL);
    lv_coord_t ver = lv_disp_get_ver_res(NULL);

    screen = lv_obj_create(NULL);
    clean_obj(screen);
    lv_obj_set_size(screen, hor, ver);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(screen, 1, 0);
    lv_obj_set_style_text_line_space(screen, 1, 0);

    wpm = lv_label_create(screen);
    connection = lv_label_create(screen);
    layer = lv_label_create(screen);
    typed = lv_label_create(screen);

    clean_obj(wpm);
    clean_obj(connection);
    clean_obj(layer);
    clean_obj(typed);

    lv_obj_set_style_text_font(wpm, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(connection, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_font(layer, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(typed, &lv_font_montserrat_24, 0);

    lv_obj_set_style_text_align(layer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(typed, LV_TEXT_ALIGN_CENTER, 0);

    /* Mac modifier icons: row 3, centered under the typed-chars row. */
    img_ctrl = make_modifier_img(&control_icon);
    img_shift = make_modifier_img(&shift_icon);
    img_alt = make_modifier_img(&alt_icon);
    img_cmd = make_modifier_img(&cmd_icon);
    lv_obj_align(img_ctrl, LV_ALIGN_CENTER, -40, 10);
    lv_obj_align(img_shift, LV_ALIGN_CENTER, -14, 10);
    lv_obj_align(img_alt, LV_ALIGN_CENTER, 12, 10);
    lv_obj_align(img_cmd, LV_ALIGN_CENTER, 38, 10);

    lv_obj_align(wpm, LV_ALIGN_TOP_LEFT, SCREEN_MARGIN, SCREEN_MARGIN);
    lv_obj_align(connection, LV_ALIGN_TOP_RIGHT, -SCREEN_MARGIN, SCREEN_MARGIN);
    lv_obj_align(layer, LV_ALIGN_CENTER, 0, -55);
    lv_obj_align(typed, LV_ALIGN_CENTER, 0, -26);

    /* Battery bars. The old LEFT / MONITOR / RIGHT labels under the bars are
     * gone; each bar now carries a colored prefix (L blue / M white / R
     * orange) plus the white "NN%" value above it. */
    configure_bar(&left_bar, &left_fill);
    configure_bar(&mid_bar, &mid_fill);
    configure_bar(&right_bar, &right_fill);
    lv_obj_align(left_bar, LV_ALIGN_BOTTOM_LEFT, SCREEN_MARGIN + 8, -(SCREEN_MARGIN + 12));
    lv_obj_align(mid_bar, LV_ALIGN_BOTTOM_MID, 0, -(SCREEN_MARGIN + 12));
    lv_obj_align(right_bar, LV_ALIGN_BOTTOM_RIGHT, -(SCREEN_MARGIN + 8), -(SCREEN_MARGIN + 12));

    /* Battery text groups, centered over their bars (group 62px wide, bar
     * centers at x=66 / 140 / 214 on the 280px panel). */
    left_grp = make_battery_group(&left_pre, &left_battery, "L",
                                  lv_color_hex(PRE_L_COLOR));
    mid_grp = make_battery_group(&mid_pre, &mid_battery, "M", lv_color_white());
    right_grp = make_battery_group(&right_pre, &right_battery, "R",
                                   lv_color_hex(PRE_R_COLOR));
    lv_obj_align(left_grp, LV_ALIGN_BOTTOM_LEFT, SCREEN_MARGIN + 5, -(SCREEN_MARGIN + 18));
    lv_obj_align(mid_grp, LV_ALIGN_BOTTOM_MID, 0, -(SCREEN_MARGIN + 18));
    lv_obj_align(right_grp, LV_ALIGN_BOTTOM_RIGHT, -(SCREEN_MARGIN + 5), -(SCREEN_MARGIN + 18));

    ready = true;
    update_screen(NULL);
    return screen;
}
