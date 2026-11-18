# INTEGRATION.md — Muse BLE pairing (Phase 2.1) for the P4 knob factory firmware

> **UNTESTED ON HARDWARE.** No P4 knob board exists yet. This component is
> compile-verification only until it is flashed and paired against the real
> Muse app. The "What could NOT be verified" section at the end is the
> honest list — read it before trusting any of this.

## What this deliverable is

A Bluedroid BLE GATT server implementing the Muse gadget SDK's **community
pairing v5** service, so the existing Muse app can pair with the Waveshare
ESP32-P4-Knob-Touch-AMOLED-1.75 factory firmware **unmodified**. It is a
port of the SDK's NimBLE server (`muse-gadget-sdk`, `esp32/main/ble_server.c`
+ `link_pairing.c`, commit `6a44074`) onto Bluedroid, because the factory
firmware already runs Bluedroid for Classic BT (A2DP sink / BT Music) and
the two BLE hosts cannot coexist (plan risks R1/R3).

Layout (installed in the factory tree at
`examples/factory/components/MuseBlePairing/`):

```
MuseBlePairing/
├── CMakeLists.txt
├── idf_component.yml
├── include/
│   └── muse_ble_pairing.h       # public API for the Muse app
├── muse_ble_pairing.c           # Bluedroid GATT server + dispatch (new code)
├── vendored/
│   ├── link_pairing.c/.h        # pairing-v5 crypto, adapted (community-only)
│   └── pairing_transcript.c/.h  # v5 transcript, verbatim
├── INTEGRATION.md               # this file
├── VENDORED.md                  # what was vendored, from where, what changed
└── README.md                    # short overview
```

The provisioning store contract lives in the `muse_vm` component
(`muse_provisioning.h`, implemented in `muse_provisioning.c`) — this
component never touches NVS, and its earlier standalone copy of that
header was removed at integration time to avoid a duplicate header.

## 1. Installation

Installed at `examples/factory/components/MuseBlePairing/` in the
firmware repo (`koneho-ai/ESP32-P4-Knob-Touch-AMOLED-1.75`, branch
`muse-app`). Local components are discovered automatically; no project
CMakeLists change is needed. The remaining steps, all done:

1. Apply the `sdkconfig.defaults` changes in §2.
2. Delete the existing `build/sdkconfig` (or the whole `build/` dir):
   `sdkconfig.defaults` is only applied on a fresh configure, so editing
   it does not update an existing build.
3. The MuseApp component adds `MuseBlePairing` to its `REQUIRES` and
   implements the pairing flow in `MuseApp.cpp` (see §3).
4. Build with ESP-IDF v6.1, target `esp32p4`, as usual for the factory
   project. The Bluedroid API signatures were verified against the
   installed ESP-IDF v6.1.1 headers at integration time.

## 2. sdkconfig.defaults changes

In `examples/factory/sdkconfig.defaults`, change the Bluetooth block from:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_BLE_ENABLED=n
CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y
```

to:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
# BLE on alongside Classic: the Muse pairing GATT server (MuseBlePairing).
# The ESP32 coprocessor is the dual-mode controller over esp-extconn; the
# host-side BLE enable below is the only Bluetooth config change needed.
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_GATTS_ENABLE=y
CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y
```

Notes:

- `CONFIG_BT_GATTS_ENABLE=y` is the default when BLE is enabled; it is
  listed explicitly so the GATT-server requirement is visible.
- No controller change is needed: the P4 has no built-in BT radio, the
  controller lives entirely on the ESP32 coprocessor, and the HCI
  transport (`esp-extconn` `trans_bt.c`) is protocol-agnostic — see §6.
- `CONFIG_BT_GATTC_ENABLE` (BLE client role) is left at its default. We
  are a GATT server only; disabling it would save a little RAM but adds
  config risk. Revisit after the first working build if RAM is tight.
- `CONFIG_BT_SMP_ENABLE` is left at its default. No BLE bonding is used
  (pairing is app-layer, §4); SMP being compiled in is harmless.
- Cost of enabling the BLE host: expect roughly tens of KB of extra DRAM
  (host structures, GATT database) plus PSRAM allocations (the
  `SPIRAM_FIRST` setting already routes Bluedroid allocations to PSRAM).
  Measure the real delta at build time (`idf.py size`); it was not
  measurable here.

