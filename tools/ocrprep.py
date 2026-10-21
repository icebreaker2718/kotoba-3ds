"""
ocrprep.py - prepares a game's text box for manga-ocr.

manga-ocr reads one text block at a time and is easily confused by furigana (the small
readings above kanji) and by icons. So we:
  1. work on the cropped text box only (ocr.crop / ocr.game_crops in config.json),
  2. find the text lines from the row profile of "ink" pixels,
  3. drop thin rows above a line (furigana) and stray small bands (icons, arrows),
  4. hand each line to the OCR model separately and join the results.
Works for dark-on-light and light-on-dark text: "ink" is anything far from the box's most
common colour.
"""
from collections import Counter

from PIL import Image


def _ink_mask(img):
    g = img.convert("L")
    w, h = g.size
    px = g.load()
    bg = Counter(px[x, y] // 8 * 8 for x in range(w) for y in range(h)).most_common(1)[0][0]
    ink = [[abs(px[x, y] - bg) > 90 for x in range(w)] for y in range(h)]
    # ignore the box's own border lines: rows/columns that are ink almost everywhere
    full_rows = {y for y in range(h) if sum(ink[y]) > 0.9 * w}
    full_cols = {x for x in range(w) if sum(ink[y][x] for y in range(h)) > 0.9 * h}
    for y in range(h):
        for x in range(w):
            if y in full_rows or x in full_cols:
                ink[y][x] = False
    return ink, w, h


def _bands(counts, min_count=2, max_gap=1):
    bands, start, gap = [], None, 0
    for y, c in enumerate(counts + [0] * (max_gap + 1)):
        if c >= min_count:
            if start is None:
                start = y
            gap = 0
            end = y
        elif start is not None:
            gap += 1
            if gap > max_gap:
                bands.append((start, end + 1))
                start, gap = None, 0
    return bands


def text_lines(img, keep_ratio=0.65, pad=2):
    """-> list of (top, bottom) rows of the main text lines, furigana and icons removed."""
    ink, w, h = _ink_mask(img)
    counts = [sum(row) for row in ink]
    bands = _bands(counts)
    if not bands:
        return []
    tallest = max(b - a for a, b in bands)
    lines = [(a, b) for a, b in bands if (b - a) >= keep_ratio * tallest]
    return [(max(0, a - pad), min(h, b + pad)) for a, b in lines]


def line_images(img, scale=3):
    """Each main text line as its own (upscaled) image. Falls back to the whole box."""
    lines = text_lines(img)
    if not lines:
        return [img]
    out = []
    for a, b in lines:
        line = img.crop((0, a, img.width, b))
        out.append(line.resize((line.width * scale, line.height * scale), Image.LANCZOS))
    return out


def read_box(model, img):
    """OCR a cropped text box line by line with `model` (a MangaOcr instance or any callable)."""
    parts = []
    for line in line_images(img):
        t = (model(line) or "").strip()
        if t:
            parts.append(t)
    return "".join(parts)


def debug_image(img, path):
    """Saves the box with kept lines (green) and dropped bands (red) outlined."""
    from PIL import ImageDraw
    ink, w, h = _ink_mask(img)
    counts = [sum(row) for row in ink]
    kept = text_lines(img, pad=0)
    vis = img.convert("RGB").resize((w * 3, h * 3), Image.NEAREST)
    d = ImageDraw.Draw(vis)
    for a, b in _bands(counts):
        color = (0, 200, 0) if (a, b) in kept else (220, 0, 0)
        d.rectangle((0, a * 3, w * 3 - 1, b * 3 - 1), outline=color, width=2)
    vis.save(path)


# ---------------------------------------------------------------------------
# Finding a text box that moves around, by its background colour
# ---------------------------------------------------------------------------
def box_colour(img):
    """Most common colour in an image (the text box's background)."""
    rgb = img.convert("RGB")
    c = Counter(tuple(v // 4 * 4 for v in p) for p in rgb.getdata()).most_common(1)[0][0]
    return [v + 2 for v in c]


def find_box(img, colour, tolerance=28, min_area=1500):
    """Bounding box (l, t, r, b) of the largest connected area of `colour`, or None.
    Text inside the box doesn't matter: only the area's outline is used."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    px = rgb.load()
    cr, cg, cb = colour
    match = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if abs(r - cr) <= tolerance and abs(g - cg) <= tolerance and abs(b - cb) <= tolerance:
                match[y * w + x] = 1
    seen = bytearray(w * h)
    best, best_area = None, 0
    for start in range(w * h):
        if not match[start] or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        area, l, t, r, b = 0, w, h, 0, 0
        while stack:
            i = stack.pop()
            area += 1
            x, y = i % w, i // w
            l, r, t, b = min(l, x), max(r, x), min(t, y), max(b, y)
            for j in (i - 1 if x > 0 else -1, i + 1 if x < w - 1 else -1, i - w if y > 0 else -1,
                      i + w if y < h - 1 else -1):
                if j >= 0 and match[j] and not seen[j]:
                    seen[j] = 1
                    stack.append(j)
        # a text box fills most of its bounding rectangle (even with text inside)
        fill = area / float((r - l + 1) * (b - t + 1))
        if area >= min_area and fill >= 0.45 and area > best_area:
            best, best_area = (l, t, r + 1, b + 1), area
    return best


def crop_for(cfg, title_id, img):
    """The part of the top screen to read for this game:
    ocr.game_boxes (moving box found by colour) > ocr.game_crops (fixed) > ocr.crop."""
    oc = cfg.get("ocr", {}) if cfg else {}
    tid = (title_id or "").upper()
    spec = oc.get("game_boxes", {}).get(tid)
    if spec and spec.get("colour"):
        found = find_box(img, spec["colour"], spec.get("tolerance", 28))
        if found:
            return list(found)
    return oc.get("game_crops", {}).get(tid) or oc.get("game_crops", {}).get(title_id) or \
        oc.get("crop") or [0, 0, 400, 240]
