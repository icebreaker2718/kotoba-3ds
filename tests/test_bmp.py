"""The plugin's screenshot converter (core/bmp.cpp) vs. the server's decoder: colours, orientation,
size, for the three screenshots the plugin saves (top screen 400x240, touchscreen 320x240, both
400x480). python3 tests/test_bmp.py"""
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from PIL import Image, ImageDraw
import ocr_server
subprocess.run(["make", "-s", "-C", HERE, "bmp_tool"], check=True)
d = tempfile.mkdtemp()
fails = 0
for w, h in ((400, 240), (320, 240), (400, 480)):
    src = Image.new("RGB", (w, h), (0, 0, 0)); dr = ImageDraw.Draw(src)
    marks = {(0, 0): (250, 10, 10), (w - 1, 0): (10, 250, 10), (0, h - 1): (10, 10, 250), (w - 1, h - 1): (250, 250, 200),
             (w // 2, h // 2): (128, 64, 32)}
    for (x, y), c in marks.items(): dr.rectangle((x - 2, y - 2, x + 2, y + 2), fill=c)
    bmp = os.path.join(d, f"shot{w}x{h}.bmp"); src.save(bmp)
    out = os.path.join(d, "out.k3")
    subprocess.run([os.path.join(HERE, "bmp_tool"), bmp, out], capture_output=True, text=True)
    data = open(out, "rb").read()
    img = ocr_server.decode_screen(data)
    ok = img.size == (w, h)
    print(("ok   " if ok else "FAIL ") + f"{w}x{h}: {len(data)} bytes (BMP was {os.path.getsize(bmp)}), size {img.size}"); fails += not ok
    for (x, y), c in marks.items():
        got = img.getpixel((x, y))
        ok = all(abs(a - b) <= 8 for a, b in zip(got, c))
        print(("ok   " if ok else "FAIL ") + f"  pixel {(x, y)} {c} -> {got}"); fails += not ok
# the QOI screenshots the plugin writes now: lossless, read by Pillow and by mine.py's own decoder
import random, mine
rnd = random.Random(1)
for w, h in ((400, 240), (320, 240), (400, 480)):
    src = Image.new("RGB", (w, h), (20, 40, 60)); dr = ImageDraw.Draw(src)
    for _ in range(60):  # flat areas, gradients, noise: every kind of QOI op
        x, y = rnd.randrange(w), rnd.randrange(h)
        dr.rectangle((x, y, x + rnd.randrange(80), y + rnd.randrange(40)), fill=tuple(rnd.randrange(256) for _ in range(3)))
    for x in range(w):
        src.putpixel((x, 5), (x % 256, (x * 3) % 256, 200))
        src.putpixel((x, 6), (x % 256, (x * 3 + 1) % 256, 201))
    raw = os.path.join(d, "in.rgb"); open(raw, "wb").write(src.tobytes())
    q = os.path.join(d, "out.qoi")
    r = subprocess.run([os.path.join(HERE, "bmp_tool"), "--qoi", raw, str(w), str(h), q])
    data = open(q, "rb").read()
    a = mine.open_screenshot(data)
    b = mine._decode_qoi(data)
    ok = r.returncode == 0 and a.tobytes() == src.tobytes() and b.tobytes() == src.tobytes()
    print(("ok   " if ok else "FAIL ") + f"QOI {w}x{h}: {len(data)} bytes, lossless (Pillow and mine.py's decoder)"); fails += not ok
    ok = ocr_server.decode_screen(data).tobytes() == src.tobytes()
    print(("ok   " if ok else "FAIL ") + "  the PC server reads it"); fails += not ok
print("ALL PASSED" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
