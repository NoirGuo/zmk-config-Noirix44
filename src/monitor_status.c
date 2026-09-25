/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/monitor_status.h>
#include <zmk/status_advertisement.h>

LOG_MODULE_REGISTER(dongle_monitor, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_monitor_status current;
static struct k_spinlock current_lock;
static struct k_work_delayable scan_start_work;
static struct k_work_delayable timeout_work;
static struct k_work notify_work;

__attribute__((weak)) void zmk_monitor_status_changed(void) {}

void zmk_monitor_status_snapshot(struct zmk_monitor_status *status) {
    k_spinlock_key_t key = k_spin_lock(&current_lock);
    *status = current;
    k_spin_unlock(&current_lock, key);
}

static void notify_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    zmk_monitor_status_changed();
}

static void timeout_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    k_work_submit(&notify_work);
}

static bool parse_field(struct bt_data *data, void *user_data) {
    const struct zmk_status_adv_data **found = user_data;

    if (data->type != BT_DATA_MANUFACTURER_DATA ||
        data->data_len < sizeof(struct zmk_status_adv_data)) {
        return true;
    }

    const struct zmk_status_adv_data *candidate =
        (const struct zmk_status_adv_data *)data->data;
    if (candidate->manufacturer_id[0] != 0xff || candidate->manufacturer_id[1] != 0xff ||
        candidate->service_uuid[0] != 0xab || candidate->service_uuid[1] != 0xcd) {
        return true;
    }

    if (CONFIG_ZMK_DONGLE_MONITOR_CHANNEL != 0 && candidate->channel != 0 &&
        candidate->channel != CONFIG_ZMK_DONGLE_MONITOR_CHANNEL) {
        return false;
    }

    *found = candidate;
    return false;
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf) {
    const struct zmk_status_adv_data *data = NULL;
    struct net_buf_simple copy = *buf;
    bt_data_parse(&copy, parse_field, &data);
    if (data == NULL) {
        return;
    }

    uint32_t now = k_uptime_get_32();
    struct zmk_monitor_status next = {
        .present = true,
        .left_battery = data->battery_level,
        .right_battery = data->peripheral_battery[0],
        .layer = data->active_layer,
        .modifiers = data->modifier_flags,
        .wpm = data->wpm_value,
        .profile = PROSPECTOR_DECODE_PROFILE(data->profile_slot),
        .usb_ready = (data->status_flags & ZMK_STATUS_FLAG_USB_HID_READY) != 0,
        .ble_connected = (data->status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED) != 0,
        .rssi = info->rssi,
        .last_seen_ms = 0,
    };

    /* Copy layer name (4 bytes in the broadcast, not NUL-terminated). */
    memcpy(next.layer_name, data->layer_name, sizeof(data->layer_name));
    next.layer_name[sizeof(next.layer_name) - 1] = '\0';

    /* Copy typed keys (5 bytes raw in the broadcast; may or may not be
     * NUL-terminated. Always terminate locally. */
    memcpy(next.typed_keys, data->typed_keys, sizeof(data->typed_keys));
    next.typed_keys[sizeof(next.typed_keys) - 1] = '\0';

    k_spinlock_key_t key = k_spin_lock(&current_lock);
    struct zmk_monitor_status previous = current;
    previous.last_seen_ms = 0;
    bool changed = memcmp(&previous, &next, sizeof(next)) != 0;
    next.last_seen_ms = now;
    current = next;
    k_spin_unlock(&current_lock, key);

    k_work_reschedule(&timeout_work, K_SECONDS(65));

    if (changed) {
        k_work_submit(&notify_work);
    }
}

static struct bt_le_scan_cb scan_callbacks = {
    .recv = scan_recv,
};

static void scan_start_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    const struct bt_le_scan_param params = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };

    int err = bt_le_scan_start(&params, NULL);
    if (err == -EAGAIN || err == -EBUSY) {
        k_work_reschedule(&scan_start_work, K_MSEC(500));
    } else if (err != 0 && err != -EALREADY) {
        LOG_ERR("status scan start failed: %d", err);
        k_work_reschedule(&scan_start_work, K_SECONDS(2));
    } else {
        LOG_INF("status monitor active on channel %d", CONFIG_ZMK_DONGLE_MONITOR_CHANNEL);
    }
}

static int monitor_init(void) {
    k_work_init(&notify_work, notify_work_cb);
    k_work_init_delayable(&scan_start_work, scan_start_work_cb);
    k_work_init_delayable(&timeout_work, timeout_work_cb);
    bt_le_scan_cb_register(&scan_callbacks);

    /* Monitor builds deliberately disable ZMK HID-over-BLE, so they own the
     * observer stack initialization instead of relying on zmk_ble_init(). */
    int err = bt_enable(NULL);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return err;
    }

    k_work_schedule(&scan_start_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(monitor_init, APPLICATION, 99);
