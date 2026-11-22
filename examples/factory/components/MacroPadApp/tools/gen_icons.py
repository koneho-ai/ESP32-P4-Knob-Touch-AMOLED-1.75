#!/usr/bin/env python3
"""Build the macropad key faces + launcher icon and emit LVGL 9 ARGB8888
C arrays (one file, assets/macropad_icons.c).

The key faces are the user's own keycap photos (one per key, white
background, in tools/keycap_photos/). Each photo is cropped to the
keycap — the caps are not all the same size in the photos — squared,
resized to 72x72, and the white background around the cap is made
transparent, so the key is just the photo on the dark screen: the
buttons carry no background color of their own.

Order matters: the transparency is cut BEFORE the squaring. Padding
first traps a ring of the photo's white background between the padding
and the cap, which showed as a white border around every key (first
photo pass, 2026-11-22). And the downscale runs in premultiplied
(RGBa) space, so the semi-transparent edge pixels blend the cap color
with transparent instead of with white.

The photos in tools/keycap_photos/ are the user's and are NOT committed
to the public repo — only the generated macropad_icons.c is committed.
Regenerating needs the photos present; the firmware itself builds from
the committed .c file alone.

History: the icons started as flat vector-style redraws (PIL shapes)
of the macropad photo, redrawn twice from side-by-side comparisons
(2026-11-21). The user then supplied individual keycap photos
(2026-11-22) and the redraws were replaced by the real photos.

The launcher icon is still drawn (a mini macropad), not a photo.

Regenerate: python3 tools/gen_icons.py
Also writes /tmp/macropad_icons_preview.png (a mockup of the 466x466
round screen as the app will look) for a visual check.
"""

import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
COMPONENT = os.path.dirname(HERE)
ASSETS = os.path.join(COMPONENT, "assets")
PHOTOS = os.path.join(HERE, "keycap_photos")

FACE = 72                  # final key-face size (px); the button is 72x72
APP_ICON = 112             # launcher icon size (px)
SS = 4                     # supersampling factor for the drawn launcher icon

FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

# Key name -> keycap photo file (the user's uploads, 2026-11-22).
# The numbers in the filenames are the upload order, not the grid order.
PHOTOS_MAP = {
    "ufo": "1000056941_10_x2xn.webp",
    "bowl": "1000056940_11_4rm9.webp",
    "smiley": "1000056943_8_z1jd.webp",
    "play_pause": "1000056950_1_xvrj.webp",
    "mute": "1000056949_2_0fbz.webp",
    "husky": "1000056942_9_hde2.webp",
    "df": "1000056937_14_46lt.webp",
    "fox": "1000056936_15_jvo2.webp",
    "globe": "1000056948_3_pszg.webp",
    "chat": "1000056947_4_pzpk.webp",
    "paw": "1000056939_12_jpob.webp",
    "check": "1000056938_13_ipj4.webp",
    "mic_mute": "1000056946_5_ntt8.webp",
    "mail": "1000056945_6_a7jn.webp",
    "folder": "1000056944_7_wu7j.webp",
}

# Grid order (row-major), matching MacroPadIcons / the config defaults.
ICONS = [
    "ufo", "bowl", "smiley", "play_pause", "mute",
    "husky", "df", "fox", "globe", "chat",
    "paw", "check", "mic_mute", "mail", "folder",
]


