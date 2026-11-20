/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "noise_control.h"

namespace esp_brookesia::apps {

/**
 * Muse Link session (Phase 4.3): the Noise-over-WebSocket connection to
 * the user's Muse VM, vendored from the SDK (muse_link component:
 * noise_control.cpp, image_fetch.c and friends).
 *
 * Lifecycle follows app visibility — the user's decision (2026-11-20):
 * MuseApp starts Link when visible and stops it on hide. There is no
 * second always-on connection and Link never starts in MuseApp::init().
 *
 * The command set mirrors the SDK's app.c on_ws_command for the commands
 * this board supports: display.draw_url, display.draw_file,
 * display.show_animation, voice.configure and wakeword.disable. The VM
 * commands (device.list_vms / set_vm / reset_vm / unpair) are handled
 * locally by the settings page instead, and discover / sensors / camera /
 * ble.* / app.* / bug.report answer "unsupported" — the board has no
 * tunnel, sensors, camera or BLE command server.
 *
 * Threading: the status and command callbacks run on the noise_ctrl task
 * and must never block — the callbacks below only post control events
 * (non-blocking). Credential resolution (find_vm, blocking HTTPS) runs on
 * a worker task with an internal-RAM stack, like the token refresh task.
 * disconnect() blocks until the session task exits and runs on the
 * control task only.
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */
class MuseLink {
public:
    struct Callbacks {
        // Server sent node.unpaired: the device was unpaired remotely.
        // Runs on the noise_ctrl task; post a control event, don't block.
        std::function<void()> onUnpaired;
        // VM credentials rejected or a refresh needed (ws_auth_failed /
        // ws_refresh_needed). Runs on the noise_ctrl task; post a control
        // event, don't block.
        std::function<void()> onAuthFailed;
    };

    explicit MuseLink(const Callbacks &callbacks);
    ~MuseLink();

    // One-time init: noise_ctrl_init (node id + display name from the
    // provisioning store) and the command callback. Idempotent; safe from
    // the control task.
    bool init();

    // Start the session: resolves the VM credentials on a worker task
    // (blocking HTTPS via muse_hatch_api_find_vm), then
    // noise_ctrl_connect. No-op while a worker is already resolving or
    // the session is already running. Safe from the control task.
    void connect();

    // Stop the session: noise_ctrl_disconnect, which blocks until the
    // session task exits. No-op when not running. Control task only —
    // never call from the noise_ctrl task (it would wait for itself).
    void disconnect();

    // Re-resolve credentials and reconnect (after a token refresh, which
    // may invalidate the minted VM token). Control task only.
    void restart();

    bool isConnected() const { return _connected.load(); }
    bool isRunning() const;
    // True while the connect worker is resolving credentials. deinit
    // waits for it before deleting this, so the worker never runs on a
    // deleted MuseLink.
    bool workerRunning() const { return _worker_running.load(); }

private:
    static constexpr int CONNECT_TASK_STACK_SIZE = 8 * 1024;

    static void statusCallback(const char *status);
    static cJSON *commandCallback(
        const char *command, cJSON *params, const char *request_id,
        noise_ctrl_session_generation_t session_generation);
    static void connectTaskEntry(void *arg);
    void connectTask();

    Callbacks _callbacks;
    std::atomic<bool> _inited{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _worker_running{false};
    // Set by disconnect(), cleared by connect(). The worker checks it
    // under _mutex just before noise_ctrl_connect, so a session can never
    // start after a disconnect: either the worker connects first and the
    // disconnect stops it, or the disconnect sets this first and the
    // worker skips the connect.
    std::atomic<bool> _stopped{false};
    SemaphoreHandle_t _mutex = nullptr;
    TaskHandle_t _worker_task = nullptr;

    // The C callbacks have no context pointer; one MuseApp means one
    // MuseLink, so a static instance routes them (same pattern as
    // MuseApp::_instance).
    static MuseLink *_instance;
};

} // namespace esp_brookesia::apps
