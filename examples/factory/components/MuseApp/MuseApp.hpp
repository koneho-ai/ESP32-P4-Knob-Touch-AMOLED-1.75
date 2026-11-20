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
#include "MuseLink.hpp"
#include "MuseSettingsUi.hpp"
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
 * half-duplex, Muse's speech played live; tap hangs up. "Hey Muse" starts
 * a hands-free turn while the app is visible (Phase 4.2, opt-in). Link
 * (Phase 4.3) holds a second Noise session to the VM for the agent's
 * commands — display.draw_url/draw_file, display.show_animation,
 * voice.configure, wakeword.disable — and follows the same visibility:
 * it starts with the voice session when the app is visible and stops on
 * hide; there is no always-on connection. A gear in the top right opens
 * the settings page (Phase 4.4): VM picker, token status, the wake-word
 * switch and sign-out, built with the factory Settings app's round_ui
 * kit; the knob scrolls it while it is open, and back returns to the
 * main view before it closes the app.
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
    // The running instance, if any — unlike requestInstance, this never
    // creates one (the Link display shim calls it from the download task).
    static MuseApp *instance()
    {
        return _instance;
    }
    // The UI, if the app has been inited — null before init. The view
    // itself may still be down (see MuseUi::imageSize).
    MuseUi *ui()
    {
        return _ui;
    }
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
        WakeHeard,      // the wake word was heard; show a prompt
        WakeTurnStart,  // value: disable generation; open a hands-free turn
        WakeTurnEnd,    // flag: submit (true) or drop (false) the hands-free turn
        WakeCancelled,  // an armed wake was cancelled; clear the prompt
        LinkUnpaired,   // server sent node.unpaired: unpair like TokenRevoked
        LinkAuthFailed, // Link needs fresh VM credentials: refresh the token
        VmPicked,       // text: the VM id from the picker ("" = the account's default)
        TestConnectionRequested, // the settings page's Test connection button
        SignOutRequested, // the settings page's Sign out, confirmed: unpair like TokenRevoked
        Shutdown,
    };

    struct ControlEvent {
        ControlEventType type = ControlEventType::NetworkChanged;
        char text[512] = {};
        char detail[128] = {};
        bool flag = false;
        uint32_t value = 0;  // BleConfirmRequired: session generation;
                             // BleStateChanged: muse_ble_pairing_state_t;
                             // WakeTurnStart: disable generation
    };

    static constexpr int CONTROL_TASK_STACK_SIZE = 12 * 1024;
    static constexpr int INPUT_TASK_STACK_SIZE = 8 * 1024;
    static constexpr int PLAYBACK_TASK_STACK_SIZE = 16 * 1024;
    static constexpr int CALL_TASK_STACK_SIZE = 16 * 1024;
    static constexpr int REFRESH_TASK_STACK_SIZE = 8 * 1024;
    static constexpr int VM_FETCH_TASK_STACK_SIZE = 8 * 1024;

    static MuseApp *_instance;

    lv_obj_t *_page_root = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    MuseUi *_ui = nullptr;
    MuseClient *_client = nullptr;
    MuseLink *_link = nullptr;
    MuseSettingsUi *_settings = nullptr;

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
    // Wake word (Phase 4.2): the wake task (muse_wakeword_glue) owns the
    // core and runner; the control task owns these flags. _wake_listening
    // makes the input task keep reading the mic while visible even when
    // no turn is recording, so the runner hears. _wake_turn_active marks
    // a turn the wake word opened (its end comes from the core, not a
    // tap). _wake_seed_gen is the pre-roll seed's generation: the input
    // task only feeds a seed whose generation matches the open turn.
    std::atomic<bool> _wake_inited{false};
    std::atomic<bool> _wake_init_failed{false};
    std::atomic<bool> _wake_listening{false};
    std::atomic<bool> _wake_turn_active{false};
    std::atomic<uint32_t> _wake_seed_gen{0};
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

    // VM picker (Phase 4.4): the fetch worker's snapshot, under
    // _state_mutex. _vm_list_dirty is set by the worker and cleared by
    // refreshUi when it hands the list to the picker.
    MuseSettingsUi::VmEntry _vm_list[MuseSettingsUi::kMaxVms] = {};
    int _vm_list_count = 0;
    bool _vm_list_error = false;
    bool _vm_list_dirty = false;
    char _vm_list_preferred[128] = {};
    std::atomic<bool> _vm_fetch_running{false};

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
    // Non-blocking variant for the call task and the wake task: a
    // realtime audio task must never stall on a full queue (captions/views
    // are best-effort; the CallEnded post still uses postControlEvent).
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
    void startTurn(bool hands_free = false);
    void endTurn();
    void cancelTurn();
    void closeTurn();

    // ---- Wake word (Phase 4.2) ----
    // initWakeWord runs once, on first AppVisible (the glue creates the
    // runner, buffers and wake task; it lives for the app's lifetime).
    // startWakeListening acquires audio and activates the runner when the
    // opt-in is on; stopWakeListening deactivates it on hide.
    // feedWakeSeedIfPending runs on the input task: after beginTurn() it
    // feeds the pre-roll seed plus the backlog chunks into the session,
    // so a hands-free note starts up to 1 s before the speech.
    void initWakeWord();
    void startWakeListening();
    void stopWakeListening();
    void feedWakeSeedIfPending();

    // ---- Link session (Phase 4.3) ----
    // initLink creates the MuseLink object once, in init() (cheap — just
    // callbacks and flags). startLink inits the Noise session lazily on
    // first AppVisible and connects it; it only runs while visible,
    // provisioned and online. stopLink disconnects on hide (blocks until
    // the session task exits). restartLink re-resolves the VM credentials
    // after a token refresh, which may invalidate the minted VM token.
    void initLink();
    void startLink();
    void stopLink();
    void restartLink();

    // ---- Settings page (Phase 4.4) ----
    // showSettings/hideSettings run on the LVGL task (from the gear tap,
    // the back pill, and back()): they swap the main view and the
    // settings page, both children of _page_root. refreshSettings runs
    // from refreshUi on the same task. The VM list is fetched on a
    // worker task (fetch_vms is blocking HTTPS): it fills _vm_list under
    // _state_mutex and sets _vm_list_dirty; refreshUi hands the snapshot
    // to the picker. The worker never touches LVGL objects.
    void showSettings();
    void hideSettings();
    void refreshSettings();
    void openVmPicker();
    void fetchVmList();
    static void vmFetchTaskEntry(void *arg);

    // ---- Realtime voice calls (Phase 4.1) ----    // startCall runs on the control task; callAudio is the dedicated call
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
    static void uiSettingsCallback(void *context);
    static void audioPreemptCallback(audio_client_handle_t client, void *user_ctx);
    static void knobCallback(int steps, void *user_ctx);
    static MuseUi::Mode uiMode(State state);
    const char *hintText(State state);
    const char *offlineHint();
    void openWlanSettings();
};

} // namespace esp_brookesia::apps
