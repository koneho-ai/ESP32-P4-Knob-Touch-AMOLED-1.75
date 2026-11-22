# MacroPad app

A touchscreen macropad for the P4 knob board: 15 touch keys in a 5×3
grid, styled like the user's macropad (the key faces are photos of
their keycaps), sending real keystrokes to a computer over USB HID. The
physical knob sends volume up/down.

> **UNTESTED ON HARDWARE** — no board exists yet. Compile verification
> only, like the rest of the `muse-app` branch.

## How it works

- Open the app from the launcher. While it runs, the device is a USB
  keyboard + consumer control (media keys) to whatever computer it is
  plugged into. Tap a key and the keystroke goes to the host.
- The USB-C plug has two orientations on this board: one selects the
  P4's USB 2.0 device port, the other the USB-to-UART bridge. **If the
  app says "No USB host", flip the plug.** The header shows a green
  dot + "USB connected" when a host has configured the device.
- The knob is volume only (the user's decision): clockwise = volume
  up, counter-clockwise = volume down. **The knob has no press
  switch** on this board (only CW/CCW GPIOs in
  `board_peripherals.yaml`), so there is no knob-press action.
- Keys do nothing while no host is connected — the footer says so
  when you tap one.
- Unassigned keys show their icon dimmed.

## The default layout (the photo)

Keys are numbered row-major, 0–14, left to right, top to bottom:

| # | Icon | Cap color | Default action |
|---|------|-----------|----------------|
| 0 | `ufo` | orange `#B85C38` | unassigned |
| 1 | `bowl` | beige `#B5AA9C` | unassigned |
| 2 | `smiley` | black `#1C1C1C` | unassigned |
| 3 | `play_pause` | black | Play/Pause (media key) |
| 4 | `mute` | black | Mute (media key) |
| 5 | `husky` | black | unassigned |
| 6 | `df` | blue-grey `#9DB4B8` | unassigned |
| 7 | `fox` | brown `#A97B50` | unassigned |
| 8 | `globe` | black | unassigned |
| 9 | `chat` | black | unassigned |
| 10 | `paw` | tan `#C2A276` | unassigned |
| 11 | `check` | green `#8A9A5B` | unassigned |
| 12 | `mic_mute` | black | unassigned (see below) |
| 13 | `mail` | black | unassigned |
| 14 | `folder` | black | unassigned |

Mic mute has no default on purpose: **macOS answers no HID usage
for microphone mute.** There is no consumer-page mic-mute usage, and
the Telephony-page mute (0x0B/0x2F) is not a system mic mute on
macOS. Map that key to your meeting app's own mute shortcut instead
(e.g. a combo) — see the examples.

## Config file

`/macropad/config.json` on the TF card. No card or no file means the
defaults above. Edit it on a Mac via the existing USB-drive function
(Settings → Storage → USB drive): the card mounts as a drive, edit
the file, eject, and the app reloads automatically when the card
remounts. The **Reload config** button at the bottom reloads on
demand. A bad file (or a bad action on one key) falls back to the
defaults for that part and the footer says why — one typo never kills
the whole pad.

```json
{
  "version": 1,
  "keys": [
    { "icon": "ufo", "color": "#B85C38", "action": null },
    { "icon": "bowl", "color": "#B5AA9C", "label": "Food",
      "action": { "type": "macro", "steps": [
        { "combo": { "modifiers": ["gui"], "key": "space" } },
        { "delay_ms": 400 },
        { "text": "Reminders" },
        { "delay_ms": 300 },
        { "combo": { "key": "enter" } }
      ] } },
    null,
    { "action": { "type": "consumer", "usage": "play_pause" } }
  ],
  "knob": {
    "cw": { "type": "consumer", "usage": "volume_up" },
    "ccw": { "type": "consumer", "usage": "volume_down" }
  }
}
```

Rules:

- `keys` is an array in photo order (index 0 = top left). Entries are
  optional: `null` or a missing entry keeps the default for that key,
  and within an entry every field is optional — set only what you
  want to change. Extra entries past 15 are ignored.
- `icon`: one of the 15 built-in names (`ufo`, `bowl`, `smiley`,
  `play_pause`, `mute`, `husky`, `df`, `fox`, `globe`, `chat`, `paw`,
  `check`, `mic_mute`, `mail`, `folder`). Unknown names show no icon.
  `""` also means no icon (use with `label` for a text-only key).
- `color`: keycap background as `#RRGGBB`.
- `label`: short text (up to 15 chars) under the icon. The icon
  shifts up a little to make room.
- `action`: `null` (or omitted) = unassigned. Otherwise one of the
  four types below.

## Action types

### Combo — modifiers + keys, pressed together

```json
{ "type": "combo", "modifiers": ["gui", "shift"], "key": "4" }
{ "type": "combo", "modifiers": ["ctrl"], "keys": ["c", "v"] }
```

- `modifiers`: `ctrl`, `shift`, `alt`/`option`, `gui`/`cmd`/
  `command`/`super`/`win`, plus `lctrl`/`rctrl`, `lshift`/`rshift`,
  `lalt`/`ralt`, `lgui`/`rgui` (`lcmd`/`rcmd`). Default is the left
  variant. Note: on macOS, `gui` is Cmd and `alt` is Option.
- `key`: one key; `keys`: up to 6 keys pressed at once.
- Key names: `a`–`z`, `0`–`9`, `f1`–`f24`, `enter`/`return`, `esc`/
  `escape`, `backspace`, `tab`, `space`, `minus`, `equal`,
  `leftbrace`/`[`, `rightbrace`/`]`, `backslash`, `semicolon`,
  `apostrophe`, `grave`, `comma`, `period`, `slash`, `capslock`,
  `printscreen`, `scrolllock`, `pause`, `insert`, `home`, `pageup`,
  `delete`/`del`, `end`, `pagedown`, `right`, `left`, `down`, `up`,
  `numlock`, `kp_0`–`kp_9`, `kp_slash`, `kp_asterisk`, `kp_minus`,
  `kp_plus`, `kp_enter`, `kp_period`, `menu`.

### Consumer — one media key

```json
{ "type": "consumer", "usage": "play_pause" }
```

Usages: `play_pause`, `play`, `pause`, `stop`, `next`,
`previous`, `fast_forward`, `rewind`, `record`, `eject`, `mute`,
`volume_up`, `volume_down`. There is no `mic_mute` (see above).

### Text — type a string

```json
{ "type": "text", "text": "hello@example.com" }
```

Typed char by char, assuming a **US keyboard layout on the host**.
ASCII letters, digits and common punctuation work; anything else is
skipped. If the Mac uses a non-US layout, shifted punctuation may
come out as a different character — use combos for those, or switch
the Mac's input source to US while typing. Max 63 characters.

### Macro — a sequence of steps

```json
{ "type": "macro", "steps": [
  { "combo": { "modifiers": ["gui"], "key": "space" } },
  { "delay_ms": 400 },
  { "text": "Mail" },
  { "delay_ms": 300 },
  { "combo": { "key": "enter" } }
] }
```

Each step is one of `{"combo": {...}}`, `{"consumer": "play_pause"}`,
`{"text": "..."}` or `{"delay_ms": 300}`. Steps run in order with a
short settle between them (30 ms). Max 16 steps. This is how
app-launch keys work: there is no "open app" HID key, so the macro
drives Spotlight (Cmd+Space, type the name, Enter) — tune the delays
if the Mac is slow to show results.

## Knob

`knob.cw` / `knob.ccw` take a combo or consumer action (text and
macros are rejected — a knob turn must be instant). Defaults are
volume up/down. Example — brightness instead of volume is not a
macOS consumer key either; map the knob to whatever your setup
answers (e.g. `{"type": "combo", "key": "f1"}` / `f2` if the Mac
treats those as brightness).

## Files

- `MacroPadApp.{hpp,cpp}` — Brookesia app lifecycle, the HID sender
  task (all USB traffic runs there, so taps never block the UI) and
  the macro engine.
- `MacroPadUi.{hpp,cpp}` — the grid, USB indicator, footer, reload
  button. Grid geometry: 72×80 keys, 8 px gaps, grid at (37, 96).
- `MacroPadConfig.{hpp,cpp}` — defaults + the JSON parser (cJSON).
- `MacroPadHid.{hpp,cpp}` — key/modifier/consumer name tables and
  the US-layout character map.
- `MacroPadIcons.{hpp,cpp}` + `assets/macropad_icons.c` — the 15 key
  faces + launcher icon, built by `tools/gen_icons.py` (PIL) from the
  user's keycap photos in `tools/keycap_photos/` (not committed — see
  the script header); regenerate with `python3 tools/gen_icons.py`.
- USB HID itself lives in `app_services` (`usb/usb_descriptors.c`,
  `usb_service.c`): one composite device — HID keyboard + HID
  consumer + MSC drive — so the macropad and the SD drive can run
  together. The device PID changed to `0x4003` with the composite
  configuration.
