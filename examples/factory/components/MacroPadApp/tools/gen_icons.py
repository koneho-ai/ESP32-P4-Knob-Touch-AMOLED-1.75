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

Second detail pass 2026-11-21: the redraw was compared against the
photo key-by-key on a side-by-side sheet and all 9 were redrawn AGAIN,
bigger — the earlier attempts drew the art far too small inside the
canvas and too thin. Rule of thumb: the art should span ~85% of the
canvas in both directions.

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
    # Photo: a FAT curved pod/bean, like a fat comma lying at a shallow
    # angle (~25 deg) — a big round head at the right tapering to a
    # rounded tail at the lower left, the belly sagging below the chord,
    # one small dot hole near the top of the head, and a tight cluster
    # of short horizontal speed dashes below the tail. No dome, no lights.
    img = canvas()
    d = ImageDraw.Draw(img)
    # speed dashes, below the tail, 3 staggered rows of 2
    for x0, x1, y in [(10, 30, 168), (34, 50, 168), (14, 34, 182),
                      (38, 54, 182), (10, 28, 196), (32, 48, 196)]:
        d.line([(x0, y), (x1, y)], fill=BROWN_DARK, width=8)
    pod = tube((44, 150), (100, 156), (182, 84), 24, 38, BROWN_DARK)
    # the dot: a hole showing the keycap color through
    punch(pod, lambda m: m.ellipse([156, 84, 172, 100], fill=0))
    img = Image.alpha_composite(img, pod)
    return img


def icon_bowl():
    # Photo: a WIDE shallow bowl with a flat rim, a lumpy food mound
    # with two kibble dots on top, and a big bone leaning diagonally
    # against the bowl's lower left — most of the bone is OUTSIDE the
    # bowl's silhouette, clearly visible, not hidden behind it.
    img = canvas()
    d = ImageDraw.Draw(img)
    # bowl body: bottom half of an ellipse, flat top at y=98
    d.pieslice([32, 28, 192, 168], 0, 180, fill=GREY_DARK)
    # rim bar, slightly wider than the body
    d.rectangle([28, 92, 196, 110], fill=GREY_DARK)
    # food mound: three merged bumps on the rim, middle tallest
    for cx, cy, r in [(70, 88, 24), (112, 78, 28), (154, 90, 22)]:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=GREY_DARK)
    # kibble dots: holes showing the keycap color through
    punch(img, lambda m: (
        m.ellipse([88, 78, 100, 90], fill=0),
        m.ellipse([120, 86, 132, 98], fill=0),
    ))
    # bone, drawn centered then rotated 42 deg (top-left to bottom-right)
    # and placed well left so most of it shows outside the bowl
    bone = canvas()
    db = ImageDraw.Draw(bone)
    db.rectangle([54, 101, 170, 123], fill=GREY_DARK)
    for cx, cy in [(56, 101), (56, 123), (168, 101), (168, 123)]:
        db.ellipse([cx - 15, cy - 15, cx + 15, cy + 15], fill=GREY_DARK)
    bone = rotate_layer(bone, 42)
    img = Image.alpha_composite(img, place(bone, -62, 30))
    return img


