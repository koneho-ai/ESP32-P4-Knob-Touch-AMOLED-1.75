/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Built-in key icons (drawn by tools/gen_icons.py from the user's
 * macropad photo) and the launcher icon.
 */

#pragma once

#include "lvgl.h"

// The image descriptors are defined in C (assets/macropad_icons.c), so
// declare them at global scope with C linkage — inside the namespace
// they would mangle and fail to link.
LV_IMG_DECLARE(img_macropad_ufo);
LV_IMG_DECLARE(img_macropad_bowl);
LV_IMG_DECLARE(img_macropad_smiley);
LV_IMG_DECLARE(img_macropad_play_pause);
LV_IMG_DECLARE(img_macropad_mute);
LV_IMG_DECLARE(img_macropad_husky);
LV_IMG_DECLARE(img_macropad_df);
LV_IMG_DECLARE(img_macropad_fox);
LV_IMG_DECLARE(img_macropad_globe);
LV_IMG_DECLARE(img_macropad_chat);
LV_IMG_DECLARE(img_macropad_paw);
LV_IMG_DECLARE(img_macropad_check);
LV_IMG_DECLARE(img_macropad_mic_mute);
LV_IMG_DECLARE(img_macropad_mail);
LV_IMG_DECLARE(img_macropad_folder);
LV_IMG_DECLARE(img_macropad_app);

namespace esp_brookesia::apps {

// Icon descriptor for a config icon name ("ufo", "paw", ...).
// Returns nullptr for "" (no icon) and for unknown names.
const lv_image_dsc_t *macropad_icon_dsc(const char *name);

// Number of built-in icons (for the README's icon list).
int macropad_icon_count();

// Icon name by index, in photo order.
const char *macropad_icon_name(int index);

} // namespace esp_brookesia::apps