## 3. How the MuseApp calls this component

### 3.1 Identity (mirror the SDK's `identity.c`)

The pairing handshake identifies the device to the app. Derive the strings
exactly as the SDK does (`esp32/main/identity.c`, `identity_init()`):

| Field | Value |
|---|---|
| BLE name | `MuseGadget-XXXXXX` — `CONFIG_HOMEHUB_BLE_NAME_PREFIX` default `"MuseGadget"` + uppercase hex of the last 3 STA MAC octets |
| node id | `homelink-xxxxxx` — lowercase hex of the last 3 STA MAC octets (default prefix; `gadget-` only with `CONFIG_MUSE_GADGET_NODE_ID_PREFIX`) |
| device id | `hatch-link:aa:bb:cc:dd:ee:ff` — full STA MAC, lowercase, colon-separated |
| mac | `aa:bb:cc:dd:ee:ff` — STA MAC, lowercase, colon-separated |
| firmware version | `esp_app_get_description()->version` |
| sdk token | `mgst_…` (build-time `CONFIG_GADGET_SDK_TOKEN` or NVS-stored), or NULL |

**MAC source (decided at integration):** the identity uses the P4's eFuse
MAC via `esp_read_mac(ESP_MAC_WIFI_STA, ...)` (`muse_vm/identity.c`) —
stable across reboots and available before Wi-Fi is up. This differs
from the SDK, which uses the Wi-Fi STA MAC (on this board, the
coprocessor's MAC); the node id only needs to be stable and unique per
device, and the tokens are minted for whatever id the device offers, so
either source works as long as it never changes.

### 3.2 Init (once, after `bt_service_init()`)

```c
muse_ble_pairing_config_t cfg = {
    .device_name = ble_name,        /* "MuseGadget-XXXXXX", max 29 bytes */
    .node_id = node_id,             /* "homelink-xxxxxx" */
    .device_id = device_id,         /* "hatch-link:aa:bb:..." */
    .mac = mac,                     /* "aa:bb:cc:dd:ee:ff" */
    .firmware_version = esp_app_get_description()->version,
    .sdk_token = sdk_token_or_null, /* "mgst_..." — never log it */
    .provisioned = muse_provisioning_is_provisioned(),
    .callbacks = {
        .on_provision = muse_on_provision,
        .on_wifi_scan = muse_on_wifi_scan,
        .on_unpair = muse_on_unpair,
        .on_get_device_info = muse_on_get_device_info,
        .on_client_connected = muse_on_ble_connected,
        .on_client_disconnected = muse_on_ble_disconnected,
        .on_confirm_required = muse_on_confirm_required,
        .on_state_changed = muse_on_pairing_state,
    },
};
ESP_ERROR_CHECK(muse_ble_pairing_init(&cfg));
```

`init()` fails with `ESP_ERR_INVALID_STATE` if Bluedroid is not enabled —
`bt_service_init()` runs at boot in `main.cpp`, so any time the app runs
is fine. `init()` does **not** start advertising.

### 3.3 Pairing screen flow

```
User opens "pair from the Muse app" screen
  -> muse_ble_pairing_start_advertising()
     (refuses with ESP_ERR_INVALID_STATE when already provisioned)
  -> state ADVERTISING; the Muse app discovers "MuseGadget-XXXXXX"
  -> app connects, hello, pairing_ready            (state PAIRING)
  -> app sends encrypted pairing_client_finished
  -> component sends encrypted "confirm_required"
  -> on_confirm_required(generation) fires
  -> MuseApp shows the confirmation prompt on screen
  -> ONLY once the prompt is visible:
     muse_ble_pairing_arm_confirmation(generation)
  -> user taps Confirm
  -> muse_ble_pairing_confirm()          (state CONFIRMED)
     (sends encrypted "pairing_confirmed" + SDK token; returns false
      on a stale press — ignore the tap in that case)
  -> app sends encrypted provision_v2
  -> on_provision(payload) fires         (state PROVISIONING)
  -> MuseApp: join Wi-Fi, store tokens, optionally connect VM
  -> muse_ble_pairing_send_pairing_status("auth_ok", generation)
  -> muse_ble_pairing_stop_advertising(true)
  -> muse_ble_pairing_set_provisioned(true)   (state PROVISIONED)
```

Timeouts (all in `vendored/link_pairing.c`, SDK values): 60 s for
client_finished after hello, 60 s for the confirm press after arming,
120 s for provisioning after confirm. On expiry the session resets and
the app must start over (the BLE link is dropped). Additionally this
component stops advertising after `advertising_timeout_s` (default 600 s)
with no connection, and disconnects a client that sends nothing for
`connection_idle_timeout_s` (default 180 s) — both visible to the app
through `on_state_changed`.

### 3.4 The `on_provision` handler (mirror the SDK's `app.c: on_provision`)

Runs on a dedicated worker task (8192-byte stack — enough for a TLS
handshake). The handler must:

1. Join Wi-Fi with `payload->ssid` / `payload->password` (via
   `wifi_service`; the factory Wi-Fi driver persists the credentials).
2. Store the device tokens (endpoint overrides ride along in the same
   call):
   `muse_provisioning_set_device_tokens(payload->access_token,
   payload->refresh_token, muse_provisioning_node_id(),
   payload->api_url_v2, payload->noise_host)` — the node id is the one
   this device offered during pairing (`homelink-XXXXXX`); the v1
   `api_url` override is not stored (the account API uses the v2 base).
3. Optionally connect to the VM (Phase 2.2 work; the SDK does it before
   reporting success, but the app may also defer it to the first chat
   session — either way, report honestly).
4. On success: `muse_ble_pairing_send_pairing_status("auth_ok",
   payload->session_generation)`, then
   `muse_ble_pairing_stop_advertising(true)` and
   `muse_ble_pairing_set_provisioned(true)`.
   On failure: send `"auth_failed"` (credentials/VM) or `"error_storage"`
   (NVS) bound to the same generation, and disconnect the session with
   `muse_ble_pairing_disconnect_pairing_session(generation)`.
5. Ignore `payload->ota_url` / `payload->ota_force`: this board has no OTA
   slots (single factory partition). The SDK treats OTA as optional; the
   app is not expected to require it (unverified, see §7).

Before sending `"auth_ok"`, check the session is still current:
`muse_ble_pairing_session_generation() == payload->session_generation`.
A stale generation means the session expired mid-handler; the send will
fail safely (returns false), but the tokens are already stored — the app
should still mark itself provisioned rather than leave a half-setup
device. (This matches the SDK's `require_provisioning_pairing_session`
checks in spirit; the SDK fails the setup on expiry, but there the tokens
were already stored too — the practical difference is only the status
sent to the phone.)

### 3.5 The other callbacks

- `on_wifi_scan`: scan (via `wifi_service`) and reply with
  `muse_ble_pairing_send_encrypted_json("{\"type\":\"wifi_scan_result\","
  "\"networks\":[{\"ssid\":...,\"rssi\":...,\"secure\":...}]}",
  muse_ble_pairing_session_generation())` — the exact shape the SDK's
  `app.c: send_cached_scan_to_client` sends.
- `on_get_device_info`: build
  `{"type":"device_info","node_id":...,"version":...,
  ...link_pairing_add_device_info fields...,"build_sha":...}` (SDK `app.c:
  on_get_device_info`) and send it with `muse_ble_pairing_send_chunked()`
  — **plaintext**, like the SDK. `link_pairing_add_device_info()` is
  declared in `vendored/link_pairing.h`; the MuseApp needs
  `MuseBlePairing`'s private include path for it — see §5 note.
- `on_unpair`: `muse_provisioning_clear_pairing()`, then
  `muse_ble_pairing_set_provisioned(false)`, then re-open the pairing
  screen. Wi-Fi is kept (SDK `config_unpair` semantics).
- `on_client_connected` / `on_client_disconnected`: status only.
- `on_state_changed`: update the pairing screen. Runs on the internal
  dispatch task — keep it short, never block, never call
  start/stop/confirm from it; post to the app's own task if a reaction
  needs BLE calls.

## 4. Protocol reference (what the app speaks)

All wire behavior is the SDK's, verified line-by-line from
`muse-gadget-sdk` commit `6a44074`, `esp32/main/`.

### 4.1 GATT service (`ble_server.c:60-77, 656-686`)

| Item | Value |
|---|---|
| Service UUID | `7fdd3d1c-38ea-46cf-8b46-314ecf5f240c` (primary) |
| RX characteristic | `4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01`, write + write-without-response |
| TX characteristic | `d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c`, read + notify |
| TX read value | last plaintext status string (or `"encrypted_status"`) |
| CCCD (0x2902) | standard, on TX |

### 4.2 Advertising (`ble_server.c:940-1010`)

- Flags `0x06` (LE General Discoverable + BR/EDR Not Supported) — see the
  honesty note in §7.
- Complete 128-bit service UUID.
- Manufacturer data: company `0xFFFF` + 1 byte paired flag (`0x00`/`0x01`).
- Scan response: complete local name (`MuseGadget-XXXXXX`).
- Undirected connectable, 20–40 ms interval during setup.

### 4.3 Chunk framing (`ble_server.c:74-77, 538-620, 700-760`)

- Each notification/write is one chunk: `[0xFE, index, total, ...payload]`,
  max 160 bytes per notification, 50 ms stagger between TX chunks.
- Single (unchunked) writes are dispatched as whole commands.
- Max reassembled command: 8192 bytes; chunks must arrive in order.
- TX chunks are capped by the negotiated MTU (local MTU proposed: 512).

### 4.4 Pairing v5 records (`link_pairing.c`, `pairing_transcript.c`)

- Version 5, model `hatch_link`, suite `p256-hkdf-sha256-aes-gcm-v1`,
  policy `confirm_press`, community auth `none`, epoch 0
  (`pairing_transcript.h`).
- Hello (plaintext): `{"action":"pairing_client_hello","version":5,
  "pairing_auth":"none","pairing_policy":"confirm_press",
  "mobile_pub":<b64url P-256 point>,"mobile_nonce":<b64url 16B>}`.
- Ready (plaintext, chunked): `{"type":"pairing_ready","version":5,
  "device_id","node_id","mac","model","firmware_version","pairing_auth",
  "pairing_auth_epoch","pairing_policy","device_pub","device_nonce",
  "transcript_hash","session_id"}` — no `device_proof` for community.
- Transcript (hashed with SHA-256): `hatch-link-pairing-v5\nversion=5\n
  initiator_role=mobile\nresponder_role=link\ndevice_id=...\nnode_id=...\n
  mac=...\nmodel=hatch_link\nfirmware_version=...\n
  selected_cipher_suite=p256-hkdf-sha256-aes-gcm-v1\npairing_auth=none\n
  pairing_auth_epoch=0\npairing_policy=confirm_press\n
  confirm_timeout_seconds=60\nmobile_pub=...\ndevice_pub=...\n
  mobile_nonce=...\ndevice_nonce=...` (no trailing newline).
- Keys: `salt = SHA256(mobile_nonce || device_nonce || transcript_hash)`;
  `session_secret = HKDF-SHA256(ECDH, salt, "hatch-link ble setup v1")`
  (single-expand HKDF, T(1) only); `rx_key = HKDF-expand(secret,
  "mobile->device")`; `tx_key = HKDF-expand(secret, "device->mobile")`;
  `session_id = SHA256("hatch-link session id v1" || transcript_hash ||
  ECDH)[0:16]`.
- Records: AES-256-GCM. Nonce = direction byte (`0` mobile→device, `1`
  device→mobile) + 3 zero bytes + 8-byte big-endian counter. AAD =
  `hatch-link ble setup v1|<session_id_b64>|m2d|N` (or `d2m`). Counters
  start at 0 per direction and must arrive exactly in order; any gap or
  auth failure resets the session.
- Envelopes: phone→device uses `{"action":"pairing_encrypted",
  "session_id","counter","ciphertext","tag"}` (note: `action` key);
  device→phone uses `{"type":"pairing_encrypted", ...}` (note: `type`
  key). All base64 is URL-safe, no padding.
- Encrypted statuses are JSON records too:
  `{"type":"status","status":"..."}`; `pairing_confirmed` additionally
  carries `"sdk_token"`.

### 4.5 Actions and statuses (`ble_server.c: dispatch_command_ex`)

| Action | Plaintext? | Notes |
|---|---|---|
| `pairing_client_hello` | yes | refused with `error_pairing_unavailable` when provisioned |
| `pairing_encrypted` | yes (outer) | decrypted; bad record → `error_pairing_decrypt` + disconnect in 300 ms |
| `pairing_client_finished` | encrypted only | must be the exact single-key object and the first encrypted record |
| `provision_v2` | encrypted only | requires ssid, password, access/refresh tokens, `token_type:"device"`; only after confirm |
| `wifi_scan` | encrypted only | reply via `send_encrypted_json` |
| `unpair` | encrypted only | |
| `get_device_info` | yes | plaintext reply, chunked |
| `get_version` | yes | replies `version:<firmware>` |
| `device.ota` / `ota` | — | `error_unknown_action` (no OTA on this board) |
| anything else | — | `error_unknown_action` |

Plaintext statuses are only ever sent for `error_encryption_required`,
`error_pairing_invalid_hello`, `error_pairing_unavailable`,
`error_pairing_decrypt`; every other status goes out encrypted or not at
all. Sensitive plaintext setup actions are always rejected with
`error_encryption_required` (SDK `link_pairing_plaintext_setup_blocked()`
is unconditionally true).

## 5. Provisioning interface (`muse_vm/muse_provisioning.h`)

The provisioning store lives in the `muse_vm` component
(`muse_provisioning.h` / `muse_provisioning.c`, NVS namespace `museapp`,
plus a RAM cache in `muse_credentials.c` for the session task).
`MuseBlePairing` never calls these functions and never touches NVS — the
dependency is one-directional (`MuseApp` → `MuseBlePairing`), which
avoids a circular component dependency. The app pushes provisioned state
into the BLE component with `muse_ble_pairing_set_provisioned()` instead
of the BLE component querying NVS.

| Function | Implemented by | Called by |
|---|---|---|
| `muse_provisioning_is_provisioned()` | muse_vm | MuseApp (boot + pairing screen gating) |
| `muse_provisioning_set_device_tokens(access, refresh, node_id, api_url_v2, noise_host)` | muse_vm | MuseApp `on_provision` handler (endpoints ride along; no separate `set_endpoints`) |
| `muse_provisioning_unpair()` | muse_vm | MuseApp `on_unpair` handler |
| `muse_provisioning_node_id()` / `ble_name()` / `sdk_token()` | muse_vm | MuseApp (identity for the BLE init config) |

Rules for the implementation (mirroring the SDK's `config_store.c` /
`app.c: accept_pairing_credentials`):

- Store the node id **first**, then the token pair, so no tokens from a
  setup are ever kept without their id.
- `is_provisioned()` is true only when tokens **and** node id are present.
- `unpair()` erases durably (commit + readback). Wi-Fi is preserved.
- Never log token values. Getters hand out copies the caller must
  zeroize (`mbedtls_platform_zeroize`) when done.
- All functions are thread-safe (serialize NVS internally); they may
  block briefly on flash I/O. Call them from tasks with internal-RAM
  stacks (never from the session task, whose stack is in PSRAM).

**`device_info` fields:** the MuseApp's `on_get_device_info` handler
re-implements the six `device_info` fields itself (they are plain
constants + the identity strings — see `link_pairing_add_device_info()`
in `vendored/link_pairing.c`): `device_id`, `mac`,
`model:"hatch_link"`, `pairing_protocol:5`, `pairing_auth:"none"`,
`pairing_auth_epoch:0`, `pairing_policy:"confirm_press"`.

## 6. bt_service coexistence contract

The factory `bt_service` (`examples/factory/components/app_services/
bt_service.c`) owns Bluetooth on this board. This component attaches to
its stack; the contract is:

1. **Init order.** `bt_service_init()` first (it calls
   `esp_bluedroid_init_with_cfg()` + `esp_bluedroid_enable()`), then
   `muse_ble_pairing_init()`. This component refuses to init if Bluedroid
   is not enabled, and it **never** calls `esp_bluedroid_init`,
   `esp_bluedroid_enable/disable/deinit`, or `esp_bt_mem_release`.
2. **Classic BT is untouched.** This component never calls `esp_bt_gap_*`,
   `esp_a2d_*`, or `esp_avrc_*`, never changes the Classic scan mode,
   never touches bonds or the Classic device name (`"ESP32-P4-Knob"`).
   BT Music keeps working exactly as before.
3. **Advertising is on-demand only.** BLE advertising runs only while the
   Muse app's pairing screen is open, and stops on completion, failure,
   timeout, or disconnect. It is a low duty-cycle activity (20–40 ms
   interval, 27-byte packets); coexistence with A2DP streaming is the
   coprocessor controller's job, as on any dual-mode device.
4. **Quiet mode.** `muse_ble_pairing_set_quiet(true)` pauses advertising
   without changing the enabled state (SDK `ble_server_set_quiet`
   semantics); clearing it resumes. The Muse app should pause advertising
   while the user is actively in BT Music, and during any Wi-Fi-heavy
   operation where the radio matters. This component does **not** gate
   advertising on the A2DP state itself — it deliberately does not depend
   on `app_services`, keeping the component standalone.
5. **One GATT connection at a time.** Undirected advertising stops when a
   client connects; a second central cannot connect until the first
   disconnects. A stale link is reclaimed by the idle timeout (§3.3).
6. **No BLE bonding.** Pairing is app-layer only (pairing-v5 records).
   The component never enables BLE security / bonding, and never touches
   the Classic Just-Works SSP pairing `bt_service` uses.
7. **BTC task discipline.** Bluedroid GAP/GATTS callbacks run on the BTC
   task, which is shared with A2DP/AVRCP. This component's callbacks only
   forward events to its own dispatch task (queue + copy); all JSON
   parsing, ECDH, and GCM work runs on the dispatch task. (This is
   deliberate deviation #1 from the SDK, which dispatches synchronously
   on the dedicated NimBLE host task.)
8. **Why BLE-over-extconn should work.** The coprocessor link
   (`esp-extconn`, `src/trans_bt.c`) attaches a generic HCI driver via
   `esp_bluedroid_attach_hci_driver()` and forwards raw HCI packets over
   SDIO — it does not filter by Classic vs LE. BLE HCI (LE commands,
   events, ACL) flows through the same pipe unchanged. The remaining
   question is whether the coprocessor firmware blob (`eagle_fw.h`)
   includes a BLE-capable controller — see §7, item 1. The coex shim
   already reports a BLE status field (`esp_sip.c: coex_evt->ble_st`),
   which is encouraging but not proof.

## 7. What could NOT be verified (no hardware, no Muse app)

1. **Whether the ESP32 coprocessor firmware includes a BLE controller.**
   The make-or-break question. If the downloaded `eagle_fw.h` firmware was
   built Classic-only, Bluedroid BLE init will fail (or LE HCI commands
   will go unanswered) and this whole approach needs a different
   coprocessor firmware — or a fallback to out-of-band provisioning (plan
   R1 option b). First hardware test: enable BLE, watch the boot log for
   Bluedroid/BTC errors and for the `muse.ble: pairing GATT service
   started` line.
2. **Bluedroid BLE + Classic coexistence on this stack.** Dual-mode
   operation, the coex scheduler, and A2DP-while-advertising behavior are
   all unverified. Test: stream BT Music while the pairing screen
   advertises; check for audio glitches and for failed discovery.
3. **Whether the Muse app discovers and pairs with a Bluedroid GATT
   server.** Service/characteristic layout, advertising bytes, framing,
   and records are byte-identical to the SDK's NimBLE server, but the
   app's discovery/filtering and its tolerance for Bluedroid timing
   (notification pacing, MTU negotiation, long-write behavior) are
   unverified. Test end-to-end with the real app.
4. **ESP-IDF v6.1 Bluedroid API signatures (verified).** The code uses
   the `esp_gap_ble_api.h` / `esp_gatts_api.h` / `esp_gatt_common_api.h`
   surface as of IDF v6.1; the four uncertain call sites (adv params,
   `config_adv_data_raw`, `set_local_mtu`, `set_device_name`) were
   verified against the installed ESP-IDF v6.1.1 headers at integration
   time. Remaining risk is runtime behavior, not signatures.
5. **`mbedtls/private/gcm.h` under IDF v6.1.** The vendored
   `link_pairing.c` includes it (as the SDK does on IDF v6.0.3). If v6.1's
   mbedtls moved or removed that header, switch the include to
   `mbedtls/gcm.h` + `mbedtls/private_access.h` (the private-access
   macros are the only thing the file needs from it, for
   `shared.MBEDTLS_PRIVATE(X)`).
6. **MTU negotiation.** The component proposes a local MTU of 512. Whether
   the coprocessor controller + Bluedroid honor it, and whether the Muse
   app chunks correctly at whatever MTU results, is unverified. The
   framing code adapts to the negotiated MTU (like the SDK), so a smaller
   MTU only costs speed.
7. **Advertising flags `0x06`.** The SDK advertises BR/EDR-Not-Supported
   because its boards have no Classic BT. This board *does* (A2DP), so
   `0x06` is strictly a lie told for byte-parity with SDK gadgets. The
   honest dual-mode value is `0x02`. If the app misbehaves (or a phone's
   BT settings hide the Classic speaker), flip the one byte in
   `build_adv_data()` and re-test.
8. **GATT edge behavior.** Prepare-write (long write) handling,
   `RSP_BY_APP` response timing, CCCD read-back, and the
   `esp_ble_gatts_send_indicate` congestion behavior are all written from
   the IDF API docs, not observed. The Muse app's chunked writes (≤160 B)
   should never exercise the long-write path.
9. **Advertising intervals and discovery timing.** 20–40 ms was chosen to
   match NimBLE's fast setup advertising; whether the app discovers
   promptly (and whether the interval needs tuning for the coprocessor
   radio scheduler) is unverified.
10. **Settings "Bluetooth off" vs BLE pairing.** `bt_service_set_enabled
    (false)` only changes the Classic scan mode; it does not stop
    Bluedroid, so BLE advertising would continue with Bluetooth "off" in
    Settings. The MuseApp should check `bt_service_is_enabled()` before
    starting advertising — or the project should decide that the Settings
    toggle means Classic-only. Open UX decision for the parent.
11. **Node-id MAC source (decided).** The P4 eFuse MAC
    (`esp_read_mac(ESP_MAC_WIFI_STA)`) is used, not the coprocessor STA
    MAC the SDK uses — see §3.1. Stable and available before Wi-Fi.
12. **RAM/flash cost** of the BLE host (§2) — measure at build time.
13. **OTA during pairing.** `device.ota` is answered
    `error_unknown_action` and `provision_v2`'s `ota_url` is ignored.
    The SDK treats OTA as optional, but if the Muse app *requires* an OTA
    step for some flows, pairing would stall. Verify against the real app.
14. **`get_device_info` field set.** The sibling implements the reply;
    verify the app accepts it (fields listed in §3.5 / §5).
15. **First-run UX details** (pairing screen copy, confirm prompt timing,
    what happens if the user leaves the screen mid-pairing) belong to the
    MuseApp and are not covered here: leaving the screen should call
    `muse_ble_pairing_stop_advertising(true)`.

## 8. First hardware test checklist

1. Build the factory project with the component + §2 config; confirm no
   new warnings-as-errors and note the `idf.py size` delta.
2. Flash; watch the boot log for Bluedroid/BTC errors and for
   `muse.ble: pairing GATT service started (rx=… tx=… cccd=…)`.
3. Open the Muse app's pairing screen: log should show
   `muse.ble: advertising as MuseGadget-XXXXXX`; a BLE scanner app should
   see the service UUID `7fdd3d1c-…` and the name.
4. With BT Music streaming, confirm audio is unaffected while advertising.
5. Pair from the Muse app; watch the serial log for each stage:
   `RX action: pairing_client_hello` → `pairing_ready sent` →
   `RX action: pairing_encrypted` → `confirm_required sent` →
   (on-device confirm) → `pairing confirmed` →
   `provision_v2 accepted` → `auth_ok` → advertising stops.
6. Reboot; confirm the device reconnects to its VM with no re-pairing
   (Phase 2.2/2.3 work) and that the pairing screen no longer offers
   pairing (provisioned state).
7. Unpair from the app; confirm credentials are erased and pairing is
   offered again.

Useful log tags: `muse.ble` (this component), `link.pairing` (vendored
crypto), `bt_service`, `BTC_TASK` / `BT_HCI` (Bluedroid). Tokens and
passwords are never logged by this component — if a log line ever shows
one, that is a bug to fix, not a debugging aid.