def icon_smiley():
    # Photo: white disc, two small ROUND dot eyes set wide apart, and a
    # WIDE shallow FILLED grin — a half-disc with a flat top edge, not
    # an arc. The grin spans most of the face width.
    img = canvas()
    d = ImageDraw.Draw(img)
    d.ellipse([36, 36, 188, 188], fill=WHITE)
    d.ellipse([77, 81, 95, 99], fill=DARK)
    d.ellipse([129, 81, 147, 99], fill=DARK)
    d.pieslice([62, 76, 162, 160], 0, 180, fill=DARK)
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
    # Photo: a WIDE white face spanning the full keycap, a black cap
    # across the top whose lower edge dips at the sides and in the
    # center (husky mask), big pointy ears with white inners at the top
    # corners, small black dot eyes, three tiny freckle dots on each
    # cheek, a small black nose, and a SMALL pink tongue below it.
    # No panda eye patches.
    img = canvas()
    d = ImageDraw.Draw(img)
    # face: wide, flat-ish bottom, spanning the keycap
    d.rounded_rectangle([14, 58, 210, 206], radius=56, fill=WHITE)
    # ears (drawn over the face's top corners; the cap covers the bases)
    d.polygon([(12, 92), (30, 16), (96, 66)], fill=DARK)
    d.polygon([(212, 92), (194, 16), (128, 66)], fill=DARK)
    d.polygon([(28, 74), (38, 32), (80, 60)], fill=WHITE)
    d.polygon([(196, 74), (186, 32), (144, 60)], fill=WHITE)
    # black cap: top follows the head curve, lower edge dips DEEPEST at
    # the sides, rising over each eye, with only a shallow dip in the
    # center
    d.polygon([
        (14, 150), (14, 84), (52, 60), (112, 54), (172, 60), (210, 84),
        (210, 150), (186, 146), (160, 96), (140, 106), (112, 110),
        (84, 106), (64, 96), (38, 146),
    ], fill=DARK)
    # eyes: small black dots with a white highlight
    for cx in (80, 144):
        d.ellipse([cx - 7, 115, cx + 7, 129], fill=DARK)
        d.ellipse([cx - 5, 117, cx - 1, 121], fill=WHITE)
    # cheek freckles: three tiny dots per cheek
    for cx, cy in [(58, 142), (66, 150), (52, 150),
                   (166, 142), (158, 150), (172, 150)]:
        d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=DARK)
    # nose: small black rounded oval, wider than tall
    d.ellipse([100, 139, 124, 155], fill=DARK)
    # mouth line down from the nose
    d.line([(112, 154), (112, 162)], fill=DARK, width=4)
    # tongue: small, pink, rounded, below the mouth
    d.rounded_rectangle([100, 162, 124, 187], radius=10, fill=PINK,
                        outline=PINK_DARK, width=3)
    d.line([(112, 168), (112, 182)], fill=PINK_DARK, width=3)
    return img


def icon_df():
    # Photo: a WIDE hexagon outline (wider than tall) with the DF mark
    # inside — the D is a big block letter with the dog-head silhouette
    # in its bowl (same mark as the center knob), the F is a big block
    # letter, and two chevrons sit to the right, the second just past
    # the hexagon's right vertex.
    img = canvas()
    d = ImageDraw.Draw(img)
    # hexagon outline, flat top/bottom, pointed sides, wider than tall
    hex_pts = [(24, 42), (124, 42), (202, 112), (124, 182), (24, 182),
               (4, 112)]
    d.line(hex_pts + [hex_pts[0]], fill=NAVY, width=14, joint="curve")
    # D and F drawn as block shapes on their own layer so the dog head
    # can be cut into the D's bowl
    letters = canvas()
    dl = ImageDraw.Draw(letters)
    # D: left bar + right half-disc
    dl.rectangle([32, 70, 60, 154], fill=NAVY)
    dl.pieslice([32, 70, 114, 154], 270, 90, fill=NAVY)
    # D's bowl: punch the counter out (transparent = keycap color)
    punch(letters, lambda m: (
        m.rectangle([48, 86, 60, 138], fill=0),
        m.pieslice([48, 86, 98, 138], 270, 90, fill=0),
    ))
    # dog head in the bowl, facing right: ear + head + snout
    dl.polygon([(52, 98), (58, 84), (68, 96)], fill=NAVY)
    dl.ellipse([50, 94, 82, 126], fill=NAVY)
    dl.polygon([(78, 106), (90, 111), (78, 118)], fill=NAVY)
    # muzzle line across the dog's face: punch a smile curve out
    punch(letters, lambda m: m.arc([50, 104, 86, 136], 20, 160, fill=0,
                                   width=4))
    # F: vertical bar + top and middle bars
    dl.rectangle([104, 70, 118, 154], fill=NAVY)
    dl.rectangle([104, 70, 148, 84], fill=NAVY)
    dl.rectangle([104, 102, 136, 116], fill=NAVY)
    img = Image.alpha_composite(img, letters)
    # chevrons: the second past the hexagon's right vertex
    d.line([(158, 76), (180, 112), (158, 148)], fill=NAVY, width=13,
           joint="curve")
    d.line([(180, 76), (208, 112), (180, 148)], fill=NAVY, width=13,
           joint="curve")
    return img


