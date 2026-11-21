#!/usr/bin/env python3
"""Draw the macropad key icons + launcher icon and emit LVGL 9 ARGB8888
C arrays (one file, assets/macropad_icons.c).

The icons redraw the user's macropad keycaps (photo:
~/workspace/user/files/1000055103_0_1oyu.heic — actually a JPEG despite
the extension) as flat vector-style art. Each icon is drawn at 4x
supersampling on a transparent background and downscaled, so edges stay
smooth at 56x56. The keycap background color is NOT baked in — the UI
paints it behind the icon, so the config file can recolor keys freely.

Detail pass 2026-11-21: the first redraw was checked against the photo
at full resolution and the 9 leftmost icons (UFO, bowl, smiley, husky,
DF, fox, paw, check, mic-mute) were redrawn from the actual keycap art
— the UFO is a curved pod with a dot hole (no dome, no lights), the
smiley's mouth is a filled grin, the husky has a black cap + freckles
(no panda eye patches), the DF's D carries the dog-head mark, the fox
has big glasses + big eyes, the paw pad is a dome, and the mic is a
hollow outline.

Regenerate: python3 tools/gen_icons.py
Also writes /tmp/macropad_icons_preview.png (the 5x3 grid on keycap
colors) for a visual check.
"""

import math
import os
from PIL import Image, ImageChops, ImageDraw, ImageFont

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
PINK_DARK = (200, 110, 110, 255)
FOX_HEAD = (192, 138, 90, 255)
FOX_EAR = (138, 90, 51, 255)
FOX_INNER = (210, 160, 120, 255)
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


def place(layer, dx, dy):
    """Paste a layer onto a fresh full-size canvas at offset (dx, dy)."""
    out = canvas()
    out.paste(layer, (dx, dy), layer)
    return out


def punch(img, shapes):
    """Cut transparent holes in img; shapes draws black on an L mask."""
    mask = Image.new("L", img.size, 255)
    d = ImageDraw.Draw(mask)
    shapes(d)
    img.putalpha(ImageChops.multiply(img.split()[3], mask))
    return img


def tube(p0, pc, p1, r0, r1, fill, steps=28):
    """A tapered curved tube (a pod/bean) as a filled polygon on a layer.

    Quadratic Bezier from p0 (radius r0) through control pc to p1
    (radius r1), with round ends.
    """
    layer = canvas()
    d = ImageDraw.Draw(layer)
    left, right = [], []
    for i in range(steps + 1):
        t = i / steps
        x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * pc[0] + t ** 2 * p1[0]
        y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * pc[1] + t ** 2 * p1[1]
        dx = 2 * (1 - t) * (pc[0] - p0[0]) + 2 * t * (p1[0] - pc[0])
        dy = 2 * (1 - t) * (pc[1] - p0[1]) + 2 * t * (p1[1] - pc[1])
        n = math.hypot(dx, dy) or 1
        nx, ny = -dy / n, dx / n
        r = r0 + (r1 - r0) * (t ** 0.7)
        left.append((x + nx * r, y + ny * r))
        right.append((x - nx * r, y - ny * r))
    d.polygon(left + right[::-1], fill=fill)
    d.ellipse([p0[0] - r0, p0[1] - r0, p0[0] + r0, p0[1] + r0], fill=fill)
    d.ellipse([p1[0] - r1, p1[1] - r1, p1[0] + r1, p1[1] + r1], fill=fill)
    return layer


# ---------------------------------------------------------------- icons

def icon_ufo():
    # Photo: a curved pod/bean tilted diagonal (fat rounded head at the
    # top right, tapering tail at the bottom left), one small dot hole
    # near the head, and a cluster of short horizontal speed dashes at
    # the lower left. No dome, no lights.
    img = canvas()
    d = ImageDraw.Draw(img)
    # speed dashes, lower left, staggered 3 rows
    for x0, x1, y in [(12, 34, 166), (38, 56, 166), (16, 38, 180),
                      (42, 58, 180), (12, 32, 194), (36, 52, 194)]:
        d.line([(x0, y), (x1, y)], fill=BROWN_DARK, width=7)
    pod = tube((58, 172), (108, 142), (168, 80), 16, 27, BROWN_DARK)
    # the dot: a hole showing the keycap color through
    punch(pod, lambda m: m.ellipse([143, 89, 157, 103], fill=0))
    img = Image.alpha_composite(img, pod)
    return img


