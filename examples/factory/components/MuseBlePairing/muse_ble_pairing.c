/*
 * Muse BLE pairing — Bluedroid GATT server port of the SDK's NimBLE server.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UNTESTED ON HARDWARE — no P4 knob board exists yet. Compile verification
 * only. See INTEGRATION.md for everything that could not be verified.
 *
 * Port of muse-gadget-sdk esp32/main/ble_server.c (commit 6a44074) from
 * NimBLE to Bluedroid. The wire behavior is kept identical: same service
 * UUIDs/characteristics, same advertising bytes, same 0xFE chunk framing,
 * same pairing-v5 record protocol (vendored/link_pairing.c), same JSON
 * actions, same status strings, same timeouts. The Muse app should not be
 * able to tell this apart from a NimBLE gadget.
 *
 * Deliberate deviations from the SDK server (all documented in
 * INTEGRATION.md):
 *  1. Command dispatch runs on a dedicated worker task fed by a queue,
 *     NOT on the Bluedroid BTC task. The SDK dispatches synchronously on
 *     the NimBLE host task, but here the BTC task is shared with Classic
 *     BT (A2DP/AVRCP) — blocking it for an ECDH key generation (~100 ms)
 *     would stall BT Music.
 *  2. No factory-test service, no companion (ble_basic) service, no OTA
 *     action: out of scope for this board (no OTA slots).
 *  3. Advertising + connection idle timeouts (configurable, defaults
 *     600 s / 180 s): the SDK advertises forever, but this board shares
 *     its radio and its single GATT slot with the rest of the device.
 *  4. "confirm_required" is sent by this component itself (synchronously
 *     in the dispatch task) before the on_confirm_required callback fires,
 *     so the encrypted record order the phone expects is guaranteed
 *     regardless of how fast the app reacts. The SDK sends it from the
 *     app's own callback.
 *  5. This component never initializes/enables Bluedroid and never stops
 *     it; bt_service owns the stack. There is no equivalent of
 *     ble_server_power_off / ble_server_full_shutdown.
 *
 * Bluedroid API note: this file uses the esp_gap_ble_api.h /
 * esp_gatts_api.h / esp_gatt_common_api.h surface as of ESP-IDF v6.x.
 * The v6.1 signatures were verified against the installed ESP-IDF v6.1.1
 * headers at integration time (adv params, config_adv_data_raw,
 * set_local_mtu, set_device_name).
 */

#include "muse_ble_pairing.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

#include "cJSON.h"
#include "link_pairing.h"

static const char *TAG = "muse.ble";

/* --- Pairing service UUIDs (SDK ble_server.c, little-endian wire order) --- */
/* Service 7fdd3d1c-38ea-46cf-8b46-314ecf5f240c */
static const uint8_t SVC_UUID128[16] = {
    0x0c, 0x24, 0x5f, 0xcf, 0x4e, 0x31, 0x46, 0x8b,
    0xcf, 0x46, 0xea, 0x38, 0x1c, 0x3d, 0xdd, 0x7f
};
/* RX 4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01 */
static const uint8_t RX_UUID128[16] = {
    0x01, 0x9b, 0x8f, 0x5e, 0x2d, 0x3c, 0xf0, 0xa1,
    0x6e, 0x4a, 0xa2, 0x28, 0x29, 0x30, 0x59, 0x4d
};
/* TX d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c */
static const uint8_t TX_UUID128[16] = {
    0x6c, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0a, 0x8f,
    0x9c, 0x4e, 0x2b, 0x7b, 0xca, 0xc4, 0x5d, 0xd7
};
#define CCCD_UUID16 0x2902

/* --- Framing (SDK ble_server.c, unchanged) --- */
#define MAX_NOTIFY_CHUNK   160
#define CHUNK_HEADER_BYTES 3
#define CHUNK_MAGIC        0xFE
#define MAX_RX_TOTAL_BYTES 8192

#define GATTS_APP_ID       0
#define SERVICE_NUM_HANDLES 8 /* service + 2 chars x (decl + value) + CCCD, +1 spare */
#define RX_ATTR_MAX_LEN    512 /* single ATT write; the app chunks at <=160 */
#define TX_ATTR_MAX_LEN    64  /* s_last_status */
#define DISPATCH_QUEUE_LEN 8
#define DISPATCH_TASK_STACK 8192
#define PROVISION_TASK_STACK 8192 /* SDK: mbedtls 3.6 TLS handshake needs ~8K */
#define WORKER_TASK_STACK  4096

#define DEFAULT_ADV_TIMEOUT_S 600
#define DEFAULT_IDLE_TIMEOUT_S 180

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    EVT_RX = 0,             /* data/len: one GATT write payload (malloc'd) */
    EVT_CONNECTED,
    EVT_DISCONNECTED,
    EVT_SUBSCRIBED,         /* generation field reused as 0/1 */
    EVT_ADV_TIMEOUT,
    EVT_IDLE_TIMEOUT,
    EVT_SESSION_RESET,      /* reset RX reassembly + pairing session */
    EVT_DISCONNECT_SESSION, /* generation: reset that session + stop adv */
    EVT_STATE_NOTIFY,       /* state: invoke on_state_changed */
    EVT_SHUTDOWN,
} dispatch_evt_type_t;

typedef struct {
    dispatch_evt_type_t type;
    uint8_t *data;
    size_t len;
    uint32_t generation;
    muse_ble_pairing_state_t state;
} dispatch_evt_t;

static muse_ble_pairing_config_t s_config;
static muse_ble_pairing_callbacks_t s_cb;

/* Guarded by s_mutex: lifecycle + policy flags + state. */
static SemaphoreHandle_t s_mutex;
static bool s_started;
static bool s_service_started;
static bool s_advertising_enabled;
static bool s_advertising_active;
static bool s_adv_config_pending;
static bool s_quiet;
static bool s_provisioned;
static muse_ble_pairing_state_t s_state = MUSE_BLE_PAIRING_STATE_IDLE;

/* Lock-free hot path (written on the BTC task, read on TX/worker tasks). */
static atomic_uint s_conn_id = ATOMIC_VAR_INIT(0xFFFF);
static atomic_bool s_subscribed = ATOMIC_VAR_INIT(false);
static atomic_uint s_mtu = ATOMIC_VAR_INIT(23);

static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_service_handle;
static uint16_t s_rx_handle;
static uint16_t s_tx_handle;
static uint16_t s_cccd_handle;
static uint8_t s_setup_step; /* 0 service create pending, 1 RX, 2 TX, 3 CCCD, 4 started */

static SemaphoreHandle_t s_tx_mutex;
static char s_last_status[64] = "idle";

/* RX reassembly — owned by the dispatch task only, no lock needed. */
static uint8_t *s_rx_buf;
static size_t s_rx_len;
static size_t s_rx_cap;
static uint8_t s_rx_total;
static uint8_t s_rx_count;
static uint8_t s_rx_next_idx;
static bool s_plaintext_status_blocked;

/* Prepare-write (long write) staging — BTC task only, GATTS events serialize. */
static uint8_t *s_prep_buf;
static size_t s_prep_len;

static QueueHandle_t s_dispatch_queue;
static TaskHandle_t s_dispatch_task;
static SemaphoreHandle_t s_shutdown_done; /* given by the dispatch task on EVT_SHUTDOWN */
static esp_timer_handle_t s_adv_timer;
static esp_timer_handle_t s_idle_timer;

static void kick_advertising(void);
static void set_state(muse_ble_pairing_state_t state);
static bool post_dispatch_evt(const dispatch_evt_t *evt);

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) {
        memcpy(out, s, n + 1);
    }
    return out;
}

static void secure_free_str(char *s)
{
    if (!s) {
        return;
    }
    mbedtls_platform_zeroize(s, strlen(s));
    free(s);
}

static void wipe_json_strings(cJSON *item)
{
    for (cJSON *cur = item; cur; cur = cur->next) {
        if (cur->valuestring) {
            mbedtls_platform_zeroize(cur->valuestring, strlen(cur->valuestring));
        }
        if (cur->child) {
            wipe_json_strings(cur->child);
        }
    }
}

