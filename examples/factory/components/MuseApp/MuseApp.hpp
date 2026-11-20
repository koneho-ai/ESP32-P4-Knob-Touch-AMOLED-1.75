/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_ae_rate_cvt.h"
#include "esp_event.h"
#include "audio_service.h"
#include "muse_ble_pairing.h"
#include "MuseClient.hpp"
#include "MuseUi.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * Muse voice-assistant app for the P4 knob board, modeled on XiaozhiApp.
 *
 * Turn-based push-to-talk over the vendored Muse chat session (muse_session):
 * tap to talk, tap again to send; the reply shows as text at reading pace
 * (v1 has no spoken replies — see MuseClient). A long press anywhere but
 * the avatar starts a realtime voice call instead (Phase 4.1): hands-free,
 * half-duplex, Muse's speech played live; tap hangs up. No wake word yet.
 *
 * Lifecycle: the session starts lazily on the first AppVisible (the app
 * costs nothing until opened), disconnects on AppHidden, and releases the
 * codec; returning re-prepares. Only 3 apps run at once, so a 4th launch
 * closes us — the session dies cleanly via AppHidden.
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */
class MuseApp: public systems::phone::App {
public:
    static MuseApp *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~MuseApp();

protected:
    MuseApp(bool use_status_bar, bool use_navigation_bar);

    bool run() override;
    bool back() override;
    bool close() override;
    bool init() override;
    bool deinit() override;
    bool pause() override;
    bool resume() override;

private:
    enum class State : uint8_t {
        NotProvisioned, // No device tokens: pair in the Muse app
        NetworkRequired,
        Preparing,
        Connecting,
        Ready,
        Listening,
        Thinking,
        Speaking,
        Error,
        CallConnecting, // A call is setting up, before READY
    };

    // The call task's view of a live call (SDK run_call's call_view):
    // Working is Thinking with the avatar's working variant.
    enum class CallView : uint8_t {
        Connecting,
        Listening,
        Thinking,
        Speaking,
        Working,
        Ending,         // Hanging up; the state stays until CallEnded
    };

    enum class ControlEventType : uint8_t {
        AppVisible,
        AppHidden,
        NetworkChanged,
        ActionPressed,
        HoldPressed,    // Long press outside the avatar: start/hang up a call
        TurnHeard,
        TurnReply,
        TurnDone,
        TurnError,
        TurnSent,
        PlaybackDrained,
        AudioPreempted,
        AudioError,
        TokenRefreshed,
        TokenRevoked,
        TokenRefreshFailed,
        BleConfirmRequired,
        BleStateChanged,
        CallViewChanged, // value: CallView
        CallHeard,      // text: what Muse heard in the call
        CallReply,      // text: the last words of Muse's reply in the call
        CallEnded,      // text: why the call ended
        Shutdown,
    };

    struct ControlEvent {
        ControlEventType type = ControlEventType::NetworkChanged;
        char text[512] = {};
        char detail[128] = {};
        bool flag = false;
        uint32_t value = 0;  // BleConfirmRequired: session generation;
                             // BleStateChanged: muse_ble_pairing_state_t
    };

    static constexpr int CONTROL_TASK_STACK_SIZE = 12 * 1024;
    static constexpr int INPUT_TASK_STACK_SIZE = 8 * 1024;
    static constexpr int PLAYBACK_TASK_STACK_SIZE = 16 * 1024;
    static constexpr int CALL_TASK_STACK_SIZE = 16 * 1024;
    static constexpr int REFRESH_TASK_STACK_SIZE = 8 * 1024;

    static MuseApp *_instance;

    lv_obj_t *_page_root = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    MuseUi *_ui = nullptr;
    MuseClient *_client = nullptr;

    SemaphoreHandle_t _state_mutex = nullptr;
    SemaphoreHandle_t _lifecycle_mutex = nullptr;
    SemaphoreHandle_t _control_exited = nullptr;
    QueueHandle_t _control_queue = nullptr;

