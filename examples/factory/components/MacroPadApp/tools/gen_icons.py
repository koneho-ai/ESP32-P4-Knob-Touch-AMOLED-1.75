#!/usr/bin/env python3
"""Draw the macropad key icons + launcher icon and emit LVGL 9 ARGB8888
C arrays (one file, assets/macropad_icons.c).

The icons redraw the user's macropad keycaps (photo:
~/workspace/user/files/1000055103_0_1oyu.heic) as flat vector-style art.
Each icon is drawn at 4x supersampling on a transparent background and
downscaled, so edges stay smooth at 56x56. The keycap background color
is NOT baked in — the UI paints it behind the icon, so the config file
can recolor keys freely.

Regenerate: python3 tools/gen_icons.py
Also writes /tmp/macropad_icons_preview.png (the 5x3 grid on keycap
colors) for a visual check.
"""

import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
COMPONENT = os.path.dirname(HERE)
ASSETS = os.path.join(COMPONENT, "assets")

SS = 4                      # supersampling factor
ICON = 56                   # final icon size (px)
S = ICON * SS               # drawing canvas size
APP_ICON = 112              # launcher icon size (px)

WHITE = (242, 242, 242, 255)
DARK = (28, 28, 28, 255)
BROWN_DARK = (62, 36, 22, 255)
GREY_DARK = (58, 54, 50, 255)
NAVY = (46, 68, 80, 255)
PINK = (232, 160, 160, 255)
FOX_HEAD = (192, 138, 90, 255)
FOX_EAR = (138, 90, 51, 255)
GREEN_DARK = (51, 72, 43, 255)
PAW_BROWN = (92, 64, 51, 255)

FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def canvas():
    return Image.new("RGBA", (S, S), (0, 0, 0, 0))


def finish(img, size):
    return img.resize((size, size), Image.LANCZOS)


def rotate_layer(layer, angle):
    """Rotate a transparent layer around the canvas center."""
    return layer.rotate(angle, resample=Image.BICUBIC, center=(S / 2, S / 2))


# ---------------------------------------------------------------- icons

def icon_ufo():
    img = canvas()
    d = ImageDraw.Draw(img)
    # speed lines, lower left
    for i, (x0, y0, x1, y1) in enumerate([
        (18, 168, 62, 150), (14, 188, 66, 172), (26, 206, 66, 192),
    ]):
        d.line([(x0, y0), (x1, y1)], fill=BROWN_DARK, width=9)
        d.ellipse([x0 - 5, y0 - 5, x0 + 5, y0 + 5], fill=BROWN_DARK)
    # saucer on its own layer, then tilted
    layer = canvas()
    dl = ImageDraw.Draw(layer)
    dl.ellipse([52, 96, 196, 152], fill=BROWN_DARK)          # disc
    dl.pieslice([86, 62, 162, 122], 180, 360, fill=BROWN_DARK)  # dome
    dl.ellipse([86, 62, 162, 122], outline=BROWN_DARK, width=2)
    for cx in (84, 124, 164):                                 # lights
        dl.ellipse([cx - 9, 118, cx + 9, 136], fill=(184, 92, 56, 255))
    img = Image.alpha_composite(img, rotate_layer(layer, -18))
    return img


def icon_bowl():
    img = canvas()
    d = ImageDraw.Draw(img)
    # food mound
    for cx, cy, r in [(82, 104, 26), (116, 92, 30), (150, 106, 24)]:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=GREY_DARK)
    # bowl body
    d.pieslice([56, 96, 176, 176], 0, 180, fill=GREY_DARK)
    d.rectangle([56, 96, 176, 118], fill=GREY_DARK)
    # bone, lower left, tilted
    layer = canvas()
    dl = ImageDraw.Draw(layer)
    dl.rectangle([28, 158, 96, 178], fill=GREY_DARK)
    for cx, cy in [(30, 156), (30, 180), (94, 156), (94, 180)]:
        dl.ellipse([cx - 13, cy - 13, cx + 13, cy + 13], fill=GREY_DARK)
    img = Image.alpha_composite(img, rotate_layer(layer, -28))
    return img


def icon_smiley():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.ellipse([26, 26, 198, 198], fill=WHITE)
    d.ellipse([72, 76, 96, 100], fill=DARK)
    d.ellipse([128, 76, 152, 100], fill=DARK)
    d.arc([68, 84, 156, 164], 15, 165, fill=DARK, width=13)
    return img