static void delete_command_json(cJSON *root, bool decrypted)
{
    if (decrypted) {
        wipe_json_strings(root);
    }
    cJSON_Delete(root);
}

static bool post_dispatch_evt(const dispatch_evt_t *evt)
{
    if (!s_dispatch_queue) {
        return false;
    }
    /* Called from BTC task callbacks too: never block. */
    return xQueueSend(s_dispatch_queue, evt, 0) == pdPASS;
}

/* Record a state change and notify the app (via the dispatch task, so the
 * callback never runs on the BTC task or inside a mutex). */
static void set_state(muse_ble_pairing_state_t state)
{
    bool changed = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    if (s_state != state) {
        s_state = state;
        changed = true;
    }
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
    if (changed) {
        dispatch_evt_t evt = { .type = EVT_STATE_NOTIFY, .state = state };
        if (!post_dispatch_evt(&evt)) {
            ESP_LOGW(TAG, "state-notify event dropped (queue full)");
        }
    }
}

/* ------------------------------------------------------------------ */
/* TX path (SDK ble_server.c: notify/send_chunked/send_status)        */
/* ------------------------------------------------------------------ */

static bool notify_payload(const uint8_t *data, size_t len)
{
    if (!atomic_load(&s_subscribed)) {
        return false;
    }
    unsigned conn = atomic_load(&s_conn_id);
    if (conn == 0xFFFF || s_gatts_if == ESP_GATT_IF_NONE || !s_tx_handle) {
        return false;
    }
    esp_err_t err = esp_ble_gatts_send_indicate(s_gatts_if, (uint16_t)conn,
                                                s_tx_handle, (uint16_t)len,
                                                (uint8_t *)data, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notify failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool plaintext_status_allowed(const char *status)
{
    return strcmp(status, "error_encryption_required") == 0
           || strcmp(status, "error_pairing_invalid_hello") == 0
           || strcmp(status, "error_pairing_unavailable") == 0
           || strcmp(status, "error_pairing_decrypt") == 0;
}

/* Caller must hold s_tx_mutex. */
static bool send_chunked_locked(const char *data, uint32_t record_generation)
{
    if (!data) {
        return false;
    }
    size_t total_len = strlen(data);
    size_t notify_max = atomic_load(&s_mtu);
    if (notify_max <= 3) {
        return false;
    }
    notify_max -= 3;
    if (notify_max > MAX_NOTIFY_CHUNK) {
        notify_max = MAX_NOTIFY_CHUNK;
    }
    if (notify_max <= CHUNK_HEADER_BYTES) {
        return false;
    }
    size_t usable = notify_max - CHUNK_HEADER_BYTES;
    size_t total_chunks = (total_len + usable - 1) / usable;
    if (total_chunks == 0) {
        total_chunks = 1;
    }
    if (total_chunks > 255) {
        ESP_LOGW(TAG, "TX message exceeds BLE framing limit");
        return false;
    }

    ESP_LOGI(TAG, "TX chunked %d bytes -> %d chunks (mtu=%d, %d/chunk)",
             (int)total_len, (int)total_chunks, (int)atomic_load(&s_mtu), (int)usable);
    uint8_t buf[MAX_NOTIFY_CHUNK];
    for (size_t i = 0; i < total_chunks; i++) {
        /* A phase change must finish this record: its counter is already used.
         * Only a replacement encryption session makes its remaining chunks stale. */
        if (record_generation != 0
            && !link_pairing_record_session_is_current(record_generation)) {
            return false;
        }
        size_t off = i * usable;
        size_t flen = (off + usable > total_len) ? (total_len - off) : usable;
        buf[0] = CHUNK_MAGIC;
        buf[1] = (uint8_t)i;
        buf[2] = (uint8_t)total_chunks;
        memcpy(buf + CHUNK_HEADER_BYTES, data + off, flen);
        if (!notify_payload(buf, flen + CHUNK_HEADER_BYTES)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static bool send_status_internal(const char *status, uint32_t generation)
{
    if (!status || !s_tx_mutex) {
        return false;
    }
    bool sent = false;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uint32_t record_generation = 0;
    char *encrypted = link_pairing_encrypt_status(status, generation, &record_generation);
    if (encrypted) {
        snprintf(s_last_status, sizeof(s_last_status), "encrypted_status");
        sent = send_chunked_locked(encrypted, record_generation);
        free(encrypted);
    } else if (generation != 0 || s_plaintext_status_blocked
               || !plaintext_status_allowed(status)) {
        ESP_LOGW(TAG, "plaintext status suppressed: %s", status);
    } else {
        size_t n = strlen(status);
        if (n >= sizeof(s_last_status)) {
            n = sizeof(s_last_status) - 1;
        }
        memcpy(s_last_status, status, n);
        s_last_status[n] = '\0';
        sent = notify_payload((const uint8_t *)s_last_status, n);
    }
    xSemaphoreGive(s_tx_mutex);
    if (sent) {
        ESP_LOGI(TAG, "status: %s", status);
    }
    return sent;
}

bool muse_ble_pairing_send_status(const char *status)
{
    return send_status_internal(status, 0);
}

bool muse_ble_pairing_send_pairing_status(const char *status, uint32_t generation)
{
    return generation != 0 && send_status_internal(status, generation);
}

void muse_ble_pairing_send_chunked(const char *data)
{
    if (!s_tx_mutex) {
        return;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    (void)send_chunked_locked(data, 0);
    xSemaphoreGive(s_tx_mutex);
}

bool muse_ble_pairing_send_encrypted_json(const char *json, uint32_t generation)
{
    if (!json || generation == 0 || !s_tx_mutex) {
        return false;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uint32_t record_generation = 0;
    char *encrypted = link_pairing_encrypt_json(json, generation, &record_generation);
    bool sent = encrypted && send_chunked_locked(encrypted, record_generation);
    free(encrypted);
    xSemaphoreGive(s_tx_mutex);
    return sent;
}

uint32_t muse_ble_pairing_session_generation(void)
{
    return link_pairing_session_generation();
}

/* ------------------------------------------------------------------ */
/* Worker tasks for app callbacks (SDK ble_server.c pattern)          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *ssid;
    char *password;
    char *access_token;
    char *refresh_token;
    char *username;
    char *ota_url;
    bool ota_force;
    char *api_url;
    char *api_url_v2;
    char *noise_host;
    uint32_t session_generation;
} provision_args_t;

static void provision_task(void *arg)
{
    provision_args_t *a = (provision_args_t *)arg;
    if (s_cb.on_provision
        && link_pairing_provisioning_session_valid(a->session_generation)) {
        muse_provision_payload_t payload = {
            .ssid = a->ssid ? a->ssid : "",
            .password = a->password ? a->password : "",
            .access_token = a->access_token ? a->access_token : "",
            .refresh_token = a->refresh_token ? a->refresh_token : "",
            .username = a->username ? a->username : "",
            .ota_url = a->ota_url ? a->ota_url : "",
            .ota_force = a->ota_force,
            .api_url = a->api_url ? a->api_url : "",
            .api_url_v2 = a->api_url_v2 ? a->api_url_v2 : "",
            .noise_host = a->noise_host ? a->noise_host : "",
            .session_generation = a->session_generation,
        };
        s_cb.on_provision(&payload);
    }
    secure_free_str(a->ssid);
    secure_free_str(a->password);
    secure_free_str(a->access_token);
    secure_free_str(a->refresh_token);
    secure_free_str(a->username);
    secure_free_str(a->ota_url);
    secure_free_str(a->api_url);
    secure_free_str(a->api_url_v2);
    secure_free_str(a->noise_host);
    free(a);
    vTaskDelete(NULL);
}

static void simple_callback_task(void *arg)
{
    /* arg selects the callback: 0 wifi_scan, 1 unpair, 2 get_device_info,
     * 3 client_connected, 4 client_disconnected. */
    uintptr_t which = (uintptr_t)arg;
    switch (which) {
    case 0:
        if (s_cb.on_wifi_scan) {
            s_cb.on_wifi_scan();
        }
        break;
    case 1:
        if (s_cb.on_unpair) {
            s_cb.on_unpair();
        }
        break;
    case 2:
        if (s_cb.on_get_device_info) {
            s_cb.on_get_device_info();
        }
        break;
    case 3:
        if (s_cb.on_client_connected) {
            s_cb.on_client_connected();
        }
        break;
    case 4:
        if (s_cb.on_client_disconnected) {
            s_cb.on_client_disconnected();
        }
        break;
    default:
        break;
    }
    vTaskDelete(NULL);
}

static void confirm_required_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    if (link_pairing_session_is_current(generation) && s_cb.on_confirm_required) {
        s_cb.on_confirm_required(generation);
    }
    vTaskDelete(NULL);
}

static void delayed_disconnect_task(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms));
    muse_ble_pairing_disconnect_client();
    vTaskDelete(NULL);
}

void muse_ble_pairing_delayed_disconnect(uint32_t ms)
{
    xTaskCreate(delayed_disconnect_task, "muse_ble_dc",
                2048, (void *)(uintptr_t)ms, 4, NULL);
}

static bool spawn_simple_callback(uintptr_t which, const char *name)
{
    return xTaskCreate(simple_callback_task, name, WORKER_TASK_STACK,
                       (void *)which, 5, NULL) == pdPASS;
}

/* ------------------------------------------------------------------ */
/* Command dispatch (SDK ble_server.c: dispatch_command_ex)           */
/* ------------------------------------------------------------------ */

static bool is_sensitive_setup_action(const char *act)
{
    return strcmp(act, "provision") == 0
           || strcmp(act, "provision_v2") == 0
           || strcmp(act, "wifi_scan") == 0
           || strcmp(act, "ota") == 0
           || strcmp(act, "device.ota") == 0
           || strcmp(act, "unpair") == 0
           || strcmp(act, "set_wifi") == 0
           || strcmp(act, "set_auth") == 0;
}

static bool is_exact_client_finished(cJSON *root)
{
    cJSON *only = cJSON_IsObject(root) ? root->child : NULL;
    return only && !only->next
           && only->string && strcmp(only->string, "action") == 0
           && cJSON_IsString(only) && only->valuestring
           && strcmp(only->valuestring, "pairing_client_finished") == 0;
}

static bool command_json_depth_valid(const uint8_t *data, size_t len)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 32) {
                return false;
            }
        } else if ((c == '}' || c == ']') && depth) {
            depth--;
        }
    }
    return true;
}

