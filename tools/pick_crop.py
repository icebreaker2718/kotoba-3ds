#!/usr/bin/env python3
"""
pick_crop.py - choose where a game's text box is, so OCR only reads the text.

    py -3.12 tools\\pick_crop.py

It opens the last screenshot the OCR server received for each game (use "Read screen" once in
that game first; any screenshot of a text box works). Drag a rectangle around the text box and
press Save. Green lines are what will be read; red bands (furigana, icons) are ignored.
"Test" sends the box to the running ocr_server.py and shows what it reads.
The crop is stored in config.json under ocr.game_crops and the server picks it up immediately.
"""
import io
import json
import os
import sys
import tkinter as tk
import urllib.request
from tkinter import messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from PIL import Image, ImageTk  # noqa: E402

import ocrprep  # noqa: E402

LAST_DIR = os.path.join(HERE, "cache", "last_screens")
CONFIG = os.path.join(HERE, "config.json")
SCALE = 2


def load_config():
    import mine
    return mine.load_config(CONFIG) or json.loads(json.dumps(mine.DEFAULT_CONFIG))


def save_crop(title_id, box, game_name=None, path=None, colour=None):
    """Saves a fixed box; with `colour`, also lets the server find the box wherever it moves."""
    path = path or CONFIG
    cfg = load_config()
    cfg.setdefault("ocr", {}).setdefault("game_crops", {})[title_id] = [int(v) for v in box]
    boxes = cfg["ocr"].setdefault("game_boxes", {})
    if colour:
        boxes[title_id] = {"colour": [int(v) for v in colour], "tolerance": 28}
    else:
        boxes.pop(title_id, None)
    if game_name:
        cfg.setdefault("games", {})[title_id] = game_name
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)


def screens():
    if not os.path.isdir(LAST_DIR):
        return []
    return sorted(f[:-4] for f in os.listdir(LAST_DIR) if f.lower().endswith(".png"))


