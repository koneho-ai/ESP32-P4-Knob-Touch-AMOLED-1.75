/*
 * Muse BLE pairing for the Waveshare ESP32-P4-Knob-Touch-AMOLED-1.75 factory
 * firmware — public API.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UNTESTED ON HARDWARE — no P4 knob board exists yet. Everything in this
 * component is compile-verification only until it is flashed and paired
 * against the real Muse app. See INTEGRATION.md ("What could NOT be
 * verified") before trusting any of it.
 *
 * What this is: a Bluedroid BLE GATT server implementing the Muse gadget
 * SDK's community pairing v5 service, so the existing Muse app can pair
 * with this board unmodified. It is a port of the SDK's NimBLE server
 * (muse-gadget-sdk, esp32/main/ble_server.c + link_pairing.c, commit
 * 6a44074) onto Bluedroid, because the factory firmware already runs
 * Bluedroid for Classic BT (A2DP sink / BT Music) and the two BLE hosts
 * cannot coexist.
 *
 * What this is NOT: it does not initialize or enable Bluedroid (the
 * factory bt_service owns the stack), it does not touch Classic BT
 * (GAP/A2DP/AVRCP), it does not store credentials (the MuseApp owns NVS
 * through muse_vm's muse_provisioning.h), and it draws no UI (status only).
 *
 * Pairing flow (see INTEGRATION.md for the full sequence):
 *   1. MuseApp calls muse_ble_pairing_init() once (after bt_service_init()),
 *      then muse_ble_pairing_start_advertising() when the user opens the
 *      "pair from the Muse app" screen.
 *   2. The Muse app connects, sends pairing_client_hello, and the component
 *      answers pairing_ready (community pairing v5, P-256 ECDH + HKDF +
 *      AES-256-GCM, all in vendored/link_pairing.c).
 *   3. The app sends the encrypted pairing_client_finished; the component
 *      sends the encrypted "confirm_required" status and invokes the
 *      on_confirm_required callback. The MuseApp shows the prompt and calls
 *      muse_ble_pairing_arm_confirmation(); the user confirms on screen and
 *      the MuseApp calls muse_ble_pairing_confirm(), which sends the
 *      encrypted "pairing_confirmed" status (carrying the SDK token).
 *   4. The app sends the encrypted provision_v2 record; the component
 *      invokes on_provision with the payload. The MuseApp joins Wi-Fi,
 *      stores the device tokens via muse_provisioning_set_device_tokens(),
 *      then calls muse_ble_pairing_send_pairing_status("auth_ok", ...) and
 *      muse_ble_pairing_stop_advertising(true).
 *
 * Threading: callbacks run on internal worker tasks created by this
 * component (on_provision gets an 8192-byte stack, like the SDK's provision
 * task, because the handler may run a TLS handshake; the others get 4096).
 * Callbacks must return; long work belongs in a task of the caller's own.
 * on_state_changed runs on the pairing dispatch task — keep it short, do
 * not block, and do not call start/stop/confirm from it; post to the
 * caller's own task instead.
 *
 * Logging: this component logs pairing stages, never credential values.
 * Tokens, passwords and keys are never passed to ESP_LOG*.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pairing service UUIDs — byte-identical to the SDK's NimBLE server
 * (ble_server.c: SVC_UUID / RX_UUID / TX_UUID). Canonical string forms:
 *   service 7fdd3d1c-38ea-46cf-8b46-314ecf5f240c
 *   RX      4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01 (write / write-without-response)
 *   TX      d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c (read / notify)
 * The arrays below are in little-endian (over-the-air) byte order, exactly
 * as the SDK declares them. */

/** Pairing state, for the Muse app's status screen. */
typedef enum {
    MUSE_BLE_PAIRING_STATE_IDLE = 0,        /*!< Not advertising, no link. */
    MUSE_BLE_PAIRING_STATE_ADVERTISING,     /*!< Advertising, waiting for the app. */
    MUSE_BLE_PAIRING_STATE_CONNECTED,       /*!< App connected, no hello yet. */
    MUSE_BLE_PAIRING_STATE_PAIRING,         /*!< Hello done, keys derived, awaiting finished. */
    MUSE_BLE_PAIRING_STATE_CONFIRM_REQUIRED,/*!< Waiting for the on-device confirm press. */
    MUSE_BLE_PAIRING_STATE_CONFIRMED,       /*!< Confirmed, waiting for provision_v2. */
    MUSE_BLE_PAIRING_STATE_PROVISIONING,    /*!< provision_v2 received, app is working. */
    MUSE_BLE_PAIRING_STATE_PROVISIONED,     /*!< Setup complete (sticky until unpair). */
} muse_ble_pairing_state_t;

