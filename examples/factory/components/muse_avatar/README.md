# muse_avatar — the Muse character, as the SDK draws it

The animated avatar face from the Muse gadget SDK, vendored verbatim
(SDK commit `6a44074`; see `../VENDORED.md`). The face is **procedural
pixel art, not video**: `muse_pixel.c` renders a 64×64 indexed grid
(Jollybot, Meta's character — its files carry a Meta copyright line,
not the Apache header) with an animation for each state (boot, idle,
listening, thinking, speaking, error, off, and a happy reaction), and
the UI upscales it to the screen with `muse_pixel_scale()`.

Files:

- `muse_pixel.c` / `muse_pixel.h` — the renderer (`muse_pixel_render`,
  `muse_pixel_scale`, `muse_pixel_set_size`, `muse_pixel_accent`) and
  the `muse_pose_t` API. From the SDK's `esp32/avatar/muse_pixel.c`
  and `esp32/components/muse/muse_pixel.h`, verbatim.
- `muse_speaking.h` — header-only speaking choreography (gestures,
  mouth, brows), included by the renderer. From the SDK's
  `esp32/components/muse/muse_speaking.h`, verbatim.
- `happy_anim.c` / `happy_anim.h` — the 75-frame happy animation
  (56×59, 40 ms/frame, ~242 KB) generated from `jollybot.gif` by the
  SDK's `tools/gen_happy_anim.py`. From the SDK's `esp32/avatar/`,
  verbatim. The LVGL face does **not** draw it — neither does the
  SDK's: it is the status animation for boards with small SPI displays
  (`main/led_status.c`) and e-paper (`main/epaper_status.c`). It is
  vendored for asset parity and compiles here (guarded by
  `CONFIG_HOMEHUB_DISPLAY`, as in the SDK); the linker drops the
  unreferenced frames, exactly as the SDK's LVGL-board builds do.

`muse_pixel.h` includes `muse_state.h` for `muse_mode_t`; the slim
`muse_state.h` in `muse_session` carries that enum (from the SDK's
`muse_state.h`).

## Custom avatar

Drop a replacement renderer at `avatar/muse_pixel.c` in this component
(the directory is gitignored) and rebuild: the CMakeLists picks it up
in place of the default, the way the SDK's `tools/muse/avatar.py` does.
The SDK's recipe (`tools/muse/AVATAR_RECIPE.md`) and prompt
(`tools/muse/avatar_prompt.md`) describe the file's contract — Muse
can draw a new one from the default. `make_gifs.py` in the SDK previews
one without a board.

## Who uses it

`MuseApp` (`../MuseApp/MuseUi.cpp`) drives it the way the SDK's
`muse_ui.c` does: a custom LVGL image decoder scales the grid up in
16-row strips, a 40 ms timer builds the pose (mode, level, happiness)
and re-renders, and a cell-diff invalidates only the changed cells.
