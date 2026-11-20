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

## Wake word

Saying "Hey Muse" starts the same voice note as tapping to talk
(Phase 4.2): the on-device detector (`../muse_wakeword`, vendored
from the SDK — an ExecuTorch microWakeWord model, `hey_muse.pte`)
hears the phrase, the screen shows "Hey Muse heard / Speak now",
and speech within 2.5 s opens a hands-free turn whose note starts
up to 1 s before the speech, so it includes the wake word. The turn
ends on 700 ms of silence, at 15 s, on a tap (which submits it), or
when listening is switched off (which drops it unsent). A wake with
no speech after it sends nothing; a tap while armed cancels the wake.

Opt-in, off by default: the switch is on the settings page (Phase
4.4), and the choice persists in NVS. Listening only works while
the Muse app is visible — the launcher releases the app's audio when
it is hidden, so there is no background listening. While listening
is on, the app holds the audio service (the mic stays powered and
other apps' audio is preempted). The detector runs entirely on the
device; audio leaves only after a detection, as the voice note.
Tap-to-talk stays primary and works exactly as before.

## Link session

While the app is visible, Link (Phase 4.3) holds a second Noise
session to the VM — separate from the voice session — so Muse can
reach the device with commands: `display.draw_url` and
`display.draw_file` draw an image full-screen over the avatar (a tap,
a talk or `display.show_animation` brings the avatar back),
`voice.configure` sets the speaker volume, and `wakeword.disable`
turns hands-free listening off at once (only someone at the device
can turn it back on). The VM commands (`device.list_vms` / `set_vm` /
`reset_vm` / `unpair`) are handled locally by the settings page
instead, and discover / sensors / camera / `ble.*` / `app.*` /
`bug.report` answer "unsupported" — the board has no tunnel, sensors,
camera or BLE command server.

Link follows the app's visibility: it starts with the voice session
when the app becomes visible and stops on hide. There is no always-on
connection, and commands only work while the app is on screen. If the
server unpairs the device remotely, the app unpairs locally the same
way a revoked token does; if Link's VM credentials are rejected, the
app refreshes the device token and reconnects Link with fresh
credentials. See `../VENDORED.md` (the `muse_link` section) for the
vendored files and the limits (two simultaneous Noise+TLS sessions,
JPEG peripheral contention with Gallery, no battery in device.health).

## Settings

A gear in the top right of the main view opens the settings page
(Phase 4.4), built with the factory Settings app's round_ui kit; the
knob scrolls it while it is open, and back returns to the main view
before it closes the app. It mirrors the SDK's Hatch page where the
factory has an equivalent:

- Status: paired or not, the voice session's state, and whether Link
  is connected.
- VM: the preferred VM if one is set, else the VM the session last
  connected to, else "Default". Tapping it opens the picker — a
  "Default VM" row plus one row per VM from the account, the effective
  one checked. Picking a VM stores the preference and reconnects both
  sessions with it. (The SDK has no picker, only a free-text VM ID
  row; the picker is new work on `vm_api_fetch_vms`.)
- Device token: "Set" or "Not set" — presence only, never the token.
- Server: the provisioned host, or "Default".
- Test connection: the SDK's `muse_hatch_test`.
- Wake word switch, with the SDK's note: 'Listens for "Hey Muse" so
  you can start a voice note without holding the talk button.' The
  switch is the opt-in the wake word needs; a remote
  `wakeword.disable` (Link) turns it off and the switch follows.
- Sign out: a confirmation dialog, then the app unpairs the same way a
  revoked token does. Unlike the SDK's "Reset pairing" this does not
  forget Wi-Fi or restart — Wi-Fi belongs to the launcher's Settings
  app, and the device stays on.

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
                 + downloaded-image overlay (Link display.*)
  muse_wakeword_glue.cpp  Wake-word glue (factory-new): wake task, tap,
                 pre-roll seed handoff, opt-in (see ../VENDORED.md)
  MuseLink.cpp   Link lifecycle (factory-new): visible-only Noise
                 session, command dispatch, credential worker
  muse_led_status.cpp  led_status shim for the Muse backend: Link's
                 image commands draw through the MuseUi overlay
  MuseSettingsUi.cpp  Settings page (factory-new): VM picker, token
                 status, wake-word switch, sign-out (round_ui kit)
  assets/        Home-screen icon (112x112 ARGB8888, bunny motif)
../muse_avatar   Vendored SDK avatar renderer (muse_pixel.c, happy_anim)
../muse_link     Vendored SDK Link session (noise_control, image_fetch,
                 noise_tunnel, link_events, link_probe, stack_monitor)
../muse_wakeword Vendored SDK wake-word core + ExecuTorch runner
                 (muse_wakeword_core, ww_runner_espnn, hey_muse.pte)
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
- **Wake word is visible-only and opt-in.** "Hey Muse" works only
  while the app is on screen and only after the settings-page switch
  is turned on (see "Settings" above). No background listening.
- **Calls are half-duplex, hands-free only.** No echo cancellation, no
  push-to-talk call (see "Realtime voice calls" above).
- **Link is visible-only.** The agent's commands work only while the
  Muse app is on screen (see "Link session" above). No tunnel, no
  sensors, no camera, no BLE commands.
- **No OTA.** Single 14 MB factory partition; updates are a reflash.
- **No power saving.** `muse_hatch_set_resting` is not called; the
  session idles with its 10-minute idle-close timer.

See `../VENDORED.md` for the full list of what was copied from the SDK,
what was stripped, and the known compile risks.