/**
 * Payload of a decrypted provision_v2 record (SDK ble_server.c:
 * provision_args_t / ble_provision_cb). All strings are NUL-terminated and
 * borrowed: they are valid only for the duration of the on_provision call.
 * The caller must copy anything it keeps (and zeroize its copies when done).
 * No field is ever logged by this component.
 *
 * NOTE on what these tokens are: provision_v2 carries the DEVICE token pair
 * (token_type is required to be "device"), minted by the Muse app for this
 * device's node id — not the user's account tokens. The MuseApp stores them
 * via muse_provisioning_set_device_tokens(). The no-refresh-token mint path
 * (SDK app.c accept_pairing_credentials) does not apply here: provision_v2
 * without a refresh_token is rejected with "error_missing_credentials",
 * exactly as in the SDK.
 */
typedef struct {
    const char *ssid;               /*!< Wi-Fi SSID to join (non-empty). */
    const char *password;           /*!< Wi-Fi password (non-empty). */
    const char *access_token;       /*!< Device access token (non-empty). */
    const char *refresh_token;      /*!< Device refresh token (non-empty). */
    const char *username;           /*!< Account username, may be "". */
    const char *ota_url;            /*!< Optional OTA URL, "" when absent. Ignored on this board (no OTA slots). */
    bool ota_force;                 /*!< OTA force flag, ignored on this board. */
    const char *api_url;            /*!< Optional API base override, "" when absent. */
    const char *api_url_v2;         /*!< Optional API v2 base override, "" when absent. */
    const char *noise_host;         /*!< Optional Noise host override, "" when absent. */
    uint32_t session_generation;    /*!< Opaque session token. Bind "auth_ok" and any
                                         further pairing calls to this generation; 0 is never valid. */
} muse_provision_payload_t;

/** Callbacks into the Muse app. All are optional (NULL = ignore). */
typedef struct {
    /**
     * @brief provision_v2 received and validated.
     *
     * Runs on a dedicated worker task (8192-byte stack). The handler is
     * expected to: join Wi-Fi with payload->ssid/password, store the device
     * tokens via muse_provisioning_set_device_tokens(), optionally connect
     * to the VM, then report back with
     * muse_ble_pairing_send_pairing_status("auth_ok", generation) on success
     * (or "auth_failed" / "error_storage" on failure, mirroring the SDK's
     * app.c on_provision), and finally stop advertising with
     * muse_ble_pairing_stop_advertising(true) and record the new state with
     * muse_ble_pairing_set_provisioned(true).
     *
     * If the handler is still running when the provisioning session expires
     * (120 s after provision_v2, see link_pairing.c), the component resets
     * the session and disconnects; the handler should check
     * muse_ble_pairing_session_generation() before sending "auth_ok".
     */
    void (*on_provision)(const muse_provision_payload_t *payload);
    /**
     * @brief The app asked for a Wi-Fi scan ("wifi_scan", encrypted).
     *
     * The handler scans and replies with
     * muse_ble_pairing_send_encrypted_json() carrying
     * {"type":"wifi_scan_result","networks":[{"ssid","rssi","secure"}]},
     * using muse_ble_pairing_session_generation() as the generation
     * (SDK app.c: send_cached_scan_to_client). Runs on a worker task.
     */
    void (*on_wifi_scan)(void);
    /** @brief The app asked to unpair ("unpair", encrypted). The handler
     * should clear pairing credentials (muse_provisioning_unpair()),
     * call muse_ble_pairing_set_provisioned(false), and re-open the pairing
     * screen. Runs on a worker task. */
    void (*on_unpair)(void);
    /**
     * @brief The app asked for device info ("get_device_info", plaintext).
     *
     * The handler builds {"type":"device_info","node_id","version",
     * ...link_pairing_add_device_info fields...,"build_sha"} and sends it
     * with muse_ble_pairing_send_chunked() (plaintext, like the SDK's
     * app.c on_get_device_info). Runs on a worker task.
     */
    void (*on_get_device_info)(void);
    /** @brief A GATT client subscribed to TX notifications. */
    void (*on_client_connected)(void);
    /** @brief The GATT client disconnected (or the link dropped). */
    void (*on_client_disconnected)(void);
    /**
     * @brief The app finished the encrypted handshake and the component has
     * sent the encrypted "confirm_required" status.
     *
     * The MuseApp must now show the pairing-confirmation prompt and, ONLY
     * once the prompt is on screen, call
     * muse_ble_pairing_arm_confirmation(generation). A confirm press before
     * arming is ignored (SDK rule: the prompt must be emitted before the
     * button is armed, so pairing_confirmed can never overtake the record
     * the phone expects first). Runs on a worker task.
     */
    void (*on_confirm_required)(uint32_t session_generation);
    /**
     * @brief Pairing state changed (for the status screen).
     *
     * Runs on the internal pairing dispatch task: keep it short, do not
     * block, and do not call muse_ble_pairing_start/stop/confirm from it.
     * Post to the caller's own task if reacting needs BLE calls.
     */
    void (*on_state_changed)(muse_ble_pairing_state_t state);
} muse_ble_pairing_callbacks_t;