static const char *optional_ota_url(cJSON *root)
{
    cJSON *u = cJSON_GetObjectItem(root, "ota_url");
    if (!cJSON_IsString(u)) {
        u = cJSON_GetObjectItem(root, "url");
    }
    return (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "";
}

static bool optional_ota_force(cJSON *root)
{
    cJSON *f = cJSON_GetObjectItem(root, "ota_force");
    if (!cJSON_IsBool(f)) {
        f = cJSON_GetObjectItem(root, "force");
    }
    return cJSON_IsTrue(f);
}

/* Forward: dispatch_command_ex recurses for decrypted commands. */
static void dispatch_command_ex(const uint8_t *data, size_t len, bool decrypted);

static void handle_provision_v2(cJSON *root)
{
    cJSON *s = cJSON_GetObjectItem(root, "ssid");
    cJSON *p = cJSON_GetObjectItem(root, "password");
    cJSON *at = cJSON_GetObjectItem(root, "access_token");
    cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    cJSON *u = cJSON_GetObjectItem(root, "username");
    cJSON *tt = cJSON_GetObjectItem(root, "token_type");
    const char *ota_url = optional_ota_url(root);
    if (!cJSON_IsString(s) || !cJSON_IsString(p) || !cJSON_IsString(at)
        || !s->valuestring || !p->valuestring || !at->valuestring
        || !*s->valuestring || !*p->valuestring || !*at->valuestring
        || !cJSON_IsString(rt) || !rt->valuestring || !*rt->valuestring
        || !cJSON_IsString(tt) || !tt->valuestring
        || strcmp(tt->valuestring, "device") != 0) {
        muse_ble_pairing_send_status("error_missing_credentials");
        return;
    }
    provision_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        muse_ble_pairing_send_status("error_operation_in_progress");
        return;
    }
    a->ssid = dup_str(s->valuestring);
    a->password = dup_str(p->valuestring);
    a->access_token = dup_str(at->valuestring);
    if (cJSON_IsString(rt) && rt->valuestring) {
        a->refresh_token = dup_str(rt->valuestring);
    }
    if (cJSON_IsString(u) && u->valuestring) {
        a->username = dup_str(u->valuestring);
    }
    if (*ota_url) {
        /* Carried through for the app to see; this board has no OTA slots,
         * so the MuseApp ignores ota_url/ota_force (documented). */
        a->ota_url = dup_str(ota_url);
        a->ota_force = optional_ota_force(root);
        if (!a->ota_url) {
            goto oom;
        }
    }
    cJSON *au = cJSON_GetObjectItem(root, "api_url");
    if (cJSON_IsString(au) && au->valuestring && *au->valuestring) {
        a->api_url = dup_str(au->valuestring);
    }
    cJSON *au2 = cJSON_GetObjectItem(root, "api_url_v2");
    if (cJSON_IsString(au2) && au2->valuestring && *au2->valuestring) {
        a->api_url_v2 = dup_str(au2->valuestring);
    }
    cJSON *nh = cJSON_GetObjectItem(root, "noise_host");
    if (cJSON_IsString(nh) && nh->valuestring && *nh->valuestring) {
        a->noise_host = dup_str(nh->valuestring);
    }
    a->session_generation = link_pairing_mark_provisioning_active();
    if (a->session_generation == 0
        || xTaskCreate(provision_task, "muse_prov", PROVISION_TASK_STACK,
                       a, 5, NULL) != pdPASS) {
        uint32_t generation = a->session_generation;
        secure_free_str(a->ssid);
        secure_free_str(a->password);
        secure_free_str(a->access_token);
        secure_free_str(a->refresh_token);
        secure_free_str(a->username);
        secure_free_str(a->ota_url);
        secure_free_str(a->api_url);
        secure_free_str(a->api_url_v2);
        secure_free_str(a->noise_host);
        free(a);
        muse_ble_pairing_send_pairing_status("error_operation_in_progress", generation);
        muse_ble_pairing_disconnect_pairing_session(generation);
        return;
    }
    ESP_LOGI(TAG, "provision_v2 accepted, handing off to the Muse app");
    set_state(MUSE_BLE_PAIRING_STATE_PROVISIONING);
    return;

oom:
    secure_free_str(a->ssid);
    secure_free_str(a->password);
    secure_free_str(a->access_token);
    secure_free_str(a->refresh_token);
    secure_free_str(a->username);
    free(a);
    muse_ble_pairing_send_status("error_operation_in_progress");
}

