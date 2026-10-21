"""Line detection on a synthetic text box: two text lines with furigana above, an icon, and a
border; plus a light-on-dark box. python3 tests/test_ocrprep.py"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from PIL import Image, ImageDraw
import ocrprep

def box(bg, ink):
    im = Image.new("RGB", (200, 80), bg); d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 199, 79), outline=ink)                        # border
    for x in range(20, 120, 12): d.rectangle((x, 10, x + 6, 15), fill=ink)   # furigana (6 px)
    for x in range(10, 180, 16): d.rectangle((x, 19, x + 12, 34), fill=ink)  # line 1 (16 px)
    d.rectangle((14, 40, 30, 45), fill=ink)                          # furigana
    for x in range(10, 160, 16): d.rectangle((x, 49, x + 12, 64), fill=ink)   # line 2
    d.ellipse((185, 68, 193, 76), fill=ink)                          # icon
    return im

fails = 0
for name, im in (("dark on light", box((245, 240, 220), (30, 30, 30))), ("light on dark", box((20, 20, 60), (250, 250, 250)))):
    lines = ocrprep.text_lines(im, pad=0)
    ok = len(lines) == 2 and lines[0][0] in (18, 19) and lines[1][0] in (48, 49)
    print(("ok   " if ok else "FAIL ") + f"{name}: lines {lines}")
    fails += not ok
    got = []
    ocrprep.read_box(lambda img: got.append(img.size) or "x", im)
    ok = len(got) == 2
    print(("ok   " if ok else "FAIL ") + f"{name}: OCR called once per line {got}")
    fails += not ok
blank = Image.new("RGB", (100, 40), (255, 255, 255))
ok = ocrprep.line_images(blank)[0].size == (100, 40)
print(("ok   " if ok else "FAIL ") + "empty box falls back to the whole image"); fails += not ok
# a text box that moves: find it by its colour, ignoring small specks of the same colour
import random
random.seed(3)
tb = box((245, 240, 220), (30, 30, 30))
colour = ocrprep.box_colour(tb)
for pos in [(5, 5), (190, 150), (100, 60)]:
    scene = Image.new("RGB", (400, 240), (40, 120, 60)); d = ImageDraw.Draw(scene)
    for _ in range(40):  # clutter, some of it the box colour
        x, y = random.randrange(390), random.randrange(230)
        d.rectangle((x, y, x + random.randrange(3, 12), y + random.randrange(3, 12)),
                    fill=random.choice([(245, 240, 220), (200, 50, 50), (20, 20, 120)]))
    scene.paste(tb, pos)
    found = ocrprep.find_box(scene, colour)
    want = (pos[0] + 1, pos[1] + 1, pos[0] + 199, pos[1] + 79)  # inside the 1-px border
    ok = found is not None and all(abs(a - b) <= 2 for a, b in zip(found, want))
    print(("ok   " if ok else "FAIL ") + f"moving box at {pos}: found {found}"); fails += not ok
cfg = {"ocr": {"game_boxes": {"00040000001BB200": {"colour": colour}}, "game_crops": {"00040000001BB200": [1, 2, 3, 4]}}}
blank = Image.new("RGB", (400, 240), (0, 0, 0))
ok = ocrprep.crop_for(cfg, "00040000001bb200", blank) == [1, 2, 3, 4]
print(("ok   " if ok else "FAIL ") + "box not on screen -> falls back to the saved fixed crop"); fails += not ok
print("ALL PASSED" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
