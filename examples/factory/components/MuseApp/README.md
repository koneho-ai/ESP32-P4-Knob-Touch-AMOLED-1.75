# MuseApp

Tap-to-talk voice assistant for the Waveshare ESP32-P4-Knob-Touch-AMOLED-1.75
factory firmware. Modeled directly on the factory Xiaozhi app
(`../XiaozhiApp/`): same Brookesia `phone::App` lifecycle, same audio-client
and knob patterns, same LVGL look — but the backend is Muse, not Xiaozhi.

> **UNTESTED ON HARDWARE** — no board exists yet. The firmware builds
> green on ESP-IDF v6.1.1, but nothing has run on a device. Expect
> bring-up fixes around the audio chain, the mic channel order, and the
> session's behavior on the real VM.

## What it does

- Tap anywhere to talk; tap again to send. Muse's reply appears as text
  on the screen, paced like speech (see "Replies are text-only" below).
- Tap while Muse is thinking or speaking to cancel.
- Long-press the avatar to pet it (the SDK's happy reaction).
- Long-press anywhere else to start a realtime voice call (hands-free,
  half-duplex — Muse's speech plays live); tap to hang up. See
  "Realtime voice calls" below.
- Turn the knob for volume. A long press / leaving the app closes the
  session and releases the audio codec, exactly like Xiaozhi.
- Offline, it offers the WLAN settings page (`Settings::openWlan`).
- If the device token is rejected, it refreshes the token pair in the
  background; if the server says the pairing is revoked, it drops back to
  the "not connected" screen and asks for pairing again.

## Build

```sh
cd examples/factory
idf.py set-target esp32p4
idf.py menuconfig   # see "Muse Application" and the SDK token below
idf.py build flash
```

Kconfig options (`Muse Application` menu):

- `MUSE_APP_ENABLED` (default y) — install the app.
- `MUSE_APP_CAPTURE_MIC` — which microphone feeds the session: both mixed
  (default), mic 1 only, or mic 2 only. The single-mic options exist for
  bring-up, in case the mic channels turn out swapped.

## Provision (pairing)

The app cannot pair itself. Pairing is done over BLE from the Muse app by
the `MuseBlePairing` component (a Bluedroid GATT server port of the SDK's
NimBLE pairing server; see `../MuseBlePairing/INTEGRATION.md`). The flow:

1. Open the Muse app while it shows NOT CONNECTED: it starts BLE
   advertising as `MuseGadget-XXXXXX` (only while this screen is visible;
   leaving the screen stops advertising). Bluetooth must be on in
   Settings — the app respects the Settings toggle.
2. In the Muse app, pair the device. The phone and the device run the
   pairing-v5 handshake (P-256 ECDH + HKDF + AES-256-GCM).
3. The device shows "CONFIRM PAIRING?" — tap to confirm.
4. The phone sends `provision_v2` (Wi-Fi credentials + the minted device
   token pair). The device joins the Wi-Fi network, stores the tokens in
   the NVS namespace `museapp` (via `../muse_vm/muse_provisioning.h`,
   the shared interface), reports `auth_ok`, and connects — no restart
   needed. The app notices the new credentials within a second.

The maker's SDK token (`mgst_…`) is a build-time setting, not a runtime
one:

```sh
idf.py menuconfig   # "Muse VM account" -> "Maker SDK token (mgst_...)"
```

Never commit a real token: `sdkconfig.defaults` ships the option empty.
The SDK token identifies the maker; the device tokens minted during
pairing identify this device. Neither is ever logged.

Before pairing, the "not connected" screen shows the device's setup node
id (`homelink-XXXXXX`, from the eFuse MAC) — that is the id to offer during
pairing. The BLE name is `MuseGadget-XXXXXX`.

## Use

| Screen state | What it means | Tap does |
|---|---|---|
| NOT CONNECTED | Not paired yet | Confirms pairing when asked, else a "pair in the Muse app" toast |
| NO NETWORK | Wi-Fi down | Opens WLAN settings |
| CONNECTING | Reaching the VM | Nothing yet |
| READY | Connected | Starts listening |
| LISTENING | Recording | Sends the turn |
| THINKING | Waiting for Muse | Cancels the turn |
| SPEAKING | Reply playing | Interrupts |
| CONNECTING (call) | A call is setting up | Cancels the call |
| LISTENING / THINKING / SPEAKING (in a call) | On a call | Hangs up |
| ERROR | Something failed (detail on screen) | Retries |

## Realtime voice calls

A long press anywhere except the avatar starts a hands-free realtime
voice call (`POST /voice/realtime`, Phase 4.1): Muse's speech arrives
as live 16 kHz audio and plays on the speaker, and the mic stays open
while Muse isn't talking. Tap hangs up; the call also ends on 45 s of
quiet, at 15 minutes, or when Muse ends it. The avatar shows the
thinking variant with a working pose (typing on a little keyboard)
while Muse is busy on work it was asked for during a call.

Half-duplex: the mic shuts while Muse is audibly talking, plus a
300 ms tail — this board has no playback reference for echo
cancellation (see `../VENDORED.md`), so talking over Muse isn't
possible. No push-to-talk call yet: the UI has no press/release
events. A call and a voice-note turn never overlap. The ring stays
empty during a call (there is no phase cap to fill towards).

## The avatar

The screen shows the SDK's animated avatar face, not the factory
Xiaozhi-style orb it replaced: the procedural pixel-art character
(`../muse_avatar`, vendored from the SDK) in the middle, a 458 px
progress ring around the bezel, a 14-segment mic level meter under the
avatar while listening, the state name at the top, and a 3-line caption
box at the bottom. While a reply (or the user transcript) shows, the
avatar shrinks to 256 px to make room, as the SDK's heard-answer layout
does. The ring fills with recording length while listening (over the
15 s a voice note runs to) and with reply progress while speaking, and
spins while thinking. The avatar's animations follow the app state
(idle, listening, thinking, speaking, error), with a wake-up animation
for the first 1.4 s after the app opens. The caption box is the page the
session core pages to (26 columns, 3 lines), and reply pages are
additionally wrapped to the box's pixel width in the caption font
(`muse_chat_set_caption_px`, wired up in `MuseApp::createUi`).

Deliberate deviations from the SDK's face tile: no volume swipe (the
knob is the volume control), no speaker/mute button, no settings or
latest-reply tile (the Brookesia launcher owns navigation), and one
caption box for both the transcript and the reply page (the SDK's
separate live-caption line would overlap the reply box here). The state
label uses Montserrat 16 — the factory LVGL has no Unscii, which the
SDK uses. See `MuseUi.hpp` for the full layout notes.

## How it fits together

```
MuseApp (this component)
  MuseApp.cpp    Brookesia app: state machine, FreeRTOS tasks
                 (control / capture / playback / call / token refresh),
                 UI glue
  MuseClient.cpp Adapter over the vendored session core + credential store
  MuseUi.cpp     LVGL avatar face / ring / meter / captions (SDK full layout)
  assets/        Home-screen icon (112x112 ARGB8888, bunny motif)
../muse_avatar   Vendored SDK avatar renderer (muse_pixel.c, happy_anim)
../muse_session  Vendored SDK session core (Noise + HTTPS + dictation)
                 + realtime voice-call sources (muse_voice_call*)
../muse_vm       VM discovery, device-token mint/refresh, NVS store,
                 and the shared provisioning interface (muse_provisioning.h)
../MuseBlePairing  BLE pairing v5 GATT server (Bluedroid port of the SDK's
                 NimBLE server); the app drives it from MuseApp.cpp
../muse_noise_core  Vendored Noise_XX_25519_AESGCM_SHA256
../minimp3       Vendored MP3 decoder (used by the session's self-test)
```

Audio chain: codec captures 24 kHz stereo via `audio_client_read` →
de-interleave / mix to mono → `esp_ae_rate_cvt` down to 16 kHz mono →
session core (which upsamples to 24 kHz for the dictation upload, or
sends the turn as a 16 kHz WAV voice note — the SDK's `VOICE_NOTE`
default). Replies come back as 16 kHz mono → resample to 24 kHz →
duplicate to stereo → `audio_client_write`.

## v1 limits (deliberate)

- **Replies are text-only.** The SDK's session core plays silence paced
  to the reply text ("Muse doesn't speak gadget replies"). The MP3 decode
  path is vendored intact, but nothing feeds MP3 into it in v1 — a TTS
  service would plug into `start_tts()` in `muse_chat_session.cpp`.
- **Voice-note mode, not streaming dictation.** The vendored session
  defaults to `VOICE_NOTE=1`: each press is uploaded as a WAV voice note
  to `/chat/stream`, not streamed to `/api/voice/dictation`. The
  streaming path exists in the same file behind `VOICE_NOTE=0`.
- **No wake word.** Turn-based tap-to-talk only (plus calls, above).
- **Calls are half-duplex, hands-free only.** No echo cancellation, no
  push-to-talk call (see "Realtime voice calls" above).
- **No Link control session.** The session core's Link hooks are stubbed;
  the app uses the own-token path only (fetch VMs from
  `api.muse.ai`, mint device tokens, Noise session to the VM).
- **No OTA.** Single 14 MB factory partition; updates are a reflash.
- **No power saving.** `muse_hatch_set_resting` is not called; the
  session idles with its 10-minute idle-close timer.

See `../VENDORED.md` for the full list of what was copied from the SDK,
what was stripped, and the known compile risks.
