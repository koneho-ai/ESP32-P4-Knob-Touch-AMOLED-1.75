/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MuseUi.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <math.h>

#include "app_fonts.h"
#include "board_app.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"   /* custom decoder (dec->name) */
#include "src/misc/lv_area_private.h"            /* lv_area_intersect, for the ring */

extern "C" {
// No extern "C" guards of their own (SDK-verbatim headers).
#include "muse_mem.h"     // MUSE_BIG_CAPS
#include "muse_pixel.h"   // the avatar renderer
}

namespace esp_brookesia::apps {

namespace {

/*
 * Muse reaches LVGL as an image whose pixels are made on demand: a decoder
 * scales the 64 px grid up one strip of rows at a time, so the full-size
 * frame never sits in RAM and LVGL never runs its (slow) image transform.
 *
 * LVGL splits each refresh into a tile per draw unit and draws them at once,
 * each with its own open decoder, so each one takes its own strip: with one
 * shared strip, a tile blended rows the other had just scaled into it.
 * (SDK muse_ui.c, verbatim in structure.)
 */
#define STRIP_ROWS 16
#define STRIPS LV_DRAW_SW_DRAW_UNIT_CNT

#define MUSE_PIXEL_MAX_PX 512   /* muse_pixel_set_size's cap */
#define DIRTY_RECTS_MAX 8       /* with the meter and labels, well inside LVGL's 32 areas */
#define DIRTY_MERGE_PX 2048     /* pixels that cost about one walk of the widget tree */
#define RING_SLAB_ROWS 52

float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

void setHidden(lv_obj_t *object, bool hidden)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void setLabelTextIfChanged(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    const char *value = text ? text : "";
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, value) != 0) {
        lv_label_set_text(label, value);
    }
}

void copyText(char *destination, size_t capacity, const char *source)
{
    snprintf(destination, capacity, "%s", source ? source : "");
}

lv_obj_t *createPlain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return obj;
}

/* The caption font the session's pixel pager measures with. The built-in
 * Montserrat 20 until preload() loads the app font (cached forever once
 * loaded, so this pointer stays valid and the pager never touches flash). */
const lv_font_t *s_caption_font = &lv_font_montserrat_20;

lv_image_dsc_t s_muse_src;
lv_draw_buf_t *s_strips[STRIPS];
bool s_strip_busy[STRIPS];
uint16_t *s_cells = nullptr;    /* each cell's colour as last invalidated */
uint16_t *s_cell_row = nullptr; /* one screen row of Muse */
bool s_cells_valid = false;
bool s_muse_dec_inited = false;
lv_obj_t *s_canvas = nullptr;   /* the avatar image object, while the UI is up */
lv_obj_t *s_ring_obj = nullptr; /* the progress ring, while the UI is up */

lv_result_t muse_dec_info(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)dec;
    if (dsc->src != &s_muse_src) {
        return LV_RESULT_INVALID;
    }
    *header = s_muse_src.header;
    return LV_RESULT_OK;
}

/* LVGL holds its decoder lock around open and close. */
lv_result_t muse_dec_open(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (!s_strip_busy[i]) {
            s_strip_busy[i] = true;
            dsc->user_data = s_strips[i];
            return LV_RESULT_OK;   /* the rest of the work is in get_area */
        }
    }
    return LV_RESULT_INVALID;
}

lv_result_t muse_dec_get_area(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                              const lv_area_t *full, lv_area_t *area)
{
    (void)dec;
    int32_t y1 = area->y1 == LV_COORD_MIN ? full->y1 : area->y2 + 1;
    if (y1 > full->y2) {
        return LV_RESULT_INVALID;
    }
    int32_t y2 = LV_MIN(y1 + STRIP_ROWS - 1, full->y2);
    int32_t w = lv_area_get_width(full);
    lv_draw_buf_t *buf = lv_draw_buf_reshape(static_cast<lv_draw_buf_t *>(dsc->user_data),
                                             LV_COLOR_FORMAT_RGB565, w, y2 - y1 + 1, LV_STRIDE_AUTO);
    if (!buf) {
        return LV_RESULT_INVALID;
    }
    muse_pixel_scale((uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t), full->x1, full->x2, y1, y2);
    area->x1 = full->x1;
    area->x2 = full->x2;
    area->y1 = y1;
    area->y2 = y2;
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

void muse_dec_close(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i] == dsc->user_data) {
            s_strip_busy[i] = false;   /* kept for the next open */
        }
    }
}

/* Created once, the first time the UI opens, and kept for the firmware's
 * life like the SDK's: the decoder can't be un-registered, and the strips
 * and cell buffers are size-independent (they reshape per frame). */
