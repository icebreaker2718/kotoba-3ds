"""On-device OCR (VOCR, the same C/C++ code the 3DS runs, built natively) on rendered 3DS
top screens: different fonts, sizes, furigana, light-on-dark and dark-on-light text boxes,
a box in the middle of the scene, UI text around it.
    python3 tests/test_local_ocr.py path/to/VOCR5_N_w8.vocr
Needs fonts-noto-cjk (and fonts-misaki for the pixel-font case, skipped if missing)."""
import glob, os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
model = sys.argv[1] if len(sys.argv) > 1 else ""
if not os.path.exists(model):
    print("::warning title=On-device OCR test::no model file, skipped")
    sys.exit(0)
noto = (glob.glob("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc") +
        glob.glob("/usr/share/fonts/**/*CJK*Regular*.tt[cf]", recursive=True))
misaki = glob.glob("/usr/share/fonts/**/misaki_gothic.ttf", recursive=True)
if not noto:
    print("::warning title=On-device OCR test::no Japanese font, skipped")
    sys.exit(0)

def screen(lines, font, size, box, fg, bg, furigana=None, aa=True, scene=(40, 90, 60)):
    img = Image.new("RGB", (400, 480), scene)
    dr = ImageDraw.Draw(img)
    if not aa:
        dr.fontmode = "1"
    for i in range(0, 400, 25):                                   # some scenery
        dr.rectangle((i, 120, i + 12, 140), fill=(120, 80 + i % 60, 50))
    dr.text((6, 4), "HP 12/20", font=ImageFont.truetype(noto[0], 11), fill=(255, 255, 255))
    x0, y0, x1, y1 = box
    dr.rounded_rectangle(box, 6, fill=bg, outline=(90, 90, 90), width=2)
    f = ImageFont.truetype(font, size)
    y = y0 + (size // 2 + 4 if furigana else 8)
    for line in lines:
        dr.text((x0 + 12, y), line, font=f, fill=fg)
        y += int(size * 1.6)
    if furigana:
        text, before = furigana
        fx = x0 + 12 + dr.textlength(before, font=f)
        dr.text((fx, y0 + 2), text, font=ImageFont.truetype(font, max(7, size // 2)), fill=fg)
    dr.rectangle((0, 240, 399, 479), fill=(0, 0, 0))
    return img

cases = [
    ("Noto 18px, dark on light, furigana", ["また夢世界へ行く時は", "声をかけるんじゃぞ！"],
     dict(font=noto[0], size=18, box=(10, 150, 390, 232), fg=(20, 20, 20), bg=(250, 250, 240), furigana=("ゆめせかい", "また"))),
    ("Noto 14px, light on dark, box at the top", ["この先に宝箱があるらしいぞ。", "気をつけて進むんだ。"],
     dict(font=noto[0], size=14, box=(20, 30, 380, 90), fg=(255, 255, 255), bg=(30, 30, 80))),
    ("Noto 13px, no anti-aliasing", ["もう何も食べられなかった。"],
     dict(font=noto[0], size=13, box=(30, 170, 370, 205), fg=(0, 0, 0), bg=(255, 255, 255), aa=False)),
]
if misaki:
    cases.append(("Misaki pixel font at 16px", ["ルイージ、はやく来てくれ！", "城の中に入るぞ。"],
                  dict(font=misaki[0], size=16, box=(10, 160, 390, 230), fg=(255, 255, 255), bg=(20, 20, 20), aa=False)))

def cer(ref, hyp):
    norm = lambda t: t.replace(" ", "").replace("!", "！").replace("?", "？")
    a, b = norm(ref), norm(hyp)
    d = list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(b) + 1):
            prev, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] != b[j - 1]))
    return d[len(b)] / max(1, len(a))

probe = os.path.join(HERE, "test_lens")
tmp = tempfile.mkdtemp()
fails, total = 0, 0.0
for name, lines, kw in cases:
    bmp = os.path.join(tmp, "shot.bmp")
    screen(lines, **kw).save(bmp)
    r = subprocess.run([probe, "--local", model, bmp], capture_output=True, text=True, timeout=120)
    paras = [l[len("PARAGRAPH: "):] for l in r.stdout.splitlines() if l.startswith("PARAGRAPH: ")]
    want = "".join(lines)
    best = min((cer(want, p) for p in paras), default=1.0)
    total += best
    ok = r.returncode == 0 and best <= 0.10
    fails += 0 if ok else 1
    print(("ok   " if ok else "FAIL ") + f"{name}: CER {best:.0%} -> {paras}" + ("" if r.returncode == 0 else " " + r.stdout[-200:]))
    if r.returncode == 0 and paras and "ゆめせかい" in "".join(paras):
        print("FAIL furigana was not dropped"); fails += 1

summary = f"{len(cases) - fails}/{len(cases)} screens read, mean CER {total / len(cases):.1%}"
print(("::notice" if not fails else "::warning") + f" title=On-device OCR ({os.path.basename(model)})::{summary}")
print("\nALL PASSED" if not fails else f"\n{fails} FAILED")
sys.exit(1 if fails else 0)
