# Vendored SDK code — Muse app

The Muse app vendors parts of the Muse gadget SDK into this factory
firmware tree, because the SDK targets its own boards (ESP32-S3 voice
boards with XMOS) and cannot be used as-is on the ESP32-P4 knob board.

- **SDK source:** https://github.com/koneho-ai/muse-gadget-sdk
- **SDK commit vendored:** `6a44074` (2026-11-17, "Add Sony Bravia TVs
  skill (#434)")
- **SDK license:** Apache-2.0 (see the `SPDX-License-Identifier` headers
  in the vendored files; `minimp3` keeps its own LICENSE).
- **Vendored on:** 2026-11-18, onto factory branch `muse-app`.

> **UNTESTED ON HARDWARE** — no board exists yet. None of this has run on
> a device; compile verification is the bar so far, and even a full
> `idf.py build` had not completed at the time of writing.

## What was copied, per component

### `components/muse_noise_core/` — Noise_XX_25519_AESGCM_SHA256

From `sdk-ref/esp32/components/noise_core/`, copied verbatim (headers
under `include/xplat/noise/core/`, sources under `src/`):

- `ClientSession.cpp`, `InitiatorHandshake.cpp`, `Transport.cpp`,
  `TransportFrameCodec.cpp`, `ServiceCodec.cpp`, `PsaCryptoBackend.cpp`,
  `MbedtlsCryptoBackend.cpp`, `Status.cpp`
- Headers: `BufferWriter`, `ClientSession`, `CryptoBackend`,
  `InitiatorHandshake`, `MbedtlsCryptoBackend`, `PsaCryptoBackend`,
  `Secret`, `ServiceCodec`, `Span`, `Status`, `StringView`, `Transport`,
  `TransportFrameCodec`

Changes vs the SDK:

- `CMakeLists.txt` is adapted from the SDK's: same sources plus
  `MbedtlsCryptoBackend.cpp` and `Status.cpp` (the SDK's CMakeLists lists
  neither — Status is needed by the transport, MbedtlsCryptoBackend is
  compiled per the task spec). C++17, `-fno-exceptions -fno-rtti`,
  `PRIV_REQUIRES mbedtls`.
- `idf_component.yml` is new (no managed dependencies).

**Compile risk:** `MbedtlsCryptoBackend.cpp` uses
`MBEDTLS_ALLOW_PRIVATE_ACCESS` plus MbedTLS `ecp` private fields. It is
compiled but nothing selects it at runtime (the session uses the PSA
backend); if MbedTLS in ESP-IDF v6.1 moved those fields, drop it from
`SRCS` — the PSA backend is the only one the session needs.

### `components/minimp3/` — MP3 decoder

From `sdk-ref/esp32/components/minimp3/`, copied verbatim:
`include/minimp3.h`, `src/minimp3.c`, `LICENSE`, `README.md`. The
`CMakeLists.txt` is the SDK's (`-O2 -Wno-unused-function`);
`idf_component.yml` is new.

Used by the session core's MP3 self-test (`muse_hatch_mp3_selftest`) and
by the reply-decode path. In v1 nothing feeds MP3 into the decoder (see
"Replies are text-only" below), but the path is vendored intact.

### `components/muse_session/` — the Muse chat session core

From `sdk-ref/esp32/components/muse/`, following the SDK's own headless
Voice-PE build file list (`CONFIG_HOMEHUB_VOICE` branch of the SDK's
`muse/CMakeLists.txt`, with `sdk-ref/esp32/main/voice_muse_chat.c` as
the stub pattern). Copied verbatim:

- `muse_chat_session.cpp` (3,019 lines — the whole session: one
  HTTP-over-Noise connection, TLS + WS upgrade on `/v1/noise`,
  Noise_XX handshake, `POST /api/voice/dictation`, `POST /chat/stream`,
  `POST /chat/subscribe`)
- `muse_chat.c` / `muse_chat.h` (public API: start/status/test,
  turn begin/audio/end/cancel, turn events, incoming, captions,
  `muse_hatch_turn_read`, MP3 self-test)
- `muse_chat_priv.h`, `muse_chat_text.c`, `muse_text.c` / `muse_text.h`,
  `muse_options.c` / `muse_options.h`, `muse_options_json.c`,
  `muse_account_api.c` / `muse_account_api.h`, `muse_mem.h`
- `test_reply.mp3` (22,125 bytes, embedded for the MP3 self-test)

New / rewritten for the factory firmware:

- `muse_factory_glue.c` — implements the session's external hooks
  against factory services: `muse_wifi_connected()` from `wifi_service`,
  settings/VM/token from the `muse_vm` RAM cache, `muse_state_page()`
  (26 cols × 4 lines, matching the MuseUi reply label), Link hooks
  stubbed to "not linked".
- Slim headers replacing SDK ones the session includes but that carried
  board/UI dependencies: `muse_link.h` (only `muse_link_hatch_linked` /
  `muse_link_hatch_vm`), `muse_settings.h` (only the four `hatch`
  getters + size bounds), `muse_wifi.h` (only `muse_wifi_connected`),
  `muse_state.h` (only `muse_state_page`).
- `Kconfig` — the five `CONFIG_MUSE_*` symbols the vendored sources
  read: `MUSE_HATCH_COMPACT_BUFFERS` (n), `MUSE_REALTIME_VOICE` (n),
  `MUSE_REALTIME_VOICE_APP_ID` ("hatch-web"), `MUSE_BOARD_SENSECAP_WATCHER`
  (n), `MUSE_LATEST_REPLY` (n).
- `CMakeLists.txt` — `SRCS` incl. the glue, `EMBED_FILES test_reply.mp3`,
  `PRIV_REQUIRES` incl. `muse_noise_core`, `muse_vm`, `minimp3`, cJSON,
  esp_http_client, esp-tls, mbedtls. C++17.
- `idf_component.yml` — `espressif/cjson ^1.7.19`.

Stripped (not vendored): `muse_audio.c` (board audio — the factory
`audio_client` API replaces it), `muse_ui.c` / avatar, `app.c`
orchestration, `ble_server.c` / `link_pairing.c` (BLE pairing is a
separate component, `MuseBlePairing`), `noise_control.cpp` / tunnel,
`image_fetch`, OTA, Opus, `muse_voice_call` realtime sources,
`muse_wakeword`.

#### Two behaviors that differ from the plan's description

1. **Voice-note mode, not streaming dictation.** The vendored session
   defaults to `VOICE_NOTE=1`: "The VM's streaming dictation has no ASR
   behind it right now, so each press goes to the chat as a voice note"
   — a base64 WAV in `POST /chat/stream`. The streaming path to
   `POST /api/voice/dictation` exists in the same file behind
   `VOICE_NOTE=0`. Kept as shipped; flip it only once the VM has ASR.
2. **Replies are TEXT ONLY in v1.** `start_tts()` plays silence paced
   to the reply text: "Muse doesn't speak gadget replies... send the
   message's text to a TTS API of your own". The K_TTS stream handling,
   `tts_data()`, `decode()`, and minimp3 machinery exist but nothing
   opens a K_TTS stream, so `muse_hatch_turn_read()` returns silence
   that paces the captions. A TTS service would plug into `start_tts()`.

Other session facts the app relies on:

- The session task ("muse_chat", 48 KB stack in PSRAM, core 0, prio 5)
  must never read flash: everything it reads (token/host/VM) comes from
  the `muse_vm` RAM cache, never NVS directly.
- `resolve_vm()`: own-token path only in this firmware (Link is
  stubbed) — `GET https://api.muse.ai/fetch_vms`, then mint. A
  VM-token 401/403 forgets the VM and retries once with a fresh fetch.
- A device-token rejection surfaces as status detail "Token rejected";
  the session does not refresh device tokens itself — the app does
  (see `MuseApp`, token-refresh task).
- `muse_hatch_chat_forget()` (CMD_FORGET) fails the active turn,
  disconnects, and forgets the VM credentials — the app-hidden teardown.
- `post()` blocks up to 100 ms on a full queue: never call session
  functions from the audio preempt callback; set flags and post a
  control event instead.

### `components/muse_vm/` — VM account + credential store (slimmed)

From `sdk-ref/esp32/main/`, slimmed to what the app needs:

- `vm_api.c` / `vm_api.h` — copied verbatim: `fetch_vms` / mint /
  refresh with retries; `Bearer hatch_refresh:<token after last colon>`
  refresh bearer; 401 on mint/refresh = `VM_API_ERR_REVOKED`.
- `config_store.c` / `config_store.h` — rewritten slim: NVS namespace
  **`museapp`** (not the SDK's `homehub`), no NVS-encryption logic, no
  `nvs_flash_init` (the factory `main.cpp` owns NVS init), no Wi-Fi
  keys, no `config_clear_setup`; `clear_pairing` also clears `vm_id`.
- `identity.c` / `identity.h` — rewritten slim: node id from the STA
  MAC (`esp_read_mac(ESP_MAC_WIFI_STA)` — the P4 eFuse MAC, which may
  differ from the coprocessor's STA MAC; documented in the code), node
  prefix `homelink` (the SDK production default; `gadget-` is bench-only), BLE name prefix `MuseGadget` (hardcoded, was a
  Kconfig), keeps `CONFIG_GADGET_SDK_TOKEN` plus the sdk-token
  validate/set/note/changed helpers. The ESP-Hosted branch is dropped.
- `muse_credentials.c` / `muse_credentials.h` — new: the RAM cache
  (access/refresh tokens 1024 B, host 64, VM 64, API base 256,
  mutex-protected) plus init/load/store/refresh/unpair/preferred-VM.
  Refresh is tri-state (`MUSE_VM_REFRESH_OK` / `FAILED` / `REVOKED`)
  so the app can tell "retry later" from "pairing is dead".
- `muse_provisioning.c` / `muse_provisioning.h` — new: the shared
  provisioning interface the BLE pairing component calls (see below).
- `Kconfig` — `CONFIG_GADGET_SDK_TOKEN` (string, default empty).
- `CMakeLists.txt`, `idf_component.yml` (`espressif/cjson ^1.7.19`).

### `components/MuseBlePairing/` — BLE pairing v5 GATT server (Bluedroid port)

From `sdk-ref/esp32/main/`, ported (not copied verbatim) because the SDK's
pairing server is NimBLE and this firmware runs Bluedroid for Classic BT
— the two hosts cannot coexist, and NimBLE has no Classic BT. Full
record: `components/MuseBlePairing/VENDORED.md` and
`components/MuseBlePairing/INTEGRATION.md`.

- `muse_ble_pairing.c` — new code: a Bluedroid GATT server implementing
  the SDK pairing service byte-for-byte at the wire level (same service
  UUID `7fdd3d1c-…`, same `0xFE` chunk framing, same JSON actions/status
  strings/timeouts as `ble_server.c`).
- `vendored/link_pairing.c` / `pairing_transcript.c` — the pairing-v5
  crypto (P-256 ECDH + HKDF-SHA256 + AES-256-GCM), adapted to
  community-only (the eFuse manufacturer-attestation path removed; the
  wire protocol is byte-identical). Transcript verbatim.
- Deliberate deviations from the SDK server: dispatch runs on a dedicated
  worker task, not the BTC task (shared with A2DP); advertising and
  connection-idle timeouts added (600 s / 180 s defaults); no OTA
  (`device.ota` → `error_unknown_action` — no OTA slots on this board).
- The component never touches NVS: credentials go through
  `muse_vm/muse_provisioning.h`, implemented by `muse_vm` and driven by
  the MuseApp (see "Provisioning interface" below). Its standalone copy
  of that header was removed at integration time to avoid a duplicate.
- sdkconfig: `CONFIG_BT_BLE_ENABLED=y` + `CONFIG_BT_GATTS_ENABLE=y`;
  Classic/A2DP unchanged.

**Make-or-break unknown:** whether the ESP32 coprocessor firmware blob
includes a BLE controller at all — only hardware can answer that. If it
is Classic-only, BLE pairing cannot work on this stack and provisioning
falls back to the out-of-band path (plan R1 option b).

### `components/MuseApp/` — the Brookesia app (new, not vendored)

Written from scratch, modeled directly on the factory `XiaozhiApp`
(same lifecycle hardening, audio-client and knob patterns, same LVGL
look). See `components/MuseApp/README.md`.

## Provisioning interface

The BLE pairing component (`MuseBlePairing`, built by a sibling agent)
needs only `muse_vm`'s `muse_provisioning.h` (`REQUIRES muse_vm`) and
must not touch the NVS namespace or the RAM cache directly:

```c
bool muse_provisioning_set_device_tokens(const char *access_token,
                                         const char *refresh_token,
                                         const char *node_id,
                                         const char *api_url_v2,
                                         const char *noise_host);
bool muse_provisioning_is_provisioned(void);
bool muse_provisioning_unpair(void);
const char *muse_provisioning_node_id(void);   /* setup id "homelink-XXXXXX" */
const char *muse_provisioning_ble_name(void);  /* "MuseGadget-XXXXXX" */
const char *muse_provisioning_sdk_token(void); /* maker token, never logged */
```

Tokens are stored in NVS namespace `museapp` and mirrored in the RAM
cache; they leave the device only in the `Authorization` header of
`vm_api` requests. They are never logged and never committed.

## Not vendored (deliberate omissions)

- Realtime voice calls (`muse_voice_call`, `CONFIG_MUSE_REALTIME_VOICE`
  sources) — Phase 4.
- Wake word (`muse_wakeword`, WakeNet) — v1 is tap-to-talk only.
- Spoken replies / TTS — see "Replies are text-only" above.
- Link control session (`muse_link` ops beyond the two stubs) — the
  factory firmware has no Link; own-token path only.
- OTA, Opus, `image_fetch`, `noise_control`/tunnel, `muse_ui`/avatar,
  `muse_audio.c`, board drivers, `app.c` orchestration.
- Power saving: `muse_hatch_set_resting` is never called; the session
  idles with its 10-minute idle-close timer.

## Known compile risks (not verifiable without a build)

- **MbedtlsCryptoBackend on IDF v6.1** — uses `MBEDTLS_ALLOW_PRIVATE_ACCESS`
  and MbedTLS `ecp` private fields; MbedTLS in IDF v6.1 may have moved
  them. Drop it from `muse_noise_core` SRCS if it fails (unused at
  runtime).
- **PSA Crypto backend on IDF v6.1** — the PSA calls used
  (`psa_import_key`, `psa_aead_encrypt/decrypt`, `psa_raw_key_agreement`,
  etc.) are stable PSA Crypto API, but the SDK pins IDF v6.0.3 ("other
  versions unsupported") and this firmware targets v6.1.
- **C++ standard skew** — the factory project defaults to C++26
  (esp-boost pinned to C++23); the vendored session/noise sources are
  built as C++17 via their component CMakeLists. If the C++17 setting
  does not propagate as expected, the vendored sources may see C++26
  (or the reverse — a C++26-only construct leaking into them).
- **`esp_ae_rate_cvt` API** — the resample pattern is copied from
  XiaozhiApp, but the exact `esp_ae_rate_cvt_process` signature and
  `esp_ae_sample_t` type are taken from the factory's
  `esp_audio_effects ^1.3.0`; a version bump could change them.
- **LVGL 9.5.x arc/transform APIs** — MuseUi uses the arc widget and
  label APIs the same way XiaozhiUi does; the factory uses
  `esp_lvgl_adapter 0.7.3` vs the SDK's 0.6.4, same major line.
- **`xTaskCreatePinnedToCoreWithCaps` / `xQueueCreateWithCaps` /
  `xStreamBufferCreateWithCaps`** — from `freertos/idf_additions.h`;
  availability in IDF v6.1 is assumed from the SDK's v6.0.3 usage.
- **`esp_read_mac` component deps** — `identity.c` uses it; if the
  linker complains, add `esp_system`/`efuse` to `muse_vm`'s
  `PRIV_REQUIRES`.
- **cJSON provider** — both `muse_session` and `muse_vm` declare
  `espressif/cjson ^1.7.19` in their `idf_component.yml`, matching
  XiaozhiApp's pattern; a version conflict with the factory's other
  cJSON users would surface at `idf.py` configure time.
