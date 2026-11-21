/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MacroPadHid.hpp"

#include <string.h>

namespace esp_brookesia::apps {

namespace {

struct NameUsage {
    const char *name;
    uint16_t usage;
};

// USB HID Usage Tables, Keyboard/Keypad page (0x07). Aliases cover the
// names people actually write ("return", "esc", "del") and the macOS
// spellings ("option" for alt, "command" for gui).
constexpr NameUsage kKeys[] = {
    {"a", 0x04}, {"b", 0x05}, {"c", 0x06}, {"d", 0x07}, {"e", 0x08},
    {"f", 0x09}, {"g", 0x0A}, {"h", 0x0B}, {"i", 0x0C}, {"j", 0x0D},
    {"k", 0x0E}, {"l", 0x0F}, {"m", 0x10}, {"n", 0x11}, {"o", 0x12},
    {"p", 0x13}, {"q", 0x14}, {"r", 0x15}, {"s", 0x16}, {"t", 0x17},
    {"u", 0x18}, {"v", 0x19}, {"w", 0x1A}, {"x", 0x1B}, {"y", 0x1C},
    {"z", 0x1D},
    {"1", 0x1E}, {"2", 0x1F}, {"3", 0x20}, {"4", 0x21}, {"5", 0x22},
    {"6", 0x23}, {"7", 0x24}, {"8", 0x25}, {"9", 0x26}, {"0", 0x27},
    {"enter", 0x28}, {"return", 0x28},
    {"esc", 0x29}, {"escape", 0x29},
    {"backspace", 0x2A}, {"back", 0x2A},
    {"tab", 0x2B}, {"space", 0x2C},
    {"minus", 0x2D}, {"-", 0x2D},
    {"equal", 0x2E}, {"=", 0x2E},
    {"leftbrace", 0x2F}, {"[", 0x2F},
    {"rightbrace", 0x30}, {"]", 0x30},
    {"backslash", 0x31}, {"\\", 0x31},
    {"semicolon", 0x33}, {";", 0x33},
    {"apostrophe", 0x34}, {"'", 0x34},
    {"grave", 0x35}, {"`", 0x35},
    {"comma", 0x36}, {",", 0x36},
    {"period", 0x37}, {".", 0x37},
    {"slash", 0x38}, {"/", 0x38},
    {"capslock", 0x39},
    {"f1", 0x3A}, {"f2", 0x3B}, {"f3", 0x3C}, {"f4", 0x3D},
    {"f5", 0x3E}, {"f6", 0x3F}, {"f7", 0x40}, {"f8", 0x41},
    {"f9", 0x42}, {"f10", 0x43}, {"f11", 0x44}, {"f12", 0x45},
    {"f13", 0x68}, {"f14", 0x69}, {"f15", 0x6A}, {"f16", 0x6B},
    {"f17", 0x6C}, {"f18", 0x6D}, {"f19", 0x6E}, {"f20", 0x6F},
    {"f21", 0x70}, {"f22", 0x71}, {"f23", 0x72}, {"f24", 0x73},
    {"printscreen", 0x46}, {"scrolllock", 0x47}, {"pause", 0x48},
    {"insert", 0x49}, {"home", 0x4A}, {"pageup", 0x4B},
    {"delete", 0x4C}, {"del", 0x4C},
    {"end", 0x4D}, {"pagedown", 0x4E},
    {"right", 0x4F}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52},
    {"numlock", 0x53},
    {"kp_slash", 0x54}, {"kp_asterisk", 0x55}, {"kp_minus", 0x56},
    {"kp_plus", 0x57}, {"kp_enter", 0x58},
    {"kp_1", 0x59}, {"kp_2", 0x5A}, {"kp_3", 0x5B}, {"kp_4", 0x5C},
    {"kp_5", 0x5D}, {"kp_6", 0x5E}, {"kp_7", 0x5F}, {"kp_8", 0x60},
    {"kp_9", 0x61}, {"kp_0", 0x62}, {"kp_period", 0x63},
    {"menu", 0x65}, {"application", 0x65},
};

constexpr NameUsage kModifiers[] = {
    {"ctrl", 0x01}, {"control", 0x01}, {"lctrl", 0x01},
    {"shift", 0x02}, {"lshift", 0x02},
    {"alt", 0x04}, {"option", 0x04}, {"lalt", 0x04}, {"loption", 0x04},
    {"gui", 0x08}, {"cmd", 0x08}, {"command", 0x08}, {"super", 0x08},
    {"win", 0x08}, {"lgui", 0x08}, {"lcmd", 0x08},
    {"rctrl", 0x10}, {"rshift", 0x20},
    {"ralt", 0x40}, {"roption", 0x40},
    {"rgui", 0x80}, {"rcmd", 0x80},
};

// USB HID Usage Tables, Consumer page (0x0C). Deliberately no mic mute:
// macOS does not answer any consumer usage for microphone mute, and the
// Telephony-page mute (0x0B/0x2F) is not a system mic mute on macOS
// either. Map mic mute to the meeting app's own shortcut instead.
constexpr NameUsage kConsumer[] = {
    {"play_pause", 0xCD},
    {"play", 0xB0}, {"pause", 0xB1}, {"stop", 0xB7},
    {"next", 0xB5}, {"track_next", 0xB5},
    {"previous", 0xB6}, {"track_prev", 0xB6},
    {"fast_forward", 0xB3}, {"rewind", 0xB4},
    {"record", 0xB2}, {"eject", 0xB8},
    {"mute", 0xE2}, {"volume_up", 0xE9}, {"volume_down", 0xEA},
};

uint16_t lookup(const NameUsage *table, size_t count, const char *name)
{
    if (!name) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return table[i].usage;
        }
    }
    return 0;
}

} // namespace

