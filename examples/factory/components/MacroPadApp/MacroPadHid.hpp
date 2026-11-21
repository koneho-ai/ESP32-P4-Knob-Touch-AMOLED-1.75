/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Name tables for the macropad config file: key names, modifier names
 * and consumer-control names to HID usages, plus the US-layout
 * character map for type-text actions. HID usages are from the USB HID
 * Usage Tables (Keyboard/Keypad page 0x07, Consumer page 0x0C).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace esp_brookesia::apps {

// HID key code (usage ID, page 0x07) for a key name ("a", "enter",
// "f5", "left", ...). 0 when the name is unknown.
uint8_t macropad_hid_key(const char *name);

// Key name for a HID key code ("space", "enter", "a"), "" when unknown.
// Used for footer descriptions of combos.
const char *macropad_hid_key_name(uint8_t code);

// HID modifier bit for a modifier name ("ctrl", "shift", "alt",
// "gui"/"cmd", "lctrl", "rshift", ...). 0 when unknown.
uint8_t macropad_hid_modifier(const char *name);

// HID consumer usage (page 0x0C) for a name ("play_pause",
// "volume_up", "mute", ...). 0 when unknown. There is deliberately no
// mic_mute here: macOS has no consumer usage it answers for
// microphone mute, so shipping one would be a dead key (see README).
uint16_t macropad_hid_consumer(const char *name);

// Short display name for a consumer usage ("Play/Pause", "Volume up").
const char *macropad_hid_consumer_name(uint16_t usage);

// Maps one ASCII character to a HID key code + shift flag, assuming a
// US keyboard layout on the host. Returns false when the character has
// no key (non-ASCII, control characters other than \n and \t).
bool macropad_hid_char(char c, uint8_t &key, bool &shift);

} // namespace esp_brookesia::apps
