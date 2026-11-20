/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// The led_status shim for the Muse backend (Phase 4.3): Link's image
// commands draw through this, the way the SDK's led_status.c does for its
// Muse backend (HOMEHUB_LED_BACKEND_MUSE). Only the five functions the
// vendored files call are defined here: display_info + display_bits
// (noise_control.cpp's register), draw_rect + draw_done (image_fetch.c),
// and show_animation (MuseLink's display.show_animation handler). The
// rest of led_status.h (init, set_state, set_title, the voice/level
// ring) has no caller in this firmware and is left undefined.
//
// led_status.h has no extern "C" guard (the SDK's noise_control.cpp
// includes it inside extern "C"), so the header and these definitions
// are wrapped the same way, at global scope. All five go through the
// MuseUi image overlay; with no app instance, no UI, or a view that is
// down, they report no display, so a download that arrives after close()
// refuses cleanly instead of drawing into a deleted view.
//
// UNTESTED ON HARDWARE — no board exists yet; compile verification only.

#include "MuseApp.hpp"
#include "MuseUi.hpp"

extern "C" {
#include "led_status.h"
}

namespace {

esp_brookesia::apps::MuseUi *linkUi()
{
    esp_brookesia::apps::MuseApp *app = esp_brookesia::apps::MuseApp::instance();
    return app ? app->ui() : nullptr;
}

} // namespace

extern "C" {

bool led_status_display_info(int *width, int *height)
{
    esp_brookesia::apps::MuseUi *ui = linkUi();
    return ui && ui->imageSize(width, height);
}

int led_status_display_bits(void)
{
    esp_brookesia::apps::MuseUi *ui = linkUi();
    int w = 0;
    int h = 0;
    return (ui && ui->imageSize(&w, &h)) ? ui->imageBits() : 0;
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels)
{
    esp_brookesia::apps::MuseUi *ui = linkUi();
    return ui && ui->imageDraw(x, y, w, h, pixels);
}

void led_status_draw_done(void)
{
    esp_brookesia::apps::MuseUi *ui = linkUi();
    if (ui) {
        ui->imageDone();
    }
}

void led_status_show_animation(void)
{
    esp_brookesia::apps::MuseUi *ui = linkUi();
    if (ui) {
        ui->imageHide();
    }
}

} // extern "C"