def icon_play_pause():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.polygon([(44, 48), (44, 176), (118, 112)], fill=WHITE)
    d.rounded_rectangle([138, 48, 162, 176], radius=8, fill=WHITE)
    d.rounded_rectangle([172, 48, 196, 176], radius=8, fill=WHITE)
    return img


def icon_mute():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.polygon([(34, 92), (66, 92), (106, 52), (106, 172), (66, 132), (34, 132)],
              fill=WHITE)
    d.arc([112, 66, 168, 158], -55, 55, fill=WHITE, width=11)
    d.arc([132, 46, 204, 178], -55, 55, fill=WHITE, width=11)
    d.line([(34, 196), (196, 28)], fill=WHITE, width=14)
    return img


def icon_husky():
    img = canvas()
    d = ImageDraw.Draw(img)
    # ears
    d.polygon([(48, 84), (66, 22), (104, 66)], fill=DARK)
    d.polygon([(176, 84), (158, 22), (120, 66)], fill=DARK)
    # face
    d.ellipse([30, 52, 194, 196], fill=WHITE)
    # eye patches
    d.ellipse([52, 84, 106, 132], fill=DARK)
    d.ellipse([118, 84, 172, 132], fill=DARK)
    # eyes
    d.ellipse([70, 98, 90, 118], fill=(255, 255, 255, 255))
    d.ellipse([134, 98, 154, 118], fill=(255, 255, 255, 255))
    d.ellipse([74, 102, 86, 114], fill=DARK)
    d.ellipse([138, 102, 150, 114], fill=DARK)
    # muzzle shading + nose
    d.ellipse([88, 128, 136, 168], fill=(230, 230, 230, 255))
    d.rounded_rectangle([98, 130, 126, 150], radius=8, fill=DARK)
    # tongue
    d.rounded_rectangle([102, 152, 122, 190], radius=10, fill=PINK)
    d.line([(112, 158), (112, 188)], fill=(200, 110, 110, 255), width=4)
    return img


def icon_df():
    img = canvas()
    d = ImageDraw.Draw(img)
    # hexagon outline
    hex_pts = [(52, 40), (128, 40), (168, 112), (128, 184), (52, 184), (12, 112)]
    d.line(hex_pts + [hex_pts[0]], fill=NAVY, width=12, joint="curve")
    # DF letters
    font = ImageFont.truetype(FONT_BOLD, 74)
    d.text((52, 72), "DF", font=font, fill=NAVY)
    # chevrons
    for x in (140, 166):
        d.line([(x, 78), (x + 22, 112), (x, 146)], fill=NAVY, width=12,
               joint="curve")
    return img


def icon_fox():
    img = canvas()
    d = ImageDraw.Draw(img)
    # ears
    d.polygon([(56, 78), (70, 18), (112, 58)], fill=FOX_EAR)
    d.polygon([(168, 78), (154, 18), (112, 58)], fill=FOX_EAR)
    d.polygon([(70, 66), (76, 34), (100, 56)], fill=(210, 160, 120, 255))
    d.polygon([(154, 66), (148, 34), (124, 56)], fill=(210, 160, 120, 255))
    # head
    d.ellipse([36, 48, 188, 192], fill=FOX_HEAD)
    # white muzzle
    d.ellipse([56, 118, 168, 192], fill=WHITE)
    # glasses
    for cx in (82, 142):
        d.ellipse([cx - 30, 82, cx + 30, 142], outline=DARK, width=10)
    d.arc([104, 96, 120, 116], 180, 360, fill=DARK, width=8)
    # eyes
    d.ellipse([74, 100, 90, 116], fill=DARK)
    d.ellipse([134, 100, 150, 116], fill=DARK)
    d.ellipse([77, 103, 83, 109], fill=WHITE)
    d.ellipse([137, 103, 143, 109], fill=WHITE)
    # nose + tongue
    d.polygon([(104, 140), (120, 140), (112, 152)], fill=DARK)
    d.rounded_rectangle([102, 152, 122, 186], radius=10, fill=PINK)
    return img


def icon_globe():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.ellipse([26, 26, 198, 198], outline=WHITE, width=11)
    d.ellipse([76, 26, 148, 198], outline=WHITE, width=9)
    d.line([(26, 112), (198, 112)], fill=WHITE, width=9)
    d.arc([30, 52, 194, 172], 200, 340, fill=WHITE, width=9)
    d.arc([30, 52, 194, 172], 20, 160, fill=WHITE, width=9)
    return img


