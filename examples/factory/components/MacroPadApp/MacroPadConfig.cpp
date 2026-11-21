/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MacroPadConfig.hpp"

#include <new>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "storage_service.h"
#include "MacroPadHid.hpp"

namespace esp_brookesia::apps {

namespace {

constexpr const char *TAG = "macropad_config";
constexpr size_t kMaxFileBytes = 16 * 1024;

// The photo layout, row-major: icon name, keycap color, default action.
// Only the media keys are wired by default; the rest are unassigned
// until the user maps them in the config file. Mic mute stays
// unassigned on purpose: macOS answers no HID usage for it (see
// MacroPadHid.hpp), so a default there would be a dead key.
struct DefaultKey {
    const char *icon;
    uint32_t color;
    uint16_t consumer_usage;  // 0 = unassigned
};

constexpr DefaultKey kDefaults[MacroPadConfig::kKeyCount] = {
    {"ufo",        0xB85C38, 0},        // row 1: orange saucer
    {"bowl",       0xB5AA9C, 0},        // beige dog bowl + bone
    {"smiley",     0x1C1C1C, 0},        // black smiley face
    {"play_pause", 0x1C1C1C, 0xCD},     // black play/pause
    {"mute",       0x1C1C1C, 0xE2},     // black mute speaker
    {"husky",      0x1C1C1C, 0},        // row 2: black/white husky face
    {"df",         0x9DB4B8, 0},        // light blue-grey DF chevron
    {"fox",        0xA97B50, 0},        // brown fox with glasses
    {"globe",      0x1C1C1C, 0},        // black globe
    {"chat",       0x1C1C1C, 0},        // black chat bubble
    {"paw",        0xC2A276, 0},        // row 3: tan paw print
    {"check",      0x8A9A5B, 0},        // green checkmark
    {"mic_mute",   0x1C1C1C, 0},        // black mic muted (unassigned: no HID path)
    {"mail",       0x1C1C1C, 0},        // black envelope
    {"folder",     0x1C1C1C, 0},        // black folder
};

void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

bool parse_hex_color(const char *str, uint32_t &color)
{
    if (!str || str[0] != '#' || strlen(str) != 7) {
        return false;
    }
    unsigned int value = 0;
    if (sscanf(str + 1, "%06x", &value) != 1) {
        return false;
    }
    color = value;
    return true;
}

// Parses {"modifiers": [...], "key": "..."} / {"keys": [...]} into the
// combo fields. Returns false when no usable key was found.
bool parse_combo(const cJSON *obj, uint8_t &modifiers, uint8_t *keys, uint8_t &key_count)
{
    modifiers = 0;
    key_count = 0;
    const cJSON *mods = cJSON_GetObjectItemCaseSensitive(obj, "modifiers");
    if (cJSON_IsArray(mods)) {
        const cJSON *mod = nullptr;
        cJSON_ArrayForEach(mod, mods) {
            if (cJSON_IsString(mod)) {
                modifiers |= macropad_hid_modifier(mod->valuestring);
            }
        }
    }
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(obj, "key");
    if (cJSON_IsString(key)) {
        uint8_t code = macropad_hid_key(key->valuestring);
        if (code != 0) {
            keys[key_count++] = code;
        }
    }
    const cJSON *keys_arr = cJSON_GetObjectItemCaseSensitive(obj, "keys");
    if (cJSON_IsArray(keys_arr)) {
        const cJSON *k = nullptr;
        cJSON_ArrayForEach(k, keys_arr) {
            if (key_count >= 6) {
                break;
            }
            if (cJSON_IsString(k)) {
                uint8_t code = macropad_hid_key(k->valuestring);
                if (code != 0) {
                    keys[key_count++] = code;
                }
            }
        }
    }
    return key_count > 0;
}

// Parses one action object (or null = unassigned). Unknown types and
// actions with no usable key fall back to None.
bool parse_action(const cJSON *obj, MacroPadAction &action)
{
    action = MacroPadAction{};
    if (!obj || cJSON_IsNull(obj)) {
        return true;
    }
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "type");
    if (!cJSON_IsString(type)) {
        return false;
    }
    if (strcmp(type->valuestring, "none") == 0) {
        return true;
    }
    if (strcmp(type->valuestring, "combo") == 0) {
        if (!parse_combo(obj, action.modifiers, action.keys, action.key_count)) {
            return false;
        }
        action.type = MacroPadAction::Type::Combo;
        return true;
    }
    if (strcmp(type->valuestring, "consumer") == 0) {
        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(obj, "usage");
        if (!cJSON_IsString(usage)) {
            return false;
        }
        action.consumer_usage = macropad_hid_consumer(usage->valuestring);
        if (action.consumer_usage == 0) {
            return false;
        }
        action.type = MacroPadAction::Type::Consumer;
        return true;
    }
    if (strcmp(type->valuestring, "text") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(obj, "text");
        if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
            return false;
        }
        copy_string(action.text, sizeof(action.text), text->valuestring);
        action.type = MacroPadAction::Type::Text;
        return true;
    }
    if (strcmp(type->valuestring, "macro") == 0) {
        const cJSON *steps = cJSON_GetObjectItemCaseSensitive(obj, "steps");
        if (!cJSON_IsArray(steps)) {
            return false;
        }
        const cJSON *step_json = nullptr;
        cJSON_ArrayForEach(step_json, steps) {
            if (action.step_count >= MacroPadAction::kMaxSteps) {
                break;
            }
            if (!cJSON_IsObject(step_json)) {
                continue;
            }
            MacroPadAction::Step &step = action.steps[action.step_count];
            const cJSON *delay = cJSON_GetObjectItemCaseSensitive(step_json, "delay_ms");
            if (cJSON_IsNumber(delay)) {
                step.kind = MacroPadAction::Step::Kind::Delay;
                step.delay_ms = (uint16_t)(delay->valueint > 0 ? delay->valueint : 0);
                action.step_count++;
                continue;
            }
            const cJSON *combo = cJSON_GetObjectItemCaseSensitive(step_json, "combo");
            if (cJSON_IsObject(combo)) {
                if (parse_combo(combo, step.modifiers, step.keys, step.key_count)) {
                    step.kind = MacroPadAction::Step::Kind::Combo;
                    action.step_count++;
                }
                continue;
            }
            const cJSON *consumer = cJSON_GetObjectItemCaseSensitive(step_json, "consumer");
            if (cJSON_IsString(consumer)) {
                uint16_t usage = macropad_hid_consumer(consumer->valuestring);
                if (usage != 0) {
                    step.kind = MacroPadAction::Step::Kind::Consumer;
                    step.consumer_usage = usage;
                    action.step_count++;
                }
                continue;
            }
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(step_json, "text");
            if (cJSON_IsString(text) && text->valuestring[0] != '\0') {
                step.kind = MacroPadAction::Step::Kind::Text;
                copy_string(step.text, sizeof(step.text), text->valuestring);
                action.step_count++;
                continue;
            }
            // Unknown step shape: skipped, the rest of the macro survives.
        }
        if (action.step_count == 0) {
            return false;
        }
        action.type = MacroPadAction::Type::Macro;
        return true;
    }
    return false;
}

} // namespace