def keycap_bbox(img):
    """Bounding box of the keycap in a white-background photo.

    Thresholds to near-white vs the rest, erodes isolated specks with a
    3x3 minimum filter, then takes the bounding box. A small margin is
    added so the cap edge is never clipped.
    """
    gray = img.convert("L")
    mask = gray.point(lambda v: 0 if v >= 242 else 255)
    mask = mask.filter(ImageFilter.MinFilter(3))
    box = mask.getbbox()
    if box is None:
        raise ValueError("no keycap found (image is all white?)")
    x0, y0, x1, y1 = box
    mx = max(1, (x1 - x0) // 100)
    my = max(1, (y1 - y0) // 100)
    return (max(0, x0 - mx), max(0, y0 - my),
            min(img.width, x1 + mx), min(img.height, y1 + my))


def transparent_corners(img):
    """Make the white background in the corners transparent.

    Thresholds to a binary background mask, flood-fills from the four
    corners (exact match, so only the background connected to the
    outside is taken — the white art inside the cap, like the smiley
    face or the husky's chest, is enclosed by the cap edge and stays
    opaque), and feathers the edge by 1px.
    """
    gray = img.convert("L")
    bw = gray.point(lambda v: 255 if v >= 230 else 0)
    w, h = bw.size
    for seed in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
        ImageDraw.floodfill(bw, seed, 128, thresh=0)
    alpha = bw.point(lambda v: 0 if v == 128 else 255)
    alpha = alpha.filter(ImageFilter.GaussianBlur(1))
    out = img.convert("RGBA")
    out.putalpha(alpha)
    return out


def load_key_face(name):
    """Crop one keycap photo to the key face: a FACE x FACE RGBA image
    with the white background around the cap transparent.

    Transparency is cut at full resolution, BEFORE the squaring: the
    flood fill needs the white margin around the cap to reach the
    corners. The squaring then pads with transparent pixels (never a
    color), and the downscale runs in premultiplied space so the edge
    blends cleanly."""
    path = os.path.join(PHOTOS, PHOTOS_MAP[name])
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"{path} missing — the keycap photos live in "
            f"tools/keycap_photos/ and are not committed to the repo")
    img = Image.open(path).convert("RGB")
    x0, y0, x1, y1 = keycap_bbox(img)
    crop = img.crop((x0, y0, x1, y1))
    w, h = crop.size
    aspect = w / h
    print(f"  {name}: {img.size} -> bbox {w}x{h} "
          f"(aspect {aspect:.3f})")
    rgba = transparent_corners(crop)
    if 0.98 <= aspect <= 1.02:
        # Near-square already: a tiny stretch is invisible.
        square = rgba
    else:
        # Off-square: pad the short side with transparency instead of
        # stretching the art.
        side = max(w, h)
        padded = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        padded.paste(rgba, ((side - w) // 2, (side - h) // 2), rgba)
        square = padded
    # Premultiplied downscale: the semi-transparent edge pixels blend
    # the cap color with transparent, not with white.
    face = square.convert("RGBa").resize((FACE, FACE), Image.LANCZOS).convert("RGBA")
    # Soften the alpha edge at display size (the full-res feather is
    # sub-pixel after the downscale).
    alpha = face.getchannel("A").filter(ImageFilter.GaussianBlur(0.5))
    face.putalpha(alpha)
    return face


def finish(img, size):
    return img.resize((size, size), Image.LANCZOS)


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
    print("loading keycap photos from", PHOTOS)
    rendered = {}
    for name in ICONS:
        rendered[name] = load_key_face(name)
    app = finish(icon_app(), APP_ICON)

    parts = []
    parts.append("/*")
    parts.append(" * SPDX-FileCopyrightText: 2026 koneho-ai")
    parts.append(" *")
    parts.append(" * SPDX-License-Identifier: Apache-2.0")
    parts.append(" *")
    parts.append(" * Macropad key faces + launcher icon, built by")
    parts.append(" * tools/gen_icons.py from the user's keycap photos")
    parts.append(" * (tools/keycap_photos/, not committed — see the")
    parts.append(" * script header). The photos are cropped to the")
    parts.append(" * keycap, squared and resized to 72x72, with the")
    parts.append(" * white background around the cap transparent —")
    parts.append(" * the key is just the photo on the dark screen.")
    parts.append(" * The launcher icon is drawn, not a photo.")
    parts.append(" * Regenerate with: python3 tools/gen_icons.py")
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
    for name in ICONS:
        parts.append(f"#ifndef LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_{name.upper()}")
        parts.append(f"#define LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_{name.upper()}")
        parts.append("#endif")
        parts.append("")
    parts.append("#ifndef LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_APP")
    parts.append("#define LV_ATTRIBUTE_IMAGE_IMG_MACROPAD_APP")
    parts.append("#endif")
    parts.append("")
    for name in ICONS:
        parts.append(emit_argb8888(name, rendered[name]))
    parts.append(emit_argb8888("app", app))

    out_path = os.path.join(ASSETS, "macropad_icons.c")
    with open(out_path, "w") as f:
        f.write("\n".join(parts))
    print(f"wrote {out_path} ({os.path.getsize(out_path)} bytes)")

    # Preview: a mockup of the 466x466 round screen as the app will
    # look — same geometry as MacroPadUi (grid 37,99; keys 72x72, gap
    # 8; title 22; USB 54; footer 360; reload 394). The buttons carry
    # no background color: the keys are just the photos on the dark
    # screen.
    screen = Image.new("RGBA", (466, 466), (14, 14, 16, 255))
    d = ImageDraw.Draw(screen)
    font_title = ImageFont.truetype(FONT_BOLD, 20)
    font_small = ImageFont.truetype(FONT_REG, 16)
    d.text((233, 22), "MACROPAD", font=font_title, fill=(232, 232, 232, 255),
           anchor="ma")
    d.ellipse([159, 57, 171, 69], fill=(180, 83, 9, 255))
    d.text((241, 54), "No USB host", font=font_small, fill=(154, 154, 160, 255),
           anchor="ma")
    for i, name in enumerate(ICONS):
        row, col = divmod(i, 5)
        x0 = 37 + col * (72 + 8)
        y0 = 99 + row * (72 + 8)
        screen.alpha_composite(rendered[name], (x0, y0))
    d.rounded_rectangle([153, 394, 313, 430], radius=18, fill=(42, 42, 46, 255))
    d.text((233, 402), "Reload config", font=font_small,
           fill=(232, 232, 232, 255), anchor="ma")
    # Round mask, like the panel.
    mask = Image.new("L", (466, 466), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, 465, 465], fill=255)
    screen.putalpha(mask)
    preview_path = "/tmp/macropad_icons_preview.png"
    screen.save(preview_path)
    print(f"wrote {preview_path}")


if __name__ == "__main__":
    main()
