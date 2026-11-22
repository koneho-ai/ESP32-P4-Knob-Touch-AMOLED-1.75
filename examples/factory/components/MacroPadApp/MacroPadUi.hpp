/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Macropad UI: a 5x3 grid of touch keys on the 466x466 round display,
 * styled like the user's macropad (the key faces are photos of their
 * keycaps), a USB connection indicator, a footer status line and a
 * "Reload config" button. Runs on the LVGL task only.
 */

#pragma once

#include "lvgl.h"
#include "MacroPadConfig.hpp"

namespace esp_brookesia::apps {

class MacroPadUi {
public:
    using KeyCallback = void (*)(int index, void *ctx);
    using ReloadCallback = void (*)(void *ctx);

    // Builds the UI under parent. Keys report LV_EVENT_PRESSED (touch
    // down sends immediately, like a real key).
    bool create(lv_obj_t *parent, KeyCallback key_cb, ReloadCallback reload_cb, void *ctx);
    void destroy();

    // Applies a config to the grid (colors, icons, labels, dimming of
    // unassigned keys). Cheap enough to call on every reload.
    void setConfig(const MacroPadConfig &config);

    // USB host indicator: green + "USB connected" vs amber + "No USB host".
    void setUsbConnected(bool connected);

    // One-line status under the grid (last action sent, config state).
    void setFooter(const char *text);

    // Grid geometry, for the README and tests.
    static constexpr int kKeyW = 72;
    static constexpr int kKeyH = 80;
    static constexpr int kKeyGap = 8;
    static constexpr int kGridX = 37;   // (466 - (5*72 + 4*8)) / 2
    static constexpr int kGridY = 96;

private:
    struct KeyView {
        lv_obj_t *btn = nullptr;
        lv_obj_t *img = nullptr;
        lv_obj_t *label = nullptr;
        bool assigned = false; // drives the pressed/released image opacity
    };

    static void keyEventCallback(lv_event_t *e);
    static void keyReleaseCallback(lv_event_t *e);
    static void reloadEventCallback(lv_event_t *e);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_usb_dot = nullptr;
    lv_obj_t *_usb_label = nullptr;
    lv_obj_t *_footer = nullptr;
    KeyView _keys[MacroPadConfig::kKeyCount] = {};

    KeyCallback _key_cb = nullptr;
    ReloadCallback _reload_cb = nullptr;
    void *_ctx = nullptr;
};

} // namespace esp_brookesia::apps
