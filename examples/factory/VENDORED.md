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
  `Status.cpp` (the SDK's CMakeLists lists neither it nor
  `MbedtlsCryptoBackend.cpp` — Status is needed by the transport).
  C++17, `-fno-exceptions -fno-rtti`, `PRIV_REQUIRES mbedtls`.
- `MbedtlsCryptoBackend.cpp` is vendored but **not compiled**: it
  includes `<mbedtls/ctr_drbg.h>`, which Mbed TLS 4.x (ESP-IDF v6.1)
  removed (DRBG is PSA-only there), so the first integrated build
  failed on it (2026-11-18). Nothing references it — the session uses
  the PSA backend — so it was dropped from `SRCS` and kept in the
  tree only to keep the vendored component whole.
- `idf_component.yml` is new (no managed dependencies).

### `components/minimp3/` — MP3 decoder

From `sdk-ref/esp32/components/minimp3/`, copied verbatim:
`include/minimp3.h`, `src/minimp3.c`, `LICENSE`, `README.md`. The
`CMakeLists.txt` is the SDK's (`-O2 -Wno-unused-function`);
`idf_component.yml` is new.

**Factory change (not in the SDK): the decoder symbols are renamed.**
The factory firmware also links Espressif's `esp_audio_codec`, whose
prebuilt MP3 decoder embeds the same minimp3 implementation — defining
`mp3dec_init`/`mp3dec_decode_frame` here collided at link time (first
seen in the integrated build, 2026-11-18). `minimp3.c` defines
`mp3dec_init`/`mp3dec_decode_frame` as `muse_mp3dec_init` /
`muse_mp3dec_decode_frame`, and `muse_chat_session.cpp` applies the
same defines before its `#include "minimp3.h"`. Sharing the codec's
copy was rejected: its embedded minimp3 version is unverifiable from
the prebuilt archive, and a struct-layout mismatch would corrupt the
decode state. A second ~30 KB decoder in 32 MB of flash is cheap
insurance.

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
- Realtime voice calls (Phase 4.1, 2026-11-20): `muse_voice_call.cpp`
  (1,577 lines), `muse_voice_call_core.c` (1,886) + `.h` (511),
  `muse_voice_call.h` (139), `muse_voice_call_priv.h` (66) — 4,179
  lines, verbatim. `muse_aec.c` deliberately NOT vendored (see below).

New / rewritten for the factory firmware:

- `muse_factory_glue.c` — implements the session's external hooks
  against factory services: `muse_wifi_connected()` from `wifi_service`,
  settings/VM/token from the `muse_vm` RAM cache, `muse_state_page()`
  (26 cols × 3 lines, matching the MuseUi caption box), Link hooks
  stubbed to "not linked".
- Slim headers replacing SDK ones the session includes but that carried
  board/UI dependencies: `muse_link.h` (only `muse_link_hatch_linked` /
  `muse_link_hatch_vm`), `muse_settings.h` (only the four `hatch`
  getters + size bounds), `muse_wifi.h` (only `muse_wifi_connected`),
  `muse_state.h` (only `muse_state_page`, plus the `muse_mode_t` enum —
  the avatar renderer includes this header for it, so the enum lives
  here as the single copy).
- `Kconfig` — the `CONFIG_MUSE_*` symbols the vendored sources
  read: `MUSE_HATCH_COMPACT_BUFFERS` (n), `MUSE_REALTIME_VOICE` (y in
  `sdkconfig.defaults` since Phase 4.1), `MUSE_REALTIME_VOICE_APP_ID`
  ("hatch-web"), `MUSE_REALTIME_VOICE_IDLE_S` (45),
  `MUSE_REALTIME_VOICE_MAX_S` (900), `MUSE_REALTIME_VOICE_GREETING`
  (n), `MUSE_APP_VIEW` (hidden, n), `MUSE_BOARD_SENSECAP_WATCHER`
  (n), `MUSE_LATEST_REPLY` (n).
