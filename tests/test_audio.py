"""Audio fallbacks in tools/enrich.py against fake JapanesePod101 / dictionary-search / Wikimedia
Commons (Lingua Libre) / Forvo servers.    python3 tests/test_audio.py"""
import hashlib, json, os, socket, sys, threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs, unquote

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import enrich, mine

fails = 0
def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    fails += 0 if cond else 1

MP3 = b"ID3" + b"\x00" * 3000
ALT = b"ID3alt" + b"\x02" * 3000
WAV = b"RIFF" + b"\x00" * 3000
FORVO = b"ID3forvo" + b"\x03" * 3000
MISSING = b"ID3missing" + b"\x01" * 3000
enrich.JPOD_MISSING_MD5 = hashlib.md5(MISSING).hexdigest()
reqs = []

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, body, ct="audio/mpeg", code=200):
        self.send_response(code); self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); reqs.append(u.path)
        if u.path == "/jpod":
            return self.reply(MP3 if q.get("kana") == ["たべる"] else MISSING)
        if u.path == "/api.php":  # Wikimedia Commons
            if q.get("list") == ["search"]:
                s = q["srsearch"][0]
                hits = []
                if s.startswith("intitle:/") and "-猫\\.wav/" in s:
                    hits = [{"ns": 6, "title": "File:LL-Q5287 (jpn)-Speaker1-子猫.wav"},  # wrong word
                            {"ns": 6, "title": "File:LL-Q5287 (jpn)-Speaker2-猫.wav"}]
                return self.reply(json.dumps({"query": {"search": hits}}).encode(), "application/json")
            if q.get("prop") == ["imageinfo"]:
                t = q["titles"][0]
                ok = t == "File:LL-Q5287 (jpn)-Speaker2-猫.wav"
                pages = {"1": {"title": t, "imageinfo": [{"url": A + "/upload/neko.wav"}] if ok else []}}
                return self.reply(json.dumps({"query": {"pages": pages}}).encode(), "application/json")
        if u.path == "/upload/neko.wav":
            return self.reply(WAV, "audio/wav")
        if u.path.startswith("/forvo/key/"):
            parts = [unquote(p) for p in u.path.split("/")]
            key, word = parts[3], parts[parts.index("word") + 1]
            if key != "SECRET":
                return self.reply(b"Incorrect domain or key", "text/html")
            items = [{"pathmp3": A + "/f/inu.mp3"}] if word == "犬" else []
            return self.reply(json.dumps({"attributes": {"total": len(items)}, "items": items}).encode(),
                              "application/json")
        if u.path == "/f/inu.mp3":
            return self.reply(FORVO)
        if u.path == "/alt/hashiru.mp3":
            return self.reply(ALT)
        if u.path == "/alt/wrong.mp3":
            return self.reply(MP3)
        self.reply(b"", "text/plain", 404)
    def do_POST(self):
        u = urlparse(self.path); reqs.append(u.path)
        form = parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
        if u.path == "/alt":  # japanesepod101.com dictionary search page
            term = form["search_query"][0]
            rows = ""
            if term == "走る":
                rows = ('<div class="dc-result-row"><span class="dc-vocab">走る</span>'
                        '<span class="dc-vocab_kana">はしらない</span>'
                        '<audio><source src="/alt/wrong.mp3" type="audio/mpeg"></audio></div>'
                        '<div class="dc-result-row"><span class="dc-vocab">走る</span>'
                        '<span class="dc-vocab_kana">はしる</span>'
                        '<audio><source src="/alt/hashiru.mp3" type="audio/mpeg"></audio></div>')
            return self.reply(("<html><body>" + rows + "</body></html>").encode(), "text/html")
        self.reply(b"", "text/plain", 404)

srv = HTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
A = f"http://127.0.0.1:{srv.server_port}"

# a port nothing listens on = Local Audio Server not running
s = socket.socket(); s.bind(("127.0.0.1", 0)); dead = s.getsockname()[1]; s.close()

SOURCES = [{"type": "local", "url": f"http://127.0.0.1:{dead}/?term={{term}}&reading={{reading}}"},
           {"type": "jpod101", "url": A + "/jpod"},
           {"type": "jpod101-alt", "url": A + "/alt"},
           {"type": "lingualibre", "url": A + "/api.php"},
           {"type": "forvo", "url": A + "/forvo"}]
logs = []
def get(expr, reading, key="SECRET"):
    return enrich.fetch_audio(SOURCES, expr, reading, log=logs.append, forvo_key=key)

g = get("食べる", "たべる")
check(g and g[0] == MP3 and g[2] == "jpod101", f"jpod101 first when it has the word ({g and g[2]})")
check("local" in enrich._unreachable, "Local Audio Server not running -> skipped from then on")
check(not any("local" in l for l in logs), "...and quietly (no error printed)")

g = get("走る", "はしる")
check(g and g[0] == ALT and g[2] == "jpod101-alt", f"jpod101 'missing' clip -> dictionary search ({g and g[2]})")

g = get("猫", "ねこ")
check(g and g[0] == WAV and g[1] == "wav" and g[2] == "lingualibre", f"-> Lingua Libre ({g and g[2]})")

g = get("犬", "いぬ")
check(g and g[0] == FORVO and g[2] == "forvo", f"-> Forvo with a key ({g and g[2]})")
n = len(reqs)
g = get("犬", "いぬ", key="")
check(g is None and not any(r.startswith("/forvo") for r in reqs[n:]), "Forvo skipped without a key")
logs.clear()
g = get("犬", "いぬ", key="WRONG")
check(g is None and any("forvo" in l and "key" in l for l in logs), f"bad Forvo key is reported {logs}")

check(get("存在しない", "そんざいしない") is None, "no source has it -> None")

# config: an old config with only jpod101 gets the new fallbacks; a customised one is kept
import tempfile
d = tempfile.mkdtemp()
p = os.path.join(d, "config.json")
old = json.loads(json.dumps(mine.DEFAULT_CONFIG)); old["audio"] = {"enabled": True, "sources": ["jpod101"]}
json.dump(old, open(p, "w"))
c = mine.load_config(p)
check(c["audio"]["sources"] == mine.DEFAULT_CONFIG["audio"]["sources"] and c["audio"]["forvo_key"] == "",
      f"old default upgraded: {c['audio']['sources']}")
old["audio"] = {"enabled": True, "sources": [{"name": "mine", "url": "http://x/{term}"}, "jpod101"]}
json.dump(old, open(p, "w"))
check(mine.load_config(p)["audio"]["sources"][0]["name"] == "mine", "custom source list kept")

print("\nALL PASSED" if not fails else f"\n{fails} FAILED")
sys.exit(1 if fails else 0)