def icon_chat():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([28, 40, 196, 156], radius=26, outline=WHITE, width=12)
    d.polygon([(64, 150), (64, 196), (108, 150)], fill=WHITE)
    return img


def icon_paw():
    img = canvas()
    d = ImageDraw.Draw(img)
    # main pad
    d.ellipse([62, 118, 162, 196], fill=PAW_BROWN)
    d.pieslice([62, 118, 162, 196], 180, 360, fill=PAW_BROWN)
    # toes
    for cx, cy in [(56, 96), (90, 66), (134, 66), (168, 96)]:
        d.ellipse([cx - 17, cy - 21, cx + 17, cy + 21], fill=PAW_BROWN)
    return img


def icon_check():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([36, 36, 188, 188], radius=28, fill=GREEN_DARK)
    d.line([(72, 116), (100, 146), (156, 78)], fill=(232, 232, 216, 255),
           width=16, joint="curve")
    return img


def icon_mic_mute():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([88, 28, 136, 128], radius=24, fill=WHITE)
    d.arc([62, 84, 162, 184], 0, 180, fill=WHITE, width=12)
    d.line([(112, 184), (112, 200)], fill=WHITE, width=12)
    d.line([(84, 200), (140, 200)], fill=WHITE, width=12)
    d.line([(34, 196), (196, 28)], fill=WHITE, width=14)
    return img


def icon_mail():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([28, 56, 196, 168], radius=12, outline=WHITE, width=11)
    d.line([(34, 62), (112, 128), (190, 62)], fill=WHITE, width=11,
           joint="curve")
    return img


def icon_folder():
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([28, 52, 112, 96], radius=10, outline=WHITE, width=11)
    d.rounded_rectangle([28, 84, 196, 176], radius=12, outline=WHITE, width=11)
    d.line([(28, 96), (196, 96)], fill=WHITE, width=11)
    return img


def icon_app():
    """Launcher icon: silver macropad, 3 knobs, 5x3 key grid."""
    size = APP_ICON * SS
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8, 8, size - 8, size - 8], radius=64,
                        fill=(200, 200, 200, 255))
    # knobs
    for i, cx in enumerate((112, 224, 336)):
        r = 46 if i == 1 else 40
        d.ellipse([cx - r, 56 - r + 20, cx + r, 56 + r + 20],
                  fill=(64, 64, 64, 255))
    font = ImageFont.truetype(FONT_BOLD, 44)
    d.text((224, 76), "DF", font=font, fill=(200, 200, 200, 255),
           anchor="mm")
    # keys
    for row in range(3):
        for col in range(5):
            x0 = 44 + col * 72
            y0 = 176 + row * 72
            d.rounded_rectangle([x0, y0, x0 + 60, y0 + 60], radius=14,
                                fill=(28, 28, 28, 255))
    return img


ICONS = [
    ("ufo", icon_ufo),
    ("bowl", icon_bowl),
    ("smiley", icon_smiley),
    ("play_pause", icon_play_pause),
    ("mute", icon_mute),
    ("husky", icon_husky),
    ("df", icon_df),
    ("fox", icon_fox),
    ("globe", icon_globe),
    ("chat", icon_chat),
    ("paw", icon_paw),
    ("check", icon_check),
    ("mic_mute", icon_mic_mute),
    ("mail", icon_mail),
    ("folder", icon_folder),
]

# Keycap background colors from the photo (also the compiled-in defaults).
CAP_COLORS = [
    "#B85C38", "#B5AA9C", "#1C1C1C", "#1C1C1C", "#1C1C1C",
    "#1C1C1C", "#9DB4B8", "#A97B50", "#1C1C1C", "#1C1C1C",
    "#C2A276", "#8A9A5B", "#1C1C1C", "#1C1C1C", "#1C1C1C",
]


