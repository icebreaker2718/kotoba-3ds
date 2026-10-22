"""Live check of the plugin's Google Lens client (the same C++ code as on the 3DS, built
natively): renders a game-like screenshot, sends it to Google Lens, checks the text.
Needs internet and a Japanese font (apt install fonts-noto-cjk). Used by CI; prints GitHub
annotations so a change on Google's side is visible.
    python3 tests/test_lens_live.py [dict.bin]"""
import glob, os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
fonts = (glob.glob("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc") +
         glob.glob("/usr/share/fonts/**/*CJK*.tt[cf]", recursive=True))
if not fonts:
    print("::warning title=Lens live test::no Japanese font installed, skipped")
    sys.exit(0)
font = ImageFont.truetype(fonts[0], 18)
small = ImageFont.truetype(fonts[0], 9)

img = Image.new("RGB", (400, 480), (40, 90, 60))                  # top screen: a "game"
dr = ImageDraw.Draw(img)
dr.rectangle((0, 0, 399, 30), fill=(20, 20, 40))
dr.text((8, 6), "メニュー", font=ImageFont.truetype(fonts[0], 14), fill=(230, 230, 230))
dr.rounded_rectangle((10, 150, 390, 232), 8, fill=(250, 250, 240), outline=(60, 60, 60), width=2)  # text box
line1, line2 = "また夢世界へ行く時は", "声をかけるんじゃぞ！"
dr.text((24, 170), line1, font=font, fill=(20, 20, 20))
x = 24 + dr.textlength("また", font=font)
dr.text((x + 2, 160), "ゆめせかい", font=small, fill=(20, 20, 20))  # furigana over 夢世界
dr.text((24, 198), line2, font=font, fill=(20, 20, 20))
dr.rectangle((0, 240, 399, 479), fill=(0, 0, 0))                   # bottom screen
d = tempfile.mkdtemp()
bmp = os.path.join(d, "shot - 000.bmp")
img.save(bmp)

probe = os.path.join(HERE, "test_lens")
args = [probe, "--live", bmp] + (sys.argv[1:2] if len(sys.argv) > 1 else [])
r = subprocess.run(args, capture_output=True, text=True, timeout=120)
print(r.stdout + r.stderr)
paras = [l[len("PARAGRAPH: "):] for l in r.stdout.splitlines() if l.startswith("PARAGRAPH: ")]
want = "また夢世界へ行く時は声をかけるんじゃぞ"
good = any(want in p.replace(" ", "") for p in paras)
no_furigana = not any(p.strip() == "ゆめせかい" for p in paras)
summary = " / ".join(paras)[:300] or (r.stdout.strip().splitlines() or ["no output"])[-1]
if r.returncode == 0 and good:
    print(f"::notice title=Google Lens live test::OK - read: {summary}" + ("" if no_furigana else " (furigana kept)"))
    sys.exit(0)
print(f"::warning title=Google Lens live test::FAILED (exit {r.returncode}): {summary}")
sys.exit(1)