uint8_t macropad_hid_key(const char *name)
{
    return (uint8_t)lookup(kKeys, sizeof(kKeys) / sizeof(kKeys[0]), name);
}

const char *macropad_hid_key_name(uint8_t code)
{
    if (code == 0) {
        return "";
    }
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
        if (kKeys[i].usage == code) {
            return kKeys[i].name;
        }
    }
    return "";
}

uint8_t macropad_hid_modifier(const char *name)
{
    return (uint8_t)lookup(kModifiers, sizeof(kModifiers) / sizeof(kModifiers[0]), name);
}

uint16_t macropad_hid_consumer(const char *name)
{
    return lookup(kConsumer, sizeof(kConsumer) / sizeof(kConsumer[0]), name);
}

const char *macropad_hid_consumer_name(uint16_t usage)
{
    switch (usage) {
    case 0xCD:
        return "Play/Pause";
    case 0xB0:
        return "Play";
    case 0xB1:
        return "Pause";
    case 0xB7:
        return "Stop";
    case 0xB5:
        return "Next track";
    case 0xB6:
        return "Previous track";
    case 0xB3:
        return "Fast forward";
    case 0xB4:
        return "Rewind";
    case 0xB2:
        return "Record";
    case 0xB8:
        return "Eject";
    case 0xE2:
        return "Mute";
    case 0xE9:
        return "Volume up";
    case 0xEA:
        return "Volume down";
    default:
        return "Media key";
    }
}

bool macropad_hid_char(char c, uint8_t &key, bool &shift)
{
    shift = false;
    if (c >= 'a' && c <= 'z') {
        key = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        key = (uint8_t)(0x04 + (c - 'A'));
        shift = true;
        return true;
    }
    if (c >= '1' && c <= '9') {
        key = (uint8_t)(0x1E + (c - '1'));
        return true;
    }
    switch (c) {
    case '0':
        key = 0x27;
        return true;
    case ' ':
        key = 0x2C;
        return true;
    case '\n':
    case '\r':
        key = 0x28;
        return true;
    case '\t':
        key = 0x2B;
        return true;
    case '-':
        key = 0x2D;
        return true;
    case '=':
        key = 0x2E;
        return true;
    case '[':
        key = 0x2F;
        return true;
    case ']':
        key = 0x30;
        return true;
    case '\\':
        key = 0x31;
        return true;
    case ';':
        key = 0x33;
        return true;
    case '\'':
        key = 0x34;
        return true;
    case '`':
        key = 0x35;
        return true;
    case ',':
        key = 0x36;
        return true;
    case '.':
        key = 0x37;
        return true;
    case '/':
        key = 0x38;
        return true;
    case '!':
        key = 0x1E;
        shift = true;
        return true;
    case '@':
        key = 0x1F;
        shift = true;
        return true;
    case '#':
        key = 0x20;
        shift = true;
        return true;
    case '$':
        key = 0x21;
        shift = true;
        return true;
    case '%':
        key = 0x22;
        shift = true;
        return true;
    case '^':
        key = 0x23;
        shift = true;
        return true;
    case '&':
        key = 0x24;
        shift = true;
        return true;
    case '*':
        key = 0x25;
        shift = true;
        return true;
    case '(':
        key = 0x26;
        shift = true;
        return true;
    case ')':
        key = 0x27;
        shift = true;
        return true;
    case '_':
        key = 0x2D;
        shift = true;
        return true;
    case '+':
        key = 0x2E;
        shift = true;
        return true;
    case '{':
        key = 0x2F;
        shift = true;
        return true;
    case '}':
        key = 0x30;
        shift = true;
        return true;
    case '|':
        key = 0x31;
        shift = true;
        return true;
    case ':':
        key = 0x33;
        shift = true;
        return true;
    case '"':
        key = 0x34;
        shift = true;
        return true;
    case '~':
        key = 0x35;
        shift = true;
        return true;
    case '<':
        key = 0x36;
        shift = true;
        return true;
    case '>':
        key = 0x37;
        shift = true;
        return true;
    case '?':
        key = 0x38;
        shift = true;
        return true;
    default:
        return false;
    }
}

} // namespace esp_brookesia::apps