void muse_image_init(int px)
{
    if (s_muse_dec_inited) {
        return;
    }
    s_muse_dec_inited = true;

    s_muse_src.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_muse_src.header.cf = LV_COLOR_FORMAT_RGB565;
    s_muse_src.header.w = px;
    s_muse_src.header.h = px;
    s_muse_src.header.stride = px * sizeof(uint16_t);
    s_muse_src.data = (const uint8_t *)&s_muse_src;   /* LVGL skips sources without data */
    muse_pixel_set_size(px);

    for (int i = 0; i < STRIPS; i++) {
        s_strips[i] = lv_draw_buf_create(px, STRIP_ROWS, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        assert(s_strips[i]);
    }
    lv_image_decoder_t *dec = lv_image_decoder_create();
    assert(dec);
    lv_image_decoder_set_info_cb(dec, muse_dec_info);
    lv_image_decoder_set_open_cb(dec, muse_dec_open);
    lv_image_decoder_set_get_area_cb(dec, muse_dec_get_area);
    lv_image_decoder_set_close_cb(dec, muse_dec_close);
    dec->name = "muse";

    s_cells = (uint16_t *)heap_caps_malloc(MUSE_PX_W * MUSE_PX_H * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_cell_row = (uint16_t *)heap_caps_malloc(MUSE_PIXEL_MAX_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
    if (!s_cells || !s_cell_row) {
        free(s_cells);
        free(s_cell_row);
        s_cells = s_cell_row = nullptr;   /* redraw all of Muse every frame */
    }
}

/*
 * Most frames change only a few of Muse's cells, so only those are redrawn.
 * Each frame reads back one screen pixel of every cell (its first, which the
 * faint grid never dims) and compares it with the last frame's. A row's
 * changed cells make one span, and neighbouring spans merge while the pixels
 * drawn for nothing cost less than LVGL's walk of the widget tree for one more
 * area. (SDK muse_ui.c, verbatim.)
 */
int32_t cell_px(int cell, int size)
{
    return (cell * size + MUSE_PX_W - 1) / MUSE_PX_W;   /* a cell's first screen pixel */
}

int32_t rect_cells(const lv_area_t *r)
{
    return lv_area_get_width(r) * lv_area_get_height(r);
}

void invalidate_muse(void)
{
    if (!s_canvas) {
        return;
    }
    if (!s_cells) {
        lv_obj_invalidate(s_canvas);
        return;
    }
    int size = (int)s_muse_src.header.w;
    lv_area_t r[MUSE_PX_H];   /* in cells */
    int n = 0;
    for (int cy = 0; cy < MUSE_PX_H; cy++) {
        int32_t y = cell_px(cy, size);
        muse_pixel_scale(s_cell_row, size, 0, size - 1, y, y);
        uint16_t *last = &s_cells[cy * MUSE_PX_W];
        int x1 = -1, x2 = -1;
        for (int cx = 0; cx < MUSE_PX_W; cx++) {
            uint16_t c = s_cell_row[cell_px(cx, size)];
            if (c != last[cx] || !s_cells_valid) {
                last[cx] = c;
                x1 = x1 < 0 ? cx : x1;
                x2 = cx;
            }
        }
        if (x2 >= 0) {
            r[n++] = lv_area_t{x1, cy, x2, cy};
        }
    }
    s_cells_valid = true;

    int64_t merge_cells = (int64_t)DIRTY_MERGE_PX * MUSE_PX_W * MUSE_PX_H / (size * size);
    while (n > 1) {
        int best = 0;
        int32_t best_cost = INT32_MAX;
        for (int i = 0; i + 1 < n; i++) {
            lv_area_t j = {LV_MIN(r[i].x1, r[i + 1].x1), r[i].y1, LV_MAX(r[i].x2, r[i + 1].x2), r[i + 1].y2};
            int32_t cost = rect_cells(&j) - rect_cells(&r[i]) - rect_cells(&r[i + 1]);
            if (cost < best_cost) {
                best = i;
                best_cost = cost;
            }
        }
        if (n <= DIRTY_RECTS_MAX && best_cost > merge_cells) {
            break;
        }
        r[best].x1 = LV_MIN(r[best].x1, r[best + 1].x1);
        r[best].x2 = LV_MAX(r[best].x2, r[best + 1].x2);
        r[best].y2 = r[best + 1].y2;
        memmove(&r[best + 1], &r[best + 2], (n - best - 2) * sizeof(r[0]));
        n--;
    }

    lv_area_t at;
    lv_obj_get_coords(s_canvas, &at);
    for (int i = 0; i < n; i++) {
        lv_area_t a = {
            at.x1 + cell_px(r[i].x1, size),
            at.y1 + cell_px(r[i].y1, size),
            at.x1 + cell_px(r[i].x2 + 1, size) - 1,
            at.y1 + cell_px(r[i].y2 + 1, size) - 1,
        };
        lv_obj_invalidate_area(s_canvas, &a);
    }
}

/*
 * A full circle's arc masks every pixel of every row it's asked to draw, hole
 * and corners included, which costs a full-screen redraw (a swipe) several ms.
 * So the ring is drawn a slab of rows at a time, each only as wide as the ring
 * is there: where a slab crosses the hole, a left piece and a right one.
 * (SDK muse_ui.c, verbatim.)
 */
void on_ring_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    lv_area_t c;
    lv_obj_get_coords(s_ring_obj, &c);
    int32_t cx = (c.x1 + c.x2) / 2;
    int32_t cy = (c.y1 + c.y2) / 2;
    int32_t out = lv_area_get_width(&c) / 2 + 1;                                     /* with antialiasing */
    int32_t hole = out - 1 - lv_obj_get_style_arc_width(s_ring_obj, LV_PART_MAIN) - 2;   /* without */
    lv_event_stop_processing(e);
    for (int32_t y = LV_MAX(clip.y1, cy - out); y <= LV_MIN(clip.y2, cy + out); y += RING_SLAB_ROWS) {
        int32_t y2 = LV_MIN(y + RING_SLAB_ROWS - 1, clip.y2);
        int32_t near = y <= cy && cy <= y2 ? 0 : LV_MIN(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t far = LV_MAX(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t ow = near < out ? (int32_t)ceilf(sqrtf((float)(out * out - near * near))) : 0;
        int32_t iw = far < hole ? (int32_t)sqrtf((float)(hole * hole - far * far)) : 0;
        lv_area_t pieces[2] = {
            { cx - ow, y, iw ? cx - iw : cx + ow, y2 },
            { cx + iw, y, cx + ow, y2 },
        };
        for (int i = 0; i < (iw ? 2 : 1); i++) {
            if (lv_area_intersect(&layer->_clip_area, &clip, &pieces[i])) {
                lv_obj_event_base(nullptr, e);   /* the arc's own drawing */
            }
        }
    }
    layer->_clip_area = clip;
}

} // namespace

int MuseUi::captionGlyphPx(uint32_t letter, uint32_t next)
{
    return lv_font_get_glyph_width(s_caption_font, letter, next);
}

MuseUi::MuseUi()
{
    _image_mutex = xSemaphoreCreateMutex();
}

MuseUi::~MuseUi()
{
    destroy();
    if (_image_mutex) {
        vSemaphoreDelete(_image_mutex);
        _image_mutex = nullptr;
    }
}

bool MuseUi::preload()
{
    _text_font = app_font_text_20();
    if (_text_font) {
        s_caption_font = _text_font;
    }
    return _text_font != nullptr;
}

bool MuseUi::create(
    lv_obj_t *parent,
    ActionCallback action_callback,
    void *action_context,
    ActionCallback hold_callback,
    void *hold_context
)
{
    destroyView();
    if (!parent || !_text_font) {
        return false;
    }
    _action_callback = action_callback;
    _action_context = action_context;
    _hold_callback = hold_callback;
    _hold_context = hold_context;

    // Black, like the SDK's face tile: the avatar's canvas is black, and a
    // different background would show as a square around the character.
    _root = createPlain(parent);
    lv_obj_set_size(_root, lv_pct(100), lv_pct(100));
    lv_obj_center(_root);
    lv_obj_set_style_bg_color(_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);
    // SHORT_CLICKED, not CLICKED: a long press on the avatar pets it, and
    // CLICKED fires even after a long press — SHORT_CLICKED is suppressed.
    lv_obj_add_event_cb(_root, rootEventCallback, LV_EVENT_SHORT_CLICKED, this);
    // LONG_PRESSED bubbles up from any child that doesn't stop it; the
    // avatar stops it (pet), so a hold anywhere else starts a call.
    lv_obj_add_event_cb(_root, rootHoldCallback, LV_EVENT_LONG_PRESSED, this);

    // The progress ring around the bezel.
    _ring = lv_arc_create(_root);
    lv_obj_set_size(_ring, kRingPx, kRingPx);
    lv_obj_center(_ring);
    lv_arc_set_bg_angles(_ring, 0, 360);
    lv_arc_set_rotation(_ring, 270);
    lv_arc_set_range(_ring, 0, kRingRange);
    lv_arc_set_value(_ring, 0);
    lv_obj_remove_style(_ring, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(_ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, lv_color_hex(kColorRingBg), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(_ring, false, LV_PART_INDICATOR);
    lv_obj_add_event_cb(_ring, on_ring_draw,
                        static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS), nullptr);
    s_ring_obj = _ring;

    /*
     * Full layout, from the bottom up (offsets from the centre): two caption
     * lines as low as the ring leaves them room, the level meter, then Muse,
     * whose blank bottom rows can tuck in behind the meter. (SDK build_screen.)
     */
    const int ring_in = kScreenPx / 2 - 10;   /* the ring's inner edge: 223 */
    const int cap_bottom = replyBottom(kCaptionWidthPx, ring_in);   /* 179 */
    const int cap_h = 2 * 16 + kCaptionLineSpace;   /* the SDK's live-caption box */
    const int cap_top = cap_bottom - cap_h;
    const int meter_y = cap_top - 6 - kMeterSegPx / 2;
    const int art_bottom = meter_y - kMeterSegPx / 2 - 4;
    _big_y = art_bottom - (kCanvasPx / 2 - kArtBlankRows * (kCanvasPx / MUSE_PX_W));

    // The character.
    muse_image_init(kCanvasPx);
    _avatar = lv_image_create(_root);
    lv_image_set_src(_avatar, &s_muse_src);
    lv_obj_align(_avatar, LV_ALIGN_CENTER, 0, _big_y);
    _avatar_px = kCanvasPx;
    _avatar_y = _big_y;
    setCanvasPx(kCanvasPx);   /* a previous open may have left it shrunk */
    s_canvas = _avatar;
    s_cells_valid = false;    /* a new object: redraw every cell */
    lv_obj_add_flag(_avatar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_avatar, avatarEventCallback, LV_EVENT_LONG_PRESSED, this);
    // The canvas's black corners reach the bezel; keep the ring on top.
    lv_obj_move_foreground(_ring);

    // Chunky level meter (hidden until listening).
    const int span = kMeterSegs * (kMeterSegPx + kMeterGapPx) - kMeterGapPx;
    for (int i = 0; i < kMeterSegs; i++) {
        lv_obj_t *seg = createPlain(_root);
        lv_obj_set_size(seg, kMeterSegPx, kMeterSegPx);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(seg, lv_color_hex(kColorMeterOff), LV_PART_MAIN);
        int x = -span / 2 + i * (kMeterSegPx + kMeterGapPx) + kMeterSegPx / 2;
        lv_obj_align(seg, LV_ALIGN_CENTER, x, meter_y);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
        _meter[i] = seg;
    }
    _meter_visible = false;

    // The state name at the top, in the mode's accent. The SDK uses
    // unscii_16; the factory LVGL has no Unscii, Montserrat 16 is the
    // closest built-in.
    _state_label = lv_label_create(_root);
    lv_obj_set_style_text_font(_state_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(_state_label, 2, LV_PART_MAIN);
    lv_obj_set_style_text_color(_state_label, lv_color_hex(muse_pixel_accent(_avatar_mode)), LV_PART_MAIN);
    lv_obj_set_style_text_align(_state_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(_state_label, LV_ALIGN_TOP_MID, 0, kStateY);
    lv_label_set_text(_state_label, _status);

    /*
     * The caption box (the SDK's heard-answer layout): Muse a size smaller,
     * where it was if there's room, over three lines at the bottom. The
     * reply page shows here, or the user transcript while thinking.
     */
    const int pitch = lv_font_get_line_height(_text_font) + kCaptionLineSpace;
    const int cap_box_h = 3 * pitch - kCaptionLineSpace;
    const int heard_cell = kCanvasPx / MUSE_PX_W - 1;
    _heard_px = MUSE_PX_W * heard_cell;
    _heard_y = _big_y;
    const int heard_art_bottom = _heard_y + _heard_px / 2 - kArtBlankRows * heard_cell;
    _caption_top = replyBottom(kCaptionWidthPx, ring_in) - cap_box_h;
    if (_caption_top < heard_art_bottom + 6) {
        _heard_y -= heard_art_bottom + 6 - _caption_top;   /* no room under Muse: it moves up */
    }
    _caption = lv_label_create(_root);
    lv_obj_set_size(_caption, kCaptionWidthPx, cap_box_h);
    lv_obj_set_style_text_font(_caption, _text_font, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(_caption, kCaptionLineSpace, LV_PART_MAIN);
    lv_obj_set_style_text_color(_caption, lv_color_hex(kColorCaption), LV_PART_MAIN);
    lv_obj_set_style_text_align(_caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    // Pages come wrapped to fit; the transcript while thinking doesn't.
    lv_label_set_long_mode(_caption, LV_LABEL_LONG_WRAP);
    lv_obj_align(_caption, LV_ALIGN_CENTER, 0, _caption_top + cap_box_h / 2);
    lv_obj_add_flag(_caption, LV_OBJ_FLAG_HIDDEN);
    _caption_shown = false;

    // What a tap does now, at the bottom.
    _hint_label = lv_label_create(_root);
    lv_obj_set_size(_hint_label, kHintW, LV_SIZE_CONTENT);
    lv_obj_align(_hint_label, LV_ALIGN_TOP_MID, 0, kHintY);
    lv_obj_set_style_text_font(_hint_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(_hint_label, lv_color_hex(kColorHint), LV_PART_MAIN);
    lv_obj_set_style_text_align(_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(_hint_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(_hint_label, _hint);

    // A downloaded image (Link display.*): over everything on the screen,
    // dismissed by a tap. Created last so it sits on top; hidden until the
    // first rectangle arrives (imageSync unhides it on the frame tick).
    _image = lv_image_create(_root);
    lv_obj_set_size(_image, kScreenPx, kScreenPx);
    lv_obj_center(_image);
    lv_obj_add_flag(_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_image, imageEventCallback, LV_EVENT_SHORT_CLICKED, this);

    _mode_valid = false;
    _mode_since_us = esp_timer_get_time();
    _boot_started_us = _mode_since_us;
    _boot_until_us = _mode_since_us + (int64_t)(kBootSecs * 1e6f);
    _ring_value = -1;
    _shown_lit = -1;
    _tick = lv_timer_create(tickCallback, kTickMs, this);
    applyMode();
    updateTexts();
    if (_image_mutex) {
        xSemaphoreTake(_image_mutex, portMAX_DELAY);
        _view_live = true;
        xSemaphoreGive(_image_mutex);
    }
    return true;
}

void MuseUi::destroy()
{
    destroyView();
}

void MuseUi::destroyView()
{
    if (_tick) {
        lv_timer_delete(_tick);
        _tick = nullptr;
    }
    lv_anim_delete(this, nullptr);   /* stop a shrink/grow mid-flight */
    // Liveness off first (under the mutex, so a download in flight either
    // finishes before this or refuses after), then free the image buffer
    // while the mutex still serializes with the download task.
    if (_image_mutex) {
        xSemaphoreTake(_image_mutex, portMAX_DELAY);
        _view_live = false;
        xSemaphoreGive(_image_mutex);
    }
    imageHideLocked();
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
    _ring = nullptr;
    _avatar = nullptr;
    _image = nullptr;
    _state_label = nullptr;
    _caption = nullptr;
    _hint_label = nullptr;
    for (int i = 0; i < kMeterSegs; i++) {
        _meter[i] = nullptr;
    }
    s_canvas = nullptr;
    s_ring_obj = nullptr;
}

void MuseUi::setNetworkReady(bool ready)
{
    // The mode already carries the offline state; nothing extra to show.
    (void)ready;
}

void MuseUi::setStatus(const char *status)
{
    copyText(_status, sizeof(_status), status);
    updateTexts();
}

muse_mode_t MuseUi::avatarMode(Mode mode)
{
    switch (mode) {
    case Mode::Listening:
        return MUSE_MODE_LISTENING;
    case Mode::Thinking:
        return MUSE_MODE_THINKING;
    case Mode::Speaking:
        return MUSE_MODE_SPEAKING;
    case Mode::Busy:
        return MUSE_MODE_THINKING;   /* preparing/connecting looks like thinking */
    case Mode::Error:
        return MUSE_MODE_ERROR;
    case Mode::Offline:
    case Mode::NotProvisioned:
    case Mode::Ready:
    default:
        return MUSE_MODE_IDLE;
    }
}

void MuseUi::setMode(Mode mode)
{
    if (_mode != mode) {
        _mode = mode;
        _avatar_mode = avatarMode(mode);
        _mode_since_us = esp_timer_get_time();
        _mode_valid = false;
    }
    applyMode();
}

void MuseUi::setWorking(bool working)
{
    _working = working;
}

void MuseUi::setLevel(float level)
{
    _level_target = clamp01(level);
}

void MuseUi::setProgress(float progress)
{
    _progress = clamp01(progress);
}

void MuseUi::setChatMessage(const char *role, const char *text)
{
    if (!text || text[0] == '\0') {
        // Empty text clears the conversation (both sides): the caption box
        // shows one of them, so a stale transcript would keep the avatar
        // shrunk into the next turn.
        _user_text[0] = '\0';
        _reply_text[0] = '\0';
    } else if (role && strcmp(role, "user") == 0) {
        copyText(_user_text, sizeof(_user_text), text);
    } else {
        copyText(_reply_text, sizeof(_reply_text), text);
    }
    updateTexts();
}

void MuseUi::clearChatMessages()
{
    _user_text[0] = '\0';
    _reply_text[0] = '\0';
    updateTexts();
}

void MuseUi::setHint(const char *hint)
{
    copyText(_hint, sizeof(_hint), hint);
    updateTexts();
}

void MuseUi::showToast(const char *text)
{
    if (!_state_label) {
        return;
    }
    setLabelTextIfChanged(_state_label, text);
    _toast_until = lv_tick_get() + kToastMs;
}

// ---- Downloaded image overlay (Phase 4.3; SDK muse_ui.c image_*) ----

bool MuseUi::imageSize(int *w, int *h)
{
    if (!w || !h || !_image_mutex || !heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        return false;
    }
    xSemaphoreTake(_image_mutex, portMAX_DELAY);
    bool live = _view_live;
    xSemaphoreGive(_image_mutex);
    if (!live) {
        return false;
    }
    *w = kScreenPx;
    *h = kScreenPx;
    return true;
}

int MuseUi::imageBits()
{
    return 16;   /* RGB565 colour panel */
}

bool MuseUi::imageDraw(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > kScreenPx || y + h > kScreenPx ||
            !pixels || !_image_mutex) {
        return false;
    }
    xSemaphoreTake(_image_mutex, portMAX_DELAY);
    // Liveness is checked inside the mutex: destroyView clears it under
    // the same mutex before freeing the buffer, so a draw that passes
    // here cannot race a teardown.
    if (!_view_live) {
        xSemaphoreGive(_image_mutex);
        return false;
    }
    if (!_image_buf) {
        /* The first rectangle of an image: a black screen to draw onto. */
        _image_buf = static_cast<uint16_t *>(heap_caps_calloc(
            (size_t)kScreenPx * kScreenPx, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
        if (!_image_buf) {
            xSemaphoreGive(_image_mutex);
            return false;
        }
    }
    const uint8_t *src = reinterpret_cast<const uint8_t *>(pixels);
    for (int row = 0; row < h; row++) {
        uint16_t *dst = _image_buf + (size_t)(y + row) * kScreenPx + x;
        for (int i = 0; i < w; i++, src += 2) {
            dst[i] = (uint16_t)(src[0] << 8 | src[1]);
        }
    }
    if (!_image_dirty) {
        _image_area = {x, y, x + w - 1, y + h - 1};
        _image_dirty = true;
    } else {
        _image_area.x1 = LV_MIN(_image_area.x1, x);
        _image_area.y1 = LV_MIN(_image_area.y1, y);
        _image_area.x2 = LV_MAX(_image_area.x2, x + w - 1);
        _image_area.y2 = LV_MAX(_image_area.y2, y + h - 1);
    }
    xSemaphoreGive(_image_mutex);
    return true;
}

void MuseUi::imageDone()
{
    /* A colour panel shows each rectangle as it comes; nothing to do. */
}

void MuseUi::imageHide()
{
    if (!board_display_lock(-1)) {
        return;
    }
    imageHideLocked();
    board_display_unlock();
}

void MuseUi::imageHideLocked()
{
    if (!_image_mutex) {
        return;
    }
    xSemaphoreTake(_image_mutex, portMAX_DELAY);
    if (_image_buf) {
        if (_image) {
            lv_obj_add_flag(_image, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(_image, nullptr);
        }
        heap_caps_free(_image_buf);
        _image_buf = nullptr;
        _image_dsc.data = nullptr;
        _image_dirty = false;
    }
    xSemaphoreGive(_image_mutex);
}

/* Each frame: shows a new image and redraws what the download changed. */
void MuseUi::imageSync()
{
    if (!_image_mutex || !_image) {
        return;
    }
    xSemaphoreTake(_image_mutex, portMAX_DELAY);
    if (_image_buf && !_image_dsc.data) {
        _image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        _image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        _image_dsc.header.w = kScreenPx;
        _image_dsc.header.h = kScreenPx;
        _image_dsc.header.stride = kScreenPx * sizeof(uint16_t);
        _image_dsc.data_size = (size_t)kScreenPx * kScreenPx * sizeof(uint16_t);
        _image_dsc.data = (const uint8_t *)_image_buf;
        lv_image_set_src(_image, &_image_dsc);
        lv_obj_remove_flag(_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (_image_dirty) {
        lv_obj_invalidate_area(_image, &_image_area);
        _image_dirty = false;
    }
    xSemaphoreGive(_image_mutex);
}

void MuseUi::imageEventCallback(lv_event_t *event)
{
    auto *instance = static_cast<MuseUi *>(lv_event_get_user_data(event));
    if (!instance || lv_event_get_code(event) != LV_EVENT_SHORT_CLICKED) {
        return;
    }
    // A tap dismisses the image — and must not also reach the root's
    // SHORT_CLICKED handler (which would start a talk): stop the event
    // bubbling here. SHORT_CLICKED fires before CLICKED, so handling
    // CLICKED (as the SDK does — its root has no SHORT_CLICKED talk)
    // would be too late. A long press still bubbles to the root, where
    // it starts a call, and the call hides the image.
    lv_event_stop_bubbling(event);
    instance->imageHideLocked();
}

void MuseUi::applyMode()
{
    if (_mode_valid || !_ring || !_state_label) {
        return;
    }
    _mode_valid = true;
    // The state name and the ring take the mode's accent (SDK update_status).
    const uint32_t accent = muse_pixel_accent(_avatar_mode);
    lv_obj_set_style_text_color(_state_label, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, lv_color_hex(accent), LV_PART_INDICATOR);
    _shown_lit = -1;   /* recolor the meter in the new accent */
}

void MuseUi::updateTexts()
{
    if (!_root) {
        return;
    }
    // A toast owns the state name until it expires.
    if (_toast_until && lv_tick_get() < _toast_until) {
        // Leave the toast text alone; the tick restores the status.
    } else {
        _toast_until = 0;
        setLabelTextIfChanged(_state_label, _status);
    }
    // One caption box: the reply page if there is one, else the transcript.
    const char *text = _reply_text[0] ? _reply_text : _user_text;
    const bool show = text[0] != '\0';
    if (show != _caption_shown) {
        _caption_shown = show;
        setHidden(_caption, !show);
        moveAvatar(show ? _heard_px : kCanvasPx, show ? _heard_y : _big_y);
    }
    if (show) {
        lv_obj_set_style_text_color(_caption,
                                    lv_color_hex(_reply_text[0] ? kColorCaption : kColorDim), LV_PART_MAIN);
        setLabelTextIfChanged(_caption, text);
    }
    setLabelTextIfChanged(_hint_label, _hint);
}

void MuseUi::frameTick()
{
    if (!_root) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    const float now = (float)now_us / 1e6f;

    // Fast attack, slow release keeps the mouth and meter lively but readable.
    _level_smooth += (_level_target - _level_smooth) * (_level_target > _level_smooth ? 0.6f : 0.2f);

    muse_mode_t pose_mode = _avatar_mode;
    float mode_t = (float)(now_us - _mode_since_us) / 1e6f;
    if (now_us < _boot_until_us) {
        // The wake-up animation plays over the app's first 1.4 s.
        pose_mode = MUSE_MODE_BOOT;
        mode_t = (float)(now_us - _boot_started_us) / 1e6f;
    }
    muse_pose_t pose = {};
    pose.mode = pose_mode;
    pose.t = now;
    pose.mode_t = mode_t;
    pose.level = _level_smooth;
    pose.happy = happiness();
    pose.working = _working ? 1 : 0;   /* live calls only (Phase 4.1) */
    muse_pixel_render(&pose);
    invalidate_muse();

    updateRing(now);
    updateMeter();

    // A downloaded image shows its new bands (imageSync attaches the
    // descriptor and unhides it on the first one).
    imageSync();

    // A toast that expired hands the state name back to the status.
    if (_toast_until && lv_tick_get() >= _toast_until) {
        _toast_until = 0;
        setLabelTextIfChanged(_state_label, _status);
    }
}

void MuseUi::updateRing(float now)
{
    // Ring: phase progress while listening/speaking, a spinner while thinking.
    int ring = 0;
    if (_avatar_mode == MUSE_MODE_LISTENING || _avatar_mode == MUSE_MODE_SPEAKING) {
        ring = (int)(_progress * kRingRange);
    } else if (_avatar_mode == MUSE_MODE_THINKING) {
        ring = kRingRange / 6;
    }
    if (_avatar_mode == MUSE_MODE_THINKING) {
        // Move the indicator rather than rotate the arc: a rotation redraws
        // the whole ring, and with it the whole screen.
        int start = (int)(now * 300.0f) % 360;
        lv_arc_set_angles(_ring, start, start + ring * 360 / kRingRange);
        _ring_value = -1;
    } else {
        if (_ring_value < 0) {
            lv_arc_set_bg_start_angle(_ring, 0);   /* unchanged, but puts the value's angles back */
        }
        if (ring != _ring_value) {
            lv_arc_set_value(_ring, ring);
            _ring_value = ring;
        }
    }
}

void MuseUi::updateMeter()
{
    // The mic's meter; a reply's page has the room while answering.
    const bool meter = _avatar_mode == MUSE_MODE_LISTENING;
    setMeterVisible(meter);
    if (!meter) {
        return;
    }
    const int lit = (int)lroundf(_level_smooth * kMeterSegs);
    const uint32_t accent = muse_pixel_accent(_avatar_mode);
    if (lit != _shown_lit || accent != _shown_accent) {
        invalidateMeter();
        for (int i = 0; i < kMeterSegs; i++) {
            // Outer segments light last, like a centred VU.
            int rank = abs(2 * i - (kMeterSegs - 1)) / 2;
            bool on = rank < (lit + 1) / 2;
            lv_obj_set_style_bg_color(_meter[i], lv_color_hex(on ? accent : kColorMeterOff), LV_PART_MAIN);
        }
        _shown_lit = lit;
        _shown_accent = accent;
    }
}

void MuseUi::setMeterVisible(bool visible)
{
    if (visible == _meter_visible) {
        return;
    }
    _meter_visible = visible;
    invalidateMeter();
    for (int i = 0; i < kMeterSegs; i++) {
        setHidden(_meter[i], !visible);
    }
}

void MuseUi::invalidateMeter()
{
    if (!_meter[0] || !_meter[kMeterSegs - 1]) {
        return;
    }
    lv_area_t a, last;
    lv_obj_get_coords(_meter[0], &a);
    lv_obj_get_coords(_meter[kMeterSegs - 1], &last);
    a.x2 = last.x2;
    lv_obj_invalidate_area(lv_obj_get_parent(_meter[0]), &a);
}

void MuseUi::moveAvatar(int px, int y)
{
    if (!_avatar) {
        return;
    }
    _move_from_px = _avatar_px;
    _move_from_y = _avatar_y;
    _move_to_px = px;
    _move_to_y = y;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, moveAnimCallback);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, kAnswerMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void MuseUi::moveAvatarStep(int32_t t)
{
    setCanvasPx(_move_from_px + (_move_to_px - _move_from_px) * t / 256);
    _avatar_y = _move_from_y + (_move_to_y - _move_from_y) * t / 256;
    lv_obj_align(_avatar, LV_ALIGN_CENTER, 0, _avatar_y);
}

void MuseUi::setCanvasPx(int px)
{
    if (!_avatar) {
        return;
    }
    _avatar_px = px;
    if ((int)s_muse_src.header.w == px) {
        return;
    }
    s_muse_src.header.w = px;
    s_muse_src.header.h = px;
    s_muse_src.header.stride = px * sizeof(uint16_t);
    muse_pixel_set_size(px);
    lv_image_set_src(_avatar, &s_muse_src);   /* picks up the new size */
}

int MuseUi::replyBottom(int w, int ring_in)
{
    int bottom = kScreenPx / 2 - 28;
    int r = ring_in - 6;
    int chord = w / 2 < r ? (int)sqrtf((float)(r * r - w * w / 4)) : 0;
    return chord < bottom ? chord : bottom;
}

void MuseUi::onPet()
{
    _happy_until_us = esp_timer_get_time() + (int64_t)(kHappySecs * 1e6f);
}

float MuseUi::happiness() const
{
    float left = (float)(_happy_until_us - esp_timer_get_time()) / 1e6f;
    if (left <= 0) {
        return 0;
    }
    /* Ease out over the last 0.4 s. (SDK muse_state_happiness.) */
    return left > 0.4f ? 1.0f : left / 0.4f;
}

void MuseUi::tickCallback(lv_timer_t *timer)
{
    auto *instance = static_cast<MuseUi *>(lv_timer_get_user_data(timer));
    if (instance) {
        instance->frameTick();
    }
}

void MuseUi::rootEventCallback(lv_event_t *event)
{
    auto *instance = static_cast<MuseUi *>(lv_event_get_user_data(event));
    if (instance && instance->_action_callback) {
        instance->_action_callback(instance->_action_context);
    }
}

void MuseUi::rootHoldCallback(lv_event_t *event)
{
    auto *instance = static_cast<MuseUi *>(lv_event_get_user_data(event));
    if (instance && instance->_hold_callback &&
            lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
        instance->_hold_callback(instance->_hold_context);
    }
}

void MuseUi::avatarEventCallback(lv_event_t *event)
{
    auto *instance = static_cast<MuseUi *>(lv_event_get_user_data(event));
    if (instance && lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
        // A pet must not also reach the root's hold handler (which would
        // start a call): stop the event bubbling here.
        lv_event_stop_bubbling(event);
        instance->onPet();
    }
}

void MuseUi::moveAnimCallback(void *var, int32_t t)
{
    auto *instance = static_cast<MuseUi *>(var);
    if (instance) {
        instance->moveAvatarStep(t);
    }
}

} // namespace esp_brookesia::apps
