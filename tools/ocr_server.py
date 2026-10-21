#!/usr/bin/env python3
"""
ocr_server.py - reads Japanese text off 3DS screenshots for the Kotoba3DS plugin.

Run it on your PC while you play:

    py -3.12 tools\\ocr_server.py

The plugin finds it automatically on your home network (or you can type the address shown
below into the plugin's "OCR server" setting). In-game: press the screenshot hotkey, open
the plugin menu, choose "Read screen", then tap the word you want.

Uses the same config.json as mine.py (ocr.crop / ocr.game_crops pick the text box).
Needs: pip install pillow manga-ocr   (Python 3.12 or older)
"""
import argparse
import io
import json
import os
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

PORT = 7331
DISCOVERY_ASK = b"KOTOBA3DS?"
DISCOVERY_REPLY = b"KOTOBA3DS!"
MAX_BODY = 4 * 1024 * 1024

# ---------------------------------------------------------------------------
# text -> tappable chunks
# ---------------------------------------------------------------------------
_KANJI = re.compile(r"[㐀-鿿豈-﫿々]")
_KATA = re.compile(r"[゠-ヿｦ-ﾟ]")
_HIRA = re.compile(r"[぀-ゟ]")
_PUNCT = set("、。！？!?…‥・「」『』（）()〈〉《》【】〜～ー―─,.:;　 \n")


def _kind(ch):
    if ch in _PUNCT:
        return "p"
    if _KANJI.match(ch):
        return "k"
    if _KATA.match(ch):
        return "K"
    if _HIRA.match(ch):
        return "h"
    return "o"


def chunk_heuristic(text):
    """Without a tokenizer: a chunk starts at each kanji or katakana run, or after punctuation.
    'もう何も食べられなかった。' -> もう|何も|食べられなかった|。"""
    chunks = []
    prev = None
    for ch in text:
        k = _kind(ch)
        if ch == "ー" and prev in ("K", "h"):  # long vowel mark belongs to the word before it
            k = prev
        start = (not chunks or k == "p" or prev == "p" or
                 (k == "k" and prev != "k") or (k == "K" and prev != "K") or
                 (k in ("h", "o") and prev == "K"))
        if start:
            chunks.append(ch)
        else:
            chunks[-1] += ch
        prev = k
    return chunks


_tagger = None


def chunk_text(text):
    """Bunsetsu-like chunks using fugashi (installed with manga-ocr), else the heuristic."""
    global _tagger
    if _tagger is None:
        try:
            import fugashi
            _tagger = fugashi.Tagger()
        except Exception:
            _tagger = False
    if not _tagger:
        return chunk_heuristic(text)
    chunks, prev_pos, prev_pos2 = [], None, None
    for w in _tagger(text):
        surface = w.surface
        try:
            pos1, pos2 = w.feature.pos1, w.feature.pos2
        except AttributeError:
            parts = str(w.feature).split(",")
            pos1, pos2 = parts[0], parts[1] if len(parts) > 1 else ""
        attach = bool(chunks) and (
            pos1 in ("助動詞", "接尾辞")
            or (pos1 == "助詞" and pos2 == "接続助詞" and prev_pos in ("動詞", "形容詞", "助動詞"))
            or (pos1 in ("動詞", "形容詞") and pos2 == "非自立可能"
                and (prev_pos in ("動詞", "助動詞") or (prev_pos == "助詞" and prev_pos2 == "接続助詞"))))
        if attach:
            chunks[-1] += surface
        else:
            chunks.append(surface)
        prev_pos, prev_pos2 = pos1, pos2
    # keep whitespace/newlines out of the chunks but preserve the exact text
    out = [c for c in chunks if c]
    return out if "".join(out) == text else chunk_heuristic(text)


# ---------------------------------------------------------------------------
# OCR
# ---------------------------------------------------------------------------
LAST_DIR = os.path.join(HERE, "cache", "last_screens")


def decode_screen(data):
    """Top screen as an RGB image, from the plugin's compact RGB565 upload or any image file."""
    from PIL import Image
    if data.startswith(b"K3RGB565 "):
        nl = data.index(b"\n")
        w, h = (int(v) for v in data[9:nl].split())
        return Image.frombytes("RGB", (w, h), data[nl + 1:nl + 1 + w * h * 2], "raw", "BGR;16")
    img = Image.open(io.BytesIO(data)).convert("RGB")
    return img.crop((0, 0, 400, 240)) if img.height >= 480 else img


