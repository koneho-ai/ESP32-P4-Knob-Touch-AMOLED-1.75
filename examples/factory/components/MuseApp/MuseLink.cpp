/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// Muse Link session (Phase 4.3). See MuseLink.hpp. The command dispatch
// mirrors the SDK's app.c on_ws_command for the commands this board
// supports; the status handling mirrors its on_ws_control_status (minus
// the tunnel, BLE and UI pieces the factory doesn't have).

#include "MuseLink.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_service.h"

extern "C" {
#include "cJSON.h"
#include "led_status.h"
}
#include "image_fetch.h"
/* The account API header has no extern "C" guard (as vendored); the SDK's
 * own C++ consumer wraps the include the same way (muse_chat_session.cpp). */
extern "C" {
#include "muse_account_api.h"
}
#include "muse_credentials.h"
#include "muse_provisioning.h"
#include "muse_settings.h"
#include "muse_wakeword_glue.h"
#include "noise_control.h"
#include "audio_service.h"

namespace esp_brookesia::apps {

namespace {

const char *TAG = "muse.link";

// ---- Command results (SDK app.c command_error, verbatim shape) ----

cJSON *command_error(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(result, "error", error);
    return result;
}

// ---- Image download (SDK app.c draw_url_ctx_t / draw_url_done) ----

struct DrawUrlCtx {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
};

void draw_url_done(const image_fetch_result_t *r, void *user)
{
    DrawUrlCtx *ctx = static_cast<DrawUrlCtx *>(user);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", r->ok);
    if (r->ok) {
        cJSON *payload = cJSON_AddObjectToObject(result, "payload");
        cJSON_AddStringToObject(payload, "format", r->format);
        cJSON_AddNumberToObject(payload, "width", r->width);
        cJSON_AddNumberToObject(payload, "height", r->height);
        cJSON_AddNumberToObject(payload, "scale", r->scale);
        cJSON_AddNumberToObject(payload, "bytes", (double)r->bytes);
        cJSON_AddNumberToObject(payload, "ms", r->ms);
    } else {
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        cJSON_AddStringToObject(error, "code", r->code);
        cJSON_AddStringToObject(error, "message", r->message);
    }
    noise_ctrl_send_command_result(ctx->session_generation, ctx->request_id, result);
    free(ctx);
}

// ---- voice.configure (SDK main/voice.c voice_configure_command) ----
//
// The factory's audio service is the one volume of the device (0-100,
// saved in NVS, applied immediately), so store_volume + apply_volume
// collapse into audio_service_set_volume; the current volume reads back
// from audio_service_get_volume.

cJSON *voice_configure_command(cJSON *params)
{
    cJSON *volume = cJSON_GetObjectItem(params, "volume");
    const char *why = nullptr;
    if (volume) {
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) {
            why = "volume must be 0-100";
        } else if (audio_service_set_volume(volume->valueint) != ESP_OK) {
            why = "volume could not be stored";
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (why) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "invalid_params");
        cJSON_AddStringToObject(error, "message", why);
        cJSON_AddItemToObject(result, "error", error);
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "volume", audio_service_get_volume());
    return result;
}

// ---- wakeword.disable (SDK app.c wakeword_disable_command) ----
//
// Only disables: the glue's set_enabled(false) saves the opt-out to NVS
// even when the wake task never started, and status() reports the
// persisted state. Only someone at the device can turn it back on.

cJSON *wakeword_disable_command()
{
    muse_wakeword_set_enabled(false);
    MuseWakewordStatus st{};
    muse_wakeword_status(&st);
    cJSON *result = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    if (!result || !payload) {
        cJSON_Delete(result);
        cJSON_Delete(payload);
        return command_error("out_of_memory", "failed to allocate wake word result");
    }
    cJSON_AddBoolToObject(payload, "enabled", st.enabled);
    cJSON_AddBoolToObject(payload, "listening", st.listening);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload);
    return result;
}

} // namespace

MuseLink *MuseLink::_instance = nullptr;

MuseLink::MuseLink(const Callbacks &callbacks) : _callbacks(callbacks)
{
    _mutex = xSemaphoreCreateMutex();
    _instance = this;
}