    TaskHandle_t _control_task = nullptr;
    TaskHandle_t _input_task = nullptr;
    TaskHandle_t _playback_task = nullptr;
    TaskHandle_t _call_task = nullptr;
    TaskHandle_t _refresh_task = nullptr;

    esp_event_handler_instance_t _ip_event_instance = nullptr;

    std::atomic<State> _state{State::Preparing};
    std::atomic<bool> _network_ready{false};
    std::atomic<bool> _visible{false};
    std::atomic<bool> _shutdown{false};
    std::atomic<bool> _destroying{false};
    std::atomic<bool> _deinit_in_progress{false};
    std::atomic<bool> _deinit_complete{false};
    std::atomic<uint32_t> _lifecycle_refs{0};
    std::atomic<uint32_t> _async_post_users{0};
    std::atomic<bool> _control_running{false};
    std::atomic<bool> _audio_tasks_running{false};
    std::atomic<bool> _input_task_running{false};
    std::atomic<bool> _playback_task_running{false};
    std::atomic<bool> _capture_enabled{false};
    std::atomic<bool> _playback_enabled{false};
    std::atomic<bool> _accept_playback{false};
    std::atomic<bool> _turn_done{false};
    std::atomic<bool> _drain_posted{false};
    // Realtime call (Phase 4.1): the call task owns the call API (start /
    // audio / read / event must all run on one task). The control task only
    // sets _call_end_requested; the call task hangs up itself and posts
    // CallEnded. _in_call distinguishes call Listening/Thinking/Speaking
    // from turn Listening/Thinking/Speaking (tap hangs up in a call).
    std::atomic<bool> _in_call{false};
    std::atomic<bool> _call_working{false};
    std::atomic<bool> _call_end_requested{false};
    std::atomic<bool> _call_task_running{false};
    std::atomic<bool> _session_started{false};
    std::atomic<bool> _provisioned{false};
    std::atomic<bool> _refresh_running{false};
    std::atomic<bool> _refresh_attempted{false};
    std::atomic<bool> _token_error{false};
    // BLE pairing (MuseBlePairing): advertising runs only while the app is
    // visible and unprovisioned; the confirm prompt is armed from the
    // control task once its message is set (see handleBleConfirmRequired).
    std::atomic<bool> _ble_inited{false};
    std::atomic<bool> _ble_advertising{false};
    std::atomic<bool> _confirm_pending{false};
    std::atomic<uint32_t> _confirm_generation{0};
    std::atomic<muse_ble_pairing_state_t> _ble_state{MUSE_BLE_PAIRING_STATE_IDLE};
    std::atomic<bool> _toast_pending{false};
    std::atomic<uint32_t> _input_pcm_peak{0};
    std::atomic<uint32_t> _output_pcm_peak{0};
    std::atomic<uint32_t> _played_frames{0};
    std::atomic<float> _progress{0};    // Reply progress 0..1 (control task writes, LVGL task reads)
    TickType_t _listen_started_tick = 0;   // When the current listen began (ring fills over 15 s)
    uint32_t _offline_since = 0;    // When "No network" first showed, 0 while online
    bool _wlan_offered = false;     // Settings opened by itself in this offline spell
    char _hint[64] = {};
    TickType_t _turn_done_tick = 0;
    TickType_t _next_retry_tick = 0;

    audio_client_handle_t _audio_client = nullptr;
    esp_ae_rate_cvt_handle_t _input_resampler = nullptr;
    esp_ae_rate_cvt_handle_t _output_resampler = nullptr;
    uint32_t _input_resampler_max_samples = 0;
    uint32_t _output_resampler_max_samples = 0;

    char _chat_role[16] = {};
    char _chat_text[512] = {};
    char _error_detail[128] = {};
    char _toast[64] = {};  // Applied by refreshUi (LVGL task); see showToastSoon