def icon_bowl():
    # Photo: a half-round bowl with a flat rim, a lumpy food mound with
    # two kibble dots, and a big bone leaning diagonally against the
    # bowl's lower left, overlapping its edge.
    img = canvas()
    d = ImageDraw.Draw(img)
    # bowl body: bottom half of an ellipse, flat top at y=100
    d.pieslice([52, 16, 180, 184], 0, 180, fill=GREY_DARK)
    # rim bar, slightly wider than the body
    d.rectangle([48, 96, 184, 112], fill=GREY_DARK)
    # food mound: three merged bumps on the rim
    for cx, cy, r in [(80, 96, 24), (112, 86, 28), (144, 98, 22)]:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=GREY_DARK)
    # kibble dots: holes showing the keycap color through
    punch(img, lambda m: (
        m.ellipse([95, 89, 105, 99], fill=0),
        m.ellipse([121, 97, 131, 107], fill=0),
    ))
    # bone, drawn centered then rotated 42 deg (top-left to bottom-right)
    # and placed lower left
    bone = canvas()
    db = ImageDraw.Draw(bone)
    db.rectangle([68, 101, 156, 123], fill=GREY_DARK)
    for cx, cy in [(70, 101), (70, 123), (154, 101), (154, 123)]:
        db.ellipse([cx - 15, cy - 15, cx + 15, cy + 15], fill=GREY_DARK)
    bone = rotate_layer(bone, 42)
    img = Image.alpha_composite(img, place(bone, -48, 38))
    return img


def icon_smiley():
    # Photo: white disc, two tall oval eyes set wide apart, and a wide
    # FILLED grin — a half-disc with a flat top edge, not an arc.
    img = canvas()
    d = ImageDraw.Draw(img)
    d.ellipse([26, 26, 198, 198], fill=WHITE)
    d.ellipse([68, 66, 94, 104], fill=DARK)
    d.ellipse([130, 66, 156, 104], fill=DARK)
    d.pieslice([66, 56, 158, 168], 0, 180, fill=DARK)
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
    # Photo: a wide white face with a black cap across the top whose
    # lower edge dips at the sides and in the center (husky mask),
    # pointy ears with white inners at the top corners, small black dot
    # eyes, three freckle dots on each cheek, a black nose, and a pink
    # tongue hanging down. No panda eye patches.
    img = canvas()
    d = ImageDraw.Draw(img)
    # face: a wide rounded shape, flat-ish bottom, spanning the keycap
    d.rounded_rectangle([22, 54, 202, 204], radius=58, fill=WHITE)
    # ears (drawn over the face's top corners; the cap covers the bases)
    d.polygon([(30, 84), (48, 18), (98, 64)], fill=DARK)
    d.polygon([(194, 84), (176, 18), (126, 64)], fill=DARK)
    d.polygon([(44, 70), (54, 34), (82, 60)], fill=WHITE)
    d.polygon([(180, 70), (170, 34), (142, 60)], fill=WHITE)
    # black cap: top follows the head curve, lower edge dips at the
    # sides and in the center, rising over each eye
    d.polygon([
        (22, 140), (22, 80), (56, 56), (112, 50), (168, 56), (202, 80),
        (202, 140), (176, 130), (154, 88), (134, 100), (112, 112),
        (90, 100), (70, 88), (48, 130),
    ], fill=DARK)
    # eyes: small black dots with a white highlight
    for cx in (78, 146):
        d.ellipse([cx - 7, 111, cx + 7, 125], fill=DARK)
        d.ellipse([cx - 5, 113, cx - 1, 117], fill=WHITE)
    # cheek freckles: three tiny dots per cheek
    for cx, cy in [(54, 138), (62, 146), (50, 146),
                   (170, 138), (162, 146), (174, 146)]:
        d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=DARK)
    # nose: black rounded oval, wider than tall
    d.ellipse([100, 136, 124, 152], fill=DARK)
    # mouth line down from the nose
    d.line([(112, 152), (112, 160)], fill=DARK, width=4)
    # tongue: pink, rounded, hanging below the mouth
    d.rounded_rectangle([99, 160, 125, 194], radius=11, fill=PINK,
                        outline=PINK_DARK, width=3)
    d.line([(112, 166), (112, 186)], fill=PINK_DARK, width=3)
    return img


def icon_df():
    # Photo: a hexagon outline with the DF mark inside — the D is a
    # block letter with the dog-head silhouette in its bowl (same mark
    # as the center knob), the F is a block letter, and two chevrons
    # run off past the hexagon's right vertex.
    img = canvas()
    d = ImageDraw.Draw(img)
    # hexagon outline, flat top/bottom, pointed sides
    hex_pts = [(48, 40), (132, 40), (174, 112), (132, 184), (48, 184),
               (6, 112)]
    d.line(hex_pts + [hex_pts[0]], fill=NAVY, width=12, joint="curve")
    # D and F drawn as block shapes on their own layer so the dog head
    # can be cut into the D's bowl
    letters = canvas()
    dl = ImageDraw.Draw(letters)
    # D: left bar + right half-disc
    dl.rectangle([40, 66, 66, 144], fill=NAVY)
    dl.pieslice([40, 66, 118, 144], 270, 90, fill=NAVY)
    # D's bowl: punch the counter out (transparent = keycap color)
    punch(letters, lambda m: (
        m.rectangle([56, 82, 66, 128], fill=0),
        m.pieslice([56, 82, 102, 128], 270, 90, fill=0),
    ))
    # dog head in the bowl, facing right: ear + head + snout
    dl.polygon([(60, 94), (66, 80), (76, 93)], fill=NAVY)
    dl.ellipse([58, 90, 86, 122], fill=NAVY)
    dl.polygon([(82, 103), (94, 109), (82, 116)], fill=NAVY)
    # muzzle line across the dog's face: punch a smile curve out
    punch(letters, lambda m: m.arc([58, 100, 94, 132], 20, 160, fill=0,
                                   width=4))
    # F: vertical bar + top and middle bars
    dl.rectangle([108, 66, 122, 144], fill=NAVY)
    dl.rectangle([108, 66, 152, 80], fill=NAVY)
    dl.rectangle([108, 98, 142, 112], fill=NAVY)
    img = Image.alpha_composite(img, letters)
    # chevrons: the second runs past the hexagon's right vertex
    for x in (148, 172):
        d.line([(x, 64), (x + 26, 112), (x, 160)], fill=NAVY, width=13,
               joint="curve")
    return img


