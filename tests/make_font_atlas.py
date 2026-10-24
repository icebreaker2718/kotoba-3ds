#!/usr/bin/env python3
"""Makes a stand-in for the 3DS system font, for drawing the plugin's screens on a PC
(tests/ui_preview.cpp). The console's own font isn't distributable, so this renders Noto Sans CJK
into cells of the same size (24 px wide, 30 px high, as the 3DS system font's glyph sheets).

    make_font_atlas.py out.bin [font.ttc]
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H, BASELINE, SIZE = 24, 30, 24, 23

# the 3DS font's button icons (private use area) drawn with look-alikes
# (a letter in a circle or a pill, an arrow in a square)
ICONS = {0xE000: ("circle", "A"), 0xE001: ("circle", "B"), 0xE002: ("circle", "X"), 0xE003: ("circle", "Y"),
         0xE052: ("pill", "L"), 0xE053: ("pill", "R"), 0xE079: ("square", "▲"), 0xE07A: ("square", "▼"),
         0xE07B: ("square", "◀"), 0xE07C: ("square", "▶"), 0xE049: ("circle", "")}


def icon(font, kind, letter):
    img = Image.new("L", (CELL_W * 2, CELL_H), 0)
    d = ImageDraw.Draw(img)
    top, bottom = 4, 26
    if kind == "circle":
        d.ellipse((1, top, 23, bottom), fill=255)
    elif kind == "pill":
        d.rounded_rectangle((1, top + 2, 23, bottom - 2), radius=8, fill=255)
    else:
        d.rounded_rectangle((2, top + 1, 22, bottom - 1), radius=4, fill=255)
    small = ImageFont.truetype(font.path, 15 if kind != "square" else 11, index=0)
    if letter:
        d.text((12, (top + bottom) / 2), letter, font=small, fill=0, anchor="mm")
    else:  # circle pad: a ring
        d.ellipse((7, top + 6, 17, bottom - 6), fill=0)
    return img


def ranges():
    yield from range(0x20, 0x7F)
    yield from range(0xA0, 0x100)
    yield from range(0x2010, 0x2070)   # dashes, quotes, …
    yield from range(0x2190, 0x2200)   # arrows
    yield from range(0x25A0, 0x2600)   # shapes ▲▼
    yield from range(0x3000, 0x3100)   # CJK punctuation, kana
    yield from range(0x4E00, 0xA000)   # kanji
    yield from range(0xFF00, 0xFFF0)   # full-width forms
    yield from ICONS


def main():
    out = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) > 2 else "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
    font = ImageFont.truetype(path, SIZE, index=0)  # index 0 = JP in Noto's collection
    glyphs = []
    for cp in ranges():
        if cp in ICONS:
            img = icon(font, *ICONS[cp])
            adv = 25
        else:
            ch = chr(cp)
            if font.getmask(ch).getbbox() is None and ch != " ":
                continue
            img = Image.new("L", (CELL_W * 2, CELL_H), 0)
            d = ImageDraw.Draw(img)
            d.text((0, BASELINE), ch, font=font, fill=255, anchor="ls")
            adv = int(round(font.getlength(ch)))
        bbox = img.getbbox()
        if bbox is None:
            left, width, cell = 0, 0, bytes(CELL_W * CELL_H)
        else:
            left, right = bbox[0], min(bbox[2], CELL_W * 2)
            crop = img.crop((left, 0, left + CELL_W, CELL_H))
            width = min(right - left, CELL_W)
            cell = crop.tobytes()
        glyphs.append(struct.pack("<IhHH", cp, left, width, min(adv, 255)) + cell)
    with open(out, "wb") as f:
        f.write(b"KTFA" + struct.pack("<HHI", CELL_W, CELL_H, len(glyphs)))
        for g in glyphs:
            f.write(g)
    print(f"{len(glyphs)} glyphs -> {out}")


if __name__ == "__main__":
    main()