/** Init-time configuration. Strings are borrowed and must outlive the component. */
typedef struct {
    const char *device_name;        /*!< BLE advertising name, e.g. "MuseGadget-XXXXXX"
                                         (SDK identity.c: BLE_NAME_PREFIX + uppercase hex
                                         of the last 3 STA MAC octets). Shown in the
                                         scan response; max 29 bytes. */
    const char *node_id;            /*!< Node id offered to the app, e.g. "homelink-xxxxxx"
                                         (lowercase hex of the last 3 STA MAC octets). */
    const char *device_id;          /*!< e.g. "hatch-link:aa:bb:cc:dd:ee:ff" (SDK identity.c). */
    const char *mac;                /*!< STA MAC, lowercase colon-separated. */
    const char *firmware_version;   /*!< e.g. esp_app_get_description()->version. */
    const char *sdk_token;          /*!< "mgst_..." SDK token, or NULL. Rides on the
                                         encrypted pairing_confirmed status so the app
                                         can present it at device-token mint. Never logged. */
    bool provisioned;               /*!< True when a device-token pair is already stored
                                         (muse_provisioning_is_provisioned()). While true,
                                         pairing hellos are refused and the advertising
                                         manufacturer byte reports "paired". */
    uint32_t advertising_timeout_s; /*!< Stop advertising this long after start if no
                                         client connects. 0 = default (600 s).
                                         UINT32_MAX = no timeout. */
    uint32_t connection_idle_timeout_s; /*!< Disconnect a connected client that sends
                                         nothing for this long. 0 = default (180 s).
                                         UINT32_MAX = off.
                                         (Defensive deviation from the SDK: the knob
                                         has one GATT slot and no other way to reclaim
                                         a stale link.) */
    muse_ble_pairing_callbacks_t callbacks;
} muse_ble_pairing_config_t;

/**
 * @brief Initialize the pairing GATT server and attach it to the running Bluedroid stack.
 *
 * Must be called AFTER bt_service_init() (which initializes and enables
 * Bluedroid) and after the ESP32 coprocessor link is up. This function
 * NEVER calls esp_bluedroid_init/enable itself: if Bluedroid is not
 * enabled it returns ESP_ERR_INVALID_STATE.
 *
 * Registers the GATT application, creates the pairing service, and starts
 * the internal dispatch task. Does NOT start advertising — call
 * muse_ble_pairing_start_advertising() when the Muse app should be able to
 * find the device.
 *
 * @param config  Borrowed strings must stay valid until deinit.
 * @return ESP_OK, ESP_ERR_INVALID_ARG (bad config), ESP_ERR_INVALID_STATE
 *         (Bluedroid not enabled, or already initialized), ESP_ERR_NO_MEM.
 */
esp_err_t muse_ble_pairing_init(const muse_ble_pairing_config_t *config);

/**
 * @brief Tear the pairing server down.
 *
 * Stops advertising, disconnects any client, resets the pairing session,
 * deletes the GATT service and unregisters the GATT application. Bluedroid
 * itself is left running — bt_service owns the stack and BT Music keeps
 * working. After deinit, init() may be called again.
 */
esp_err_t muse_ble_pairing_deinit(void);

/**
 * @brief Start BLE advertising for pairing.
 *
 * No-op (ESP_OK) when already advertising or when a client is connected.
 * Advertising carries the pairing service UUID + manufacturer data
 * (company 0xFFFF + paired flag), with the device name in the scan
 * response — byte-compatible with the SDK's NimBLE advertising.
 *
 * Does NOT check whether BT Music is streaming: BLE advertising is a low
 * duty-cycle activity and coexistence is the coprocessor controller's job.
 * If the Muse app wants the radio quiet while BT Music streams, it should
 * call muse_ble_pairing_set_quiet(true) (see the coexistence contract in
 * INTEGRATION.md).
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (not initialized).
 */
esp_err_t muse_ble_pairing_start_advertising(void);

