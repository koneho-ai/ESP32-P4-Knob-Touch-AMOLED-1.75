/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "storage_service.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"
#include "MacroPadConfig.hpp"
#include "MacroPadUi.hpp"

namespace esp_brookesia::apps {

/**
 * MacroPad: a touchscreen macropad for the P4 knob board, modeled on
 * MuseApp's lifecycle. 15 touch keys in a 5x3 grid, styled like the
 * user's macropad photo (see MacroPadUi), send real keystrokes to a
 * computer over USB HID (keyboard + consumer control); the physical
 * knob sends volume up/down. Key mappings live in a JSON config file
 * on the TF card (/macropad/config.json) and reload without a reflash.
 *
 * Lifecycle: HID starts when the app runs and stops when it closes
 * (pausing for another app keeps HID attached, so the host does not
 * re-enumerate on every app switch). The config loads on run/resume
 * and whenever the card remounts after USB-drive use, plus on demand
 * from the Reload button. A sender task owns all HID traffic: key
 * taps and knob steps queue as jobs, and macros execute there with
 * their delays, so the LVGL task never blocks on USB.
 *
 * The knob has no press switch on this board (only CW/CCW GPIOs), so
 * there is no knob-press action. Mic mute has no default: macOS
 * answers no HID usage for it (see MacroPadHid.hpp).
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */
class MacroPadApp : public systems::phone::App {
public:
    static MacroPadApp *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~MacroPadApp();

protected:
    MacroPadApp(bool use_status_bar, bool use_navigation_bar);

    bool run() override;
    bool back() override;
    bool close() override;
    bool init() override;
    bool deinit() override;
    bool pause() override;
    bool resume() override;

private:
    // A key tap or a knob turn, resolved to its action by the sender
    // task at execution time (a config reload between tap and send
    // just sends the new mapping).
    struct Job {
        bool is_knob = false;
        int knob_steps = 0;  // +CW, -CCW (only when is_knob)
        int key_index = -1;  // only when !is_knob
    };

    static constexpr int kSenderStackSize = 4096;
    static constexpr int kJobQueueLength = 32;
    static constexpr int kMaxKnobStepsPerCallback = 16;

    static MacroPadApp *_instance;

    lv_obj_t *_page_root = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    MacroPadUi *_ui = nullptr;

    SemaphoreHandle_t _state_mutex = nullptr;  // guards _config + footer pending
    QueueHandle_t _job_queue = nullptr;
    TaskHandle_t _sender_task = nullptr;

    MacroPadConfig _config;
    char _footer_pending[128] = {};
    bool _footer_dirty = false;
    bool _usb_connected = false;
    bool _hid_started = false;
    bool _storage_listener_added = false;

    std::atomic<bool> _visible{false};
    std::atomic<bool> _config_dirty{false};
    std::atomic<bool> _shutdown{false};
    std::atomic<bool> _sender_running{false};

    void createUi();
    void destroyUi();
    void refreshUi();
    // Defaults + config file, applied to the grid with a footer line
    // saying where the mapping came from. LVGL task only.
    void loadConfigAndApply();
    void setFooterSoon(const char *text);

    void onKeyPressed(int index);
    void onReloadPressed();

    // ---- HID sender task ----
    void senderLoop();
    bool waitHostReady(uint32_t timeout_ms);
    bool sendKeyboard(uint8_t modifier, const uint8_t *keys, uint8_t key_count);
    bool sendKeyboardRelease();
    bool sendConsumer(uint16_t usage);
    bool sendConsumerRelease();
    void executeAction(const MacroPadAction &action);
    void executeCombo(uint8_t modifiers, const uint8_t *keys, uint8_t key_count);
    void executeConsumer(uint16_t usage);
    void executeText(const char *text);
    void executeMacro(const MacroPadAction &action);

    static void senderTaskEntry(void *arg);
    static void uiTimerCallback(lv_timer_t *timer);
    static void uiKeyCallback(int index, void *ctx);
    static void uiReloadCallback(void *ctx);
    static void knobCallback(int steps, void *user_ctx);
    static void storageCallback(storage_event_t event, void *user_ctx);
};

} // namespace esp_brookesia::apps