class Reader:
    def __init__(self, cfg, engine, model=None, config_path=None):
        self.cfg = cfg
        self.engine = engine
        self.model = model
        self.config_path = config_path
        self._cfg_mtime = os.path.getmtime(config_path) if config_path and os.path.exists(config_path) else 0
        self.lock = threading.Lock()
        self.shots = {}

    def load(self):
        if self.engine == "manga-ocr" and self.model is None:
            from manga_ocr import MangaOcr
            print("Loading manga-ocr (first start downloads the model)...")
            self.model = MangaOcr()

    def _reload_config(self):
        """Picks up crops saved by pick_crop.py without restarting the server."""
        if not self.config_path or not os.path.exists(self.config_path):
            return
        m = os.path.getmtime(self.config_path)
        if m != self._cfg_mtime:
            try:
                with open(self.config_path, encoding="utf-8") as f:
                    self.cfg = json.load(f)
                self._cfg_mtime = m
            except (OSError, ValueError):
                pass

    def crop_for(self, title_id):
        oc = self.cfg.get("ocr", {})
        return oc.get("game_crops", {}).get(title_id) or oc.get("crop") or [0, 0, 400, 240]

    def remember(self, title_id, shot_name, img):
        """Keeps recent screenshots so in-game card creation needn't upload them again."""
        if not shot_name:
            return
        with self.lock:
            self.shots[(title_id, shot_name)] = img
            while len(self.shots) > 30:
                self.shots.pop(next(iter(self.shots)))

    def screenshot(self, title_id, shot_name):
        with self.lock:
            return self.shots.get((title_id, shot_name))

    @staticmethod
    def top_screen(image_bytes):
        return decode_screen(image_bytes)

    def read(self, image_bytes, title_id, crop=None, shot_name=""):
        import ocrprep
        self._reload_config()
        img = self.top_screen(image_bytes)
        self.remember(title_id, shot_name, img)
        if title_id and re.fullmatch(r"[0-9A-Fa-f]{16}", title_id):  # for pick_crop.py
            os.makedirs(LAST_DIR, exist_ok=True)
            img.save(os.path.join(LAST_DIR, f"{title_id.upper()}.png"))
        box = crop or ocrprep.crop_for(self.cfg, title_id, img)
        self.last_box = box
        img = img.crop(tuple(box))
        with self.lock:
            self.load()
            text = ocrprep.read_box(self.model, img) if self.model else ""
        return re.sub(r"\s+", "", text or "")


# ---------------------------------------------------------------------------
# in-game card creation
# ---------------------------------------------------------------------------
class CardMaker:
    """Makes Anki cards for words saved on the 3DS, one at a time in the background."""

    def __init__(self, reader, log):
        import queue
        self.reader, self.log = reader, log
        self.jobs = queue.Queue()
        self.enricher = None
        threading.Thread(target=self._run, daemon=True).start()

    def submit(self, row, img):
        self.jobs.put((row, img))

    def _run(self):
        import mine
        while True:
            row, img = self.jobs.get()
            try:
                cfg = self.reader.cfg
                if self.enricher is None:
                    self.enricher = mine.Enricher(cfg, log=self.log)
                model = self.reader.model
                if model is not None:  # share the loaded OCR model, one user at a time
                    def locked(image, _m=model):
                        with self.reader.lock:
                            return _m(image)
                    mine._ocr_model = locked
                else:
                    cfg = dict(cfg, ocr=dict(cfg.get("ocr", {}), engine="none"))
                result = mine.make_card(cfg, self.enricher, row, img, log=self.log)
                if result in ("added", "duplicate"):
                    mine.mark_done(row)
                elif result == "unreachable":
                    self.log("  (is Anki open? The word stays saved on the 3DS; mine.py will add it later)")
            except Exception as e:  # never kill the worker
                self.log(f"  card for {row.get('expression')} failed: {type(e).__name__}: {e}")