def icon_fox():
    # Photo: big tall pointy ears with white inners at the top corners,
    # a WIDE brown head, a wide white muzzle across the lower face, BIG
    # round glasses with thick rims, big black eyes with white
    # highlights, a small nose, and a SMALL pink tongue below it.
    img = canvas()
    d = ImageDraw.Draw(img)
    # ears
    d.polygon([(24, 90), (42, 14), (106, 62)], fill=FOX_EAR)
    d.polygon([(200, 90), (182, 14), (118, 62)], fill=FOX_EAR)
    d.polygon([(40, 72), (50, 32), (90, 58)], fill=FOX_INNER)
    d.polygon([(184, 72), (174, 32), (134, 58)], fill=FOX_INNER)
    # head: wide
    d.ellipse([20, 48, 204, 198], fill=FOX_HEAD)
    # white muzzle across the lower face
    d.ellipse([36, 118, 188, 202], fill=WHITE)
    # glasses: big thick-rimmed circles + bridge arch
    for cx in (78, 146):
        d.ellipse([cx - 36, 72, cx + 36, 144], outline=DARK, width=12)
    d.arc([102, 94, 122, 114], 180, 360, fill=DARK, width=8)
    # eyes: big black circles with a white highlight
    for cx in (78, 146):
        d.ellipse([cx - 19, 89, cx + 19, 127], fill=DARK)
        d.ellipse([cx - 13, 95, cx - 3, 105], fill=WHITE)
    # nose: small rounded triangle
    d.polygon([(103, 141), (121, 141), (112, 154)], fill=DARK)
    # tongue: small, rounded, below the nose
    d.rounded_rectangle([101, 156, 123, 182], radius=11, fill=PINK,
                        outline=PINK_DARK, width=3)
    d.line([(112, 162), (112, 178)], fill=PINK_DARK, width=3)
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
    # Photo: a BIG paw print filling the keycap — four large toe beans
    # in an arc (the middle two larger and vertical, the outer two
    # smaller and tilted outward) over one large dome-shaped pad with
    # a flat bottom.
    img = canvas()
    d = ImageDraw.Draw(img)
    # middle toes: tall vertical ovals
    d.ellipse([64, 48, 108, 112], fill=PAW_BROWN)
    d.ellipse([116, 48, 160, 112], fill=PAW_BROWN)
    # outer toes: smaller, tilted outward (tops lean away from center)
    toe = canvas()
    dt = ImageDraw.Draw(toe)
    dt.ellipse([92, 84, 132, 140], fill=PAW_BROWN)
    img = Image.alpha_composite(img, place(rotate_layer(toe, 35), -64, -24))
    img = Image.alpha_composite(img, place(rotate_layer(toe, -35), 64, -24))
    # pad: a big dome, flat bottom at y=194
    d = ImageDraw.Draw(img)
    d.pieslice([40, 120, 184, 268], 180, 360, fill=PAW_BROWN)
    return img


def icon_check():
    # Photo: a BIG dark rounded square filling the keycap, with a light
    # check in the center — the check is smaller than the square, thin,
    # vertex just below center, short arm up-left, long arm up-right.
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([32, 32, 192, 192], radius=24, fill=GREEN_DARK)
    d.line([(80, 120), (104, 146), (148, 90)], fill=(232, 232, 216, 255),
           width=10, joint="curve")
    return img


def icon_mic_mute():
    # Photo: a TALL narrow HOLLOW mic — an outlined capsule, not a
    # filled one — with a U-shaped holder hugging it, a stem and foot,
    # and a thin diagonal slash at ~45 deg from bottom left to top
    # right, extending past the mic on both ends.
    img = canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([86, 26, 138, 126], radius=26, outline=WHITE,
                        width=12)
    d.arc([70, 84, 154, 184], 0, 180, fill=WHITE, width=11)
    d.line([(112, 184), (112, 198)], fill=WHITE, width=11)
    d.line([(90, 198), (134, 198)], fill=WHITE, width=11)
    d.line([(32, 198), (198, 34)], fill=WHITE, width=9)
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
