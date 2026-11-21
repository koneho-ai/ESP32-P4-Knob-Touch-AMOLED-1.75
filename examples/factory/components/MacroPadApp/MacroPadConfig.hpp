/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Macropad key configuration: the compiled-in defaults (the user's
 * photo layout) plus the JSON file on the TF card that overrides them.
 * The full file format is documented in README.md.
 */

#pragma once

#include <stdint.h>

namespace esp_brookesia::apps {

// One key action. Macro steps reuse the same shape, minus nesting.
struct MacroPadAction {
    enum class Type : uint8_t {
        None,       // unassigned key: tapping it does nothing
        Combo,      // modifiers + up to 6 keys, pressed together
        Consumer,   // one media key (play/pause, volume, mute, ...)
        Text,       // a string, typed char by char (US layout)
        Macro,      // a sequence of steps with delays
    };

    struct Step {
        enum class Kind : uint8_t { Combo, Consumer, Text, Delay };
        Kind kind = Kind::Delay;
        uint8_t modifiers = 0;
        uint8_t keys[6] = {};
        uint8_t key_count = 0;
        uint16_t consumer_usage = 0;
        char text[32] = {};
        uint16_t delay_ms = 0;
    };

    static constexpr int kMaxSteps = 16;
    static constexpr int kMaxText = 64;

    Type type = Type::None;
    uint8_t modifiers = 0;
    uint8_t keys[6] = {};
    uint8_t key_count = 0;
    uint16_t consumer_usage = 0;
    char text[kMaxText] = {};
    Step steps[kMaxSteps] = {};
    uint8_t step_count = 0;
};

struct MacroPadKey {
    static constexpr int kMaxIcon = 16;
    static constexpr int kMaxLabel = 16;

    char icon[kMaxIcon] = {};   // name from the built-in icon set, "" = no icon
    uint32_t color = 0;         // keycap background, 0xRRGGBB
    char label[kMaxLabel] = {}; // optional short text under the icon
    MacroPadAction action;
};

struct MacroPadConfig {
    static constexpr int kKeyCount = 15;
    static constexpr const char *kPath = "/sdcard/macropad/config.json";
    static constexpr int kMaxError = 96;

    MacroPadKey keys[kKeyCount];
    MacroPadAction knob_cw;     // default: volume up
    MacroPadAction knob_ccw;    // default: volume down
    bool loaded_from_file = false;
    char error[kMaxError] = {}; // parse error, "" when the file was fine or absent
};

// The photo layout: icons and keycap colors as photographed, media keys
// wired (play/pause, mute), everything else unassigned, knob on volume.
void macropad_config_defaults(MacroPadConfig &config);

// Reads kPath on top of the defaults. Returns true when a file was read
// and parsed; on any failure the defaults stay in place and error says
// why ("no card", "no file", "JSON error at ..."). Safe to call when no
// card is mounted — it just reports "no card".
bool macropad_config_load(MacroPadConfig &config);

// One-line description of an action for the footer ("Play/Pause",
// "Cmd+Space", "Type \"Mail\"", "Macro (4 steps)", "Unassigned").
void macropad_action_desc(const MacroPadAction &action, char *out, size_t out_size);

} // namespace esp_brookesia::apps