/**
 * @brief Stop BLE advertising.
 *
 * @param disconnect_client  Also terminate the current GATT connection and
 *                           reset the pairing session.
 */
esp_err_t muse_ble_pairing_stop_advertising(bool disconnect_client);

/**
 * @brief Quiet mode: pause advertising without changing the enabled state
 * (SDK ble_server_set_quiet). While quiet, no advertising runs; clearing
 * quiet resumes whatever advertising was on. Connections are left alone.
 */
void muse_ble_pairing_set_quiet(bool quiet);

/**
 * @brief Tell the component whether device credentials are stored.
 *
 * The MuseApp calls this after storing tokens (true) and after unpairing
 * (false). While provisioned, pairing hellos are refused with
 * "error_pairing_unavailable" and the advertising paired-flag byte is 1.
 * The component reads this state; it never touches NVS itself.
 */
esp_err_t muse_ble_pairing_set_provisioned(bool provisioned);

/** @brief Last provisioned state pushed via init/set_provisioned. */
bool muse_ble_pairing_is_provisioned(void);

/** @brief True while BLE advertising is actually running. */
bool muse_ble_pairing_is_advertising(void);

/** @brief True while a GATT client is connected. */
bool muse_ble_pairing_has_connection(void);

/** @brief Current pairing state (see muse_ble_pairing_state_t). */
muse_ble_pairing_state_t muse_ble_pairing_get_state(void);

/**
 * @brief Arm physical confirmation for a session (SDK link_pairing_arm_confirmation).
 *
 * Call only after the confirmation prompt is on screen, from the
 * on_confirm_required handler. Re-arms the 60 s confirm deadline.
 *
 * @param generation  Session token from on_confirm_required.
 * @return True when the session is current, awaiting confirmation, and now armed.
 */
bool muse_ble_pairing_arm_confirmation(uint32_t generation);

/**
 * @brief Confirm the armed pairing session (user pressed confirm).
 *
 * Confirms the session (SDK link_pairing_confirm_active_session) and, on
 * success, sends the encrypted "pairing_confirmed" status carrying the SDK
 * token. Returns false when no session is armed (stale press, timeout, or
 * already confirmed) — the UI should ignore the press in that case.
 */
bool muse_ble_pairing_confirm(void);

/**
 * @brief Send a short status string (SDK ble_server_send_status).
 *
 * If an encrypted pairing session is active the status is sent as an
 * encrypted record; otherwise it is sent as a plaintext notification only
 * when the status is on the SDK's plaintext allow-list
 * (error_encryption_required, error_pairing_invalid_hello,
 * error_pairing_unavailable, error_pairing_decrypt) — anything else is
 * suppressed, exactly as in the SDK.
 */
bool muse_ble_pairing_send_status(const char *status);

/** @brief Send a status bound to one pairing session (SDK ble_server_send_pairing_status).
 * Returns false when generation is 0 or stale. */
bool muse_ble_pairing_send_pairing_status(const char *status, uint32_t generation);

/**
 * @brief Send a payload as framed chunked notifications (SDK ble_server_send_chunked).
 * Frame: [0xFE, chunk index, chunk count, ...payload], max 160 bytes per
 * notification, 50 ms stagger. Used for pairing_ready and device_info.
 */
void muse_ble_pairing_send_chunked(const char *data);

/**
 * @brief Encrypt a JSON payload as a pairing record and send it chunked
 * (SDK ble_server_send_encrypted_json). Used for wifi_scan_result.
 * Returns false when generation is 0 or stale, or no session is active.
 */
bool muse_ble_pairing_send_encrypted_json(const char *json, uint32_t generation);

/**
 * @brief Current pairing session generation (SDK link_pairing_session_generation).
 * 0 when no session is active. Use it to bind deferred replies (e.g. a
 * Wi-Fi scan result) to the session that asked for them.
 */
uint32_t muse_ble_pairing_session_generation(void);

/** @brief Disconnect the current GATT client without changing advertising policy. */
void muse_ble_pairing_disconnect_client(void);

/**
 * @brief Disconnect the current GATT client after a delay (ms), giving
 * pending status notifications time to go out (SDK ble_server_delayed_disconnect).
 */
void muse_ble_pairing_delayed_disconnect(uint32_t ms);

/**
 * @brief If generation is the current pairing session, reset it, stop
 * advertising and disconnect (SDK ble_server_disconnect_pairing_session).
 * Stale generations are ignored.
 */
void muse_ble_pairing_disconnect_pairing_session(uint32_t generation);

#ifdef __cplusplus
}
#endif
