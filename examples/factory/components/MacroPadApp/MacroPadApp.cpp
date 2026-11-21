/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MacroPadApp.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "haptic_service.h"
#include "knob_service.h"
#include "usb_service.h"
#include "MacroPadHid.hpp"
#include "MacroPadIcons.hpp"

namespace esp_brookesia::apps {

namespace {

constexpr const char *TAG = "macropad";

// HID send timing. The host polls interrupt endpoints every 8 ms, so a
// 30 ms hold is comfortably one poll, and a 5 ms retry step drains a
// busy endpoint quickly.
constexpr uint32_t kHoldMs = 30;
constexpr uint32_t kTypeHoldMs = 15;
constexpr uint32_t kTypeGapMs = 10;
constexpr uint32_t kMacroStepGapMs = 30;
constexpr uint32_t kReportRetryMs = 5;
constexpr int kReportRetries = 20;      // 100 ms per report
constexpr uint32_t kHostWaitMs = 500;   // per job, for a just-plugged host

void copyString(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

} // namespace

MacroPadApp *MacroPadApp::_instance = nullptr;

MacroPadApp *MacroPadApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new MacroPadApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

MacroPadApp::MacroPadApp(bool use_status_bar, bool use_navigation_bar):
    App("MacroPad", &img_macropad_app, true, use_status_bar, use_navigation_bar)
{
    macropad_config_defaults(_config);
}

MacroPadApp::~MacroPadApp()
{
    _instance = nullptr;
}

bool MacroPadApp::init()
{
    if (!_state_mutex) {
        _state_mutex = xSemaphoreCreateMutex();
        if (!_state_mutex) {
            ESP_LOGE(TAG, "Create state mutex failed");
            return false;
        }
    }
    if (!_job_queue) {
        _job_queue = xQueueCreate(kJobQueueLength, sizeof(Job));
        if (!_job_queue) {
            ESP_LOGE(TAG, "Create job queue failed");
            return false;
        }
    }
    if (!_ui) {
        _ui = new (std::nothrow) MacroPadUi();
        if (!_ui) {
            ESP_LOGE(TAG, "Create UI failed");
            return false;
        }
    }
    if (!_sender_running.load() && !_sender_task) {
        _shutdown.store(false);
        if (xTaskCreate(senderTaskEntry, "macropad_hid", kSenderStackSize, this,
                        tskIDLE_PRIORITY + 2, &_sender_task) != pdPASS) {
            ESP_LOGE(TAG, "Create sender task failed");
            _sender_task = nullptr;
            return false;
        }
    }
    if (!_storage_listener_added) {
        if (storage_add_listener(storageCallback, this) == ESP_OK) {
            _storage_listener_added = true;
        } else {
            ESP_LOGW(TAG, "Storage listener not added; config reloads only via the button");
        }
    }
    return true;
}

bool MacroPadApp::deinit()
{
    _shutdown.store(true);
    // Wake the sender task so it sees the flag (it blocks on the queue).
    if (_job_queue) {
        Job wake = {};
        wake.key_index = -2;  // no-op job, just a wakeup
        xQueueSend(_job_queue, &wake, 0);
    }
    for (int i = 0; i < 100 && _sender_running.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (_sender_running.load() && _sender_task) {
        vTaskDelete(_sender_task);
        _sender_task = nullptr;
    }
    _sender_task = nullptr;
    if (_storage_listener_added) {
        storage_remove_listener(storageCallback, this);
        _storage_listener_added = false;
    }
    if (_job_queue) {
        vQueueDelete(_job_queue);
        _job_queue = nullptr;
    }
    if (_state_mutex) {
        vSemaphoreDelete(_state_mutex);
        _state_mutex = nullptr;
    }
    delete _ui;
    _ui = nullptr;
    return true;
}

bool MacroPadApp::run()
{
    createUi();
    loadConfigAndApply();
    _visible.store(true);
    knob_service_attach(knobCallback, this);
    if (usb_service_start_hid() != ESP_OK) {
        ESP_LOGW(TAG, "USB HID did not start; keys will do nothing until it does");
        _hid_started = false;
    } else {
        _hid_started = true;
    }
    return true;
}

bool MacroPadApp::back()
{
    _visible.store(false);
    knob_service_detach(knobCallback, this);
    return notifyCoreClosed();
}

bool MacroPadApp::close()
{
    _visible.store(false);
    knob_service_detach(knobCallback, this);
    if (_hid_started) {
        (void)usb_service_stop_hid();
        _hid_started = false;
    }
    destroyUi();
    return true;
}

bool MacroPadApp::pause()
{
    _visible.store(false);
    knob_service_detach(knobCallback, this);
    if (_ui_timer) {
        lv_timer_pause(_ui_timer);
    }
    // HID stays attached while paused: the host must not re-enumerate
    // on every app switch, and a paused macropad sends nothing anyway.
    return true;
}

bool MacroPadApp::resume()
{
    if (!_page_root) {
        createUi();
    }
    loadConfigAndApply();
    _visible.store(true);
    knob_service_attach(knobCallback, this);
    if (_ui_timer) {
        lv_timer_resume(_ui_timer);
    }
    return true;
}

void MacroPadApp::createUi()
{
    destroyUi();
    if (!_ui) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    _page_root = lv_obj_create(screen);
    lv_obj_remove_style_all(_page_root);
    lv_obj_set_size(_page_root, lv_pct(100), lv_pct(100));
    lv_obj_center(_page_root);
    lv_obj_set_style_bg_color(_page_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_page_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_page_root, LV_OBJ_FLAG_SCROLLABLE);

    if (!_ui->create(_page_root, uiKeyCallback, uiReloadCallback, this)) {
        destroyUi();
        return;
    }
    _ui_timer = lv_timer_create(uiTimerCallback, 250, this);
    refreshUi();
}

void MacroPadApp::destroyUi()
{
    if (_ui_timer) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }
    if (_ui) {
        _ui->destroy();
    }
    _page_root = nullptr;
}

void MacroPadApp::refreshUi()
{
    if (!_ui || !_page_root) {
        return;
    }
    bool connected = usb_hid_is_ready();
    if (connected != _usb_connected) {
        _usb_connected = connected;
        _ui->setUsbConnected(connected);
    }
    if (_config_dirty.exchange(false)) {
        loadConfigAndApply();
        return;
    }
    if (_state_mutex && xSemaphoreTake(_state_mutex, 0) == pdTRUE) {
        if (_footer_dirty) {
            _footer_dirty = false;
            _ui->setFooter(_footer_pending);
        }
        xSemaphoreGive(_state_mutex);
    }
}

void MacroPadApp::loadConfigAndApply()
{
    if (!_ui || !_page_root) {
        return;
    }
    MacroPadConfig fresh;
    macropad_config_defaults(fresh);
    bool from_file = macropad_config_load(fresh);
    if (_state_mutex) {
        xSemaphoreTake(_state_mutex, portMAX_DELAY);
    }
    _config = fresh;
    if (_state_mutex) {
        xSemaphoreGive(_state_mutex);
    }
    _ui->setConfig(_config);
    char footer[128];
    if (from_file) {
        snprintf(footer, sizeof(footer), "Config loaded from the card");
    } else if (strcmp(_config.error, "no file") == 0) {
        snprintf(footer, sizeof(footer), "Defaults — add /macropad/config.json to remap");
    } else if (strcmp(_config.error, "no card") == 0) {
        snprintf(footer, sizeof(footer), "Defaults — no card");
    } else {
        snprintf(footer, sizeof(footer), "Config error — defaults (%s)", _config.error);
    }
    _ui->setFooter(footer);
}

void MacroPadApp::setFooterSoon(const char *text)
{
    if (!_state_mutex) {
        return;
    }
    if (xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    copyString(_footer_pending, sizeof(_footer_pending), text);
    _footer_dirty = true;
    xSemaphoreGive(_state_mutex);
}

void MacroPadApp::onKeyPressed(int index)
{
    static const uint16_t kTapPattern[] = {15};
    (void)haptic_play(kTapPattern, 1);
    MacroPadAction action;
    {
        if (!_state_mutex) {
            return;
        }
        xSemaphoreTake(_state_mutex, portMAX_DELAY);
        action = _config.keys[index].action;
        xSemaphoreGive(_state_mutex);
    }
    if (action.type == MacroPadAction::Type::None) {
        _ui->setFooter("Unassigned — map it in config.json");
        return;
    }
    Job job;
    job.is_knob = false;
    job.key_index = index;
    if (_job_queue && xQueueSend(_job_queue, &job, 0) != pdPASS) {
        ESP_LOGW(TAG, "Job queue full; key %d dropped", index);
    }
}

void MacroPadApp::onReloadPressed()
{
    loadConfigAndApply();
}

// ---- HID sender task ----

void MacroPadApp::senderTaskEntry(void *arg)
{
    static_cast<MacroPadApp *>(arg)->senderLoop();
}

void MacroPadApp::senderLoop()
{
    _sender_running.store(true);
    while (!_shutdown.load()) {
        Job job;
        if (!_job_queue || xQueueReceive(_job_queue, &job, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }
        if (_shutdown.load()) {
            break;
        }
        if (job.key_index == -2 && !job.is_knob) {
            continue;  // wakeup job from deinit
        }
        if (!_hid_started || !_visible.load()) {
            continue;  // app closed or hidden since the tap; drop quietly
        }
        if (job.is_knob) {
            MacroPadAction action;
            if (!_state_mutex) {
                continue;
            }
            xSemaphoreTake(_state_mutex, portMAX_DELAY);
            action = job.knob_steps > 0 ? _config.knob_cw : _config.knob_ccw;
            xSemaphoreGive(_state_mutex);
            int steps = job.knob_steps > 0 ? job.knob_steps : -job.knob_steps;
            for (int i = 0; i < steps && !_shutdown.load(); i++) {
                executeAction(action);
            }
            continue;
        }
        MacroPadAction action;
        if (!_state_mutex) {
            continue;
        }
        xSemaphoreTake(_state_mutex, portMAX_DELAY);
        action = _config.keys[job.key_index].action;
        xSemaphoreGive(_state_mutex);
        if (action.type == MacroPadAction::Type::None) {
            continue;  // unmapped since the tap (config reloaded)
        }
        executeAction(action);
        char desc[64];
        macropad_action_desc(action, desc, sizeof(desc));
        char footer[96];
        snprintf(footer, sizeof(footer), "Sent: %s", desc);
        setFooterSoon(footer);
    }
    _sender_running.store(false);
    vTaskDelete(nullptr);
}

bool MacroPadApp::waitHostReady(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (usb_hid_is_ready()) {
            return true;
        }
        if (_shutdown.load()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return usb_hid_is_ready();
}

bool MacroPadApp::sendKeyboard(uint8_t modifier, const uint8_t *keys, uint8_t key_count)
{
    uint8_t report_keys[6] = {};
    if (keys && key_count > 0) {
        memcpy(report_keys, keys, key_count > 6 ? 6 : key_count);
    }
    for (int i = 0; i < kReportRetries && !_shutdown.load(); i++) {
        if (usb_hid_keyboard_report(modifier, report_keys)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kReportRetryMs));
    }
    return false;
}

bool MacroPadApp::sendKeyboardRelease()
{
    return sendKeyboard(0, nullptr, 0);
}

bool MacroPadApp::sendConsumer(uint16_t usage)
{
    for (int i = 0; i < kReportRetries && !_shutdown.load(); i++) {
        if (usb_hid_consumer_report(usage)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kReportRetryMs));
    }
    return false;
}

bool MacroPadApp::sendConsumerRelease()
{
    return sendConsumer(0);
}

void MacroPadApp::executeAction(const MacroPadAction &action)
{
    switch (action.type) {
    case MacroPadAction::Type::None:
        return;
    case MacroPadAction::Type::Combo:
        executeCombo(action.modifiers, action.keys, action.key_count);
        return;
    case MacroPadAction::Type::Consumer:
        executeConsumer(action.consumer_usage);
        return;
    case MacroPadAction::Type::Text:
        executeText(action.text);
        return;
    case MacroPadAction::Type::Macro:
        executeMacro(action);
        return;
    }
}

void MacroPadApp::executeCombo(uint8_t modifiers, const uint8_t *keys, uint8_t key_count)
{
    if (!waitHostReady(kHostWaitMs)) {
        setFooterSoon("No USB host — plug in USB-C");
        return;
    }
    if (!sendKeyboard(modifiers, keys, key_count)) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(kHoldMs));
    (void)sendKeyboardRelease();
}

void MacroPadApp::executeConsumer(uint16_t usage)
{
    if (!waitHostReady(kHostWaitMs)) {
        setFooterSoon("No USB host — plug in USB-C");
        return;
    }
    if (!sendConsumer(usage)) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(kHoldMs));
    (void)sendConsumerRelease();
}

void MacroPadApp::executeText(const char *text)
{
    if (!waitHostReady(kHostWaitMs)) {
        setFooterSoon("No USB host — plug in USB-C");
        return;
    }
    for (const char *p = text; *p && !_shutdown.load(); p++) {
        uint8_t key = 0;
        bool shift = false;
        if (!macropad_hid_char(*p, key, shift)) {
            continue;  // no US-layout key for this character; skipped
        }
        if (!sendKeyboard(shift ? 0x02 : 0x00, &key, 1)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(kTypeHoldMs));
        if (!sendKeyboardRelease()) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(kTypeGapMs));
    }
}