def emit_argb8888(name, img):
    """Emit one LVGL 9 ARGB8888 image (bytes B,G,R,A) as C source."""
    w, h = img.size
    px = list(img.getdata())
    out = []
    out.append(f"const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST "
               f"LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_{name.upper()} "
               f"uint8_t img_macropad_{name}_map[] = {{")
    row = []
    for i, (r, g, b, a) in enumerate(px):
        row.append(f"0x{b:02x}, 0x{g:02x}, 0x{r:02x}, 0x{a:02x},")
        if len(row) == 8:
            out.append("  " + " ".join(row))
            row = []
    if row:
        out.append("  " + " ".join(row))
    out.append("};\n")
    out.append(f"const lv_image_dsc_t img_macropad_{name} = {{")
    out.append("  .header.cf = LV_COLOR_FORMAT_ARGB8888,")
    out.append("  .header.magic = LV_IMAGE_HEADER_MAGIC,")
    out.append(f"  .header.w = {w},")
    out.append(f"  .header.h = {h},")
    out.append(f"  .data_size = {w * h * 4},")
    out.append(f"  .data = img_macropad_{name}_map,")
    out.append("};\n")
    return "\n".join(out)


def main():
    rendered = {}
    for name, fn in ICONS:
        rendered[name] = finish(fn(), ICON)
    app = finish(icon_app(), APP_ICON)

    parts = []
    parts.append("/*")
    parts.append(" * SPDX-FileCopyrightText: 2026 koneho-ai")
    parts.append(" *")
    parts.append(" * SPDX-License-Identifier: Apache-2.0")
    parts.append(" *")
    parts.append(" * Macropad key icons + launcher icon, drawn by tools/gen_icons.py")
    parts.append(" * from the user's macropad photo (flat vector-style redraws,")
    parts.append(" * transparent background — the keycap color is painted behind")
    parts.append(" * them by the UI). Regenerate with: python3 tools/gen_icons.py")
    parts.append(" */")
    parts.append("")
    parts.append("#ifdef __has_include")
    parts.append("    #if __has_include(\"lvgl.h\")")
    parts.append("        #ifndef LV_LVGL_H_INCLUDE_SIMPLE")
    parts.append("        #define LV_LVGL_H_INCLUDE_SIMPLE")
    parts.append("        #endif")
    parts.append("    #endif")
    parts.append("#endif")
    parts.append("")
    parts.append("#if defined(LV_LVGL_H_INCLUDE_SIMPLE)")
    parts.append("    #include \"lvgl.h\"")
    parts.append("#else")
    parts.append("    #include \"lvgl/lvgl.h\"")
    parts.append("#endif")
    parts.append("")
    parts.append("")
    parts.append("#ifndef LV_ATTRIBUTE_MEM_ALIGN")
    parts.append("#define LV_ATTRIBUTE_MEM_ALIGN")
    parts.append("#endif")
    parts.append("")
    for name, _ in ICONS:
        parts.append(f"#ifndef LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_{name.upper()}")
        parts.append(f"#define LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_{name.upper()}")
        parts.append("#endif")
        parts.append("")
    parts.append("#ifndef LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_APP")
    parts.append("#define LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_APP")
    parts.append("#endif")
    parts.append("")
    for name, _ in ICONS:
        parts.append(emit_argb8888(name, rendered[name]))
    parts.append(emit_argb8888("app", app))

    out_path = os.path.join(ASSETS, "macropad_icons.c")
    with open(out_path, "w") as f:
        f.write("\n".join(parts))
    print(f"wrote {out_path} ({os.path.getsize(out_path)} bytes)")

    # Preview: the 5x3 grid on keycap colors, plus the launcher icon.
    cell, gap, pad = 120, 10, 20
    pw = pad * 2 + 5 * cell + 4 * gap
    ph = pad * 2 + 3 * cell + 2 * gap + 150
    preview = Image.new("RGBA", (pw, ph), (16, 16, 16, 255))
    dp = ImageDraw.Draw(preview)
    for i, (name, _) in enumerate(ICONS):
        row, col = divmod(i, 5)
        x0 = pad + col * (cell + gap)
        y0 = pad + row * (cell + gap)
        dp.rounded_rectangle([x0, y0, x0 + cell, y0 + cell], radius=16,
                             fill=CAP_COLORS[i])
        icon = rendered[name].resize((96, 96), Image.LANCZOS)
        preview.alpha_composite(icon, (x0 + 12, y0 + 12))
    app_big = app.resize((120, 120), Image.LANCZOS)
    preview.alpha_composite(app_big, (pad, ph - 140))
    dp.text((pad + 140, ph - 120), "img_macropad_app (launcher icon)",
            fill=(255, 255, 255, 255))
    preview_path = "/tmp/macropad_icons_preview.png"
    preview.convert("RGB").save(preview_path)
    print(f"wrote {preview_path}")


if __name__ == "__main__":
    main()