static void dispatch_command_ex(const uint8_t *data, size_t len, bool decrypted)
{
    if (!command_json_depth_valid(data, len)) {
        muse_ble_pairing_send_status("error_invalid_command");
        return;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!root) {
        ESP_LOGW(TAG, "RX invalid JSON (%d bytes)", (int)len);
        muse_ble_pairing_send_status("error_invalid_command");
        return;
    }
    cJSON *action = cJSON_GetObjectItem(root, "action");
    const char *act = (cJSON_IsString(action) && action->valuestring) ? action->valuestring : "";
    ESP_LOGI(TAG, "RX action: %s", act);

    bool provisioned = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        provisioned = s_provisioned;
        xSemaphoreGive(s_mutex);
    }

    if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && provisioned) {
        /* Re-pairing goes through unpair/reset, never a new session here. */
        muse_ble_pairing_send_status("error_pairing_unavailable");
    } else if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
        char *reply = NULL;
        const char *err = link_pairing_handle_client_hello(root, &reply);
        if (err) {
            muse_ble_pairing_send_status(err);
        } else if (reply) {
            s_plaintext_status_blocked = true;
            muse_ble_pairing_send_chunked(reply);
            free(reply);
            ESP_LOGI(TAG, "pairing_ready sent, waiting for client_finished");
            set_state(MUSE_BLE_PAIRING_STATE_PAIRING);
        }
    } else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {
        char *plain = NULL;
        const char *err = link_pairing_decrypt_command(root, &plain);
        if (err) {
            muse_ble_pairing_send_status(err);
            muse_ble_pairing_delayed_disconnect(300);
        } else if (plain) {
            dispatch_command_ex((const uint8_t *)plain, strlen(plain), true);
            mbedtls_platform_zeroize(plain, strlen(plain));
            free(plain);
        }
    } else if (!decrypted && s_plaintext_status_blocked) {
        ESP_LOGW(TAG, "plaintext command suppressed after pairing ready: %s", act);
    } else if (!decrypted && is_sensitive_setup_action(act)
               && link_pairing_plaintext_setup_blocked()) {
        muse_ble_pairing_send_status("error_encryption_required");
    } else if (decrypted && strcmp(act, "pairing_client_finished") == 0) {
        uint32_t generation = is_exact_client_finished(root)
            ? link_pairing_handle_client_finished() : 0;
        if (generation) {
            /* Deviation from the SDK (see file header note 4): send the
             * encrypted confirm_required record HERE, before the app
             * callback, so its record order is guaranteed. The app then
             * shows the prompt and arms confirmation. */
            if (!muse_ble_pairing_send_pairing_status("confirm_required", generation)) {
                muse_ble_pairing_disconnect_pairing_session(generation);
            } else if (xTaskCreate(confirm_required_task, "muse_confirm", WORKER_TASK_STACK,
                                   (void *)(uintptr_t)generation, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "failed to dispatch confirm-required callback");
                muse_ble_pairing_send_pairing_status("error_pairing_unavailable", generation);
                muse_ble_pairing_disconnect_pairing_session(generation);
            } else {
                ESP_LOGI(TAG, "confirm_required sent, waiting for on-device confirm");
                set_state(MUSE_BLE_PAIRING_STATE_CONFIRM_REQUIRED);
            }
        } else {
            muse_ble_pairing_send_status("error_pairing_decrypt");
            link_pairing_reset();
            muse_ble_pairing_delayed_disconnect(300);
        }
    } else if (decrypted && is_sensitive_setup_action(act)
               && !link_pairing_session_confirmed()) {
        muse_ble_pairing_send_status("error_pairing_confirm_required");
    } else if (decrypted && strcmp(act, "wifi_scan") == 0) {
        if (!spawn_simple_callback(0, "muse_scan")) {
            muse_ble_pairing_send_status("error_operation_in_progress");
        }
    } else if (decrypted && strcmp(act, "device.ota") == 0) {
        /* No OTA on this board (single factory partition, no OTA slots):
         * the honest answer is "unknown action", like any unhandled one. */
        muse_ble_pairing_send_status("error_unknown_action");
    } else if (decrypted && strcmp(act, "unpair") == 0) {
        if (!spawn_simple_callback(1, "muse_unpair")) {
            muse_ble_pairing_send_status("error_operation_in_progress");
        }
    } else if (decrypted && strcmp(act, "provision_v2") != 0) {
        muse_ble_pairing_send_status("error_unknown_action");
    } else if (decrypted && strcmp(act, "provision_v2") == 0) {
        handle_provision_v2(root);
    } else if (strcmp(act, "get_device_info") == 0) {
        if (!spawn_simple_callback(2, "muse_devinfo")) {
            muse_ble_pairing_send_status("error_operation_in_progress");
        }
    } else if (strcmp(act, "get_version") == 0) {
        char ver[80];
        snprintf(ver, sizeof(ver), "version:%s",
                 s_config.firmware_version ? s_config.firmware_version : "unknown");
        muse_ble_pairing_send_status(ver);
    } else {
        muse_ble_pairing_send_status("error_unknown_action");
    }
    delete_command_json(root, decrypted);
}

/* ------------------------------------------------------------------ */
/* RX reassembly (SDK ble_server.c: handle_rx_write, dispatch-owned)  */
/* ------------------------------------------------------------------ */

static void rx_reset_locked(void)
{
    free(s_rx_buf);
    s_rx_buf = NULL;
    s_rx_len = s_rx_cap = 0;
    s_rx_total = 0;
    s_rx_count = 0;
    s_rx_next_idx = 0;
}

static void handle_rx_write(const uint8_t *data, size_t len)
{
    if (len < CHUNK_HEADER_BYTES || data[0] != CHUNK_MAGIC) {
        dispatch_command_ex(data, len, false);
        return;
    }

    /* Chunked: [0xFE, idx, total, ...payload] */
    uint8_t idx = data[1];
    uint8_t total = data[2];
    const uint8_t *frag = data + CHUNK_HEADER_BYTES;
    size_t flen = len - CHUNK_HEADER_BYTES;
    ESP_LOGI(TAG, "RX chunk %d/%d (%d bytes)", idx + 1, total, (int)flen);

    if (total == 0) {
        rx_reset_locked();
        return;
    }

    if (idx == 0 || s_rx_total != total) {
        rx_reset_locked();
        s_rx_total = total;
        unsigned mtu = atomic_load(&s_mtu);
        size_t chunk_max = mtu > 3 + CHUNK_HEADER_BYTES
                           ? (size_t)mtu - 3 - CHUNK_HEADER_BYTES : 0;
        if (flen > chunk_max) {
            chunk_max = flen;
        }
        s_rx_cap = (size_t)total * chunk_max;
        if (s_rx_cap > MAX_RX_TOTAL_BYTES) {
            s_rx_cap = MAX_RX_TOTAL_BYTES;
        }
        s_rx_buf = malloc(s_rx_cap);
        if (!s_rx_buf) {
            ESP_LOGE(TAG, "no memory to reassemble a %u-byte command", (unsigned)s_rx_cap);
            rx_reset_locked();
            return;
        }
        s_rx_len = 0;
    }

    if (idx != s_rx_next_idx || idx >= s_rx_total) {
        rx_reset_locked();
        return;
    }

    if (s_rx_len + flen > MAX_RX_TOTAL_BYTES) {
        ESP_LOGE(TAG, "chunked command exceeds %d bytes", MAX_RX_TOTAL_BYTES);
        rx_reset_locked();
        return;
    }
    if (s_rx_len + flen > s_rx_cap) {
        size_t cap = s_rx_len + (size_t)(s_rx_total - idx) * flen;
        if (cap > MAX_RX_TOTAL_BYTES) {
            cap = MAX_RX_TOTAL_BYTES;
        }
        uint8_t *grown = realloc(s_rx_buf, cap);
        if (!grown) {
            ESP_LOGE(TAG, "no memory to reassemble a %u-byte command", (unsigned)cap);
            rx_reset_locked();
            return;
        }
        s_rx_buf = grown;
        s_rx_cap = cap;
    }
    memcpy(s_rx_buf + s_rx_len, frag, flen);
    s_rx_len += flen;
    s_rx_count++;
    s_rx_next_idx = idx + 1;

    if (s_rx_count < s_rx_total) {
        return;
    }

    ESP_LOGI(TAG, "RX reassembled (%d bytes)", (int)s_rx_len);
    uint8_t *complete = s_rx_buf;
    size_t complete_len = s_rx_len;
    s_rx_buf = NULL;
    rx_reset_locked();
    dispatch_command_ex(complete, complete_len, false);
    free(complete);
}

/* ------------------------------------------------------------------ */
/* Dispatch task                                                      */
/* ------------------------------------------------------------------ */

static void restart_idle_timer(void)
{
    if (s_idle_timer && s_config.connection_idle_timeout_s != UINT32_MAX
        && atomic_load(&s_conn_id) != 0xFFFF) {
        esp_timer_stop(s_idle_timer);
        esp_timer_start_once(s_idle_timer,
                             (uint64_t)s_config.connection_idle_timeout_s * 1000000ULL);
    }
}