void macropad_config_defaults(MacroPadConfig &config)
{
    config = MacroPadConfig{};
    for (int i = 0; i < MacroPadConfig::kKeyCount; i++) {
        copy_string(config.keys[i].icon, sizeof(config.keys[i].icon), kDefaults[i].icon);
        config.keys[i].color = kDefaults[i].color;
        if (kDefaults[i].consumer_usage != 0) {
            config.keys[i].action.type = MacroPadAction::Type::Consumer;
            config.keys[i].action.consumer_usage = kDefaults[i].consumer_usage;
        }
    }
    config.knob_cw.type = MacroPadAction::Type::Consumer;
    config.knob_cw.consumer_usage = 0xE9;  // volume up
    config.knob_ccw.type = MacroPadAction::Type::Consumer;
    config.knob_ccw.consumer_usage = 0xEA;  // volume down
}

bool macropad_config_load(MacroPadConfig &config)
{
    config.loaded_from_file = false;
    config.error[0] = '\0';
    if (!storage_sd_is_mounted()) {
        copy_string(config.error, sizeof(config.error), "no card");
        return false;
    }
    FILE *file = fopen(MacroPadConfig::kPath, "rb");
    if (!file) {
        copy_string(config.error, sizeof(config.error), "no file");
        return false;
    }
    char *buffer = new (std::nothrow) char[kMaxFileBytes + 1];
    if (!buffer) {
        fclose(file);
        copy_string(config.error, sizeof(config.error), "out of memory");
        return false;
    }
    size_t len = fread(buffer, 1, kMaxFileBytes, file);
    fclose(file);
    buffer[len] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        // cJSON_GetErrorPtr points into the input buffer — read it
        // before freeing the buffer.
        const char *err = cJSON_GetErrorPtr();
        char detail[MacroPadConfig::kMaxError];
        if (err) {
            char snippet[48] = {};
            size_t n = 0;
            while (n < sizeof(snippet) - 1 && err[n] != '\0' && err[n] != '\n' && err[n] != '\r') {
                snippet[n] = err[n];
                n++;
            }
            snprintf(detail, sizeof(detail), "JSON error near: %s", snippet);
        } else {
            snprintf(detail, sizeof(detail), "JSON error");
        }
        delete[] buffer;
        copy_string(config.error, sizeof(config.error), detail);
        ESP_LOGW(TAG, "Config parse failed: %s", config.error);
        return false;
    }
    delete[] buffer;

    // Keys: entries override the defaults by index; missing entries and
    // unknown fields keep their defaults. A bad action on one key leaves
    // that key unassigned instead of failing the whole file.
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys");
    if (cJSON_IsArray(keys)) {
        int index = 0;
        const cJSON *entry = nullptr;
        cJSON_ArrayForEach(entry, keys) {
            if (index >= MacroPadConfig::kKeyCount) {
                break;
            }
            MacroPadKey &key = config.keys[index];
            index++;
            if (!cJSON_IsObject(entry)) {
                continue;
            }
            const cJSON *icon = cJSON_GetObjectItemCaseSensitive(entry, "icon");
            if (cJSON_IsString(icon)) {
                copy_string(key.icon, sizeof(key.icon), icon->valuestring);
            }
            const cJSON *color = cJSON_GetObjectItemCaseSensitive(entry, "color");
            if (cJSON_IsString(color)) {
                uint32_t parsed = 0;
                if (parse_hex_color(color->valuestring, parsed)) {
                    key.color = parsed;
                }
            }
            const cJSON *label = cJSON_GetObjectItemCaseSensitive(entry, "label");
            if (cJSON_IsString(label)) {
                copy_string(key.label, sizeof(key.label), label->valuestring);
            }
            const cJSON *action = cJSON_GetObjectItemCaseSensitive(entry, "action");
            if (action) {
                MacroPadAction parsed_action;
                if (parse_action(action, parsed_action)) {
                    key.action = parsed_action;
                } else {
                    ESP_LOGW(TAG, "Key %d: bad action, left unassigned", index - 1);
                    key.action = MacroPadAction{};
                }
            }
        }
    }

    const cJSON *knob = cJSON_GetObjectItemCaseSensitive(root, "knob");
    if (cJSON_IsObject(knob)) {
        const cJSON *cw = cJSON_GetObjectItemCaseSensitive(knob, "cw");
        if (cw) {
            MacroPadAction parsed_action;
            if (parse_action(cw, parsed_action) &&
                    (parsed_action.type == MacroPadAction::Type::Combo ||
                     parsed_action.type == MacroPadAction::Type::Consumer)) {
                config.knob_cw = parsed_action;
            } else {
                ESP_LOGW(TAG, "Knob cw: only combo/consumer actions apply");
            }
        }
        const cJSON *ccw = cJSON_GetObjectItemCaseSensitive(knob, "ccw");
        if (ccw) {
            MacroPadAction parsed_action;
            if (parse_action(ccw, parsed_action) &&
                    (parsed_action.type == MacroPadAction::Type::Combo ||
                     parsed_action.type == MacroPadAction::Type::Consumer)) {
                config.knob_ccw = parsed_action;
            } else {
                ESP_LOGW(TAG, "Knob ccw: only combo/consumer actions apply");
            }
        }
    }

    cJSON_Delete(root);
    config.loaded_from_file = true;
    return true;
}