- `CMakeLists.txt` — `SRCS` incl. the glue, `EMBED_FILES test_reply.mp3`,
  `PRIV_REQUIRES` incl. `muse_noise_core`, `muse_vm`, `minimp3`, cJSON,
  esp_http_client, esp-tls, mbedtls. C++17. The call sources join `SRCS`
  under `if(CONFIG_MUSE_REALTIME_VOICE)` (the SDK's own pattern).
- `idf_component.yml` — `espressif/cjson ^1.7.19`.

Stripped (not vendored): `muse_audio.c` (board audio — the factory
`audio_client` API replaces it), `muse_ui.c` (MuseUi re-implements its
full layout; the avatar renderer itself IS vendored — see
`muse_avatar` below), `app.c` orchestration, `ble_server.c` /
`link_pairing.c` (BLE pairing is a separate component, `MuseBlePairing`),
OTA, Opus, `muse_aec.c` (see "Realtime voice calls" below).
`noise_control.cpp` / tunnel and `image_fetch` live in their own
component now — see `muse_link` below — as does `muse_wakeword` (see
`muse_wakeword` below). The slim `muse_link.h` stubs
in `muse_factory_glue.c` report the provisioning state and resolve
the VM through the account API (Phase 4.3).

One deliberate boundary crossing: `MuseClient::stop()` includes
`muse_chat_priv.h` to call `muse_hatch_chat_forget()` (drop the Noise
connection + cached VM credentials on app hide / unpair / token
revocation). The SDK keeps it private because its gadget never
disconnects; the factory app's lifecycle needs the explicit disconnect.
The session task itself stays alive across it (`muse_hatch_start()` is
idempotent) and reconnects on the next `muse_hatch_test()` /
`muse_hatch_config_changed()`.

One deviation in the otherwise-verbatim `muse_chat_session.cpp`: two
`#define`s before `#include "minimp3.h"` rename `mp3dec_init` /
`mp3dec_decode_frame` to `muse_mp3dec_*` — see the `minimp3` section
above for why.

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

#### Realtime voice calls (Phase 4.1, 2026-11-20)

`POST /voice/realtime` over the voice session's Noise connection,
NDJSON both ways, 40 ms appends of 24 kHz PCM16LE base64 up,
`continuous_audio_video.delta` audio down; every exit sends
`POST /voice/end-call` (an unconfirmed end-call is kept as one pending
cleanup in RAM and retried on reconnect, up to 3 tries / 10 minutes).
The vendored `muse_chat_session.cpp` already contained every call hook
— no change to that file was needed. The app side is new factory code:
`MuseClient` wraps the call API, and `MuseApp::callAudio` is a
dedicated call task (the SDK's `run_call`, hands-free only) that pumps
one 10 ms pass per loop — read mic, drain call events, feed gated mic
audio, read reply audio, write it — using the same audio client and
resamplers as the turn tasks, which idle during a call. A call reuses
the Listening/Thinking/Speaking states with an `_in_call` flag (tap
hangs up in a call; `CallConnecting` covers setup), and the WORKING
pose (`pose.working`, `MuseUi::setWorking`) shows when Muse still has
a caller turn open. A turn and a call must never overlap — the session
has no guard, so the app enforces it (`startTurn` refuses in a call;
`startCall` closes any turn first). `muse_voice_call_device_id()` is
provided by `MuseClient.cpp`, returning `muse_vm_node_id()` (RAM-cached,
safe on the session task). No PTT call yet: the UI has no press/release
events.

- **Half-duplex, no AEC.** `muse_aec.c` needs mic + the exact
  just-played samples paired once per 20 ms pass on a single task, plus
  a fixed bench-measured reference delay. This board has neither: mic
  and speaker live on separate unsynchronised tasks, the playback
  reference delay is unknown and variable (no hardware loopback, no
  board to measure on), Xiaozhi's own code already concluded "the
  full-duplex one needs a playback reference this board does not have",
  and the SDK's `MUSE_VOICE_SOFTWARE_AEC` depends on
  `IDF_TARGET_ESP32S3`. So the SDK's standard mic gate applies
  (`muse_vc_gate_t`): the mic stays shut while Muse is audibly talking
  plus a 300 ms tail, with a 120 ms speaker-lag estimate — an SDK board
  value that may need retuning for this audio-service path once
  hardware exists. AEC stays a future experiment (single duplex task,
  bench-measured delay, esp-sr 2.4.7-vs-2.5.2 decision).
- **app_id side effect.** With `MUSE_REALTIME_VOICE` on, the session
  connects with `app_id=hatch-web` for EVERYTHING on that connection —
  calls, voice notes and typed turns included. Inherent to the feature,
  not optional; the VM refuses calls without one.
- **Server side unverified.** The VM must accept calls from the
  gadget's credentials; no board exists to test against. Greeting off
  (SDK default); idle hang-up 45 s, max call 900 s (SDK defaults).
- **Call buffers** are PSRAM-resident by design (~700 KB: 192 KB in,
  128 KB out, 2×160 KB PTT rings, 32 KB line buffer, ~24 KB decode,
  ~17 KB reply gate, ~8 KB turn table) — comfortable on 32 MB PSRAM.

### `components/muse_avatar/` — the Muse character (2026-11-20)

From the SDK, copied verbatim (SDK commit `6a44074`):

- `muse_pixel.c` (40,319 bytes) — the procedural pixel-art renderer:
  a 64×64 indexed grid (Jollybot, Meta's character) with an animation
  for each state (boot, idle, listening, thinking, speaking, error,
  off, happy). From `sdk-ref/esp32/avatar/muse_pixel.c`.
- `muse_pixel.h` / `muse_speaking.h` — the renderer's API
  (`muse_pixel_render`, `muse_pixel_scale`, `muse_pixel_set_size`,
  `muse_pixel_accent`, `muse_pose_t`) and its speaking choreography.
  From `sdk-ref/esp32/components/muse/`.
- `happy_anim.c` (546,254 bytes) / `happy_anim.h` — the 75-frame happy
  animation (56×59, 40 ms/frame, ~242 KB rodata), generated from
  `jollybot.gif` by the SDK's `tools/gen_happy_anim.py`. From
  `sdk-ref/esp32/avatar/`.

The avatar is **procedural pixel art, not video**: there is no avatar
video in the SDK to port. The SDK's `libvpx_vp8` VP8 decoder belongs to
its phone-camera streaming feature (`MUSE_VIDEO`, opt-in), not to the
avatar. `happy_anim` is used only by the SDK's non-LVGL status displays
(`main/epaper_status.c`, `main/led_status.c`) — the LVGL face never
draws it, here or in the SDK. It is vendored for asset parity and
compiles here (guarded by `CONFIG_HOMEHUB_DISPLAY`, as in the SDK);
the linker drops the unreferenced frames, exactly as the SDK's
LVGL-board builds do (verified: `happy_anim_frames` is absent from
the linked ELF — see the build note below).

Licensing: the Apache License does not cover the Jollybot avatar in
`avatar/`; its files carry only a Meta copyright line — no Apache
header was added to them. `muse_pixel.h` includes `muse_state.h` for
`muse_mode_t`; the slim `muse_state.h` in `muse_session` carries that
enum (from the SDK's `muse_state.h`).

New for the factory firmware: `CMakeLists.txt` (with the SDK's
custom-avatar override — a `avatar/muse_pixel.c` in this component,
gitignored, replaces the default renderer, the way the SDK's
`tools/muse/avatar.py` does), `Kconfig` (`CONFIG_HOMEHUB_DISPLAY`,
default y, so the vendored files compile verbatim),
`idf_component.yml`, `README.md`.

`MuseUi` drives the renderer the way the SDK's `muse_ui.c` does: a
custom LVGL image decoder scales the grid up in 16-row strips, a
40 ms timer builds the pose and re-renders, a cell-diff invalidates
only the changed cells, and the 458 px ring + 14-segment meter follow
the SDK's behaviour (progress while listening/speaking, 60° spinner
while thinking). Deliberate deviations: no volume swipe (the knob is
the volume control), no speaker/mute button, no settings/latest-reply
tile, one caption box for transcript + reply, pet on long-press
(the SDK's face tap is pet, but here a tap talks), Montserrat 16 for
the state label (no Unscii in the factory LVGL).

### `components/muse_wakeword/` — "Hey Muse" wake word (Phase 4.2, 2026-11-20)

From `sdk-ref/esp32/components/muse_wakeword/`, verbatim except the
Kconfig and CMakeLists (see below):

- `muse_wakeword_core.c` / `include/muse_wakeword.h` — the session
  state machine (IDLE/ARMED/CAPTURING/CAPTURING_PTT), the 1 s pre-roll
  ring, the numbered-chunk history, the energy fallback. Pure C, no
  IDF types.
- `ww_runner_espnn.cpp` / `include/ww_runner.h` — the ExecuTorch
  runner (microfrontend + `models/hey_muse.pte` through the ESP-NN
  delegate + `ww_detector.c`). Zero target-specific code; the P4 is
  covered by ExecuTorch's nightly CI (build-only upstream — the
  runtime on P4 is unproven until hardware exists).
- `ww_frontend.c` / `ww_detector.c` (+ headers), `ww_model.h`,
  `models/hey_muse.pte` (23,492 bytes, int8 microWakeWord streaming
  MixedNet, phrase "Hey Muse" only — 73.9% caught, 0.38 false
  wakes/hour at the Default cutoff 0.22, measured by the SDK on
  synthetic speech), `models/hey_muse.json`, `models/README.md`,
  `models/ATTRIBUTION.md`, `README.md`, `hey_muse_pte.S.in` (the
  model is embedded 16-aligned in flash; `EMBED_FILES` has no
  alignment directive and ExecuTorch `Program::load` requires
  aligned program data), `idf_component.yml` (verbatim: registry
  deps `pytorch/executorch ==1.6.0-dev.20261112`,
  `espressif/esp-nn ==1.5.0`, `esphome/esp-micro-speech-features
  ==1.2.3`, gated on `CONFIG{MUSE_WAKEWORD_EXECUTORCH}`).
- Kconfig adapted: the menu depends on `MUSE_APP_ENABLED` (the
  factory equivalent of the SDK's `MUSE_ENABLED`); the FreeInk
  exclusion is dropped (no e-paper UI here). Sensitivity stays a
  choice (Low/Default/High); the timeouts stay Kconfig ints.
- CMakeLists adapted: same sources and the same aligned `.pte`
  embed; `executorch_add_backend(EspNNBackend)` (the model runs
  entirely in the delegate, so no kernel bundle and no executorch
  Python at build time); `-ffp-contract=off` on `ww_frontend.c`;
  `cxx_std_17` like `muse_session`. With `MUSE_WAKEWORD_ENABLE=n`
  the component registers empty (no sources, no include dirs) and
  the registry deps are not downloaded.

The glue is factory-new (`components/MuseApp/muse_wakeword_glue.h` /
`.cpp`), modeled on the SDK's `components/muse/muse_wakeword_glue.c`,
which cannot be ported as-is (it drives the SDK's `muse_voice` /
`muse_input` / `muse_state` tasks, none of which exist here). Same
shape: a single wake task (8 KB PSRAM stack, prio 5, core 0), a
320-frame tap stream buffer (25 chunks), a 1 s pre-roll ring, a
32-slot numbered-chunk history, the same core tick / apply-requests /
ARMED wall-clock timeout / tap-gap handling, the same disable
generation (`muse_wakeword_turn_wanted`). Factory seams, each
documented in the glue header: turns open through callbacks that post
MuseApp control events; the pre-roll seed is fed by the input task
after `beginTurn()` (the session resets its mic buffer at turn start,
so a seed fed earlier would be discarded) via
`MuseClient::feedSeedAudio` (`muse_hatch_turn_audio_wait`, 100 ms);
listening is gated on app visibility in addition to the opt-in; the
tap accumulates the resampler's variable frame counts into exact
320-frame chunks.

Behavior (all from the SDK core, unchanged): a wake arms the core
("Hey Muse heard / Speak now"); speech within 2.5 s opens a
hands-free turn whose note starts with the pre-roll (up to 1 s before
the speech, so it includes the wake word) plus the backlog chunks the
input task tapped while the wake task caught up — no gap, no repeat.
The turn ends on 700 ms of trailing silence, at the 15 s cap, on a
tap (which submits it), or when listening is switched off (which
drops it unsent). A wake with no speech sends nothing. A tap while
armed cancels the wake. Own-voice playback and push-to-talk turns
suppress the runner; a call suppresses it too (the call task owns
the mic then). The opt-in persists in NVS namespace `muse_ww`, key
`enabled`, and defaults OFF — flashing never starts listening on its
own. The switch lives on the MuseApp settings page (Phase 4.4); until
then the opt-in can only be on if it was saved on.

Limits, intrinsic to the feature (not port gaps):

- **Visible-only, absolute.** The wake word works only while MuseApp
  is on screen: the Brookesia launcher runs at most 3 apps and the
  app's audio is released when it is hidden. There is no background
  listening.
- **Audio hold.** Listening holds the audio service while visible
  (the input task keeps reading the mic even with no turn open), so
  it preempts other apps' audio and keeps the mic/codecs powered.
  Opt-in-gated; documented in the Kconfig help too.
- **WakeNet is not a fallback.** No "Hey Muse" model exists among
  esp-sr's shipped models; a custom model needs Espressif paid
  training (≥20,000 utterances) or a free TTS request (weeks–months).
  A different phrase would be a product decision, not engineering.
- **Runtime on P4 unproven.** The runner is build-proven upstream
  and compiles here; the first on-device run is the proof. The
  SDK's S3 measurements (~2.5 ms CPU per 30 ms, 14 KB internal RAM,
  29 KB PSRAM runner, ~133 KB PSRAM audio buffers) are the only
  numbers until then.

### `components/muse_link/` — Link session to the VM (Phase 4.3, 2026-11-20)

From `sdk-ref/esp32/main/`, verbatim except the two adaptations below
(5,877 lines):

- `noise_control.cpp` / `.h` — the Noise-over-WebSocket session to the
  VM (TLS to hatch.metaaivm.com:443, WS upgrade, Noise_XX handshake,
  link.register on stream 1, daemon requests on streams ≥16, one
  12 KB-stack task, reconnect backoff 1 s doubling to 15 s, 60 s on
  auth failure, WS ping every 60 s, 150 s silence reconnect,
  link.heartbeat every 24 h). With PSRAM the inbound scratch is 64 KB
  (so `noise_ctrl_req_max_body()` is ~62 KB) and the outbound scratch
  12 KB.
- `image_fetch.c` / `.h` — the image downloader (baseline JPEG only,
  hardware JPEG peripheral on P4 — the P4's ROM has no TJpgDec, which
  the SDK already anticipates; ≤4 MB file into PSRAM, scale 1/2/4/8,
  16-row bands via draw_rect, 60 s deadline, one download at a time,
  draw_file via Range requests over the Noise session).
- `noise_tunnel.cpp` / `.h` + `noise_tunnel_internal.h` — the tunnel
  data plane, needed even with the tunnel OFF: it owns the
  DMA-headroom functions noise_control.cpp calls unconditionally.
- `link_events.c` / `.h` — the connectivity history ring (128 events,
  PSRAM; device.health carries the newest 40).
- `link_probe.c` / `.h` — the stall probe (TCP to 1.1.1.1/8.8.8.8:443).
- `stack_monitor.c` / `.h` — the session task's stack watermark.
- Header-only: `noise_upgrade.h`, `ws_mask.h` (both static-inline),
  `ble_commands.h`, `led_status.h`, `tunnel_netif.h` (declared only —
  the tunnel is off, so nothing calls them; the one call site is
  guarded by CONFIG_HOMEHUB_TUNNEL).
- `muse_device_guide.c` — factory-new: the device guide Link sends to
  Muse (device.guide), adapted from the SDK's main/guide.md for the
  P4 Knob (no discover/sensors/ble.*/camera; recovery via the settings
  page's sign-out; Link runs only while the Muse app is on screen).
  1,464 bytes, under the 4,096 cap.

Adaptations (two, both marked in the files):

- `link_probe.c`: the SDK's wifi_mgr does not exist here; the probe
  reads the factory wifi_service (WIFI_SERVICE_CONNECTED = has an IP).
- `noise_control.cpp`: the 12 KB stack reserve is guarded on
  CONFIG_MUSE_LINK_STACK_RESERVE (factory Kconfig, default y, depends
  on SPIRAM) instead of the SDK's CONFIG_MUSE_ENABLED, which the
  factory deliberately leaves undefined (defining it would pull
  muse_state_power() and the SDK's power UI).

Kconfig (factory-new): model id `muse-gadget-p4-knob`, model name
`Waveshare ESP32-P4 Knob Touch AMOLED 1.75`, HOMEHUB_DAEMON_REQUESTS /
DISPLAY_COMMANDS / LED_BACKEND_MUSE / VOICE all default y
(voice.configure is the only voice command — there is no
voice.play_url in the SDK). Deliberately undefined: MUSE_ENABLED,
HOMEHUB_TUNNEL, HOMEHUB_BLE_COMMANDS, MUSE_WASM_APPS,
HOMEHUB_FAST_GCM, MUSE_WATCHER_CAMERA / MUSE_P4_CSI_CAMERA,
HOMEHUB_SHOW_AGENT_NAME (no GET /identity fetch).
CONFIG_MUSE_WAKEWORD_ENABLE (Phase 4.2) makes Link advertise
wakeword.disable.

The lifecycle lives in the MuseApp component (the MuseLink class +
the led_status shim), so this component has no MuseApp dependency.
Link follows app visibility — the user's decision (2026-11-20): it
starts with the voice session when MuseApp becomes visible and stops
on hide; there is no always-on connection and it never starts in
MuseApp::init(). Credential resolution (one fetch_vms call) runs on
a worker task with an internal-RAM stack; a stop flag + mutex make a
disconnect always win over a connect in flight.

Commands (mirroring the SDK's app.c on_ws_command): display.draw_url,
display.draw_file, display.show_animation, voice.configure (the factory
audio service is the one volume, 0-100, NVS-backed), wakeword.disable
(opt-out saved to NVS; only someone at the device can turn listening
back on). Not implemented via Link: device.list_vms / set_vm /
reset_vm / unpair (handled locally by the settings page, Phase 4.4),
device.discover, sensors.read, camera.capture, ble.*, app.*,
bug.report — all answer "unsupported".

Limits, intrinsic to the port (not port gaps):

- **Visible-only, absolute.** Commands work only while the Muse app is
  on screen; the session (and its reconnect backoff) dies on hide.
- **Two Noise+TLS sessions.** Voice and Link hold separate sessions to
  the VM at once; untested on hardware.
- **JPEG peripheral contention.** Gallery holds its JPEG engine for its
  whole worker lifetime; image_fetch creates its own engine per
  download. Whether IDF v6.1.1 allows two concurrent decoder engines
  on the P4's single JPEG peripheral is unverified — a failure is
  graceful (a command error, no crash).
- **No battery in device.health.** MUSE_ENABLED is undefined, so the
  health JSON carries nulls for battery/power (like the SDK's
  non-MUSE boards); uptime, memory, network and connectivity history
  are there.

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
look). See `components/MuseApp/README.md`. Phase 4 pieces live here:
the wake-word glue (`muse_wakeword_glue`, Phase 4.2), the Link
lifecycle (`MuseLink` class + the `led_status` shim in
`muse_led_status.cpp`, Phase 4.3) and the downloaded-image overlay in
MuseUi (full-screen lv_image over the avatar, PSRAM RGB565 buffer,
dismissed by a tap, a talk or display.show_animation), and the
settings page (`MuseSettingsUi`, Phase 4.4 — VM picker, token status,
wake-word switch, sign-out; the VM picker is factory-new, the SDK has
only a free-text VM ID row, and sign-out unpairs Muse without
forgetting Wi-Fi or restarting, unlike the SDK's Reset pairing).

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

- Spoken replies / TTS — see "Replies are text-only" above.
- Software echo cancellation (`muse_aec.c`) — no playback reference on
  this board; calls are half-duplex (see "Realtime voice calls" above).
- The Link tunnel (HOMEHUB_TUNNEL), BLE commands, WASM apps, fast GCM,
  camera capture, sensors, bug.report — no hardware or server behind
  them on this board; Link answers "unsupported" for those commands.
- OTA, Opus, `muse_ui.c`
  (MuseUi re-implements its full layout; the avatar renderer itself IS
  vendored — see `muse_avatar` above), `muse_audio.c`, board drivers,
  `app.c` orchestration.
- Power saving: `muse_hatch_set_resting` is never called; the session
  idles with its 10-minute idle-close timer.

## USB Screen removed (2026-11-20)

The factory's USB Screen app (Windows-only extended display — Espressif
proprietary protocol plus a Windows-only driver) was removed from
`muse-app` to save flash. Removed: `components/UsbScreen/` (whole
component), `components/usb_device_uac/` (whole component — its UAC
served only the screen's speaker/mic), `components/app_services/usb_display.c`
+ `include/usb_display.h`, `components/app_services/usb/usb_descriptors.h`;
`usb_service` lost `USB_SERVICE_MODE_DISPLAY` +
`usb_service_start_display()`, `usb_descriptors.c` was rewritten MSC-only
(fixed MSC PID, no display/HID/audio descriptors), `tusb_config.h`
dropped the UAC includes and `CFG_TUD_VENDOR`/`CFG_TUD_HID`, `main.cpp`
no longer installs the app, and the `CONFIG_USB_DEVICE_UAC_*` lines left
`sdkconfig.defaults`. Kept deliberately: TinyUSB + MSC (the USB-drive
function — Settings StoragePage only uses SD drive), `esp_driver_jpeg`
(Gallery + VideoPlayer use it), `esp_driver_ppa` (VideoPlayer).

Build #9 (2026-11-20) after the removal: GREEN, `esp-brookesia.bin`
11,951,504 bytes (0xb65d90) vs 12,025,088 before — 73,584 bytes (71.9 KiB)
saved, 19% of the 14 MiB app partition free.

## Known compile risks (not verifiable without a build)

**Build status 2026-11-20: GREEN.** The integrated firmware builds and
links on ESP-IDF v6.1.1 (`idf.py set-target esp32p4`, then `make -j2`
in `build/` — `idf.py` has no `-j` flag in v6.1 and ninja is not
installed, so the build uses Unix Makefiles). `esp-brookesia.bin` is
0xbab860 bytes (12,236,896) against a 0xe00000 (14 MiB) app partition —
17% free, after the USB Screen removal, the avatar port, the Phase 4.1
realtime calls, the Phase 4.2 wake word, the Phase 4.3 Link session
and the Phase 4.4 settings page (was 0xba8ec0 / 12,226,240, 17% free,
after Link; the settings page added 10,656 bytes — the round_ui page,
the VM picker and the fetch worker). DIRAM
267,346 / 655,360 (40.79%) at Phases 0-3. Build #10 notes: the
`muse_avatar` component's `file(GLOB ... CONFIGURE_DEPENDS ...)`
failed configure — ESP-IDF includes component CMakeLists in script
mode while gathering requirements, where that flag is invalid
(dropped it); and a failed configure leaves `build/` in a state
`idf.py` refuses to fullclean ("doesn't seem to be a CMake build
directory") — delete `build/` manually and re-run. `nm` on the
build #10 ELF confirms `happy_anim_frames` is absent (GC'd, ~242 KB)
while `muse_pixel_render` (15,626 B), `muse_pixel_scale`,
`muse_pixel_set_size` and `muse_pixel_accent` are linked in. `nm` on
the build #11 ELF confirms the call core is linked in
(`muse_voice_call_poll`, `muse_voice_call_audio`/`read`,
`muse_vc_gate_init`/`mic_open`/`flush`, `muse_voice_call_device_id`)
and `MuseApp::callAudio` (2,568 B). `nm` on the build #12 ELF confirms
the wake word is linked in: the embedded model
(`_binary_hey_muse_pte_start`/`_end`, exactly 23,492 bytes),
`muse_wakeword_core_tick` (480 B), `ww_runner_create` (2,060 B),
`ww_detector_push`, and the glue (`muse_wakeword_set_active`,
`set_playback`, `set_ptt_turn`, `seed_generation`, `turn_ended`).
`nm` on the build #13 ELF confirms the Link session is linked in:
`noise_ctrl_init` (192 B) / `connect` (582 B) / `disconnect` (242 B),
`image_fetch_start` / `start_vm`, `link_events_record`,
`noise_ctrl_send_command_result`, the `led_status` shim
(`led_status_display_info`, 30 B), `MuseLink::connectTask` (512 B)
and `commandCallback` (1,032 B), the MuseUi image overlay
(`imageDraw` 384 B, `imageSync`, `imageHide`, `imageSize`), and
`muse_device_guide_md` (1,465 bytes incl. NUL). Build #13 notes:
`lv_obj_add_flag` with two OR'd flags is an `int` in C++ and the
factory LVGL rejects it — add flags one call at a time; and
`muse_account_api.h` has no `extern "C"` guard, so a C++ consumer
must wrap the include (the SDK's own `muse_chat_session.cpp` does).
`nm` on the build #14 ELF confirms the settings page is linked in:
`MuseSettingsUi::create` (1,114 B) / `refresh` (560 B) / `setVmList`
(612 B) / `showVmPicker` / `knob`, `MuseUi::gearEventCallback`,
`MuseApp::refreshSettings` (490 B) / `vmFetchTaskEntry` (478 B) /
`fetchVmList`, `vm_api_fetch_vms` (1,152 B) and
`muse_vm_set_preferred_vm` (`showSettings`/`hideSettings`/
`openVmPicker` were inlined). Build #14 notes: `strncpy` trips
`-Wstringop-truncation` (an error here) — the codebase's `copyString`
(`snprintf`) is the safe copy; and `lv_event_get_target` returns
`void*` in this LVGL, so a C++ switch handler reads the state off
`lv_event_get_current_target_obj` instead (the SDK's C code passes the
`void*` straight through). Items marked RESOLVED were
settled by the integrated build. Everything below that is not marked
RESOLVED is a runtime-on-hardware question, not a compile question.

- **MbedtlsCryptoBackend on IDF v6.1 — RESOLVED 2026-11-18.** It failed
  the first integrated build: `<mbedtls/ctr_drbg.h>` no longer exists
  in Mbed TLS 4.x (IDF v6.1). Dropped from `muse_noise_core` SRCS;
  the file stays in the tree uncompiled. The PSA backend is the only
  one the session uses.
- **PSA Crypto backend on IDF v6.1 — RESOLVED (compile) 2026-11-18.**
  `PsaCryptoBackend.cpp` compiled cleanly as part of `muse_noise_core`;
  runtime behavior on v6.1 is still untested on hardware.
- **C++ standard skew — RESOLVED (compile) 2026-11-18.** The vendored
  C++17 sources (`muse_noise_core`, and the C++ parts of the session
  glue) compiled under the factory's C++26-default project; the
  `cxx_std_17` setting propagated as expected.
- **`esp_ae_rate_cvt` API — RESOLVED (compile) 2026-11-18.** The
  resampler calls compiled; the only failure was the channel/bit-depth
  constants, which live in `esp_audio_types.h` (`esp_audio_codec`),
  not in `esp_audio_effects` — MuseApp now includes that header and
  requires `esp_audio_codec`. (XiaozhiApp gets the constants
  transitively via its Opus codec headers.)
- **`esp_app_desc_t` header — RESOLVED 2026-11-18.** In IDF v6.1 the
  type moved to `esp_app_desc.h`; `esp_app_format.h` still exists but
  is the bootloader's image-format header and does not declare it.
  MuseApp includes `esp_app_desc.h` now, matching Settings/Xiaozhi.
- **LVGL 9.5.x arc/transform APIs** — MuseUi uses the arc widget and
  label APIs the same way XiaozhiUi does; the factory uses
  `esp_lvgl_adapter 0.7.3` vs the SDK's 0.6.4, same major line.
- **BLE 4.2 GAP advertising API — RESOLVED 2026-11-18.** The first
  link failed on `esp_ble_gap_start_advertising`,
  `esp_ble_gap_config_adv_data_raw`, `esp_ble_gap_config_scan_rsp_data_raw`,
  `esp_ble_gap_stop_advertising`: IDF defaults to BLE 5.0 features,
  which compiles the legacy 4.2 functions out of Bluedroid
  (`BLE_42_FEATURE_SUPPORT` false). `sdkconfig.defaults` now sets
  `CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y` and
  `CONFIG_BT_BLE_42_ADV_EN=y` (correct for this hardware — the
  coprocessor is BLE 4.2); 5.0 features stay on, both coexist.
- **`identity.h` C++ linkage — RESOLVED 2026-11-18.** The header had
  no `extern "C"` guards, so `MuseApp.cpp` mangled the `identity_*`
  calls and the link failed. Guards added to `identity.h`, and
  pre-emptively to the other new `muse_vm` headers (`config_store.h`,
  `vm_api.h`). The SDK-verbatim headers that lack guards
  (`muse_account_api.h`, `muse_mem.h`) are only included from C files
  or inside `extern "C"` blocks, so they are left untouched.
- **`xTaskCreatePinnedToCoreWithCaps` / `xQueueCreateWithCaps` /
  `xStreamBufferCreateWithCaps` — RESOLVED (compile) 2026-11-18.**
  `muse_session` (which uses them) compiled on IDF v6.1.1.
- **`esp_read_mac` component deps — RESOLVED 2026-11-18.** The link
  succeeded with `muse_vm`'s existing `PRIV_REQUIRES`; no
  `esp_system`/`efuse` addition was needed.
- **cJSON provider — RESOLVED 2026-11-18.** Configure succeeded with
  `espressif/cjson ^1.7.19` declared by `muse_session`, `muse_vm`,
  `MuseApp`, and `MuseBlePairing` alongside the factory's other users.
