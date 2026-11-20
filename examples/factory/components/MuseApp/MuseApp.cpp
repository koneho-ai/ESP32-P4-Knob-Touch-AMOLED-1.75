/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MuseApp.hpp"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "audio_service.h"
#include "bt_service.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_audio_types.h"
#include "esp_timer.h"
#include "identity.h"
#include "knob_service.h"
#include "muse_ble_pairing.h"
#include "muse_provisioning.h"
#include "wifi_service.h"
#include "Settings.hpp"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_REALTIME_VOICE
#include "muse_voice_call_core.h"   // muse_vc_gate_t, the half-duplex mic gate
#endif
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:MuseApp"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(img_app_muse);

namespace esp_brookesia::apps {

namespace {

constexpr uint32_t RETRY_INTERVAL_MS = 5000;
constexpr uint32_t ERROR_RETRY_MS = 30000;  // Re-test a failed session this often
constexpr uint32_t TURN_DRAIN_TIMEOUT_MS = 5000;
constexpr uint32_t CHAT_SAMPLE_RATE = 16000;
constexpr uint32_t DEVICE_AUDIO_SAMPLE_RATE = 24000;
constexpr uint32_t CONTROL_EVENT_POST_TIMEOUT_MS = 100;
constexpr uint32_t LIFECYCLE_WAIT_MS = 10000;
constexpr size_t CONTROL_QUEUE_LENGTH = 24;
// The audio service records the two ES7210 microphones as one stereo stream
constexpr size_t DEVICE_TDM_CHANNELS = 2;
constexpr size_t DEVICE_MIC1_SLOT = 0;
constexpr size_t DEVICE_MIC2_SLOT = 1;
constexpr size_t INPUT_READ_FRAMES = DEVICE_AUDIO_SAMPLE_RATE / 100;   // 10 ms
constexpr size_t REPLY_READ_FRAMES = CHAT_SAMPLE_RATE / 50;            // 20 ms
// A call pumps in 10 ms passes like the capture task (the SDK uses 20 ms;
// the call core accepts any chunk size and accumulates 40 ms wire frames).
constexpr size_t CALL_CHUNK_FRAMES = CHAT_SAMPLE_RATE / 100;           // 10 ms
// Half-duplex gate (SDK muse_voice.c): the mic stays shut while Muse is
// audibly talking plus this tail; the speaker lag is the SDK board value —
// it may need retuning for this audio-service path once hardware exists.
constexpr int64_t CALL_SPEAKER_LAG_US = 120000;
constexpr int64_t CALL_TAIL_US = 300000;
constexpr int64_t CALL_END_WAIT_US = 20LL * 1000000;   // Hung up: wait this long for ENDED
constexpr int64_t CALL_DRAIN_US = 2LL * 1000000;       // Drain queued reply audio after ENDED
constexpr uint32_t OFFLINE_JUMP_MS = 1500;     // How long "No network" shows before Settings opens
constexpr int VOLUME_STEP = 5;
constexpr float VOICE_FULL_SCALE = 9000.0f;    // A peak this loud fills the ring

void copyString(char *destination, size_t capacity, const char *source)
{
    if (destination && capacity > 0) {
        snprintf(destination, capacity, "%s", source ? source : "");
    }
}

// Mixes the two ES7210 microphones to mono (or picks one, per Kconfig) and
// returns the peak magnitude. Shared by the capture task and the call task.
int32_t mixMicToMono(const int16_t *device_stereo, int16_t *mono, size_t frames)
{
    int32_t peak = 0;
    for (size_t i = 0; i < frames; i++) {
        const int16_t mic1 =
            device_stereo[i * DEVICE_TDM_CHANNELS + DEVICE_MIC1_SLOT];
        const int16_t mic2 =
            device_stereo[i * DEVICE_TDM_CHANNELS + DEVICE_MIC2_SLOT];
#if defined(CONFIG_MUSE_APP_CAPTURE_MIC_1)
        const int16_t sample = mic1;
#elif defined(CONFIG_MUSE_APP_CAPTURE_MIC_2)
        const int16_t sample = mic2;
#else
        const int16_t sample =
            static_cast<int16_t>(((int32_t)mic1 + (int32_t)mic2) / 2);
#endif
        mono[i] = sample;
        const int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

BaseType_t createMuseTask(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_size,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core,
    bool stack_in_psram
)
{
    // The refresh task does HTTPS and NVS writes: its stack must stay in
    // internal RAM (a flash write asserts on a PSRAM stack).
    if (stack_in_psram) {
        return xTaskCreatePinnedToCoreWithCaps(
            task,
            name,
            stack_size,
            argument,
            priority,
            handle,
            core,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    }
    return xTaskCreatePinnedToCore(
        task, name, stack_size, argument, priority, handle, core
    );
}

class ScopedSemaphoreLock {
public:
    ScopedSemaphoreLock(SemaphoreHandle_t semaphore, TickType_t timeout):
        _semaphore(semaphore)
    {
        _locked =
            _semaphore && xSemaphoreTake(_semaphore, timeout) == pdTRUE;
    }

    ~ScopedSemaphoreLock()
    {
        if (_locked) {
            xSemaphoreGive(_semaphore);
        }
    }

    explicit operator bool() const
    {
        return _locked;
    }

private:
    SemaphoreHandle_t _semaphore = nullptr;
    bool _locked = false;
};

constexpr uint32_t LIFECYCLE_CLOSED_BIT = 1U << 31;
constexpr uint32_t LIFECYCLE_REF_MASK = LIFECYCLE_CLOSED_BIT - 1;

class ScopedLifecycleAdmission {
public:
    explicit ScopedLifecycleAdmission(std::atomic<uint32_t> &state):
        _state(state)
    {
        uint32_t current = _state.load();
        while ((current & LIFECYCLE_CLOSED_BIT) == 0 &&
                (current & LIFECYCLE_REF_MASK) != LIFECYCLE_REF_MASK) {
            if (_state.compare_exchange_weak(current, current + 1)) {
                _acquired = true;
                break;
            }
        }
    }

    ~ScopedLifecycleAdmission()
    {
        if (_acquired) {
            _state.fetch_sub(1);
        }
    }

    explicit operator bool() const
    {
        return _acquired;
    }

private:
    std::atomic<uint32_t> &_state;
    bool _acquired = false;
};

class ScopedOptionalCounter {
public:
    explicit ScopedOptionalCounter(std::atomic<uint32_t> *counter):
        _counter(counter)
    {
        if (_counter) {
            _counter->fetch_add(1);
        }
    }

    ~ScopedOptionalCounter()
    {
        if (_counter) {
            _counter->fetch_sub(1);
        }
    }

private:
    std::atomic<uint32_t> *_counter = nullptr;
};

} // namespace

MuseApp *MuseApp::_instance = nullptr;

MuseApp *MuseApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new MuseApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

MuseApp::MuseApp(bool use_status_bar, bool use_navigation_bar):
    App("Muse", &img_app_muse, true, use_status_bar, use_navigation_bar)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

MuseApp::~MuseApp()
{
    _destroying.store(true);
    while (!deinit()) {
        ESP_UTILS_LOGW("Waiting to deinitialize Muse safely");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    _lifecycle_refs.fetch_or(LIFECYCLE_CLOSED_BIT);
    while ((_lifecycle_refs.load() & LIFECYCLE_REF_MASK) != 0) {
        vTaskDelay(1);
    }
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
    _instance = nullptr;
}

bool MuseApp::run()
{
    _offline_since = 0;
    createUi();
    _visible.store(true);
    knob_service_attach(knobCallback, this);
    updateNetworkState();
    ControlEvent event = {};
    event.type = ControlEventType::AppVisible;
    postControlEvent(event);
    return true;
}

bool MuseApp::back()
{
    _visible.store(false);
    knob_service_detach(knobCallback, this);
    postAppHidden();
    return notifyCoreClosed();
}

bool MuseApp::close()
{
    _visible.store(false);
    knob_service_detach(knobCallback, this);
    postAppHidden();
    destroyUi();
    return true;
}

/*
 * The control task may be inside a blocking network request for several
 * seconds. Waiting for it on the UI task froze the screen and failed the
 * navigation, so the hidden state is only queued: the control task stops
 * preparing as soon as it sees _visible cleared, then releases the audio.
 */
void MuseApp::postAppHidden()
{
    ControlEvent event = {};
    event.type = ControlEventType::AppHidden;
    event.flag = true;
    (void)postControlEvent(event);
}

bool MuseApp::init()
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    bool deinit_was_complete = _deinit_complete.load();
    if (_destroying.load() || _deinit_in_progress.load()) {
        return false;
    }
    ScopedSemaphoreLock lifecycle_lock(
        _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
    );
    if (!lifecycle_lock) {
        return false;
    }
    if (_destroying.load() || _deinit_in_progress.load() ||
            (!deinit_was_complete && _deinit_complete.load())) {
        return false;
    }
    if (_control_running.load()) {
        return true;
    }

    _deinit_complete.store(false);
    _shutdown.store(false);
    _state_mutex = _state_mutex ? _state_mutex : xSemaphoreCreateMutex();
    if (!_state_mutex) {
        setError("Create Muse state mutex");
        return false;
    }
    _control_exited = _control_exited ? _control_exited : xSemaphoreCreateBinary();
    _control_queue = _control_queue ? _control_queue :
                     xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(ControlEvent));
    if (!_lifecycle_mutex || !_control_exited || !_control_queue) {
        setError("Create Muse synchronization objects");
        return false;
    }

    _ui = _ui ? _ui : new (std::nothrow) MuseUi();
    if (!_ui) {
        setError("Create Muse UI");
        return false;
    }
    if (!_client) {
        MuseClient::Callbacks callbacks;
        callbacks.onHeard = [this](const std::string &text) {
            ControlEvent event = {};
            event.type = ControlEventType::TurnHeard;
            copyString(event.text, sizeof(event.text), text.c_str());
            postControlEvent(event);
        };
        callbacks.onReply = [this](const std::string &text) {
            ControlEvent event = {};
            event.type = ControlEventType::TurnReply;
            copyString(event.text, sizeof(event.text), text.c_str());
            postControlEvent(event);
        };
        callbacks.onDone = [this](const std::string &reason) {
            ControlEvent event = {};
            event.type = ControlEventType::TurnDone;
            copyString(event.text, sizeof(event.text), reason.c_str());
            postControlEvent(event);
        };
        callbacks.onError = [this](const std::string &error) {
            ControlEvent event = {};
            event.type = ControlEventType::TurnError;
            copyString(event.text, sizeof(event.text), error.c_str());
            postControlEvent(event);
        };
        callbacks.onSent = [this]() {
            ControlEvent event = {};
            event.type = ControlEventType::TurnSent;
            postControlEvent(event);
        };
        _client = new (std::nothrow) MuseClient(std::move(callbacks));
        if (!_client) {
            setError("Create Muse client");
            return false;
        }
    }
    (void)_ui->preload();

    if (!_audio_client) {
        const audio_client_config_t audio_config = {
            .name = "Muse",
            .on_preempt = audioPreemptCallback,
            .user_ctx = this,
        };
        if (audio_client_register(&audio_config, &_audio_client) != ESP_OK) {
            setError("Register Muse audio client");
            return false;
        }
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        setError("Initialize network interfaces");
        return false;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        setError("Create default event loop");
        return false;
    }
    if (!_ip_event_instance) {
        ret = esp_event_handler_instance_register(
                  IP_EVENT, ESP_EVENT_ANY_ID, ipEventHandler, this, &_ip_event_instance
              );
        if (ret != ESP_OK) {
            setError("Register network event handler");
            return false;
        }
    }

    _control_running.store(true);
    if (createMuseTask(
            controlTaskEntry,
            "muse_control",
            CONTROL_TASK_STACK_SIZE,
            this,
            5,
            &_control_task,
            0,
            false
         ) != pdPASS) {
        _control_running.store(false);
        _control_task = nullptr;
        setError("Create Muse control task");
        return false;
    }
    updateNetworkState();
    return true;
}

bool MuseApp::deinit()
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    if (_deinit_complete.load()) {
        return true;
    }
    bool expected = false;
    if (!_deinit_in_progress.compare_exchange_strong(expected, true)) {
        TickType_t started = xTaskGetTickCount();
        while (_deinit_in_progress.load() &&
                xTaskGetTickCount() - started <
                    pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return _deinit_complete.load();
    }
    auto finish_deinit = [this](bool complete) {
        _deinit_complete.store(complete);
        _deinit_in_progress.store(false);
        return complete;
    };

    if (!_lifecycle_mutex && !_control_running.load()) {
        _shutdown.store(true);
        return finish_deinit(true);
    }

    ScopedSemaphoreLock lifecycle_lock(
        _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
    );
    if (!lifecycle_lock) {
        ESP_UTILS_LOGE("Timed out waiting for Muse lifecycle lock");
        return finish_deinit(false);
    }

    _visible.store(false);
    bool was_shutdown = _shutdown.exchange(true);
    bool control_stopped = true;
    if (_control_running.load()) {
        control_stopped = _control_queue && _control_exited;
        if (control_stopped && !was_shutdown) {
            ControlEvent event = {};
            event.type = ControlEventType::Shutdown;
            control_stopped =
                xQueueSendToFront(
                    _control_queue,
                    &event,
                    pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
                ) == pdTRUE;
            if (!control_stopped) {
                _shutdown.store(false);
            }
        }
        if (control_stopped) {
            control_stopped =
                xSemaphoreTake(
                    _control_exited, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
                ) == pdTRUE;
        }
    }
    if (!control_stopped) {
        ESP_UTILS_LOGE("Muse control task did not stop cleanly");
        return finish_deinit(false);
    }
    if (_ip_event_instance) {
        esp_event_handler_instance_unregister(
            IP_EVENT, ESP_EVENT_ANY_ID, _ip_event_instance
        );
        _ip_event_instance = nullptr;
    }

    TickType_t post_wait_started = xTaskGetTickCount();
    while (_async_post_users.load() != 0 &&
            xTaskGetTickCount() - post_wait_started <
                pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (_async_post_users.load() != 0) {
        ESP_UTILS_LOGE("Muse callbacks still have active producers");
        return finish_deinit(false);
    }

    // Let the task finish returning from the exit semaphore give before the
    // semaphore and queues are deleted.
    vTaskDelay(1);

    destroyUi();
    delete _ui;
    _ui = nullptr;
    delete _client;
    _client = nullptr;

    if (_control_queue) {
        vQueueDelete(_control_queue);
        _control_queue = nullptr;
    }
    if (_control_exited) {
        vSemaphoreDelete(_control_exited);
        _control_exited = nullptr;
    }
    if (_state_mutex) {
        vSemaphoreDelete(_state_mutex);
        _state_mutex = nullptr;
    }
    if (_ble_inited.load()) {
        (void)muse_ble_pairing_deinit();
        _ble_inited.store(false);
    }
    return finish_deinit(true);
}

bool MuseApp::pause()
{
    _visible.store(false);
    knob_service_detach(knobCallback, this);
    postAppHidden();
    if (_ui_timer) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool MuseApp::resume()
{
    if (!_page_root) {
        createUi();
    }
    _visible.store(true);
    _offline_since = 0;
    knob_service_attach(knobCallback, this);
    if (_ui_timer) {
        lv_timer_resume(_ui_timer);
    }
    updateNetworkState();
    ControlEvent event = {};
    event.type = ControlEventType::AppVisible;
    postControlEvent(event);
    return true;
}

void MuseApp::createUi()
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

    if (!_ui->create(_page_root, uiActionCallback, this, uiHoldCallback, this)) {
        destroyUi();
        return;
    }
    // Reply pages wrap to the caption box's pixel width, measured in the
    // caption font, so no line wraps again on screen (SDK muse_ui.c: the
    // full UI sets this when it picks its reply layout).
    muse_chat_set_caption_px(MuseUi::kCaptionWidthPx, &MuseUi::captionGlyphPx);
    _ui_timer = lv_timer_create(uiTimerCallback, 100, this);
    refreshUi();
}

void MuseApp::destroyUi()
{
    if (_ui_timer) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }
    if (_ui) {
        _ui->destroy();
    }
    if (_page_root) {
        lv_obj_delete(_page_root);
        _page_root = nullptr;
    }
}

void MuseApp::refreshUi()
{
    if (!_ui || !_page_root) {
        return;
    }
    char role[sizeof(_chat_role)] = {};
    char text[sizeof(_chat_text)] = {};
    char toast[sizeof(_toast)] = {};
    if (!_state_mutex ||
            xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    memcpy(role, _chat_role, sizeof(role));
    memcpy(text, _chat_text, sizeof(text));
    if (_toast_pending.exchange(false)) {
        memcpy(toast, _toast, sizeof(toast));
    }
    xSemaphoreGive(_state_mutex);

    State state = _state.load();
    if (_network_ready.load()) {
        _wlan_offered = false;
    }
    _ui->setNetworkReady(_network_ready.load());
    if (_confirm_pending.load()) {
        _ui->setStatus("CONFIRM PAIRING?");
        _ui->setHint("Tap to confirm");
    } else {
        _ui->setStatus(stateText(state));
        _ui->setHint(hintText(state));
    }
    _ui->setMode(uiMode(state));
    _ui->setWorking(_call_working.load());
    if (state == State::Listening) {
        _ui->setLevel(_input_pcm_peak.load() / VOICE_FULL_SCALE);
    } else if (state == State::Speaking) {
        _ui->setLevel(_output_pcm_peak.exchange(0) / VOICE_FULL_SCALE);
    }
    // The ring's progress: while listening it fills over the 15 s a voice
    // note runs to (the SDK's note cap); while speaking it follows the
    // session's playback progress, sampled by the control task. In a call
    // there is no phase cap to fill towards (the idle limit is about
    // quiet, not elapsed), so the ring stays empty.
    float progress = 0;
    if (!_in_call.load()) {
        if (state == State::Listening) {
            const uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - _listen_started_tick);
            progress = elapsed_ms / 15000.0f;
            if (progress > 1) {
                progress = 1;
            }
        } else if (state == State::Speaking) {
            progress = _progress.load();
        }
    }
    _ui->setProgress(progress);
    _ui->setChatMessage(role, text);
    if (toast[0] != '\0') {
        _ui->showToast(toast);
    }
}

MuseUi::Mode MuseApp::uiMode(State state)
{
    switch (state) {
    case State::NotProvisioned:
        return MuseUi::Mode::NotProvisioned;
    case State::NetworkRequired:
        return MuseUi::Mode::Offline;
    case State::Preparing:
    case State::Connecting:
    case State::CallConnecting:
        return MuseUi::Mode::Busy;
    case State::Ready:
        return MuseUi::Mode::Ready;
    case State::Listening:
        return MuseUi::Mode::Listening;
    case State::Thinking:
        return MuseUi::Mode::Thinking;
    case State::Speaking:
        return MuseUi::Mode::Speaking;
    case State::Error:
    default:
        return MuseUi::Mode::Error;
    }
}

/* What a tap does now, below the avatar; offline it says what happens to the network */
const char *MuseApp::hintText(State state)
{
    switch (state) {
    case State::NotProvisioned:
        if (!bt_service_is_enabled()) {
            return "Turn on Bluetooth in Settings";
        }
        switch (_ble_state.load()) {
        case MUSE_BLE_PAIRING_STATE_CONNECTED:
            return "Phone connected…";
        case MUSE_BLE_PAIRING_STATE_PAIRING:
            return "Pairing…";
        case MUSE_BLE_PAIRING_STATE_CONFIRM_REQUIRED:
            return "Tap to confirm";
        case MUSE_BLE_PAIRING_STATE_CONFIRMED:
        case MUSE_BLE_PAIRING_STATE_PROVISIONING:
            return "Setting up…";
        default:
            return "Pair in the Muse app";
        }
    case State::NetworkRequired:
        return offlineHint();
    case State::Ready:
#if CONFIG_MUSE_REALTIME_VOICE
        return "Tap to talk \u2022 hold: call";
#else
        return "Tap to talk";
#endif
    case State::CallConnecting:
        return "Connecting\u2026";
    case State::Listening:
        if (_in_call.load()) {
            return "On a call \u2022 tap to end";
        }
        return "Listening \u2022 tap to send";
    case State::Thinking:
        if (_in_call.load()) {
            return "On a call \u2022 tap to end";
        }
        return "Thinking \u2022 tap to cancel";
    case State::Speaking:
        if (_in_call.load()) {
            return "On a call \u2022 tap to end";
        }
        return "Tap to interrupt";
    case State::Error:
        return "Tap to retry";
    default:
        return "";
    }
}

/*
 * Offline, Settings opens on its WLAN page by itself once per offline spell, unless Wi-Fi is
 * still joining the saved network; Settings brings Muse back when a network is joined.
 * After that a tap opens it again.
 */
const char *MuseApp::offlineHint()
{
    wifi_service_info_t info = {};
    wifi_service_get_info(&info);
    if (info.state == WIFI_SERVICE_CONNECTING || info.state == WIFI_SERVICE_CONNECTED) {
        _offline_since = 0;
        snprintf(_hint, sizeof(_hint), "Joining %s...", info.ssid);
        return _hint;
    }
    if (_wlan_offered) {
        return "No network \u2022 tap to set up Wi-Fi";
    }
    const uint32_t now = lv_tick_get();
    if (_offline_since == 0) {
        _offline_since = now ? now : 1;
    } else if (now - _offline_since >= OFFLINE_JUMP_MS) {
        openWlanSettings();
    }
    return "No network \u2022 opening WLAN settings";
}

void MuseApp::openWlanSettings()
{
    _wlan_offered = true;
    _offline_since = 0;
    if (!Settings::openWlan(getId())) {
        ESP_UTILS_LOGE("Open WLAN settings failed");
    }
}

void MuseApp::knobCallback(int steps, void *user_ctx)
{
    auto *instance = static_cast<MuseApp *>(user_ctx);
    const int volume = audio_service_get_volume() + steps * VOLUME_STEP;
    audio_service_set_volume(volume < 0 ? 0 : (volume > 100 ? 100 : volume));
    if (instance->_ui) {
        char text[24];
        snprintf(text, sizeof(text), "Volume %d%%", audio_service_get_volume());
        instance->_ui->showToast(text);
    }
}

void MuseApp::setState(State state)
{
    const State previous = _state.exchange(state);
    if (previous != state) {
        ESP_UTILS_LOGI("Muse state %s -> %s", stateText(previous), stateText(state));
    }
}

void MuseApp::setError(const char *detail)
{
    copyString(_error_detail, sizeof(_error_detail), detail ? detail : "Error");
    setChatMessage("", _error_detail);
    setState(State::Error);
    _next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ERROR_RETRY_MS);
}

void MuseApp::setChatMessage(const char *role, const char *text)
{
    if (!_state_mutex ||
            xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    copyString(_chat_role, sizeof(_chat_role), role);
    copyString(_chat_text, sizeof(_chat_text), text);
    xSemaphoreGive(_state_mutex);
}

void MuseApp::showToastSoon(const char *text)
{
    if (!_state_mutex ||
            xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    copyString(_toast, sizeof(_toast), text);
    _toast_pending.store(true);
    xSemaphoreGive(_state_mutex);
}

void MuseApp::clearConversationUi()
{
    setChatMessage("", "");
}

bool MuseApp::canPrepare() const
{
    return _visible.load() && !_shutdown.load() && _client != nullptr;
}

bool MuseApp::isNetworkReady() const
{
    wifi_service_info_t info = {};
    wifi_service_get_info(&info);
    return info.state == WIFI_SERVICE_CONNECTED;
}

void MuseApp::updateNetworkState()
{
    const bool ready = isNetworkReady();
    if (ready != _network_ready.load()) {
        _network_ready.store(ready);
        ControlEvent event = {};
        event.type = ControlEventType::NetworkChanged;
        event.flag = ready;
        postControlEvent(event);
    }
}

bool MuseApp::postControlEvent(const ControlEvent &event)
{
    if (!_control_queue || _shutdown.load()) {
        return false;
    }
    return xQueueSend(
        _control_queue, &event, pdMS_TO_TICKS(CONTROL_EVENT_POST_TIMEOUT_MS)
    ) == pdTRUE;
}

bool MuseApp::postControlEventNowait(const ControlEvent &event)
{
    if (!_control_queue || _shutdown.load()) {
        return false;
    }
    return xQueueSend(_control_queue, &event, 0) == pdTRUE;
}

void MuseApp::controlLoop()
{
    bool exit_requested = false;
    uint32_t tick = 0;
    while (!exit_requested) {
        ControlEvent event = {};
        if (xQueueReceive(_control_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (event.type == ControlEventType::Shutdown) {
                exit_requested = true;
            } else {
                handleControlEvent(event);
            }
        }
        if (!exit_requested) {
            controlTick(tick++);
        }
    }

    _visible.store(false);
    closeTurn();
    requestCallEnd();
    if (_client) {
        _client->stop();
    }
    shutdownAudio();
    _control_task = nullptr;
    _control_running.store(false);
    if (_control_exited) {
        xSemaphoreGive(_control_exited);
    }
    vTaskDelete(nullptr);
}

void MuseApp::controlTick(uint32_t tick)
{
    if (!_visible.load() || !_session_started.load() || !_client) {
        return;
    }
    State state = _state.load();
    // A call owns its own event drain (the call task); turn polling would
    // read a turn that isn't there.
    if (!_in_call.load() &&
            (state == State::Listening || state == State::Thinking ||
             state == State::Speaking)) {
        _client->pollEvents();
        if (state == State::Speaking) {
            // While a reply plays, the session pages the reply text by the
            // playback cursor so the screen keeps up with the voice, and
            // reports progress at the same cursor for the ring.
            char page[256] = {};
            if (_client->captionAt(_played_frames.load(), page, sizeof(page))) {
                setChatMessage("assistant", page);
            }
            _progress.store(_client->turnProgress(_played_frames.load()));
        }
    }
    // Session status every 500 ms drives Preparing/Connecting/Ready/Error.
    if (tick % 5 == 0) {
        pollSessionStatus();
    }
    // Provisioning (pairing can finish while the app is open) and late replies
    // every 1 s.
    if (tick % 10 == 0) {
        pollProvisioning();
        pollIncoming();
    }
    // A turn that ended but never drained (e.g. the playback task died) must
    // not leave the UI stuck in Speaking.
    if (_turn_done.load() && _state.load() == State::Speaking &&
            tickReached(_turn_done_tick, TURN_DRAIN_TIMEOUT_MS)) {
        _turn_done.store(false);
        setState(State::Ready);
    }
    // Retry a failed connection on the error timer.
    if (_state.load() == State::Error && _next_retry_tick != 0 &&
            tickReached(_next_retry_tick, 0)) {
        _next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ERROR_RETRY_MS);
        if (_token_error.load()) {
            startTokenRefresh();
        } else {
            setState(State::Connecting);
            _client->testConnection();
        }
    }
}

void MuseApp::pollSessionStatus()
{
    if (!_client) {
        return;
    }
    State state = _state.load();
    if (state == State::Listening || state == State::Thinking ||
            state == State::Speaking || state == State::CallConnecting) {
        return;  // A turn or a call owns the state machine.
    }
    muse_hatch_status_t status = _client->status();
    const bool token_rejected =
        status.detail[0] != '\0' &&
        strstr(status.detail, "Token rejected") != nullptr;
    switch (status.state) {
    case MUSE_HATCH_NOT_SET:
        if (state != State::NotProvisioned) {
            setState(State::NotProvisioned);
        }
        break;
    case MUSE_HATCH_OFFLINE:
        if (state != State::NetworkRequired) {
            setState(State::NetworkRequired);
        }
        break;
    case MUSE_HATCH_TESTING:
        if (state == State::Preparing || state == State::Connecting ||
                state == State::Error) {
            setState(State::Connecting);
        }
        break;
    case MUSE_HATCH_REACHABLE:
        if (state != State::Ready) {
            _token_error.store(false);
            _refresh_attempted.store(false);
            setState(State::Ready);
        }
        break;
    case MUSE_HATCH_UNREACHABLE:
        if (token_rejected) {
            _token_error.store(true);
            if (!_refresh_attempted.load()) {
                _refresh_attempted.store(true);
                startTokenRefresh();
            }
        }
        if (state == State::Preparing || state == State::Connecting ||
                state == State::Ready) {
            setError(status.detail);
        }
        break;
    case MUSE_HATCH_UNTESTED:
    default:
        break;
    }
}

void MuseApp::pollProvisioning()
{
    if (!_client) {
        return;
    }
    bool provisioned = _client->isProvisioned();
    if (provisioned == _provisioned.load()) {
        return;
    }
    _provisioned.store(provisioned);
    if (provisioned) {
        _refresh_attempted.store(false);
        _token_error.store(false);
        stopBleAdvertising();
        _client->configChanged();
        if (isNetworkReady()) {
            setState(State::Connecting);
            _client->testConnection();
        } else {
            setState(State::NetworkRequired);
        }
    } else {
        closeTurn();
        requestCallEnd();
        _client->stop();
        setState(State::NotProvisioned);
        setChatMessage("", "Pair in the Muse app\nto link this device");
        startBleAdvertising();
    }
}

void MuseApp::pollIncoming()
{
    if (!_client || _state.load() != State::Ready) {
        return;
    }
    if (!_client->checkIncoming()) {
        return;
    }
    // A reply to an earlier turn arrived after we showed Ready. Play it.
    if (!ensureAudioReady()) {
        _client->cancelTurn();
        return;
    }
    _turn_done.store(false);
    _drain_posted.store(false);
    _played_frames.store(0);
    _playback_enabled.store(true);
    _accept_playback.store(true);
    setState(State::Speaking);
}

void MuseApp::handleControlEvent(const ControlEvent &event)
{
    if (_shutdown.load() && event.type != ControlEventType::Shutdown) {
        return;
    }
    switch (event.type) {
    case ControlEventType::AppVisible:
        handleAppVisible();
        break;
    case ControlEventType::AppHidden:
        handleAppHidden(event.flag);
        break;
    case ControlEventType::NetworkChanged:
        if (event.flag) {
            if (_state.load() == State::NetworkRequired) {
                if (_provisioned.load()) {
                    setState(State::Connecting);
                    _client->testConnection();
                } else {
                    setState(State::NotProvisioned);
                }
            }
        } else {
            closeTurn();
            requestCallEnd();
            setState(State::NetworkRequired);
        }
        break;
    case ControlEventType::ActionPressed: {
        State state = _state.load();
        switch (state) {
        case State::Ready:
            startTurn();
            break;
        case State::CallConnecting:
            // Tapping while a call sets up cancels it.
            requestCallEnd();
            break;
        case State::Listening:
            if (_in_call.load()) {
                requestCallEnd();
            } else {
                endTurn();
            }
            break;
        case State::Thinking:
        case State::Speaking:
            if (_in_call.load()) {
                requestCallEnd();
            } else {
                cancelTurn();
                setState(State::Ready);
            }
            break;
        case State::Error:
            _next_retry_tick = 0;  // Retry now instead of waiting for the timer.
            if (_token_error.load()) {
                startTokenRefresh();
            } else {
                setState(State::Connecting);
                _client->testConnection();
            }
            break;
        case State::NotProvisioned:
            if (_confirm_pending.load()) {
                if (muse_ble_pairing_confirm()) {
                    _confirm_pending.store(false);
                    setChatMessage("", "Confirmed\nWaiting for the app…");
                } else {
                    _confirm_pending.store(false);
                    showToastSoon("Pairing expired");
                }
            } else {
                showToastSoon("Pair in the Muse app");
            }
            break;
        default:
            break;
        }
        break;
    }
    case ControlEventType::TurnHeard:
        setChatMessage("user", event.text);
        break;
    case ControlEventType::HoldPressed:
        if (_state.load() == State::Ready) {
            startCall();
        } else if (_in_call.load()) {
            requestCallEnd();
        }
        break;
    case ControlEventType::CallViewChanged:
        handleCallView(static_cast<CallView>(event.value));
        break;
    case ControlEventType::CallHeard:
        setChatMessage("user", event.text);
        break;
    case ControlEventType::CallReply:
        // Caller and reply text go to captions only, never the log.
        setChatMessage("assistant", event.text);
        break;
    case ControlEventType::CallEnded: {
        _in_call.store(false);
        _call_working.store(false);
        setChatMessage("", event.text[0] != '\0' ? event.text : "Call ended");
        State state = _state.load();
        if (_visible.load() &&
                (state == State::CallConnecting || state == State::Listening ||
                 state == State::Thinking || state == State::Speaking)) {
            setState(State::Ready);
        }
        break;
    }
    case ControlEventType::TurnReply:
        setChatMessage("assistant", event.text);
        if (_state.load() == State::Thinking) {
            _playback_enabled.store(true);
            _accept_playback.store(true);
            setState(State::Speaking);
        }
        break;
    case ControlEventType::TurnDone:
        _turn_done.store(true);
        _turn_done_tick = xTaskGetTickCount();
        _capture_enabled.store(false);
        if (_state.load() != State::Speaking) {
            setState(State::Ready);
        }
        break;
    case ControlEventType::TurnError:
        _capture_enabled.store(false);
        _playback_enabled.store(false);
        _accept_playback.store(false);
        setError(event.text);
        break;
    case ControlEventType::TurnSent:
        // The voice note reached Muse; nothing to show until the reply arrives.
        break;
    case ControlEventType::PlaybackDrained:
        if (_state.load() == State::Speaking) {
            setState(State::Ready);
        }
        break;
    case ControlEventType::AudioPreempted:
        closeTurn();
        requestCallEnd();
        shutdownAudio();
        if (_state.load() == State::Listening || _state.load() == State::Thinking ||
                _state.load() == State::Speaking || _state.load() == State::CallConnecting) {
            setState(State::Ready);
        }
        break;
    case ControlEventType::AudioError:
        _capture_enabled.store(false);
        _playback_enabled.store(false);
        _accept_playback.store(false);
        setError(event.detail);
        break;
    case ControlEventType::TokenRefreshed:
        _token_error.store(false);
        _refresh_attempted.store(false);
        _client->configChanged();
        _client->testConnection();
        setState(State::Connecting);
        break;
    case ControlEventType::TokenRevoked:
        closeTurn();
        requestCallEnd();
        _client->stop();
        (void)muse_provisioning_unpair();
        _provisioned.store(false);
        if (_ble_inited.load()) {
            (void)muse_ble_pairing_set_provisioned(false);
        }
        setState(State::NotProvisioned);
        setChatMessage("", "Pairing expired\nPair again in the Muse app");
        startBleAdvertising();
        break;
    case ControlEventType::TokenRefreshFailed:
        // Stay in Error; the retry timer re-attempts the refresh.
        _next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ERROR_RETRY_MS);
        break;
    case ControlEventType::BleConfirmRequired:
        handleBleConfirmRequired(event.value);
        break;
    case ControlEventType::BleStateChanged:
        handleBleStateChanged(
            static_cast<muse_ble_pairing_state_t>(event.value)
        );
        break;
    case ControlEventType::Shutdown:
        break;
    }
}

void MuseApp::handleAppVisible()
{
    if (!canPrepare()) {
        return;
    }
    // The session starts lazily on the first open so the app costs nothing
    // (no task, no RAM) until the user opens it.
    if (!_session_started.load()) {
        if (!_client->start()) {
            setError("Start Muse session");
            return;
        }
        _session_started.store(true);
        _provisioned.store(_client->isProvisioned());
    }
    // Identity is ready once the session started (muse_vm_init ran there).
    initBlePairing();
    if (!_provisioned.load()) {
        setState(State::NotProvisioned);
        char text[128];
        snprintf(
            text, sizeof(text), "Pair in the Muse app\n%s", _client->nodeId()
        );
        setChatMessage("", text);
        startBleAdvertising();
        return;
    }
    if (!isNetworkReady()) {
        setState(State::NetworkRequired);
        return;
    }
    muse_hatch_status_t status = _client->status();
    if (status.state == MUSE_HATCH_REACHABLE) {
        setState(State::Ready);
    } else {
        setState(State::Connecting);
        _client->testConnection();
    }
}

void MuseApp::handleAppHidden(bool release_audio)
{
    closeTurn();
    requestCallEnd();
    // Leaving the screen mid-pairing ends pairing: the Muse app can only
    // pair while this screen is visible.
    stopBleAdvertising();
    if (release_audio) {
        if (_client) {
            _client->stop();
        }
        shutdownAudio();
    }
}

void MuseApp::startTokenRefresh()
{
    if (_refresh_running.load() || !_client) {
        return;
    }
    _refresh_running.store(true);
    if (createMuseTask(
            refreshTaskEntry,
            "muse_refresh",
            REFRESH_TASK_STACK_SIZE,
            this,
            4,
            &_refresh_task,
            0,
            false
         ) != pdPASS) {
        _refresh_running.store(false);
        _refresh_task = nullptr;
        ControlEvent event = {};
        event.type = ControlEventType::TokenRefreshFailed;
        postControlEvent(event);
    }
}

// ---- BLE pairing (MuseBlePairing; see its INTEGRATION.md) ----
//
// The pairing GATT server lives in the MuseBlePairing component; this app
// owns the flow around it: init once, advertise while visible and
// unprovisioned, show the confirm prompt, and handle provision_v2 (join
// Wi-Fi, store the device tokens, report auth_ok). UNTESTED ON HARDWARE.

void MuseApp::initBlePairing()
{
    if (_ble_inited.load()) {
        return;
    }
    // Identity strings are static buffers (identity.c) and stay valid for
    // the life of the component, as muse_ble_pairing_init requires.
    const esp_app_desc_t *desc = esp_app_get_description();
    muse_ble_pairing_config_t config = {};
    config.device_name = muse_provisioning_ble_name();
    config.node_id = muse_provisioning_node_id();
    config.device_id = identity_device_id();
    config.mac = identity_mac();
    config.firmware_version = desc ? desc->version : "unknown";
    config.sdk_token = muse_provisioning_sdk_token();
    config.provisioned = muse_provisioning_is_provisioned();
    config.callbacks.on_provision = bleProvisionCallback;
    config.callbacks.on_wifi_scan = bleWifiScanCallback;
    config.callbacks.on_unpair = bleUnpairCallback;
    config.callbacks.on_get_device_info = bleGetDeviceInfoCallback;
    config.callbacks.on_confirm_required = bleConfirmRequiredCallback;
    config.callbacks.on_state_changed = bleStateChangedCallback;
    if (muse_ble_pairing_init(&config) != ESP_OK) {
        ESP_UTILS_LOGE("BLE pairing init failed");
        return;
    }
    _ble_inited.store(true);
}

void MuseApp::startBleAdvertising()
{
    if (!_ble_inited.load() || !_visible.load() || _provisioned.load()) {
        return;
    }
    if (!bt_service_is_enabled()) {
        // Settings has Bluetooth off. The toggle only changes the Classic
        // scan mode, so BLE advertising would still work — but respect the
        // toggle; the hint tells the user to turn Bluetooth on.
        return;
    }
    if (muse_ble_pairing_start_advertising() == ESP_OK) {
        _ble_advertising.store(true);
    }
}

void MuseApp::stopBleAdvertising()
{
    _confirm_pending.store(false);
    if (!_ble_inited.load()) {
        return;
    }
    (void)muse_ble_pairing_stop_advertising(true);
    _ble_advertising.store(false);
}

void MuseApp::handleBleConfirmRequired(uint32_t generation)
{
    _confirm_pending.store(true);
    _confirm_generation.store(generation);
    setChatMessage("", "Pairing request\nTap to confirm");
    // Arm only once the prompt is set: the control task owns the UI state
    // and the LVGL timer applies it within 100 ms; a tap queues behind
    // this event, so the user cannot confirm before seeing the prompt.
    if (!muse_ble_pairing_arm_confirmation(generation)) {
        _confirm_pending.store(false);
    }
}

void MuseApp::handleBleStateChanged(muse_ble_pairing_state_t state)
{
    _ble_state.store(state);
    if (state == MUSE_BLE_PAIRING_STATE_IDLE ||
            state == MUSE_BLE_PAIRING_STATE_ADVERTISING) {
        _confirm_pending.store(false);
    }
    if (state == MUSE_BLE_PAIRING_STATE_PROVISIONED) {
        _ble_advertising.store(false);
    }
}

void MuseApp::handleBleProvision(const muse_provision_payload_t *payload)
{
    // Runs on the BLE component's provision worker task (8 KB stack,
    // internal RAM, so NVS writes are safe). Payload strings are borrowed
    // for this call only; copy what the Wi-Fi wait below needs.
    char ssid[WIFI_SERVICE_SSID_MAX] = {};
    char password[WIFI_SERVICE_PASSWORD_MAX] = {};
    char access[MUSE_VM_TOKEN_MAX + 1] = {};
    char refresh[MUSE_VM_TOKEN_MAX + 1] = {};
    char api_url_v2[MUSE_VM_API_BASE_MAX + 1] = {};
    char noise_host[MUSE_VM_HOST_MAX + 1] = {};
    copyString(ssid, sizeof(ssid), payload->ssid);
    copyString(password, sizeof(password), payload->password);
    copyString(access, sizeof(access), payload->access_token);
    copyString(refresh, sizeof(refresh), payload->refresh_token);
    copyString(api_url_v2, sizeof(api_url_v2), payload->api_url_v2);
    copyString(noise_host, sizeof(noise_host), payload->noise_host);
    const uint32_t generation = payload->session_generation;

    auto fail = [generation](const char *status) {
        (void)muse_ble_pairing_send_pairing_status(status, generation);
        muse_ble_pairing_disconnect_pairing_session(generation);
    };

    if (wifi_service_connect(ssid, password) != ESP_OK) {
        ESP_UTILS_LOGE("Provisioning Wi-Fi join failed to start");
        fail("auth_failed");
        return;
    }
    // The join runs in the background; wait up to 20 s for an IP address.
    bool connected = false;
    for (int i = 0; i < 40; i++) {
        wifi_service_info_t info = {};
        wifi_service_get_info(&info);
        if (info.state == WIFI_SERVICE_CONNECTED) {
            connected = true;
            break;
        }
        if (info.state == WIFI_SERVICE_IDLE &&
                info.fail != WIFI_SERVICE_FAIL_NONE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!connected) {
        ESP_UTILS_LOGE("Provisioning Wi-Fi join timed out or failed");
        fail("auth_failed");
        return;
    }
    // The tokens were minted for the node id this device offered.
    if (!muse_provisioning_set_device_tokens(
            access, refresh, muse_provisioning_node_id(), api_url_v2,
            noise_host)) {
        ESP_UTILS_LOGE("Provisioning token store failed");
        fail("error_storage");
        return;
    }
    if (muse_ble_pairing_session_generation() != generation) {
        // The session expired mid-handler; the tokens are stored, so the
        // device is provisioned either way — report it and let the app
        // notice on its next poll.
        (void)muse_ble_pairing_set_provisioned(true);
        return;
    }
    (void)muse_ble_pairing_send_pairing_status("auth_ok", generation);
    (void)muse_ble_pairing_stop_advertising(true);
    (void)muse_ble_pairing_set_provisioned(true);
    _ble_advertising.store(false);
    // pollProvisioning (1 s) notices the new tokens and connects.
}

void MuseApp::handleBleWifiScan()
{
    // Runs on a BLE worker task. Trigger a fresh scan and wait for a new
    // scan generation (up to 10 s); on timeout the last list goes out.
    uint32_t before = 0;
    wifi_service_ap_t aps[WIFI_SERVICE_SCAN_MAX] = {};
    int count = wifi_service_get_scan(aps, WIFI_SERVICE_SCAN_MAX, &before);
    (void)wifi_service_scan();
    for (int i = 0; i < 20; i++) {
        uint32_t generation = 0;
        count = wifi_service_get_scan(aps, WIFI_SERVICE_SCAN_MAX, &generation);
        if (generation != before) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "wifi_scan_result");
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    if (networks) {
        for (int i = 0; i < count; i++) {
            cJSON *ap = cJSON_CreateObject();
            if (!ap) {
                break;
            }
            cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(ap, "secure", aps[i].secure);
            cJSON_AddItemToArray(networks, ap);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        (void)muse_ble_pairing_send_encrypted_json(
            json, muse_ble_pairing_session_generation()
        );
        free(json);
    }
}

void MuseApp::handleBleUnpair()
{
    // Runs on a BLE worker task. Wi-Fi is kept (SDK config_unpair
    // semantics); pollProvisioning re-opens pairing on its next tick.
    (void)muse_provisioning_unpair();
    (void)muse_ble_pairing_set_provisioned(false);
}

void MuseApp::handleBleGetDeviceInfo()
{
    // Runs on a BLE worker task. Mirrors the SDK app.c on_get_device_info,
    // with the six link_pairing_add_device_info() fields re-implemented
    // here (MuseBlePairing keeps link_pairing.h private).
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "device_info");
    // The id a setup on this connection takes (identity.h).
    cJSON_AddStringToObject(root, "node_id", identity_setup_node_id());
    cJSON_AddStringToObject(root, "version", desc ? desc->version : "unknown");
    cJSON_AddStringToObject(root, "device_id", identity_device_id());
    cJSON_AddStringToObject(root, "mac", identity_mac());
    cJSON_AddStringToObject(root, "model", "hatch_link");
    cJSON_AddNumberToObject(root, "pairing_protocol", 5);
    cJSON_AddStringToObject(root, "pairing_auth", "none");
    cJSON_AddNumberToObject(root, "pairing_auth_epoch", 0);
    cJSON_AddStringToObject(root, "pairing_policy", "confirm_press");
    char build_sha[17] = {};
    if (desc) {
        for (int i = 0; i < 8; i++) {
            snprintf(build_sha + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
        }
    }
    cJSON_AddStringToObject(root, "build_sha", build_sha);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        muse_ble_pairing_send_chunked(json);
        free(json);
    }
}

void MuseApp::bleProvisionCallback(const muse_provision_payload_t *payload)
{
    if (_instance) {
        _instance->handleBleProvision(payload);
    }
}

void MuseApp::bleWifiScanCallback()
{
    if (_instance) {
        _instance->handleBleWifiScan();
    }
}

void MuseApp::bleUnpairCallback()
{
    if (_instance) {
        _instance->handleBleUnpair();
    }
}

void MuseApp::bleGetDeviceInfoCallback()
{
    if (_instance) {
        _instance->handleBleGetDeviceInfo();
    }
}

void MuseApp::bleConfirmRequiredCallback(uint32_t generation)
{
    if (!_instance) {
        return;
    }
    ControlEvent event = {};
    event.type = ControlEventType::BleConfirmRequired;
    event.value = generation;
    (void)_instance->postControlEvent(event);
}

void MuseApp::bleStateChangedCallback(muse_ble_pairing_state_t state)
{
    // Runs on the BLE dispatch task: keep it short, never block, never
    // call BLE APIs — post with a zero timeout and drop if the queue is
    // full (the state is also readable via muse_ble_pairing_get_state).
    if (!_instance || !_instance->_control_queue ||
            _instance->_shutdown.load()) {
        return;
    }
    ControlEvent event = {};
    event.type = ControlEventType::BleStateChanged;
    event.value = static_cast<uint32_t>(state);
    (void)xQueueSend(_instance->_control_queue, &event, 0);
}

bool MuseApp::ensureAudioReady()
{
    if (audio_client_is_owner(_audio_client) && _audio_tasks_running.load() &&
            _input_resampler && _output_resampler) {
        return true;
    }
    shutdownAudio();
    if (!_audio_client) {
        return false;
    }
    esp_err_t ret = audio_client_acquire(_audio_client);
    if (ret == ESP_OK) {
        ret = audio_client_set_format(
                  _audio_client,
                  DEVICE_AUDIO_SAMPLE_RATE,
                  16,
                  DEVICE_TDM_CHANNELS
              );
    }
    if (ret != ESP_OK) {
        ESP_UTILS_LOGE("Muse audio acquire/format failed: %s", esp_err_to_name(ret));
        return false;
    }
    esp_ae_rate_cvt_cfg_t input_config = {};
    input_config.src_rate = DEVICE_AUDIO_SAMPLE_RATE;
    input_config.dest_rate = CHAT_SAMPLE_RATE;
    input_config.channel = ESP_AUDIO_MONO;
    input_config.bits_per_sample = ESP_AUDIO_BIT16;
    input_config.complexity = 2;
    input_config.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    if (esp_ae_rate_cvt_open(&input_config, &_input_resampler) != ESP_AE_ERR_OK) {
        ESP_UTILS_LOGE("Muse input resampler open failed");
        shutdownAudio();
        return false;
    }
    if (esp_ae_rate_cvt_get_max_out_sample_num(
            _input_resampler, INPUT_READ_FRAMES, &_input_resampler_max_samples
        ) != ESP_AE_ERR_OK || _input_resampler_max_samples == 0) {
        ESP_UTILS_LOGE("Muse input resampler size query failed");
        shutdownAudio();
        return false;
    }
    esp_ae_rate_cvt_cfg_t output_config = {};
    output_config.src_rate = CHAT_SAMPLE_RATE;
    output_config.dest_rate = DEVICE_AUDIO_SAMPLE_RATE;
    output_config.channel = ESP_AUDIO_MONO;
    output_config.bits_per_sample = ESP_AUDIO_BIT16;
    output_config.complexity = 2;
    output_config.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    if (esp_ae_rate_cvt_open(&output_config, &_output_resampler) != ESP_AE_ERR_OK) {
        ESP_UTILS_LOGE("Muse output resampler open failed");
        shutdownAudio();
        return false;
    }
    if (esp_ae_rate_cvt_get_max_out_sample_num(
            _output_resampler, REPLY_READ_FRAMES, &_output_resampler_max_samples
        ) != ESP_AE_ERR_OK || _output_resampler_max_samples == 0) {
        ESP_UTILS_LOGE("Muse output resampler size query failed");
        shutdownAudio();
        return false;
    }
    _audio_tasks_running.store(true);
    // Mark the tasks running before creating them so shutdownAudio() cannot
    // close the resamplers while a task is still starting up.
    _input_task_running.store(true);
    if (createMuseTask(
            inputTaskEntry,
            "muse_input",
            INPUT_TASK_STACK_SIZE,
            this,
            8,
            &_input_task,
            0,
            false
         ) != pdPASS) {
        _input_task_running.store(false);
        _input_task = nullptr;
        shutdownAudio();
        return false;
    }
    _playback_task_running.store(true);
    if (createMuseTask(
            playbackTaskEntry,
            "muse_playback",
            PLAYBACK_TASK_STACK_SIZE,
            this,
            6,
            &_playback_task,
            0,
            true
         ) != pdPASS) {
        _playback_task_running.store(false);
        _playback_task = nullptr;
        shutdownAudio();
        return false;
    }
    return true;
}

void MuseApp::shutdownAudio()
{
    _capture_enabled.store(false);
    _playback_enabled.store(false);
    _accept_playback.store(false);
    _audio_tasks_running.store(false);
    // The call task owns the resamplers too while a call runs: ask it to
    // hang up and wait for it before closing them (it exits within its
    // bounded end-wait once ENDED arrives, or the give-up deadline).
    requestCallEnd();
    while (_input_task_running.load() || _playback_task_running.load() ||
            _call_task_running.load()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _input_task = nullptr;
    _playback_task = nullptr;
    _call_task = nullptr;
    if (_input_resampler) {
        esp_ae_rate_cvt_close(_input_resampler);
        _input_resampler = nullptr;
    }
    if (_output_resampler) {
        esp_ae_rate_cvt_close(_output_resampler);
        _output_resampler = nullptr;
    }
    _input_resampler_max_samples = 0;
    _output_resampler_max_samples = 0;
    if (_audio_client) {
        (void)audio_client_release(_audio_client);
    }
}

void MuseApp::startTurn()
{
    if (!_client) {
        return;
    }
    // A turn and a call must never overlap: the session has no guard, and
    // muse_hatch_turn_begin() would start a voice note even mid-call.
    if (_in_call.load()) {
        return;
    }
    if (!_provisioned.load()) {
        setState(State::NotProvisioned);
        return;
    }
    if (!isNetworkReady()) {
        setState(State::NetworkRequired);
        return;
    }
    if (!_client->isReady()) {
        setState(State::Connecting);
        _client->testConnection();
        return;
    }
    if (!ensureAudioReady()) {
        setError("Audio unavailable");
        return;
    }
    clearConversationUi();
    _turn_done.store(false);
    _drain_posted.store(false);
    _played_frames.store(0);
    _progress.store(0);
    _listen_started_tick = xTaskGetTickCount();
    _client->beginTurn();
    _capture_enabled.store(true);
    setState(State::Listening);
}

void MuseApp::endTurn()
{
    _capture_enabled.store(false);
    if (_client) {
        _client->endTurn();
    }
    setState(State::Thinking);
}

void MuseApp::cancelTurn()
{
    _capture_enabled.store(false);
    _playback_enabled.store(false);
    _accept_playback.store(false);
    if (_client) {
        _client->cancelTurn();
    }
}

void MuseApp::closeTurn()
{
    cancelTurn();
    _turn_done.store(false);
    _drain_posted.store(false);
}

// ---- Realtime voice calls (Phase 4.1) ----

void MuseApp::startCall()
{
#if CONFIG_MUSE_REALTIME_VOICE
    if (!_client || _in_call.load() || _call_task_running.load()) {
        return;
    }
    if (!_provisioned.load()) {
        setState(State::NotProvisioned);
        return;
    }
    if (!isNetworkReady()) {
        setState(State::NetworkRequired);
        return;
    }
    if (!_client->callReady()) {
        setState(State::Connecting);
        _client->testConnection();
        return;
    }
    // A turn and a call must never overlap (see startTurn).
    closeTurn();
    if (!ensureAudioReady()) {
        setError("Audio unavailable");
        return;
    }
    clearConversationUi();
    _call_end_requested.store(false);
    _call_working.store(false);
    _in_call.store(true);
    _call_task_running.store(true);
    // Mark the task running before creating it so shutdownAudio() cannot
    // close the resamplers while it is still starting up.
    if (createMuseTask(
            callTaskEntry,
            "muse_call",
            CALL_TASK_STACK_SIZE,
            this,
            8,
            &_call_task,
            0,
            false
         ) != pdPASS) {
        _call_task_running.store(false);
        _call_task = nullptr;
        _in_call.store(false);
        setError("Start call task");
        return;
    }
    setState(State::CallConnecting);
#else
    showToastSoon("Calls unavailable");
#endif
}

void MuseApp::requestCallEnd()
{
    // The call task hangs up itself (muse_voice_call_end must run on the
    // voice task); the control task only raises the flag. Safe with no
    // call active: startCall clears the flag before creating the task.
    _call_end_requested.store(true);
}

void MuseApp::handleCallView(CallView view)
{
    if (!_in_call.load()) {
        return;  // Stale: the call already ended.
    }
    switch (view) {
    case CallView::Connecting:
        _call_working.store(false);
        setState(State::CallConnecting);
        break;
    case CallView::Listening:
        _call_working.store(false);
        setState(State::Listening);
        break;
    case CallView::Thinking:
        _call_working.store(false);
        setState(State::Thinking);
        break;
    case CallView::Speaking:
        _call_working.store(false);
        setState(State::Speaking);
        break;
    case CallView::Working:
        _call_working.store(true);
        setState(State::Thinking);
        break;
    case CallView::Ending:
        _call_working.store(false);
        setChatMessage("", "Hanging up\u2026");
        break;
    }
}

/*
 * The call task: the SDK's run_call (muse_voice.c), hands-free only, in
 * 10 ms passes. One read + one write per pass paces the duplex bus:
 * drain call events, feed gated mic audio, read reply audio, write it.
 * The half-duplex gate keeps the mic shut while Muse is audibly talking
 * plus a 300 ms tail; silent keepalive audio doesn't hold it shut.
 * No PTT call yet: the UI has no press/release events (see MuseUi).
 *
 * The turn tasks idle during a call (_capture_enabled/_playback_enabled
 * are false), so this task alone uses the audio client and resamplers.
 * It exits on ENDED (after draining up to 2 s of queued reply audio
 * without reopening the mic), on the hang-up give-up deadline, or when
 * the audio goes away; in every case it ends the call first if it hasn't
 * ended already, and posts CallEnded with the reason.
 */
void MuseApp::callAudio()
{
#if CONFIG_MUSE_REALTIME_VOICE
    const size_t input_bytes =
        INPUT_READ_FRAMES * DEVICE_TDM_CHANNELS * sizeof(int16_t);
    int16_t *device_stereo = static_cast<int16_t *>(malloc(input_bytes));
    int16_t *mono24 =
        static_cast<int16_t *>(malloc(INPUT_READ_FRAMES * sizeof(int16_t)));
    int16_t *mono16 = static_cast<int16_t *>(
                          malloc(_input_resampler_max_samples * sizeof(int16_t))
                      );
    int16_t *out16 =
        static_cast<int16_t *>(malloc(CALL_CHUNK_FRAMES * sizeof(int16_t)));
    int16_t *out24 = static_cast<int16_t *>(
                         malloc(_output_resampler_max_samples * sizeof(int16_t))
                     );
    int16_t *stereo = static_cast<int16_t *>(
                          malloc(
                              _output_resampler_max_samples *
                              DEVICE_TDM_CHANNELS * sizeof(int16_t)
                          )
                      );
    if (!device_stereo || !mono24 || !mono16 || !out16 || !out24 || !stereo) {
        free(device_stereo);
        free(mono24);
        free(mono16);
        free(out16);
        free(out24);
        free(stereo);
        ControlEvent event = {};
        event.type = ControlEventType::AudioError;
        copyString(
            event.detail, sizeof(event.detail), "Allocate Muse call buffers"
        );
        postControlEvent(event);
        _call_task_running.store(false);
        vTaskDelete(nullptr);
        return;
    }

    muse_vc_gate_t gate;
    muse_vc_gate_init(&gate, false, CALL_TAIL_US);
    _client->startCall();

    bool live = false;
    bool server_hold = false;
    bool hung_up = false;
    bool ended = false;
    int64_t give_up = 0;
    CallView view = CallView::Connecting;
    char end_reason[96] = {};
    char text[96] = {};
    {
        ControlEvent event = {};
        event.type = ControlEventType::CallViewChanged;
        event.value = static_cast<uint32_t>(CallView::Connecting);
        postControlEventNowait(event);
    }

    while (_audio_tasks_running.load() && !_shutdown.load() && !ended) {
        if (_call_end_requested.load() && !hung_up) {
            hung_up = true;
            give_up = esp_timer_get_time() + CALL_END_WAIT_US;
            _client->endCall();
            view = CallView::Ending;
            ControlEvent event = {};
            event.type = ControlEventType::CallViewChanged;
            event.value = static_cast<uint32_t>(CallView::Ending);
            postControlEventNowait(event);
        }

        esp_err_t ret =
            audio_client_read(_audio_client, device_stereo, input_bytes);
        bool captured = ret == ESP_OK;
        int32_t peak = 0;
        if (captured) {
            peak = mixMicToMono(device_stereo, mono24, INPUT_READ_FRAMES);
        }
        const int64_t now = esp_timer_get_time();

        muse_voice_call_ev_t ev;
        while ((ev = _client->pollCallEvent(text, sizeof(text))) !=
               MUSE_VOICE_CALL_EV_NONE) {
            ControlEvent event = {};
            switch (ev) {
            case MUSE_VOICE_CALL_EV_READY:
                live = true;
                break;
            case MUSE_VOICE_CALL_EV_HEARD:
                event.type = ControlEventType::CallHeard;
                copyString(event.text, sizeof(event.text), text);
                postControlEventNowait(event);
                break;
            case MUSE_VOICE_CALL_EV_REPLY:
                event.type = ControlEventType::CallReply;
                copyString(event.text, sizeof(event.text), text);
                postControlEventNowait(event);
                break;
            case MUSE_VOICE_CALL_EV_HOLD:
                server_hold = true;
                break;
            case MUSE_VOICE_CALL_EV_RESUME:
                server_hold = false;
                break;
            case MUSE_VOICE_CALL_EV_ENDED:
                ended = true;
                copyString(end_reason, sizeof(end_reason), text);
                break;
            case MUSE_VOICE_CALL_EV_NONE:
            default:
                break;
            }
            if (ended) {
                break;
            }
        }
        if (ended) {
            break;
        }

        const bool mic = captured && !hung_up && !server_hold && live &&
                         muse_vc_gate_mic_open(&gate, now);
        _input_pcm_peak.store(mic ? peak : 0);
        if (mic && _client) {
            uint32_t out_samples = 0;
            if (esp_ae_rate_cvt_process(
                    _input_resampler,
                    reinterpret_cast<esp_ae_sample_t>(mono24),
                    INPUT_READ_FRAMES,
                    reinterpret_cast<esp_ae_sample_t>(mono16),
                    &out_samples
                ) == ESP_AE_ERR_OK && out_samples > 0 &&
                    out_samples <= _input_resampler_max_samples) {
                _client->feedCallAudio(mono16, out_samples);
            }
        }

        bool flushed = false;
        const size_t n = hung_up ? 0 :
                         _client ? _client->readCallAudio(out16, CALL_CHUNK_FRAMES, &flushed) : 0;
        if (flushed) {
            muse_vc_gate_flush(&gate, now);
        }
        if (n > 0) {
            muse_vc_gate_playout(&gate, out16, n, now, CALL_SPEAKER_LAG_US);
            uint32_t out_samples = 0;
            if (esp_ae_rate_cvt_process(
                    _output_resampler,
                    reinterpret_cast<esp_ae_sample_t>(out16),
                    n,
                    reinterpret_cast<esp_ae_sample_t>(out24),
                    &out_samples
                ) == ESP_AE_ERR_OK && out_samples > 0 &&
                    out_samples <= _output_resampler_max_samples) {
                int32_t out_peak = 0;
                for (size_t i = 0; i < out_samples; i++) {
                    stereo[i * DEVICE_TDM_CHANNELS] = out24[i];
                    stereo[i * DEVICE_TDM_CHANNELS + 1] = out24[i];
                    const int32_t magnitude =
                        out24[i] < 0 ? -(int32_t)out24[i] : (int32_t)out24[i];
                    if (magnitude > out_peak) {
                        out_peak = magnitude;
                    }
                }
                _output_pcm_peak.store(out_peak);
                (void)audio_client_write(
                    _audio_client,
                    stereo,
                    out_samples * DEVICE_TDM_CHANNELS * sizeof(int16_t)
                );
            }
        } else {
            // Nothing to play: write silence to pace the duplex bus, as
            // the SDK does (one write per pass).
            const size_t silence_frames = INPUT_READ_FRAMES;
            memset(
                stereo, 0,
                silence_frames * DEVICE_TDM_CHANNELS * sizeof(int16_t)
            );
            (void)audio_client_write(
                _audio_client,
                stereo,
                silence_frames * DEVICE_TDM_CHANNELS * sizeof(int16_t)
            );
        }

        const bool speaking = !muse_vc_gate_mic_open(&gate, now);
        CallView next = CallView::Listening;
        if (hung_up) {
            next = CallView::Ending;
        } else if (!live) {
            next = CallView::Connecting;
        } else if (server_hold) {
            next = CallView::Thinking;
        } else if (speaking) {
            next = CallView::Speaking;
        } else if (_client && _client->callWorking()) {
            next = CallView::Working;
        }
        if (next != view) {
            view = next;
            ControlEvent event = {};
            event.type = ControlEventType::CallViewChanged;
            event.value = static_cast<uint32_t>(view);
            postControlEventNowait(event);
        }

        if (!captured) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (hung_up && now > give_up) {
            break;  // ENDED never arrived; the session-side cleanup retries it.
        }
    }

    if (!ended) {
        // Audio going away, shutdown, or the give-up deadline: end the
        // call explicitly rather than by dropping (the VM keeps a dropped
        // call through its disconnect grace; an unconfirmed end-call is
        // kept as one pending cleanup and retried on reconnect).
        _client->endCall();
    } else if (!hung_up && _audio_tasks_running.load()) {
        // ENDED closes the network, not the speaker queue. Drain its
        // bounded tail without reopening the mic; a new press only
        // interrupts this tail.
        const int64_t until = esp_timer_get_time() + CALL_DRAIN_US;
        while (esp_timer_get_time() < until &&
               _audio_tasks_running.load() && !_shutdown.load() &&
               !_call_end_requested.load()) {
            bool flushed = false;
            const size_t n = _client ?
                             _client->readCallAudio(out16, CALL_CHUNK_FRAMES, &flushed) : 0;
            if (n == 0) {
                break;
            }
            uint32_t out_samples = 0;
            if (esp_ae_rate_cvt_process(
                    _output_resampler,
                    reinterpret_cast<esp_ae_sample_t>(out16),
                    n,
                    reinterpret_cast<esp_ae_sample_t>(out24),
                    &out_samples
                ) != ESP_AE_ERR_OK || out_samples == 0 ||
                    out_samples > _output_resampler_max_samples) {
                break;
            }
            for (size_t i = 0; i < out_samples; i++) {
                stereo[i * DEVICE_TDM_CHANNELS] = out24[i];
                stereo[i * DEVICE_TDM_CHANNELS + 1] = out24[i];
            }
            if (audio_client_write(
                    _audio_client,
                    stereo,
                    out_samples * DEVICE_TDM_CHANNELS * sizeof(int16_t)
                ) != ESP_OK) {
                break;
            }
        }
    }

    {
        ControlEvent event = {};
        event.type = ControlEventType::CallEnded;
        copyString(
            event.text, sizeof(event.text),
            end_reason[0] != '\0' ? end_reason : "Call ended"
        );
        postControlEvent(event);
    }
    free(device_stereo);
    free(mono24);
    free(mono16);
    free(out16);
    free(out24);
    free(stereo);
    _input_pcm_peak.store(0);
    _output_pcm_peak.store(0);
    _call_task_running.store(false);
    vTaskDelete(nullptr);
#else
    _call_task_running.store(false);
    vTaskDelete(nullptr);
#endif
}

/*
 * Capture: the audio service hands us 10 ms of 24 kHz stereo. Mix (or pick)
 * the microphones to mono, resample to 16 kHz, and feed the session core,
 * which upsamples to 24 kHz for the wire.
 */
void MuseApp::inputAudio()
{
    const size_t input_bytes =
        INPUT_READ_FRAMES * DEVICE_TDM_CHANNELS * sizeof(int16_t);
    int16_t *device_stereo = static_cast<int16_t *>(malloc(input_bytes));
    int16_t *mono24 =
        static_cast<int16_t *>(malloc(INPUT_READ_FRAMES * sizeof(int16_t)));
    int16_t *mono16 = static_cast<int16_t *>(
                          malloc(_input_resampler_max_samples * sizeof(int16_t))
                      );
    if (!device_stereo || !mono24 || !mono16) {
        free(device_stereo);
        free(mono24);
        free(mono16);
        ControlEvent event = {};
        event.type = ControlEventType::AudioError;
        copyString(
            event.detail, sizeof(event.detail), "Allocate Muse capture buffers"
        );
        postControlEvent(event);
        _input_task_running.store(false);
        vTaskDelete(nullptr);
        return;
    }
    while (_audio_tasks_running.load()) {
        if (!_capture_enabled.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        esp_err_t ret =
            audio_client_read(_audio_client, device_stereo, input_bytes);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        _input_pcm_peak.store(mixMicToMono(device_stereo, mono24, INPUT_READ_FRAMES));
        if (_client && _capture_enabled.load()) {
            uint32_t out_samples = 0;
            if (esp_ae_rate_cvt_process(
                    _input_resampler,
                    reinterpret_cast<esp_ae_sample_t>(mono24),
                    INPUT_READ_FRAMES,
                    reinterpret_cast<esp_ae_sample_t>(mono16),
                    &out_samples
                ) == ESP_AE_ERR_OK && out_samples > 0 &&
                    out_samples <= _input_resampler_max_samples) {
                _client->feedAudio(mono16, out_samples);
            }
        }
    }
    free(device_stereo);
    free(mono24);
    free(mono16);
    _input_task_running.store(false);
    vTaskDelete(nullptr);
}

/*
 * Playback: the session core hands us 16 kHz mono reply audio in 20 ms
 * chunks (silence paced to the text in v1 — Muse replies are text-only, see
 * VENDORED.md). Resample to 24 kHz, duplicate to stereo, and write.
 */
void MuseApp::playAudio()
{
    int16_t *in16 =
        static_cast<int16_t *>(malloc(REPLY_READ_FRAMES * sizeof(int16_t)));
    int16_t *out24 = static_cast<int16_t *>(
                         malloc(_output_resampler_max_samples * sizeof(int16_t))
                     );
    int16_t *stereo = static_cast<int16_t *>(
                          malloc(
                              _output_resampler_max_samples *
                              DEVICE_TDM_CHANNELS * sizeof(int16_t)
                          )
                      );
    if (!in16 || !out24 || !stereo) {
        free(in16);
        free(out24);
        free(stereo);
        ControlEvent event = {};
        event.type = ControlEventType::AudioError;
        copyString(
            event.detail, sizeof(event.detail), "Allocate Muse playback buffers"
        );
        postControlEvent(event);
        _playback_task_running.store(false);
        vTaskDelete(nullptr);
        return;
    }
    while (_audio_tasks_running.load()) {
        if (!_playback_enabled.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t read = _client ? _client->readReply(in16, REPLY_READ_FRAMES, 20) : 0;
        if (read == 0) {
            if (_turn_done.load() && !_drain_posted.exchange(true)) {
                _playback_enabled.store(false);
                _accept_playback.store(false);
                ControlEvent event = {};
                event.type = ControlEventType::PlaybackDrained;
                postControlEvent(event);
            }
            continue;
        }
        if (!_accept_playback.load()) {
            continue;  // Stale audio from a cancelled turn: drop it.
        }
        uint32_t out_samples = 0;
        if (esp_ae_rate_cvt_process(
                _output_resampler,
                reinterpret_cast<esp_ae_sample_t>(in16),
                read,
                reinterpret_cast<esp_ae_sample_t>(out24),
                &out_samples
            ) != ESP_AE_ERR_OK || out_samples == 0 ||
                out_samples > _output_resampler_max_samples) {
            continue;
        }
        int32_t peak = 0;
        for (size_t i = 0; i < out_samples; i++) {
            stereo[i * DEVICE_TDM_CHANNELS] = out24[i];
            stereo[i * DEVICE_TDM_CHANNELS + 1] = out24[i];
            const int32_t magnitude =
                out24[i] < 0 ? -(int32_t)out24[i] : (int32_t)out24[i];
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        _output_pcm_peak.store(peak);
        _played_frames.fetch_add(read);
        if (audio_client_write(
                _audio_client,
                stereo,
                out_samples * DEVICE_TDM_CHANNELS * sizeof(int16_t)
            ) != ESP_OK) {
            ControlEvent event = {};
            event.type = ControlEventType::AudioError;
            copyString(
                event.detail, sizeof(event.detail), "Write Muse audio output"
            );
            postControlEvent(event);
            _accept_playback.store(false);
        }
    }
    free(in16);
    free(out24);
    free(stereo);
    _playback_task_running.store(false);
    vTaskDelete(nullptr);
}

const char *MuseApp::stateText(State state)
{
    switch (state) {
    case State::NotProvisioned:
        return "NOT CONNECTED";
    case State::NetworkRequired:
        return "NO NETWORK";
    case State::Preparing:
        return "PREPARING";
    case State::Connecting:
        return "CONNECTING";
    case State::Ready:
        return "READY";
    case State::Listening:
        return "LISTENING";
    case State::Thinking:
        return "THINKING";
    case State::Speaking:
        return "SPEAKING";
    case State::CallConnecting:
        return "CONNECTING";
    case State::Error:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

bool MuseApp::tickReached(TickType_t due_tick, uint32_t timeout_ms)
{
    return xTaskGetTickCount() - due_tick >= pdMS_TO_TICKS(timeout_ms);
}

void MuseApp::controlTaskEntry(void *arg)
{
    static_cast<MuseApp *>(arg)->controlLoop();
}

void MuseApp::inputTaskEntry(void *arg)
{
    static_cast<MuseApp *>(arg)->inputAudio();
}

void MuseApp::playbackTaskEntry(void *arg)
{
    static_cast<MuseApp *>(arg)->playAudio();
}

void MuseApp::callTaskEntry(void *arg)
{
    static_cast<MuseApp *>(arg)->callAudio();
}

void MuseApp::refreshTaskEntry(void *arg)
{
    auto *self = static_cast<MuseApp *>(arg);
    muse_vm_refresh_result_t result = MUSE_VM_REFRESH_FAILED;
    if (self->_client) {
        result = self->_client->refreshTokens();
    }
    ControlEvent event = {};
    event.type = result == MUSE_VM_REFRESH_OK ? ControlEventType::TokenRefreshed :
                 result == MUSE_VM_REFRESH_REVOKED ? ControlEventType::TokenRevoked :
                 ControlEventType::TokenRefreshFailed;
    self->postControlEvent(event);
    self->_refresh_task = nullptr;
    self->_refresh_running.store(false);
    vTaskDelete(nullptr);
}

void MuseApp::ipEventHandler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    auto *instance = static_cast<MuseApp *>(arg);
    ScopedOptionalCounter async_post_user(&instance->_async_post_users);
    if (instance->_shutdown.load()) {
        return;
    }
    instance->updateNetworkState();
}

void MuseApp::uiTimerCallback(lv_timer_t *timer)
{
    auto *instance = static_cast<MuseApp *>(lv_timer_get_user_data(timer));
    instance->refreshUi();
}

void MuseApp::uiActionCallback(void *user_data)
{
    auto *instance = static_cast<MuseApp *>(user_data);
    if (instance->_state.load() == State::NetworkRequired) {
        instance->openWlanSettings();
        return;
    }
    ControlEvent event = {};
    event.type = ControlEventType::ActionPressed;
    instance->postControlEvent(event);
}

void MuseApp::uiHoldCallback(void *user_data)
{
    auto *instance = static_cast<MuseApp *>(user_data);
    ControlEvent event = {};
    event.type = ControlEventType::HoldPressed;
    instance->postControlEvent(event);
}

void MuseApp::audioPreemptCallback(audio_client_handle_t client, void *user_ctx)
{
    (void)client;
    auto *instance = static_cast<MuseApp *>(user_ctx);
    instance->_capture_enabled.store(false);
    instance->_accept_playback.store(false);
    instance->_playback_enabled.store(false);
    // Never block here: the audio service calls this from another task's
    // context, so the event is posted with a zero timeout and dropped if the
    // queue is full (the flags above already stopped the audio).
    if (instance->_control_queue && !instance->_shutdown.load()) {
        ControlEvent event = {};
        event.type = ControlEventType::AudioPreempted;
        (void)xQueueSend(instance->_control_queue, &event, 0);
    }
}

} // namespace esp_brookesia::apps