static void stop_idle_timer(void)
{
    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
    }
}

static void on_client_disconnected_locked(void)
{
    atomic_store(&s_conn_id, 0xFFFF);
    atomic_store(&s_subscribed, false);
    atomic_store(&s_mtu, 23);
    stop_idle_timer();
    s_plaintext_status_blocked = false;
    rx_reset_locked();
    link_pairing_reset();
    spawn_simple_callback(4, "muse_disc");
    bool enabled = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_advertising_active = false;
        enabled = s_advertising_enabled;
        xSemaphoreGive(s_mutex);
    }
    if (enabled) {
        set_state(MUSE_BLE_PAIRING_STATE_ADVERTISING);
        kick_advertising();
    } else {
        set_state(MUSE_BLE_PAIRING_STATE_IDLE);
    }
}

static void dispatch_task(void *arg)
{
    (void)arg;
    dispatch_evt_t evt;
    for (;;) {
        if (xQueueReceive(s_dispatch_queue, &evt, portMAX_DELAY) != pdPASS) {
            continue;
        }
        switch (evt.type) {
        case EVT_RX:
            if (evt.data) {
                restart_idle_timer();
                handle_rx_write(evt.data, evt.len);
                free(evt.data);
            }
            break;
        case EVT_CONNECTED:
            ESP_LOGI(TAG, "GATT client connected");
            restart_idle_timer();
            s_plaintext_status_blocked = false;
            set_state(MUSE_BLE_PAIRING_STATE_CONNECTED);
            break;
        case EVT_DISCONNECTED:
            ESP_LOGI(TAG, "GATT client disconnected");
            on_client_disconnected_locked();
            break;
        case EVT_SUBSCRIBED:
            if (evt.generation) {
                spawn_simple_callback(3, "muse_conn");
            }
            break;
        case EVT_ADV_TIMEOUT:
            ESP_LOGI(TAG, "advertising timed out, stopping");
            muse_ble_pairing_stop_advertising(false);
            break;
        case EVT_IDLE_TIMEOUT:
            ESP_LOGI(TAG, "connection idle, disconnecting");
            muse_ble_pairing_disconnect_client();
            break;
        case EVT_SESSION_RESET:
            rx_reset_locked();
            link_pairing_reset();
            s_plaintext_status_blocked = false;
            break;
        case EVT_DISCONNECT_SESSION:
            if (link_pairing_reset_session(evt.generation)) {
                rx_reset_locked();
                s_plaintext_status_blocked = false;
                muse_ble_pairing_stop_advertising(true);
            }
            break;
        case EVT_STATE_NOTIFY:
            if (s_cb.on_state_changed) {
                s_cb.on_state_changed(evt.state);
            }
            break;
        case EVT_SHUTDOWN:
            ESP_LOGI(TAG, "dispatch task exiting");
            if (s_shutdown_done) {
                xSemaphoreGive(s_shutdown_done);
            }
            vTaskDelete(NULL);
            break;
        default:
            break;
        }
    }
}

void muse_ble_pairing_disconnect_pairing_session(uint32_t generation)
{
    if (generation == 0) {
        return;
    }
    dispatch_evt_t evt = { .type = EVT_DISCONNECT_SESSION, .generation = generation };
    if (!post_dispatch_evt(&evt)) {
        /* Queue full: invalidate the keys anyway, like the SDK's fallback. */
        (void)link_pairing_reset_session(generation);
        ESP_LOGE(TAG, "failed to queue pairing disconnect");
    }
}

/* ------------------------------------------------------------------ */
/* Advertising data (SDK ble_server.c: start_advertising)             */
/* ------------------------------------------------------------------ */

/* Adv packet: flags + complete 128-bit service UUID + manufacturer data.
 * Manufacturer data: 0xFFFF (test/unassigned company ID) + paired flag.
 * Total 27 bytes, inside the 31-byte legacy limit. */
static size_t build_adv_data(uint8_t *out, size_t cap, bool paired)
{
    uint8_t *p = out;
    /* Flags: LE General Discoverable + BR/EDR Not Supported (0x06), exactly
     * as the SDK advertises. NOTE: this board DOES support BR/EDR (A2DP),
     * so 0x06 is a lie told for byte-parity with the SDK gadgets the Muse
     * app was tested against; the flag only affects LE discovery. If the
     * app ever misbehaves, 0x02 (LE General Discoverable only) is the
     * honest value for a dual-mode device. See INTEGRATION.md. */
    *p++ = 2; *p++ = 0x01; *p++ = 0x06;
    /* Complete list of 128-bit service UUIDs. */
    *p++ = 17; *p++ = 0x07;
    memcpy(p, SVC_UUID128, 16);
    p += 16;
    /* Manufacturer specific data. */
    *p++ = 5; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = paired ? 0x01 : 0x00;
    (void)cap;
    return (size_t)(p - out);
}

/* Scan response: complete local name (split out so the UUID + name both
 * fit within the 31-byte legacy advertising limit). */
static size_t build_scan_rsp_data(uint8_t *out, size_t cap)
{
    const char *name = s_config.device_name ? s_config.device_name : "MuseGadget";
    size_t nlen = strlen(name);
    if (nlen > 29) {
        nlen = 29;
    }
    if (nlen + 2 > cap) {
        nlen = cap - 2;
    }
    out[0] = (uint8_t)(nlen + 1);
    out[1] = 0x09;
    memcpy(out + 2, name, nlen);
    return nlen + 2;
}

