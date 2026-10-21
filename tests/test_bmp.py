"""The plugin's screenshot converter (core/bmp.cpp) vs. the server's decoder: colours, orientation,
size. python3 tests/test_bmp.py"""
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from PIL import Image, ImageDraw
import ocr_server
subprocess.run(["make", "-s", "-C", HERE, "bmp_tool"], check=True)
d = tempfile.mkdtemp()
src = Image.new("RGB", (400, 480), (0, 0, 0)); dr = ImageDraw.Draw(src)
marks = {(0, 0): (250, 10, 10), (399, 0): (10, 250, 10), (0, 239): (10, 10, 250), (399, 239): (250, 250, 200),
         (200, 120): (128, 64, 32)}
for (x, y), c in marks.items(): dr.rectangle((x - 2, y - 2, x + 2, y + 2), fill=c)
dr.rectangle((0, 240, 399, 479), fill=(200, 0, 200))  # bottom screen: must not appear
bmp = os.path.join(d, "shot - 000.bmp"); src.save(bmp)
fails = 0
for name, path in (("CTRPF-style bottom-up BMP", bmp),):
    out = os.path.join(d, "out.k3")
    r = subprocess.run([os.path.join(HERE, "bmp_tool"), path, out], capture_output=True, text=True)
    data = open(out, "rb").read()
    img = ocr_server.decode_screen(data)
    ok = img.size == (400, 240)
    print(("ok   " if ok else "FAIL ") + f"{name}: {len(data)} bytes (BMP was {os.path.getsize(path)}), size {img.size}"); fails += not ok
    for (x, y), c in marks.items():
        got = img.getpixel((x, y))
        ok = all(abs(a - b) <= 8 for a, b in zip(got, c))
        print(("ok   " if ok else "FAIL ") + f"pixel {(x, y)} {c} -> {got}"); fails += not ok
    raw = img.tobytes(); purple = sum(1 for i in range(0, len(raw), 3) if raw[i] > 150 and raw[i + 2] > 150 and raw[i + 1] < 50)
    ok = purple == 0
    print(("ok   " if ok else "FAIL ") + "bottom screen left out"); fails += not ok
print("ALL PASSED" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