MuseLink::~MuseLink()
{
    disconnect();
    if (_instance == this) {
        _instance = nullptr;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

bool MuseLink::init()
{
    if (_inited.load()) {
        return true;
    }
    // The node id and BLE name come from the provisioning store (the
    // stored id once paired, else the setup id). noise_ctrl_init copies
    // both into its own static buffers.
    noise_ctrl_init(
        muse_provisioning_node_id(),
        muse_provisioning_ble_name(),
        statusCallback
    );
    noise_ctrl_set_command_cb(commandCallback);
    _inited.store(true);
    return true;
}

bool MuseLink::isRunning() const
{
    return _inited.load() && noise_ctrl_is_running();
}

void MuseLink::connect()
{
    if (!_inited.load() || _worker_running.load() || noise_ctrl_is_running()) {
        return;
    }
    _stopped.store(false);
    _worker_running.store(true);
    // Internal-RAM stack, like the token refresh task (blocking HTTPS;
    // a flash write asserts on a PSRAM stack).
    if (xTaskCreatePinnedToCore(
            connectTaskEntry, "muse_link", CONNECT_TASK_STACK_SIZE,
            this, 4, &_worker_task, 0) != pdPASS) {
        _worker_running.store(false);
        _worker_task = nullptr;
        ESP_LOGW(TAG, "link connect worker unavailable");
    }
}

void MuseLink::disconnect()
{
    if (!_inited.load()) {
        return;
    }
    // Stop first, under the mutex, so a worker resolving right now sees
    // it before it connects (see _stopped). The disconnect itself runs
    // outside the mutex: it blocks until the session task exits.
    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
    }
    _stopped.store(true);
    bool running = noise_ctrl_is_running();
    if (_mutex) {
        xSemaphoreGive(_mutex);
    }
    if (!running) {
        _connected.store(false);
        return;
    }
    // Blocks until the session task exits. Control task only: the
    // noise_ctrl task must never wait for itself.
    noise_ctrl_disconnect();
    _connected.store(false);
}

void MuseLink::restart()
{
    disconnect();
    connect();
}

void MuseLink::statusCallback(const char *status)
{
    if (!_instance) {
        return;
    }
    // Runs on the noise_ctrl task: set the flag directly, and only post
    // control events (non-blocking) for the states the app must act on.
    // Never disconnect inline — noise_ctrl_disconnect() would wait for
    // this very task to exit.
    if (strcmp(status, "ws_connected") == 0) {
        _instance->_connected.store(true);
        ESP_LOGI(TAG, "control WebSocket connected");
    } else if (strcmp(status, "ws_disconnected") == 0) {
        _instance->_connected.store(false);
        ESP_LOGI(TAG, "control WebSocket disconnected");
    } else if (strcmp(status, "ws_unpaired") == 0) {
        _instance->_connected.store(false);
        ESP_LOGW(TAG, "server sent node.unpaired");
        if (_instance->_callbacks.onUnpaired) {
            _instance->_callbacks.onUnpaired();
        }
    } else if (strcmp(status, "ws_auth_failed") == 0 ||
               strcmp(status, "ws_refresh_needed") == 0) {
        _instance->_connected.store(false);
        ESP_LOGW(TAG, "control WebSocket needs fresh VM credentials: %s", status);
        if (_instance->_callbacks.onAuthFailed) {
            _instance->_callbacks.onAuthFailed();
        }
    }
}

cJSON *MuseLink::commandCallback(
    const char *command, cJSON *params, const char *request_id,
    noise_ctrl_session_generation_t session_generation)
{
    // Runs on the noise_ctrl task: synchronous commands answer inline;
    // the image downloads answer later via draw_url_done ({"_async":true}).
    bool draw_file = strcmp(command, "display.draw_file") == 0;
    if (draw_file || strcmp(command, "display.draw_url") == 0) {
        cJSON *target = cJSON_GetObjectItem(params, draw_file ? "path" : "url");
        cJSON *row = cJSON_GetObjectItem(params, "row");
        if (!cJSON_IsString(target) || !target->valuestring || !target->valuestring[0]) {
            return command_error("missing_param",
                                 draw_file ? "path is required" : "url is required");
        }
        DrawUrlCtx *ctx = static_cast<DrawUrlCtx *>(calloc(1, sizeof(*ctx)));
        if (!ctx) {
            return command_error("out_of_memory", "failed to allocate");
        }
        ctx->session_generation = session_generation;
        strncpy(ctx->request_id, request_id, sizeof(ctx->request_id) - 1);
        const char *code = nullptr;
        const char *message = nullptr;
        int top = cJSON_IsNumber(row) ? row->valueint : IMAGE_FETCH_DEFAULT_ROW;
        bool started = draw_file
            ? image_fetch_start_vm(target->valuestring, top, draw_url_done, ctx, &code, &message)
            : image_fetch_start(target->valuestring, top, draw_url_done, ctx, &code, &message);
        if (!started) {
            free(ctx);
            return command_error(code, message);
        }
        cJSON *async = cJSON_CreateObject();
        cJSON_AddBoolToObject(async, "_async", true);
        return async;
    }
    if (strcmp(command, "display.show_animation") == 0) {
        led_status_show_animation();
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        return result;
    }
    if (strcmp(command, "voice.configure") == 0) {
        return voice_configure_command(params);
    }
    if (strcmp(command, "wakeword.disable") == 0) {
        return wakeword_disable_command();
    }

    // Not implemented via Link: device.list_vms / set_vm / reset_vm /
    // unpair are handled locally by the settings page; device.discover,
    // sensors.read, camera.capture, ble.*, app.* and bug.report have no
    // hardware behind them on this board.
    char msg[128];
    snprintf(msg, sizeof(msg), "unsupported command: %s", command);
    return command_error("unsupported", msg);
}

void MuseLink::connectTaskEntry(void *arg)
{
    static_cast<MuseLink *>(arg)->connectTask();
}

void MuseLink::connectTask()
{
    // Blocking HTTPS: resolve the VM credentials ourselves (one fetch_vms
    // call, which returns the VM's auth token) rather than deviating from
    // the verbatim session file to share its resolution.
    char device_token[MUSE_TOKEN_MAX + 1] = {};
    char want_vm[MUSE_VM_MAX + 1] = {};
    char host[MUSE_HOST_MAX + 1] = {};
    muse_vm_cached_token(device_token, sizeof(device_token));
    muse_vm_cached_vm(want_vm, sizeof(want_vm));
    muse_vm_cached_host(host, sizeof(host));

    if (!device_token[0]) {
        ESP_LOGW(TAG, "no device token; Link needs pairing first");
    } else {
        muse_hatch_vm_t vm{};
        int rc = muse_hatch_api_find_vm(device_token, want_vm, &vm);
        if (rc == 0) {
            // Check-then-connect under the mutex: a disconnect between
            // the resolution and here must win (see _stopped).
            if (_mutex) {
                xSemaphoreTake(_mutex, portMAX_DELAY);
            }
            bool stopped = _stopped.load();
            if (!stopped) {
                noise_ctrl_set_host(host[0] ? host : nullptr);
                wifi_service_info_t wifi_info{};
                wifi_service_get_info(&wifi_info);
                if (!noise_ctrl_connect(vm.vm_id, vm.vm_token, wifi_info.ssid)) {
                    ESP_LOGW(TAG, "noise_ctrl_connect failed for VM %s", vm.vm_id);
                }
            }
            if (_mutex) {
                xSemaphoreGive(_mutex);
            }
            free(vm.vm_token);
        } else if (rc == MUSE_HATCH_API_AUTH) {
            ESP_LOGW(TAG, "device token rejected while resolving the VM");
            if (!_stopped.load() && _callbacks.onAuthFailed) {
                _callbacks.onAuthFailed();
            }
        } else {
            ESP_LOGW(TAG, "VM resolution failed; Link retries on next show");
        }
    }

    _worker_task = nullptr;
    _worker_running.store(false);
    vTaskDelete(nullptr);
}

} // namespace esp_brookesia::apps
