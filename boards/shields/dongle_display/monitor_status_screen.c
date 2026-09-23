/* SPDX-License-Identifier: MIT */

#include <stdio.h>

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/display.h>
#include <zmk/display_settings.h>
#include <zmk/monitor_status.h>

#include "custom_status_screen.h"

static lv_obj_t *screen;
static lv_obj_t *left_battery;
static lv_obj_t *right_battery;
static lv_obj_t *connection;
static lv_obj_t *details;
static lv_obj_t *left_bar;
static lv_obj_t *right_bar;
static lv_obj_t *left_fill;
static lv_obj_t *right_fill;
static bool ready;

static void clean_obj(lv_obj_t *obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void configure_bar(lv_obj_t **track, lv_obj_t **fill) {
    *track = lv_obj_create(screen);
    clean_obj(*track);
    lv_obj_set_size(*track, 43, 5);
    lv_obj_set_style_border_width(*track, 1, 0);
    lv_obj_set_style_border_color(*track, lv_color_black(), 0);

    *fill = lv_obj_create(*track);
    clean_obj(*fill);
    lv_obj_set_style_bg_color(*fill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    lv_obj_align(*fill, LV_ALIGN_LEFT_MID, 1, 0);
}

static void set_bar(lv_obj_t *fill, uint8_t level) {
    level = MIN(level, 100);
    lv_obj_set_size(fill, MAX(1, (41 * level) / 100), 3);
}

static void set_battery_text(lv_obj_t *label, uint8_t level) {
    char text[4];
    if (level > 99) {
        lv_label_set_text(label, "99");
        return;
    }
    snprintf(text, sizeof(text), "%02u", level);
    lv_label_set_text(label, text);
}

static void apply_layout(bool yads) {
    if (yads) {
        lv_obj_align(left_battery, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_align(right_battery, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_align(connection, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_align(details, LV_ALIGN_CENTER, 0, 2);
        lv_obj_align(left_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_align(right_bar, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    } else {
        lv_obj_align(connection, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_align(left_battery, LV_ALIGN_TOP_RIGHT, -31, 0);
        lv_obj_align(right_battery, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_align(details, LV_ALIGN_CENTER, 0, 4);
        lv_obj_align(left_bar, LV_ALIGN_BOTTOM_LEFT, 8, 0);
        lv_obj_align(right_bar, LV_ALIGN_BOTTOM_RIGHT, -8, 0);
    }
}

static void update_screen(struct k_work *work) {
    ARG_UNUSED(work);
    if (!ready) {
        return;
    }

    struct zmk_monitor_status status;
    zmk_monitor_status_snapshot(&status);
    bool alive = status.present && (k_uptime_get_32() - status.last_seen_ms) < 65000U;
    char text[24];

    if (alive) {
        set_battery_text(left_battery, status.left_battery);
        set_battery_text(right_battery, status.right_battery);
        snprintf(text, sizeof(text), "%c%c", status.usb_ready ? 'U' : '-',
                 status.ble_connected ? 'B' : '-');
        lv_label_set_text(connection, text);
        snprintf(text, sizeof(text), "L%u  W%03u  M%02X", status.layer,
                 status.wpm, status.modifiers);
        lv_label_set_text(details, text);
        set_bar(left_fill, status.left_battery);
        set_bar(right_fill, status.right_battery);
    } else {
        lv_label_set_text(left_battery, "--");
        lv_label_set_text(right_battery, "--");
        lv_label_set_text(connection, "MON");
        lv_label_set_text(details, "WAITING FOR KEYBOARD");
        set_bar(left_fill, 0);
        set_bar(right_fill, 0);
    }

    apply_layout(zmk_display_settings_theme() == 1);
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

lv_obj_t *zmk_display_status_screen(void) {
    screen = lv_obj_create(NULL);
    clean_obj(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, &lv_font_unscii_8, 0);

    left_battery = lv_label_create(screen);
    right_battery = lv_label_create(screen);
    connection = lv_label_create(screen);
    details = lv_label_create(screen);
    clean_obj(left_battery);
    clean_obj(right_battery);
    clean_obj(connection);
    clean_obj(details);
    lv_obj_set_style_text_align(details, LV_TEXT_ALIGN_CENTER, 0);

    configure_bar(&left_bar, &left_fill);
    configure_bar(&right_bar, &right_fill);

    ready = true;
    update_screen(NULL);
    return screen;
}