    void createUi();
    void destroyUi();
    void refreshUi();
    void setState(State state);
    void setError(const char *detail);
    void setChatMessage(const char *role, const char *text);
    void clearConversationUi();
    // Queues a toast for refreshUi: MuseUi::showToast touches LVGL objects
    // and must only run on the LVGL task, never from the control task.
    void showToastSoon(const char *text);
    bool canPrepare() const;

    bool isNetworkReady() const;
    void updateNetworkState();

    bool postControlEvent(const ControlEvent &event);
    // Non-blocking variant for the call task: a realtime audio task must
    // never stall on a full queue (captions/views are best-effort; the
    // CallEnded post still uses postControlEvent).
    bool postControlEventNowait(const ControlEvent &event);
    void controlLoop();
    void controlTick(uint32_t tick);
    void pollSessionStatus();
    void pollProvisioning();
    void pollIncoming();
    void handleControlEvent(const ControlEvent &event);
    void handleAppVisible();
    void handleAppHidden(bool release_audio);
    void postAppHidden();
    void startTokenRefresh();

    // ---- BLE pairing (MuseBlePairing; see its INTEGRATION.md) ----
    // initBlePairing runs once, on first AppVisible after the session
    // started (identity needs muse_vm_init). Advertising runs only while
    // visible and unprovisioned. The provision/scan/device-info/unpair
    // handlers run on the BLE component's worker tasks (internal-RAM
    // stacks, so NVS writes are safe there); the confirm/state callbacks
    // only post control events.
    void initBlePairing();
    void startBleAdvertising();
    void stopBleAdvertising();
    void handleBleConfirmRequired(uint32_t generation);
    void handleBleStateChanged(muse_ble_pairing_state_t state);
    void handleBleProvision(const muse_provision_payload_t *payload);
    void handleBleWifiScan();
    void handleBleUnpair();
    void handleBleGetDeviceInfo();
    static void bleProvisionCallback(const muse_provision_payload_t *payload);
    static void bleWifiScanCallback();
    static void bleUnpairCallback();
    static void bleGetDeviceInfoCallback();
    static void bleConfirmRequiredCallback(uint32_t generation);
    static void bleStateChangedCallback(muse_ble_pairing_state_t state);

    bool ensureAudioReady();
    void shutdownAudio();
    void startTurn();
    void endTurn();
    void cancelTurn();
    void closeTurn();

    // ---- Realtime voice calls (Phase 4.1) ----
    // startCall runs on the control task; callAudio is the dedicated call
    // task (the SDK's run_call): one 10 ms pass reads the mic, drains call
    // events, feeds gated mic audio, reads reply audio and writes it. It
    // uses the same audio client and resamplers as the turn tasks, which
    // idle during a call (_capture_enabled/_playback_enabled are false),
    // so the resamplers are never shared. requestCallEnd is safe from any
    // task; shutdownAudio requests it and waits for the call task too.
    void startCall();
    void requestCallEnd();
    void callAudio();
    void handleCallView(CallView view);

    void inputAudio();
    void playAudio();

    static const char *stateText(State state);
    static bool tickReached(TickType_t due_tick, uint32_t timeout_ms);
    static void controlTaskEntry(void *arg);
    static void inputTaskEntry(void *arg);
    static void playbackTaskEntry(void *arg);
    static void callTaskEntry(void *arg);
    static void refreshTaskEntry(void *arg);
    static void ipEventHandler(
        void *arg,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data
    );
    static void uiTimerCallback(lv_timer_t *timer);
    static void uiActionCallback(void *context);
    static void uiHoldCallback(void *context);
    static void audioPreemptCallback(audio_client_handle_t client, void *user_ctx);
    static void knobCallback(int steps, void *user_ctx);
    static MuseUi::Mode uiMode(State state);
    const char *hintText(State state);
    const char *offlineHint();
    void openWlanSettings();
};

} // namespace esp_brookesia::apps