/* Verified against ESP-IDF v6.1.1 headers (esp_gap_ble_api.h). */
static const esp_ble_adv_params_t ADV_PARAMS = {
    .adv_int_min = 0x20, /* 20 ms */
    .adv_int_max = 0x40, /* 40 ms */
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr = { 0 },
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void stop_adv_timer(void)
{
    if (s_adv_timer) {
        esp_timer_stop(s_adv_timer);
    }
}

static void start_adv_timer(void)
{
    if (s_adv_timer && s_config.advertising_timeout_s != UINT32_MAX) {
        esp_timer_stop(s_adv_timer);
        esp_timer_start_once(s_adv_timer,
                             (uint64_t)s_config.advertising_timeout_s * 1000000ULL);
    }
}

/* Start the async config-adv-data -> config-scan-rsp -> start-advertising
 * chain when all conditions hold. Safe to call from any task, including
 * the BTC task (GAP/GATTS callbacks). */
static void kick_advertising(void)
{
    bool go = false;
    bool paired = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    if (s_started && s_service_started && s_advertising_enabled && !s_quiet
        && !s_advertising_active && !s_adv_config_pending
        && atomic_load(&s_conn_id) == 0xFFFF) {
        s_adv_config_pending = true;
        paired = s_provisioned;
        go = true;
    }
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
    if (!go) {
        return;
    }
    uint8_t raw[31];
    size_t len = build_adv_data(raw, sizeof(raw), paired);
    esp_err_t err = esp_ble_gap_config_adv_data_raw(raw, (uint32_t)len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_adv_data_raw failed: %s", esp_err_to_name(err));
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_adv_config_pending = false;
            xSemaphoreGive(s_mutex);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bluedroid GAP callback                                             */
/* ------------------------------------------------------------------ */

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    /* gap_cb stays registered for the life of the process (Bluedroid has no
     * GAP unregister): ignore everything once the component is down. */
    if (!s_started) {
        return;
    }
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        if (param->adv_data_raw_set.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "adv data set failed: %d", param->adv_data_raw_set.status);
            if (s_mutex) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_adv_config_pending = false;
                xSemaphoreGive(s_mutex);
            }
            break;
        }
        {
            uint8_t rsp[31];
            size_t len = build_scan_rsp_data(rsp, sizeof(rsp));
            esp_err_t err = esp_ble_gap_config_scan_rsp_data_raw(rsp, (uint32_t)len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "config_scan_rsp_data_raw failed: %s", esp_err_to_name(err));
                if (s_mutex) {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s_adv_config_pending = false;
                    xSemaphoreGive(s_mutex);
                }
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT: {
        bool enabled = false;
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_adv_config_pending = false;
            enabled = s_advertising_enabled && !s_quiet;
            xSemaphoreGive(s_mutex);
        }
        if (param->scan_rsp_data_raw_set.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan rsp data set failed: %d", param->scan_rsp_data_raw_set.status);
            break;
        }
        if (!enabled) {
            /* Stopped while the chain was in flight; do not start. */
            break;
        }
        esp_err_t err = esp_ble_gap_start_advertising((esp_ble_adv_params_t *)&ADV_PARAMS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adv_start failed: %s", esp_err_to_name(err));
        }
        break;
    }
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            if (s_mutex) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_advertising_active = true;
                xSemaphoreGive(s_mutex);
            }
            start_adv_timer();
            ESP_LOGI(TAG, "advertising as %s", s_config.device_name);
            set_state(MUSE_BLE_PAIRING_STATE_ADVERTISING);
        } else {
            ESP_LOGE(TAG, "adv_start failed: %d", param->adv_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_advertising_active = false;
            xSemaphoreGive(s_mutex);
        }
        stop_adv_timer();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Bluedroid GATTS callback                                           */
/* ------------------------------------------------------------------ */

static esp_bt_uuid_t uuid128(const uint8_t bytes[16])
{
    esp_bt_uuid_t uuid = { .len = ESP_UUID_LEN_128 };
    memcpy(uuid.uuid.uuid128, bytes, 16);
    return uuid;
}

static void add_next_char(void)
{
    esp_err_t err;
    if (s_setup_step == 1) {
        /* RX: write / write-without-response. */
        esp_bt_uuid_t uuid = uuid128(RX_UUID128);
        static uint8_t rx_value[RX_ATTR_MAX_LEN];
        esp_attr_value_t val = {
            .attr_max_len = RX_ATTR_MAX_LEN,
            .attr_len = 0,
            .attr_value = rx_value,
        };
        esp_attr_control_t control = { .auto_rsp = ESP_GATT_RSP_BY_APP };
        err = esp_ble_gatts_add_char(s_service_handle, &uuid,
                                     ESP_GATT_PERM_WRITE,
                                     ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                                     &val, &control);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add RX char failed: %s", esp_err_to_name(err));
        }
    } else if (s_setup_step == 2) {
        /* TX: read / notify. */
        esp_bt_uuid_t uuid = uuid128(TX_UUID128);
        static uint8_t tx_value[TX_ATTR_MAX_LEN];
        esp_attr_value_t val = {
            .attr_max_len = TX_ATTR_MAX_LEN,
            .attr_len = 0,
            .attr_value = tx_value,
        };
        esp_attr_control_t control = { .auto_rsp = ESP_GATT_RSP_BY_APP };
        err = esp_ble_gatts_add_char(s_service_handle, &uuid,
                                     ESP_GATT_PERM_READ,
                                     ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                     &val, &control);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add TX char failed: %s", esp_err_to_name(err));
        }
    } else if (s_setup_step == 3) {
        /* CCCD for TX notifications. */
        esp_bt_uuid_t uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = CCCD_UUID16 } };
        static uint8_t cccd_value[2] = { 0x00, 0x00 };
        esp_attr_value_t val = {
            .attr_max_len = 2,
            .attr_len = 2,
            .attr_value = cccd_value,
        };
        esp_attr_control_t control = { .auto_rsp = ESP_GATT_RSP_BY_APP };
        err = esp_ble_gatts_add_char_descr(s_service_handle, &uuid,
                                           ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                           &val, &control);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add CCCD failed: %s", esp_err_to_name(err));
        }
    }
}

static void send_write_response(esp_gatt_if_t gatts_if, uint16_t conn_id,
                                uint32_t trans_id, esp_gatt_status_t status)
{
    esp_err_t err = esp_ble_gatts_send_response(gatts_if, conn_id, trans_id, status, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send_response failed: %s", esp_err_to_name(err));
    }
}

static void handle_gatts_write(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    uint16_t handle = param->write.handle;
    bool need_rsp = param->write.need_rsp;

    if (handle == s_cccd_handle) {
        /* Client (un)subscribed to TX notifications. */
        bool notify = param->write.len >= 2 && (param->write.value[0] & 0x01);
        atomic_store(&s_subscribed, notify);
        ESP_LOGI(TAG, "subscribe notify=%d", notify);
        if (need_rsp) {
            send_write_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK);
        }
        dispatch_evt_t evt = { .type = EVT_SUBSCRIBED, .generation = notify ? 1 : 0 };
        post_dispatch_evt(&evt);
        return;
    }

    if (handle != s_rx_handle) {
        if (need_rsp) {
            send_write_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                ESP_GATT_NOT_FOUND);
        }
        return;
    }

    if (param->write.is_prep) {
        /* Long write: stage the fragment; EXEC_WRITE delivers the whole. */
        size_t off = param->write.offset;
        size_t len = param->write.len;
        esp_gatt_status_t status = ESP_GATT_OK;
        if (!s_prep_buf) {
            s_prep_buf = malloc(MAX_RX_TOTAL_BYTES);
            s_prep_len = 0;
            if (!s_prep_buf) {
                status = ESP_GATT_NO_RESOURCES;
            }
        }
        if (status == ESP_GATT_OK && off + len > MAX_RX_TOTAL_BYTES) {
            status = ESP_GATT_INVALID_ATTR_LEN;
        }
        if (status == ESP_GATT_OK) {
            memcpy(s_prep_buf + off, param->write.value, len);
            if (off + len > s_prep_len) {
                s_prep_len = off + len;
            }
        }
        /* Prepare responses echo the fragment (GATT spec). */
        esp_gatt_rsp_t rsp = { 0 };
        rsp.attr_value.handle = handle;
        rsp.attr_value.offset = (uint16_t)off;
        rsp.attr_value.len = (uint16_t)len;
        if (len <= sizeof(rsp.attr_value.value)) {
            memcpy(rsp.attr_value.value, param->write.value, len);
        }
        esp_err_t err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                    param->write.trans_id, status, &rsp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "prep-write response failed: %s", esp_err_to_name(err));
        }
        return;
    }

    /* Single write: hand a copy to the dispatch task (never parse/crypto
     * on the BTC task — it is shared with Classic BT). */
    if (param->write.len > 0) {
        uint8_t *copy = malloc(param->write.len);
        if (copy) {
            memcpy(copy, param->write.value, param->write.len);
            dispatch_evt_t evt = { .type = EVT_RX, .data = copy, .len = param->write.len };
            if (!post_dispatch_evt(&evt)) {
                ESP_LOGW(TAG, "RX event dropped (queue full)");
                free(copy);
            }
        } else {
            ESP_LOGE(TAG, "no memory for RX write (%d bytes)", param->write.len);
        }
    }
    if (need_rsp) {
        send_write_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK);
    }
}