class App:
    def __init__(self, root):
        self.root = root
        self.cfg = load_config()
        root.title("Kotoba3DS - pick the text box")
        top = ttk.Frame(root, padding=6)
        top.pack(fill="x")
        ttk.Label(top, text="Game:").pack(side="left")
        self.ids = screens()
        names = [f"{self.cfg.get('games', {}).get(t, '?')}  ({t})" for t in self.ids]
        self.combo = ttk.Combobox(top, values=names, state="readonly", width=48)
        self.combo.pack(side="left", padx=4)
        self.combo.bind("<<ComboboxSelected>>", lambda e: self.load(self.combo.current()))
        ttk.Label(top, text="Name:").pack(side="left", padx=(8, 0))
        self.name = tk.StringVar()
        ttk.Entry(top, textvariable=self.name, width=22).pack(side="left", padx=4)

        self.canvas = tk.Canvas(root, width=400 * SCALE, height=240 * SCALE, bg="black",
                                highlightthickness=0, cursor="crosshair")
        self.canvas.pack(padx=6)
        self.canvas.bind("<ButtonPress-1>", self.start)
        self.canvas.bind("<B1-Motion>", self.drag)
        self.canvas.bind("<ButtonRelease-1>", self.done)

        bottom = ttk.Frame(root, padding=6)
        bottom.pack(fill="x")
        self.moving = tk.BooleanVar(value=False)
        ttk.Checkbutton(bottom, text="Box moves around (find it by colour)", variable=self.moving,
                        command=self.redraw).pack(side="left")
        ttk.Button(bottom, text="Save", command=self.save).pack(side="right")
        ttk.Button(bottom, text="Test", command=self.test).pack(side="right", padx=4)
        self.info = tk.StringVar(value="Drag a box around the game's text.")
        ttk.Label(root, textvariable=self.info, wraplength=400 * SCALE, padding=(6, 0, 6, 6)).pack(fill="x")

        self.box = None
        self.rect = None
        self.overlay = []
        if not self.ids:
            messagebox.showinfo("Kotoba3DS", "No screenshots yet.\n\nStart ocr_server.py, then use "
                                             "'Read screen' once in the game, and open this again.")
            root.destroy()
            return
        self.combo.current(0)
        self.load(0)

    def load(self, i):
        self.tid = self.ids[i]
        self.img = Image.open(os.path.join(LAST_DIR, self.tid + ".png")).convert("RGB")
        self.photo = ImageTk.PhotoImage(self.img.resize((400 * SCALE, 240 * SCALE), Image.NEAREST))
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, anchor="nw", image=self.photo)
        self.name.set(self.cfg.get("games", {}).get(self.tid, ""))
        box = self.cfg.get("ocr", {}).get("game_crops", {}).get(self.tid)
        spec = self.cfg.get("ocr", {}).get("game_boxes", {}).get(self.tid)
        self.moving.set(bool(spec))
        if spec:  # show where the box is on this screenshot
            found = ocrprep.find_box(self.img, spec["colour"], spec.get("tolerance", 28))
            box = list(found) if found else box
        self.box = box
        self.rect = None
        if box:
            self.draw_box()
        else:
            self.info.set("Drag a box around the game's text.")

    def start(self, e):
        self.x0, self.y0 = e.x, e.y
        if self.rect:
            self.canvas.delete(self.rect)
        self.rect = self.canvas.create_rectangle(e.x, e.y, e.x, e.y, outline="#00a0ff", width=2)

    def drag(self, e):
        self.canvas.coords(self.rect, self.x0, self.y0, e.x, e.y)

    def done(self, e):
        l, r = sorted((self.x0, e.x))
        t, b = sorted((self.y0, e.y))
        l, t = max(0, l // SCALE), max(0, t // SCALE)
        r, b = min(400, r // SCALE + 1), min(240, b // SCALE + 1)
        if r - l < 8 or b - t < 8:
            return
        self.box = [l, t, r, b]
        self.draw_box()

    def draw_box(self):
        for o in self.overlay:
            self.canvas.delete(o)
        self.overlay = []
        if self.rect:
            self.canvas.delete(self.rect)
        l, t, r, b = self.box
        self.rect = self.canvas.create_rectangle(l * SCALE, t * SCALE, r * SCALE, b * SCALE,
                                                 outline="#00a0ff", width=2)
        crop = self.img.crop((l, t, r, b))
        lines = ocrprep.text_lines(crop, pad=0)
        for a, z in lines:
            self.overlay.append(self.canvas.create_rectangle(
                l * SCALE, (t + a) * SCALE, r * SCALE, (t + z) * SCALE, outline="#20d020", width=2))
        mode = " Saving will remember its colour and find it anywhere." if self.moving.get() else ""
        self.info.set(f"Box {self.box}: {len(lines)} text line(s) (green). "
                      + ("Save to use it." if lines else "No text found - try a bigger box.") + mode)

    def redraw(self):
        if self.box:
            self.draw_box()

    def save(self):
        if not self.box:
            return
        colour = None
        if self.moving.get():
            l, t, r, b = self.box
            colour = ocrprep.box_colour(self.img.crop((l, t, r, b)))
            found = ocrprep.find_box(self.img, colour)
            if not found:
                messagebox.showwarning("Kotoba3DS", "Couldn't find a box of that colour. Draw the box so it "
                                                    "is mostly the text box's background, then try again.")
                return
            self.box = list(found)
            self.draw_box()
        save_crop(self.tid, self.box, self.name.get().strip() or None, colour=colour)
        self.cfg = load_config()
        i = self.combo.current()
        self.combo["values"] = [f"{self.cfg.get('games', {}).get(t, '?')}  ({t})" for t in self.ids]
        self.combo.current(i)
        how = "found by colour wherever it moves" if colour else "fixed position"
        self.info.set(f"Saved for {self.name.get() or self.tid} ({how}). The server uses it right away.")

    def test(self):
        if not self.box:
            return
        buf = io.BytesIO()
        self.img.save(buf, "PNG")
        req = urllib.request.Request("http://127.0.0.1:7331/ocr", data=buf.getvalue(), headers={
            "X-Title-Id": self.tid, "X-Crop": ",".join(str(v) for v in self.box),
            "Content-Type": "application/octet-stream"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                lines = r.read().decode("utf-8").split("\n")
            self.info.set("Reads: 「" + (lines[1] if len(lines) > 1 else "") + "」")
        except OSError as e:
            self.info.set(f"Test needs ocr_server.py running ({e})")


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
