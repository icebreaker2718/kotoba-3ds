"""Runs tools/ocr_server.py with a stand-in OCR model and drives it with the plugin's own
network code (tests/test_net.cpp, compiled natively).
    python3 tests/test_ocr.py path/to/dict.bin
"""
import os, socket, subprocess, sys, tempfile, threading
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from PIL import Image
import mine, ocr_server

seen = []
def fake_model(img):
    seen.append(img.size)
    return "もう何も 食べられなかった。"

import json, time
from http.server import BaseHTTPRequestHandler, HTTPServer
notes = []
class Anki(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        a, p = req["action"], req.get("params", {})
        res, err = 1, None
        if a == "addNote":
            if any(n["fields"]["Expression"] == p["note"]["fields"]["Expression"] for n in notes):
                err = "cannot create note because it is a duplicate"
            else:
                notes.append(p["note"])
        body = json.dumps({"result": res, "error": err}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
anki_srv = HTTPServer(("127.0.0.1", 0), Anki)
threading.Thread(target=anki_srv.serve_forever, daemon=True).start()
mine.STATE_FILE = os.path.join(tempfile.mkdtemp(), "imported.json")

s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
cfg = json.loads(json.dumps(mine.DEFAULT_CONFIG))
cfg["ocr"]["game_crops"] = {"0004000000123400": [0, 160, 400, 240]}
cfg["anki"]["url"] = f"http://127.0.0.1:{anki_srv.server_port}"
cfg["audio"]["enabled"] = False
cfg["dictionaries"] = {"folder": "", "glossary": [], "pitch": [], "frequency": []}
cfg["games"]["0004000000123400"] = "Test Game"
ready, stop = threading.Event(), threading.Event()
threading.Thread(target=ocr_server.serve, kwargs=dict(cfg=cfg, port=port, engine="fake", preload=False,
                 ready=ready, stop=stop, model=fake_model, log=lambda *a: print("  server:", *a)), daemon=True).start()
ready.wait(5)
bmp = os.path.join(tempfile.mkdtemp(), "shot - 000.bmp")
Image.new("RGB", (400, 480), (20, 20, 60)).save(bmp)
subprocess.run(["make", "-s", "-C", HERE, "test_net"], check=True)
line = ("2026-10-19 18:50:00\t0004000000123400\t宝箱\tたからばこ\t1. (n) treasure chest\tshot - 007.bmp"
        "\t宝箱\t\t4000\tこの先に宝箱があるらしいぞ。")
r = subprocess.run([os.path.join(HERE, "test_net"), str(port), bmp, sys.argv[1], line])
for _ in range(50):
    if len(notes) >= 2: break
    time.sleep(0.1)
for _ in range(20):
    if mine.row_key(mine.parse_log(line.encode())[0]) in mine.load_state(): break
    time.sleep(0.1)
notes = [n for n in notes if n["fields"]["Expression"] == "宝箱"] + [n for n in notes if n["fields"]["Expression"] != "宝箱"]
card_ok = (len(notes) == 2 and notes[0]["deckName"] == "Mining Deck"
           and notes[0]["fields"]["Sentence"] == "この先に<b>宝箱</b>があるらしいぞ。"
           and notes[0]["fields"]["Picture"].startswith("<img")
           and mine.row_key(mine.parse_log(line.encode())[0]) in mine.load_state())
print("in-game card made in Anki:", card_ok, notes[0]["fields"]["Sentence"] if notes else None)
import urllib.request
again = urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}/mine", data=(line + "\n").encode()), timeout=5).read().decode()
print("sending the same word again:", again.strip().replace(chr(10), " "))
card_ok = card_ok and "already" in again
# pick_crop.py's Test button: X-Crop overrides the configured box
import urllib.request
req = urllib.request.Request(f"http://127.0.0.1:{port}/ocr", data=open(bmp, "rb").read(),
                             headers={"X-Title-Id": "0004000000123400", "X-Crop": "10,20,110,60"})
reply = urllib.request.urlopen(req, timeout=10).read().decode()
stop.set()
last = os.path.join(os.path.dirname(ocr_server.__file__), "cache", "last_screens", "0004000000123400.png")
had_last = os.path.exists(last)
if had_last: os.remove(last)
print("server cropped to the game's text box:", seen, "| last screen saved for pick_crop:", had_last)
ok = (r.returncode == 0 and seen == [(400, 80), (400, 80), (100, 40)] and reply.startswith("KOTOBA1") and had_last
      and card_ok)
print("ALL PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