static void handle_gatts_read(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    uint16_t handle = param->read.handle;
    esp_gatt_rsp_t rsp = { 0 };
    esp_gatt_status_t status = ESP_GATT_OK;
    if (handle == s_tx_handle) {
        /* TX read returns the last plaintext status (SDK tx_access_cb). */
        size_t n = strlen(s_last_status);
        rsp.attr_value.handle = handle;
        rsp.attr_value.len = (uint16_t)n;
        memcpy(rsp.attr_value.value, s_last_status, n);
    } else if (handle == s_cccd_handle) {
        rsp.attr_value.handle = handle;
        rsp.attr_value.len = 2;
        rsp.attr_value.value[0] = atomic_load(&s_subscribed) ? 0x01 : 0x00;
        rsp.attr_value.value[1] = 0x00;
    } else {
        status = ESP_GATT_READ_NOT_PERMIT;
    }
    esp_err_t err = esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                                param->read.trans_id, status, &rsp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read response failed: %s", esp_err_to_name(err));
    }
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                     esp_ble_gatts_cb_param_t *param)
{
    if (!s_started) {
        return;
    }
    /* Ignore events for other GATT applications (there should be none). */
    if (event != ESP_GATTS_REG_EVT && gatts_if != ESP_GATT_IF_NONE
        && gatts_if != s_gatts_if) {
        return;
    }
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.app_id != GATTS_APP_ID) {
            break;
        }
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "gatts app register failed: %d", param->reg.status);
            break;
        }
        s_gatts_if = gatts_if;
        {
            esp_gatt_srvc_id_t svc_id = {
                .is_primary = true,
                .id = { .inst_id = 0, .uuid = uuid128(SVC_UUID128) },
            };
            esp_err_t err = esp_ble_gatts_create_service(gatts_if, &svc_id, SERVICE_NUM_HANDLES);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "create service failed: %s", esp_err_to_name(err));
            }
        }
        break;
    case ESP_GATTS_CREATE_EVT:
        if (param->create.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "create service failed: %d", param->create.status);
            break;
        }
        s_service_handle = param->create.service_handle;
        s_setup_step = 1;
        add_next_char();
        break;
    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "add char failed: %d", param->add_char.status);
            break;
        }
        if (s_setup_step == 1) {
            s_rx_handle = param->add_char.attr_handle;
            s_setup_step = 2;
            add_next_char();
        } else if (s_setup_step == 2) {
            s_tx_handle = param->add_char.attr_handle;
            s_setup_step = 3;
            add_next_char();
        }
        break;
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        if (param->add_char_descr.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "add descr failed: %d", param->add_char_descr.status);
            break;
        }
        s_cccd_handle = param->add_char_descr.attr_handle;
        {
            esp_err_t err = esp_ble_gatts_start_service(s_service_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "start service failed: %s", esp_err_to_name(err));
            }
        }
        break;
    case ESP_GATTS_START_EVT:
        if (param->start.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "start service failed: %d", param->start.status);
            break;
        }
        s_setup_step = 4;
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_service_started = true;
            xSemaphoreGive(s_mutex);
        }
        ESP_LOGI(TAG, "pairing GATT service started (rx=%d tx=%d cccd=%d)",
                 s_rx_handle, s_tx_handle, s_cccd_handle);
        kick_advertising();
        break;
    case ESP_GATTS_CONNECT_EVT:
        atomic_store(&s_conn_id, param->connect.conn_id);
        atomic_store(&s_subscribed, false);
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_advertising_active = false; /* undirected adv stops on connect */
            xSemaphoreGive(s_mutex);
        }
        stop_adv_timer();
        ESP_LOGI(TAG, "connected conn_id=%d", param->connect.conn_id);
        {
            dispatch_evt_t evt = { .type = EVT_CONNECTED };
            post_dispatch_evt(&evt);
        }
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "disconnect reason=%d", param->disconnect.reason);
        {
            dispatch_evt_t evt = { .type = EVT_DISCONNECTED };
            post_dispatch_evt(&evt);
        }
        break;
    case ESP_GATTS_WRITE_EVT:
        handle_gatts_write(gatts_if, param);
        break;
    case ESP_GATTS_READ_EVT:
        handle_gatts_read(gatts_if, param);
        break;
    case ESP_GATTS_EXEC_WRITE_EVT:
        if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && s_prep_buf
            && s_prep_len > 0) {
            uint8_t *copy = malloc(s_prep_len);
            if (copy) {
                memcpy(copy, s_prep_buf, s_prep_len);
                dispatch_evt_t evt = { .type = EVT_RX, .data = copy, .len = s_prep_len };
                if (!post_dispatch_evt(&evt)) {
                    ESP_LOGW(TAG, "RX event dropped (queue full)");
                    free(copy);
                }
            }
        }
        free(s_prep_buf);
        s_prep_buf = NULL;
        s_prep_len = 0;
        send_write_response(gatts_if, param->exec_write.conn_id,
                            param->exec_write.trans_id, ESP_GATT_OK);
        break;
    case ESP_GATTS_MTU_EVT:
        atomic_store(&s_mtu, param->mtu.mtu);
        ESP_LOGI(TAG, "mtu=%d conn=%d", param->mtu.mtu, param->mtu.conn_id);
        break;
    case ESP_GATTS_CONGEST_EVT:
        ESP_LOGW(TAG, "link congested=%d", param->congest.congested);
        break;
    case ESP_GATTS_CONF_EVT:
    case ESP_GATTS_STOP_EVT:
    case ESP_GATTS_DELETE_EVT:
    case ESP_GATTS_UNREG_EVT:
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Timers                                                             */
/* ------------------------------------------------------------------ */

static void adv_timeout_cb(void *arg)
{
    (void)arg;
    dispatch_evt_t evt = { .type = EVT_ADV_TIMEOUT };
    post_dispatch_evt(&evt);
}