void MacroPadApp::executeMacro(const MacroPadAction &action)
{
    for (int i = 0; i < action.step_count && !_shutdown.load(); i++) {
        const MacroPadAction::Step &step = action.steps[i];
        switch (step.kind) {
        case MacroPadAction::Step::Kind::Combo:
            executeCombo(step.modifiers, step.keys, step.key_count);
            break;
        case MacroPadAction::Step::Kind::Consumer:
            executeConsumer(step.consumer_usage);
            break;
        case MacroPadAction::Step::Kind::Text:
            executeText(step.text);
            break;
        case MacroPadAction::Step::Kind::Delay:
            vTaskDelay(pdMS_TO_TICKS(step.delay_ms));
            break;
        }
        if (step.kind != MacroPadAction::Step::Kind::Delay) {
            vTaskDelay(pdMS_TO_TICKS(kMacroStepGapMs));
        }
    }
}

void MacroPadApp::uiTimerCallback(lv_timer_t *timer)
{
    static_cast<MacroPadApp *>(lv_timer_get_user_data(timer))->refreshUi();
}

void MacroPadApp::uiKeyCallback(int index, void *ctx)
{
    static_cast<MacroPadApp *>(ctx)->onKeyPressed(index);
}

void MacroPadApp::uiReloadCallback(void *ctx)
{
    static_cast<MacroPadApp *>(ctx)->onReloadPressed();
}

void MacroPadApp::knobCallback(int steps, void *user_ctx)
{
    // Runs in the knob task with the LVGL lock held: never block, never
    // touch LVGL or the config — just queue the turn.
    MacroPadApp *app = static_cast<MacroPadApp *>(user_ctx);
    if (!app->_visible.load() || !app->_job_queue) {
        return;
    }
    if (steps > kMaxKnobStepsPerCallback) {
        steps = kMaxKnobStepsPerCallback;
    } else if (steps < -kMaxKnobStepsPerCallback) {
        steps = -kMaxKnobStepsPerCallback;
    }
    if (steps == 0) {
        return;
    }
    Job job;
    job.is_knob = true;
    job.knob_steps = steps;
    (void)xQueueSend(app->_job_queue, &job, 0);
}

void MacroPadApp::storageCallback(storage_event_t event, void *user_ctx)
{
    MacroPadApp *app = static_cast<MacroPadApp *>(user_ctx);
    if (event == STORAGE_EVENT_SD_MOUNTED) {
        // The card came back (e.g. after editing config.json over the
        // USB drive); the UI timer reloads on the LVGL task.
        app->_config_dirty.store(true);
    }
}

} // namespace esp_brookesia::apps