# ---------------------------------------------------------------------------
# HTTP + discovery
# ---------------------------------------------------------------------------
def make_handler(reader, log, maker=None):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def _send(self, code, body):
            data = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.startswith("/ping"):
                self._send(200, "KOTOBA3DS\n")
            else:
                self._send(404, "ERR\nnot found\n")

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0 or n > MAX_BODY:
                return None
            body = b""
            while len(body) < n:
                part = self.rfile.read(n - len(body))
                if not part:
                    break
                body += part
            return body

        def do_POST(self):
            if self.path.startswith("/shot"):
                return self._post_shot()
            if self.path.startswith("/mine"):
                return self._post_mine()
            if not self.path.startswith("/ocr"):
                return self._send(404, "ERR\nnot found\n")
            body = self._body()
            if body is None:
                return self._send(400, "ERR\nbad image size\n")
            title_id = self.headers.get("X-Title-Id", "").upper()
            crop = None
            if self.headers.get("X-Crop"):  # pick_crop.py's "Test" button
                try:
                    crop = [int(v) for v in self.headers["X-Crop"].split(",")][:4]
                except ValueError:
                    crop = None
            t0 = time.time()
            try:
                text = reader.read(body, title_id, crop, self.headers.get("X-Shot-Name", ""))
            except Exception as e:
                log(f"OCR failed: {e}")
                return self._send(500, f"ERR\n{type(e).__name__}: {e}\n")
            chunks = chunk_text(text) if text else []
            box = getattr(reader, "last_box", None) or [0, 0, 400, 240]
            hint = "" if list(box) != [0, 0, 400, 240] else "   <- whole screen; run pick_crop.py to set this game's text box"
            log(f"[{time.strftime('%H:%M:%S')}] {title_id or '?'}: 「{text}」 ({time.time() - t0:.1f}s){hint}")
            # KOTOBA1 / sentence / chunks separated by tabs (they join back to the sentence)
            self._send(200, "KOTOBA1\n" + text + "\n" + "\t".join(chunks) + "\n")

        def _post_shot(self):
            body = self._body()
            if body is None:
                return self._send(400, "ERR\nbad image size\n")
            try:
                img = reader.top_screen(body)
            except Exception as e:
                return self._send(400, f"ERR\nnot an image: {e}\n")
            reader.remember(self.headers.get("X-Title-Id", "").upper(), self.headers.get("X-Shot-Name", ""), img)
            self._send(200, "OK\n")

        def _post_mine(self):
            import mine
            if maker is None:
                return self._send(503, "ERR\ncard creation is off\n")
            body = self._body()
            if body is None:
                return self._send(400, "ERR\nempty\n")
            rows = mine.parse_log(body)
            if not rows:
                return self._send(400, "ERR\nno word in request\n")
            row = rows[0]
            row["title_id"] = row["title_id"].upper()
            if mine.row_key(row) in mine.load_state():
                return self._send(200, "OK\nalready a card\n")
            img = None
            if row["screenshot"]:
                img = reader.screenshot(row["title_id"], row["screenshot"])
                if img is None:
                    return self._send(200, "NEED_SHOT\n")
            maker.submit(row, img)
            log(f"[{time.strftime('%H:%M:%S')}] making card: {row['expression']}【{row['reading']}】")
            self._send(200, "OK\nqueued\n")

    return Handler


def discovery_responder(port, log, stop):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", port))
    except OSError as e:
        log(f"(automatic discovery unavailable: {e})")
        return
    s.settimeout(0.5)
    while not stop.is_set():
        try:
            data, addr = s.recvfrom(256)
        except socket.timeout:
            continue
        except OSError:
            break
        if data.strip() == DISCOVERY_ASK:
            s.sendto(DISCOVERY_REPLY + b" " + str(port).encode(), addr)
            log(f"3DS at {addr[0]} found this PC")


def local_ips():
    ips = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith("127."))


def serve(cfg, port=PORT, engine="manga-ocr", preload=True, log=print, ready=None, stop=None, model=None,
          config_path=None, cards=True):
    reader = Reader(cfg, engine, model, config_path)
    maker = CardMaker(reader, log) if cards else None
    if preload:
        reader.load()
    stop = stop or threading.Event()
    threading.Thread(target=discovery_responder, args=(port, log, stop), daemon=True).start()
    class Server(ThreadingHTTPServer):
        def handle_error(self, request, client_address):
            err = sys.exc_info()[1]
            if isinstance(err, (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)):
                log(f"{client_address[0]} connected but closed the connection before sending anything")
            else:
                super().handle_error(request, client_address)

    httpd = Server(("0.0.0.0", port), make_handler(reader, log, maker))
    httpd.timeout = 0.5
    ips = local_ips()
    log(f"Kotoba3DS OCR server ready on port {port}.")
    log("Address for the plugin: " + (", ".join(f"{ip}:{port}" for ip in ips) or f"<this PC's IP>:{port}"))
    log("(If Windows asks, allow Python on private networks.)  Ctrl+C to stop.")
    if maker:
        log("Words you save in-game become Anki cards right away (keep Anki open).")
    if ready:
        ready.set()
    try:
        while not stop.is_set():
            httpd.handle_request()
    finally:
        httpd.server_close()
        stop.set()


def main():
    import mine
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=os.path.join(HERE, "config.json"))
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--no-cards", action="store_true", help="only do OCR; make cards later with mine.py")
    a = ap.parse_args()
    cfg = mine.load_config(a.config) or json.loads(json.dumps(mine.DEFAULT_CONFIG))
    try:
        serve(cfg, a.port, config_path=a.config, cards=not a.no_cards)
    except KeyboardInterrupt:
        print("stopped")
    except ImportError as e:
        sys.exit(f"{e}\nInstall the OCR model with: py -3.12 -m pip install manga-ocr")


if __name__ == "__main__":
    main()