def icon_fox():
    # Photo: tall pointy ears with white inners, a wide brown head, a
    # white muzzle across the lower face, BIG round glasses with thick
    # rims, big black eyes with white highlights, a small nose, and a
    # wide pink tongue below it.
    img = canvas()
    d = ImageDraw.Draw(img)
    # ears
    d.polygon([(48, 84), (64, 16), (108, 62)], fill=FOX_EAR)
    d.polygon([(176, 84), (160, 16), (116, 62)], fill=FOX_EAR)
    d.polygon([(62, 70), (70, 34), (96, 58)], fill=FOX_INNER)
    d.polygon([(162, 70), (154, 34), (128, 58)], fill=FOX_INNER)
    # head
    d.ellipse([30, 44, 194, 196], fill=FOX_HEAD)
    # white muzzle across the lower face
    d.ellipse([48, 116, 176, 198], fill=WHITE)
    # glasses: big thick-rimmed circles + bridge arch
    for cx in (80, 144):
        d.ellipse([cx - 32, 74, cx + 32, 138], outline=DARK, width=11)
    d.arc([100, 92, 124, 114], 180, 360, fill=DARK, width=7)
    # eyes: big black circles with a white highlight
    for cx in (80, 144):
        d.ellipse([cx - 17, 89, cx + 17, 123], fill=DARK)
        d.ellipse([cx - 11, 95, cx - 1, 105], fill=WHITE)
    # nose: small rounded triangle
    d.polygon([(103, 139), (121, 139), (112, 152)], fill=DARK)
    # tongue: wide, rounded, below the nose
    d.rounded_rectangle([96, 154, 128, 190], radius=12, fill=PINK,
                        outline=PINK_DARK, width=3)
    d.line([(112, 160), (112, 182)], fill=PINK_DARK, width=3)
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
    # Photo: four toe beans in an arc — the middle two larger and
    # vertical, the outer two smaller and tilted outward — over one
    # large dome-shaped pad with a flat bottom.
    img = canvas()
    d = ImageDraw.Draw(img)
    # middle toes: tall vertical ovals
    d.ellipse([69, 58, 107, 108], fill=PAW_BROWN)
    d.ellipse([117, 58, 155, 108], fill=PAW_BROWN)
    # outer toes: smaller, tilted outward (tops lean away from center)
    toe = canvas()
    dt = ImageDraw.Draw(toe)
    dt.ellipse([98, 92, 126, 132], fill=PAW_BROWN)
    img = Image.alpha_composite(img, place(rotate_layer(toe, 30), -58, -16))
    img = Image.alpha_composite(img, place(rotate_layer(toe, -30), 58, -16))
    # pad: a dome, flat bottom at y=195
    d = ImageDraw.Draw(img)
    d.pieslice([50, 125, 174, 265], 180, 360, fill=PAW_BROWN)
    return img


def icon_check():
    # Photo: a dark rounded square with a light check, the check's
    # vertex just below center, short arm up-left, long arm up-right.
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([36, 36, 188, 188], radius=28, fill=GREEN_DARK)
    d.line([(72, 118), (102, 148), (156, 80)], fill=(232, 232, 216, 255),
           width=13, joint="curve")
    return img


def icon_mic_mute():
    # Photo: a HOLLOW mic — an outlined capsule, not a filled one —
    # with a U-shaped holder, a stem and foot, and a thin diagonal
    # slash from bottom left to top right.
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([88, 26, 136, 126], radius=24, outline=WHITE,
                        width=12)
    d.arc([68, 80, 156, 180], 0, 180, fill=WHITE, width=11)
    d.line([(112, 180), (112, 196)], fill=WHITE, width=11)
    d.line([(88, 196), (136, 196)], fill=WHITE, width=11)
    d.line([(36, 196), (194, 30)], fill=WHITE, width=9)
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