static void idle_timeout_cb(void *arg)
{
    (void)arg;
    dispatch_evt_t evt = { .type = EVT_IDLE_TIMEOUT };
    post_dispatch_evt(&evt);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t muse_ble_pairing_init(const muse_ble_pairing_config_t *config)
{
    if (!config || !config->device_name || !*config->device_name
        || strlen(config->device_name) > 29
        || !config->node_id || !config->device_id || !config->mac) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Attach only: bt_service owns Bluedroid init/enable. Never init here. */
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(TAG, "Bluedroid is not enabled; call bt_service_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    bool app_registered = false;

    s_config = *config;
    /* Timeout defaults: a zeroed field means "default" (600 s / 180 s);
     * UINT32_MAX disables that timeout. See muse_ble_pairing.h. */
    if (s_config.advertising_timeout_s == 0) {
        s_config.advertising_timeout_s = DEFAULT_ADV_TIMEOUT_S;
    }
    if (s_config.connection_idle_timeout_s == 0) {
        s_config.connection_idle_timeout_s = DEFAULT_IDLE_TIMEOUT_S;
    }
    s_cb = config->callbacks;

    s_mutex = xSemaphoreCreateMutex();
    s_tx_mutex = xSemaphoreCreateMutex();
    s_shutdown_done = xSemaphoreCreateBinary();
    s_dispatch_queue = xQueueCreate(DISPATCH_QUEUE_LEN, sizeof(dispatch_evt_t));
    if (!s_mutex || !s_tx_mutex || !s_shutdown_done || !s_dispatch_queue) {
        goto oom;
    }
    esp_timer_create_args_t adv_args = {
        .callback = adv_timeout_cb,
        .name = "muse_ble_adv_to",
    };
    esp_timer_create_args_t idle_args = {
        .callback = idle_timeout_cb,
        .name = "muse_ble_idle_to",
    };
    if (esp_timer_create(&adv_args, &s_adv_timer) != ESP_OK
        || esp_timer_create(&idle_args, &s_idle_timer) != ESP_OK) {
        goto oom;
    }

    link_pairing_init(config->node_id, config->device_id, config->mac,
                      config->firmware_version ? config->firmware_version : "unknown",
                      config->sdk_token);

    /* Verified against ESP-IDF v6.1.1 headers (esp_gatt_common_api.h). */
    if (esp_ble_gatt_set_local_mtu(512) != ESP_OK) {
        ESP_LOGW(TAG, "set_local_mtu(512) failed; continuing with the default");
    }

    esp_err_t err = esp_ble_gap_register_callback(gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap_register_callback failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (esp_ble_gap_set_device_name(config->device_name) != ESP_OK) {
        ESP_LOGW(TAG, "gap_set_device_name failed; continuing");
    }

    err = esp_ble_gatts_register_callback(gatts_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gatts_register_callback failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_ble_gatts_app_register(GATTS_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gatts_app_register failed: %s", esp_err_to_name(err));
        goto fail;
    }
    app_registered = true;

    if (xTaskCreate(dispatch_task, "muse_ble", DISPATCH_TASK_STACK,
                    NULL, 5, &s_dispatch_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create dispatch task");
        goto fail;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_started = true;
    s_provisioned = config->provisioned;
    s_state = config->provisioned ? MUSE_BLE_PAIRING_STATE_PROVISIONED
                                  : MUSE_BLE_PAIRING_STATE_IDLE;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "pairing server ready (node %s, provisioned=%d)",
             config->node_id, config->provisioned);
    return ESP_OK;

oom:
    err = ESP_ERR_NO_MEM;
fail:
    if (app_registered) {
        esp_ble_gatts_app_unregister(GATTS_APP_ID);
    }
    if (s_adv_timer) {
        esp_timer_delete(s_adv_timer);
        s_adv_timer = NULL;
    }
    if (s_idle_timer) {
        esp_timer_delete(s_idle_timer);
        s_idle_timer = NULL;
    }
    if (s_dispatch_queue) {
        vQueueDelete(s_dispatch_queue);
        s_dispatch_queue = NULL;
    }
    if (s_shutdown_done) {
        vSemaphoreDelete(s_shutdown_done);
        s_shutdown_done = NULL;
    }
    if (s_tx_mutex) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return err;
}

esp_err_t muse_ble_pairing_deinit(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    muse_ble_pairing_stop_advertising(true);
    stop_adv_timer();
    stop_idle_timer();

    /* From here the BTC callbacks ignore everything (s_started == false),
     * so no callback can touch the mutex/queue while they are deleted. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_started = false;
    xSemaphoreGive(s_mutex);

    /* Ask the dispatch task to exit and wait for its goodbye. If it does
     * not exit in time, do NOT tear down under it — return and let the
     * caller retry deinit later. */
    if (s_dispatch_task) {
        dispatch_evt_t evt = { .type = EVT_SHUTDOWN };
        post_dispatch_evt(&evt);
        if (xSemaphoreTake(s_shutdown_done, pdMS_TO_TICKS(5000)) != pdPASS) {
            ESP_LOGE(TAG, "dispatch task did not exit; deinit aborted");
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_started = true; /* restore: the component is still alive */
            xSemaphoreGive(s_mutex);
            return ESP_ERR_TIMEOUT;
        }
        s_dispatch_task = NULL;
    }

    if (s_service_handle) {
        esp_ble_gatts_stop_service(s_service_handle);
        esp_ble_gatts_delete_service(s_service_handle);
        s_service_handle = 0;
    }
    esp_ble_gatts_app_unregister(GATTS_APP_ID);
    /* NOTE: no esp_ble_gap_unregister_callback exists; gap_cb stays
     * registered but ignores everything once s_started is false. */
    link_pairing_reset();
    free(s_prep_buf);
    s_prep_buf = NULL;
    s_prep_len = 0;
    free(s_rx_buf);
    s_rx_buf = NULL;
    s_rx_len = s_rx_cap = 0;
    s_rx_total = s_rx_count = s_rx_next_idx = 0;
    s_plaintext_status_blocked = false;
    atomic_store(&s_conn_id, 0xFFFF);
    atomic_store(&s_subscribed, false);
    atomic_store(&s_mtu, 23);
    snprintf(s_last_status, sizeof(s_last_status), "idle");
    s_rx_handle = s_tx_handle = s_cccd_handle = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_started = false;
    s_service_started = false;
    s_advertising_enabled = false;
    s_advertising_active = false;
    s_adv_config_pending = false;
    s_quiet = false;
    s_setup_step = 0;
    s_gatts_if = ESP_GATT_IF_NONE;
    xSemaphoreGive(s_mutex);

    esp_timer_delete(s_adv_timer);
    esp_timer_delete(s_idle_timer);
    s_adv_timer = s_idle_timer = NULL;
    vQueueDelete(s_dispatch_queue);
    s_dispatch_queue = NULL;
    vSemaphoreDelete(s_shutdown_done);
    s_shutdown_done = NULL;
    vSemaphoreDelete(s_tx_mutex);
    s_tx_mutex = NULL;
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    ESP_LOGI(TAG, "pairing server shut down (Bluedroid left running)");
    return ESP_OK;
}

esp_err_t muse_ble_pairing_start_advertising(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    bool provisioned = false;
    bool active = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        provisioned = s_provisioned;
        active = s_advertising_active;
        xSemaphoreGive(s_mutex);
    }
    if (provisioned) {
        /* SDK parity: no setup advertising once provisioned. Re-pairing
         * goes through unpair first. */
        ESP_LOGW(TAG, "already provisioned; refusing to advertise");
        return ESP_ERR_INVALID_STATE;
    }
    if (active) {
        return ESP_OK;
    }
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_advertising_enabled = true;
        xSemaphoreGive(s_mutex);
    }
    kick_advertising();
    return ESP_OK;
}

esp_err_t muse_ble_pairing_stop_advertising(bool disconnect_client)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    bool active = false;
    bool provisioned = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_advertising_enabled = false;
        active = s_advertising_active;
        provisioned = s_provisioned;
        xSemaphoreGive(s_mutex);
    }
    stop_adv_timer();
    if (active) {
        esp_err_t err = esp_ble_gap_stop_advertising();
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "adv_stop rc=%s", esp_err_to_name(err));
        }
    }
    if (disconnect_client) {
        muse_ble_pairing_disconnect_client();
        dispatch_evt_t evt = { .type = EVT_SESSION_RESET };
        post_dispatch_evt(&evt);
    }
    set_state(provisioned ? MUSE_BLE_PAIRING_STATE_PROVISIONED
                          : MUSE_BLE_PAIRING_STATE_IDLE);
    return ESP_OK;
}

void muse_ble_pairing_set_quiet(bool quiet)
{
    if (!s_started) {
        return;
    }
    bool active = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_quiet == quiet) {
            xSemaphoreGive(s_mutex);
            return;
        }
        s_quiet = quiet;
        active = s_advertising_active;
        xSemaphoreGive(s_mutex);
    }
    if (quiet) {
        stop_adv_timer();
        if (active) {
            esp_ble_gap_stop_advertising();
            ESP_LOGI(TAG, "advertising paused (quiet)");
        }
    } else {
        kick_advertising();
    }
}

esp_err_t muse_ble_pairing_set_provisioned(bool provisioned)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_provisioned = provisioned;
        xSemaphoreGive(s_mutex);
    }
    set_state(provisioned ? MUSE_BLE_PAIRING_STATE_PROVISIONED
                          : MUSE_BLE_PAIRING_STATE_IDLE);
    return ESP_OK;
}

bool muse_ble_pairing_is_provisioned(void)
{
    bool provisioned = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        provisioned = s_provisioned;
        xSemaphoreGive(s_mutex);
    }
    return provisioned;
}

bool muse_ble_pairing_is_advertising(void)
{
    bool active = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        active = s_started && s_advertising_active;
        xSemaphoreGive(s_mutex);
    }
    return active;
}

bool muse_ble_pairing_has_connection(void)
{
    return atomic_load(&s_conn_id) != 0xFFFF;
}

muse_ble_pairing_state_t muse_ble_pairing_get_state(void)
{
    muse_ble_pairing_state_t state = MUSE_BLE_PAIRING_STATE_IDLE;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        state = s_state;
        xSemaphoreGive(s_mutex);
    }
    return state;
}

bool muse_ble_pairing_arm_confirmation(uint32_t generation)
{
    return link_pairing_arm_confirmation(generation);
}

bool muse_ble_pairing_confirm(void)
{
    uint32_t generation = link_pairing_confirm_active_session();
    if (generation == 0) {
        ESP_LOGW(TAG, "confirm ignored: no armed session");
        return false;
    }
    /* pairing_confirmed carries the SDK token (link_pairing.c). */
    if (!muse_ble_pairing_send_pairing_status("pairing_confirmed", generation)) {
        muse_ble_pairing_disconnect_pairing_session(generation);
        return false;
    }
    ESP_LOGI(TAG, "pairing confirmed, waiting for provision_v2");
    set_state(MUSE_BLE_PAIRING_STATE_CONFIRMED);
    return true;
}

void muse_ble_pairing_disconnect_client(void)
{
    unsigned conn = atomic_load(&s_conn_id);
    if (conn == 0xFFFF || s_gatts_if == ESP_GATT_IF_NONE) {
        return;
    }
    esp_err_t err = esp_ble_gatts_close(s_gatts_if, (uint16_t)conn);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "gatts_close rc=%s", esp_err_to_name(err));
    }
}