void macropad_action_desc(const MacroPadAction &action, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    switch (action.type) {
    case MacroPadAction::Type::None:
        snprintf(out, out_size, "Unassigned");
        return;
    case MacroPadAction::Type::Consumer:
        snprintf(out, out_size, "%s", macropad_hid_consumer_name(action.consumer_usage));
        return;
    case MacroPadAction::Type::Text:
        snprintf(out, out_size, "Type \"%s\"", action.text);
        return;
    case MacroPadAction::Type::Macro:
        snprintf(out, out_size, "Macro (%d steps)", (int)action.step_count);
        return;
    case MacroPadAction::Type::Combo: {
        // "Cmd+Space", "Ctrl+Shift+T": modifiers first, then the keys.
        char desc[64] = {};
        size_t pos = 0;
        auto add_part = [&](const char *name) {
            if (pos > 0) {
                pos += (size_t)snprintf(desc + pos, sizeof(desc) - pos, "+");
            }
            pos += (size_t)snprintf(desc + pos, sizeof(desc) - pos, "%s", name);
        };
        if (action.modifiers & 0x01) {
            add_part("Ctrl");
        }
        if (action.modifiers & 0x02) {
            add_part("Shift");
        }
        if (action.modifiers & 0x04) {
            add_part("Alt");
        }
        if (action.modifiers & 0x08) {
            add_part("Cmd");
        }
        if (action.modifiers & 0x10) {
            add_part("RCtrl");
        }
        if (action.modifiers & 0x20) {
            add_part("RShift");
        }
        if (action.modifiers & 0x40) {
            add_part("RAlt");
        }
        if (action.modifiers & 0x80) {
            add_part("RCmd");
        }
        for (int i = 0; i < action.key_count; i++) {
            const char *name = macropad_hid_key_name(action.keys[i]);
            if (name[0] != '\0') {
                add_part(name);
            }
        }
        if (pos == 0) {
            snprintf(out, out_size, "Combo");
        } else {
            snprintf(out, out_size, "%s", desc);
        }
        return;
    }
    }
    snprintf(out, out_size, "?");
}

} // namespace esp_brookesia::apps
