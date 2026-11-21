/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MacroPadIcons.hpp"

#include <string.h>

namespace esp_brookesia::apps {

namespace {

struct IconEntry {
    const char *name;
    const lv_image_dsc_t *dsc;
};

const IconEntry kIcons[] = {
    {"ufo", &img_macropad_ufo},
    {"bowl", &img_macropad_bowl},
    {"smiley", &img_macropad_smiley},
    {"play_pause", &img_macropad_play_pause},
    {"mute", &img_macropad_mute},
    {"husky", &img_macropad_husky},
    {"df", &img_macropad_df},
    {"fox", &img_macropad_fox},
    {"globe", &img_macropad_globe},
    {"chat", &img_macropad_chat},
    {"paw", &img_macropad_paw},
    {"check", &img_macropad_check},
    {"mic_mute", &img_macropad_mic_mute},
    {"mail", &img_macropad_mail},
    {"folder", &img_macropad_folder},
};

} // namespace

const lv_image_dsc_t *macropad_icon_dsc(const char *name)
{
    if (!name || name[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0; i < sizeof(kIcons) / sizeof(kIcons[0]); i++) {
        if (strcmp(kIcons[i].name, name) == 0) {
            return kIcons[i].dsc;
        }
    }
    return nullptr;
}

int macropad_icon_count()
{
    return (int)(sizeof(kIcons) / sizeof(kIcons[0]));
}

const char *macropad_icon_name(int index)
{
    if (index < 0 || index >= macropad_icon_count()) {
        return "";
    }
    return kIcons[index].name;
}

} // namespace esp_brookesia::apps
