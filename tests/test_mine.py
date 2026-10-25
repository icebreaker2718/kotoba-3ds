"""End-to-end test of tools/mine.py with a fake 3DS (FTP server like ftpd), a fake AnkiConnect
that has the Lapis note type, fake audio servers (JapanesePod101-style and Local-Audio-Server
style) and small Yomitan dictionaries. manga-ocr is stubbed.

    pip install pyftpdlib pillow
    python3 tests/test_mine.py [path/to/jitendex-yomitan.zip]

With a Jitendex zip, real entries for the test words are copied into the test dictionary.
"""
import base64, hashlib, io, json, os, re, sys, tempfile, threading, types, zipfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from PIL import Image, ImageDraw
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer

import enrich
import mine

fails = 0
def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    fails += 0 if cond else 1

LAPIS_FIELDS = ["Expression", "ExpressionFurigana", "ExpressionReading", "ExpressionAudio", "SelectionText",
                "MainDefinition", "DefinitionPicture", "Sentence", "SentenceFurigana", "SentenceAudio", "Picture",
                "Glossary", "Hint", "IsWordAndSentenceCard", "IsClickCard", "IsSentenceCard", "IsAudioCard",
                "PitchPosition", "PitchCategories", "Frequency", "FreqSort", "MiscInfo"]

work = tempfile.mkdtemp()
enrich.CACHE_DIR = os.path.join(work, "cache")

# ---- Yomitan dictionaries ----
def make_zip(path, index, files):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("index.json", json.dumps(index))
        for name, data in files.items():
            z.writestr(name, data if isinstance(data, str) else json.dumps(data, ensure_ascii=False))

jitendex = sys.argv[1] if len(sys.argv) > 1 else None
words = {"食べる", "静か", "あくどい"}
if jitendex:
    import yomitan
    rows = [r for r in yomitan.iter_terms(jitendex) if r[0] in words or r[0] in ("惡どい", "悪どい")]
    styles = yomitan.read_styles(jitendex)
    title = yomitan.read_index(jitendex)["title"]
else:  # minimal Jitendex-shaped rows
    sc = lambda g: [{"type": "structured-content", "content": {"tag": "ol", "content": [
        {"tag": "li", "data": {"content": "sense"}, "content": {"tag": "ul", "data": {"content": "glossary"},
                                                                 "content": {"tag": "li", "content": g}}}]}}]
    rows = [["食べる", "たべる", "★", "v1", 200, sc("to eat"), 1358280, ""],
            ["静か", "しずか", "★", "", 200, sc("quiet"), 1, ""]]
    styles, title = "span[data-sc-content=\"x\"] { color: red; }", "Jitendex.org [test]"
os.makedirs(os.path.join(mine.HERE, "dictionaries"), exist_ok=True)
gz = os.path.join(work, "jitendex-test.zip")
make_zip(gz, {"title": title, "format": 3, "sequenced": True},
         {"term_bank_1.json": rows, "styles.css": styles})
pz = os.path.join(work, "pitch.zip")
make_zip(pz, {"title": "Kanjium", "format": 3}, {"term_meta_bank_1.json": [
    ["食べる", "pitch", {"reading": "たべる", "pitches": [{"position": 2}]}],
    ["静か", "pitch", {"reading": "しずか", "pitches": [{"position": 1}]}]]})
fz = os.path.join(work, "jpdb.zip")
make_zip(fz, {"title": "JPDB", "format": 3}, {"term_meta_bank_1.json": [
    ["食べる", "freq", {"reading": "たべる", "frequency": {"value": 350, "displayValue": "350"}}],
    ["静か", "freq", {"reading": "しずか", "frequency": 2400}]]})

# ---- fake SD card ----
sd = tempfile.mkdtemp()
kdir = os.path.join(sd, "3ds", "kotoba3ds")
os.makedirs(os.path.join(kdir, "shots"))
img = Image.new("RGB", (400, 480), (0, 0, 80))
ImageDraw.Draw(img).rectangle((0, 240, 399, 479), fill=(200, 0, 0))
img.save(os.path.join(kdir, "shots", "shot - 000.bmp"))
log = ("#time\ttitle_id\texpression\treading\tdefinition\tscreenshot\tquery\tinflection\tentry\n"
       "2026-10-19 08:30:00\t0004000000123400\t食べる\tたべる\t1. (v1,vt) to eat<br>2. to live on\tshot - 000.bmp\ttaberarenakatta\tpassive\t11866\n"
       "2026-10-19 08:31:10\t0004000000123400\t静か\tしずか\t1. (adj-na) quiet\t\tshizukana\tna-adjective\t2000\n"
       "2026-10-19 08:32:00\t0004000000123400\t猫\tねこ\t1. (n) cat\t\tneko\t\t3000\n"
       "2026-10-19 08:33:00\t0004000000123400\t宝箱\tたからばこ\t1. (n) treasure chest\tshot - 000.bmp\t宝箱\t\t4000\tこの先に宝箱があるらしいぞ。\n"
       "2026-10-19 08:34:00\t0004000000123400\t走る\tはしる\t1. (v5r) to run\tshot - 000.bmp\thashiru\t\t5000\n")
with open(os.path.join(kdir, "mined.tsv"), "w", encoding="utf-8") as f:
    f.write(log)
auth = DummyAuthorizer(); auth.add_anonymous(sd, perm="elradfmw")
FTPHandler.authorizer = auth
ftp = FTPServer(("127.0.0.1", 0), FTPHandler)
threading.Thread(target=ftp.serve_forever, kwargs={"timeout": 0.5}, daemon=True).start()

# ---- fake audio servers ----
MP3 = b"ID3" + b"\x00" * 3000
MISSING = b"ID3missing" + b"\x01" * 3000
enrich.JPOD_MISSING_MD5 = hashlib.md5(MISSING).hexdigest()
audio_reqs = []
class Audio(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        audio_reqs.append((u.path, q))
        if u.path == "/jpod":  # JapanesePod101: "missing" clip instead of 404
            body, ct = (MP3, "audio/mpeg") if q.get("kana") == ["たべる"] else (MISSING, "audio/mpeg")
        elif u.path == "/local":  # Local Audio Server for Yomitan: JSON list of sources
            if q.get("term") == ["静か"]:
                body, ct = json.dumps({"type": "audioSourceList", "audioSources": [
                    {"name": "nhk16", "url": "/file/shizuka.opus"}]}).encode(), "application/json"
            else:
                body, ct = json.dumps({"type": "audioSourceList", "audioSources": []}).encode(), "application/json"
        elif u.path == "/file/shizuka.opus":
            body, ct = b"OggS" + b"\x00" * 2000, "audio/ogg"
        else:
            self.send_response(404); self.end_headers(); return
        self.send_response(200); self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
audio_srv = HTTPServer(("127.0.0.1", 0), Audio)
threading.Thread(target=audio_srv.serve_forever, daemon=True).start()
A = f"http://127.0.0.1:{audio_srv.server_port}"

# ---- fake AnkiConnect with Lapis ----
calls, notes, media, flags = [], [], {}, {}
other_notes = [{"fields": {"Front": "飲[の]み 物[もの]", "Back": "drink"}},   # an older deck's cards
               {"fields": {"Front": "dog", "Back": "犬"}},
               {"fields": {"Vocab": "<b>猫</b>&nbsp;", "Front": "x"}}]
class Anki(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        a, p = req["action"], req.get("params", {})
        calls.append(a)
        res, err = None, None
        if a == "modelNames": res = ["Basic", "Lapis"]
        elif a == "modelFieldNames": res = LAPIS_FIELDS if p["modelName"] == "Lapis" else []
        elif a == "deckNames": res = ["Default", "Mining Deck"]
        elif a == "createDeck": res = 1
        elif a == "storeMediaFile": media[p["filename"]] = base64.b64decode(p["data"]); res = p["filename"]
        elif a == "findCards": res = [int(p["query"].split(":")[1]) * 10]
        elif a == "setSpecificValueOfCard": flags[p["card"]] = (p["keys"], p["newValues"], p.get("warning_check")); res = [True]
        elif a == "findNotes": res = list(range(1, len(notes) + len(other_notes) + 1))
        elif a == "notesInfo":
            alln = notes + other_notes
            res = [{"noteId": i, "fields": {k: {"value": v, "order": 0} for k, v in alln[i - 1]["fields"].items()}}
                   for i in p["notes"]]
        elif a == "addNote":
            if set(p["note"]["fields"]) - set(LAPIS_FIELDS): err = "unknown field"
            elif any(n["fields"]["Expression"] == p["note"]["fields"]["Expression"] for n in notes):
                err = "cannot create note because it is a duplicate"
            else: notes.append(p["note"]); res = len(notes)
        else: err = "unsupported action"
        body = json.dumps({"result": res, "error": err}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
http = HTTPServer(("127.0.0.1", 0), Anki)
threading.Thread(target=http.serve_forever, daemon=True).start()

class FakeMangaOcr:
    def __call__(self, im): return "もう何も食べられなかった。"
sys.modules["manga_ocr"] = types.SimpleNamespace(MangaOcr=FakeMangaOcr)

# ---- config ----
mine.STATE_FILE = os.path.join(work, "imported.json")
cfgp = os.path.join(work, "config.json")
cfg = json.loads(json.dumps(mine.DEFAULT_CONFIG))
cfg["source"]["ftp_host"], cfg["source"]["ftp_port"] = "127.0.0.1", ftp.socket.getsockname()[1]
cfg["anki"]["url"] = f"http://127.0.0.1:{http.server_port}"
cfg["ocr"]["crop"] = [0, 160, 400, 240]
cfg["games"]["0004000000123400"] = "Test Game"
cfg["dictionaries"] = {"glossary": [gz], "pitch": [pz], "frequency": [fz]}
cfg["audio"]["sources"] = [{"name": "local", "url": A + "/local?term={term}&reading={reading}"},
                           {"type": "jpod101", "url": A + "/jpod"}]
json.dump(cfg, open(cfgp, "w"))

def run(*argv):
    sys.argv = ["mine.py", "--config", cfgp, *argv]
    return mine.main()

check(run("setup") == 0, "setup finds Lapis + Mining Deck and indexes dictionaries")
check("createDeck" not in calls, "existing deck not recreated")
check(len(os.listdir(enrich.CACHE_DIR)) == 3, "three dictionary indexes cached")
check(run() == 0, "import")
check(len(notes) == 5, f"5 notes added (got {len(notes)})")
by = {n["fields"]["Expression"]: n for n in notes}
t, s, k = by["食べる"]["fields"], by["静か"]["fields"], by["猫"]["fields"]
n0 = by["食べる"]
check(n0["deckName"] == "Mining Deck" and n0["modelName"] == "Lapis", "deck + note type")
check(set(t) == set(LAPIS_FIELDS), "all Lapis fields present")
check(t["ExpressionFurigana"] == "食[た]べる", "furigana " + t["ExpressionFurigana"])
check(t["ExpressionReading"] == "たべる", "reading")
check(t["MainDefinition"].startswith('<div style="text-align: left;" class="yomitan-glossary"><ol><li data-dictionary="Jitendex'),
      "MainDefinition is Yomitan-style Jitendex HTML")
check("to eat" in t["MainDefinition"] and "data-sc-content=" in t["MainDefinition"], "definition content + Jitendex markup")
check("<style>" in t["MainDefinition"] and '.yomitan-glossary [data-dictionary="' in t["MainDefinition"], "dictionary CSS included, scoped")
check(t["Glossary"] == t["MainDefinition"], "Glossary field filled")
check(re.search(r"\[</span><span>2</span><span>\]", t["PitchPosition"]) is not None, "pitch position [2]")
check(t["PitchCategories"] == "kifuku", "verb pitch category kifuku: " + t["PitchCategories"])
check(s["PitchCategories"] == "atamadaka", "静か [1] -> atamadaka: " + s["PitchCategories"])
check("JPDB: 350" in t["Frequency"] and t["FreqSort"] == "350", "frequency + FreqSort")
check(k["FreqSort"] == "9999999" and k["Frequency"] == "", "no frequency -> 9999999 like Yomitan")
check(t["ExpressionAudio"].startswith("[sound:kotoba3ds_jpod101_") and t["ExpressionAudio"].endswith(".mp3]"),
      "jpod101 audio for 食べる " + t["ExpressionAudio"])
check(s["ExpressionAudio"].startswith("[sound:kotoba3ds_local_") and s["ExpressionAudio"].endswith(".opus]"),
      "local audio server for 静か " + s["ExpressionAudio"])
check(k["ExpressionAudio"] == "", "jpod 'missing' clip rejected for 猫")
check(any(q.get("kanji") == ["食べる"] and q.get("kana") == ["たべる"] for p, q in audio_reqs if p == "/jpod"),
      "jpod request has kanji + kana")
check(t["Sentence"] == "もう何も<b>食べられなかった</b>。", "sentence " + t["Sentence"])
check(t["Picture"].startswith('<img src="kotoba3ds_') and s["Picture"] == "", "picture")
tb = by["宝箱"]["fields"]
check(tb["Sentence"] == "この先に<b>宝箱</b>があるらしいぞ。", "sentence read on the console is used, tapped word bold: " + tb["Sentence"])
check(tb["Picture"].startswith("<img"), "console-OCR word still gets the picture")
check(t["MiscInfo"] == "Test Game (2026-10-19 08:30:00)", "MiscInfo " + t["MiscInfo"])
check(n0["tags"] == ["Test_Game"], f"tag from game name {n0['tags']}")
check(all(t[f] == "" for f in ("IsWordAndSentenceCard", "IsClickCard", "IsSentenceCard", "IsAudioCard",
                               "SentenceFurigana", "SelectionText")), "card-type flags left empty")
check("1. (n) cat" not in k["MainDefinition"] and "cat" in k["MainDefinition"],
      "word missing from Jitendex falls back to the 3DS definition")
# words no dictionary has, or without audio: tagged and flagged, the others not
for w in ("食べる", "静か"):
    check(not {"no_definition", "no_audio"} & set(by[w]["tags"]), f"{w}: found in the dictionary, with audio: not tagged {by[w]['tags']}")
check({"no_definition", "no_audio"} <= set(by["猫"]["tags"]), f"猫: no entry, no audio -> both tags {by['猫']['tags']}")
nid = notes.index(by["猫"]) + 1
check(flags.get(nid * 10) == (["flags"], ["1"], True), f"...and its card gets flag 1 (red) {flags}")
check(all(notes[c // 10 - 1]["fields"]["Expression"] not in ("食べる", "静か") for c in flags), "complete cards aren't flagged")
# an older config: its flag setting is kept under the new name, the old keys dropped
oldp = os.path.join(work, "old_config.json")
old = json.loads(json.dumps(cfg))
old["anki"]["flag_missing_word"] = 3
old["anki"]["missing_word_tag"] = "check_sentence"
for k in ("flag_missing", "missing_definition_tag", "missing_audio_tag"):
    old["anki"].pop(k, None)
json.dump(old, open(oldp, "w"))
oc = mine.load_config(oldp)
check(oc["anki"]["flag_missing"] == 3 and "missing_word_tag" not in oc["anki"] and oc["anki"]["missing_audio_tag"] == "no_audio",
      "older config: flag kept, sentence check gone, new tags added")
aud = [v for n, v in media.items() if n.endswith(".mp3")]
check(aud and aud[0] == MP3, "audio bytes stored in Anki media")

calls.clear()
check(run() == 0 and "addNote" not in calls, "second run imports nothing new")

# auto-detection: zips dropped in a folder, no explicit lists
dfolder = os.path.join(work, "dicts"); os.makedirs(dfolder)
for src in (gz, pz, fz):
    os.link(src, os.path.join(dfolder, os.path.basename(src)))
cfg["dictionaries"] = {"folder": dfolder, "glossary": [], "pitch": [], "frequency": []}
json.dump(cfg, open(cfgp, "w"))
enr = mine.Enricher(mine.load_config(cfgp), log=lambda *a: None)
check([len(enr.glossaries), len(enr.pitches), len(enr.freqs)] == [1, 1, 1], "dictionaries auto-detected by kind")

# the words of the Anki cards, for the 3DS's green words
check(mine.clean_known_word("<ruby>食<rt>た</rt></ruby>べる") == "食べる" and mine.clean_known_word("dog") == "",
      "card fields cleaned to the word")
words = mine.known_words(mine.load_config(cfgp), log=lambda *a: None)
check("食べる" in words and "飲み物" in words and "dog" not in words, f"words of all notes: {words}")
check("猫" in words and "x" not in words, "a note's word field found by its usual name (Vocab before Front)")
check(words == sorted(words, key=lambda w: w.encode()), "sorted by bytes, as the 3DS searches")
check(run("known") == 0 and open(os.path.join(kdir, "known.txt"), encoding="utf-8").read().splitlines()[1:] == words,
      "mine.py known puts known.txt on the 3DS")

# dry run with dictionaries missing doesn't crash
cfg["dictionaries"] = {"glossary": ["nope.zip"], "pitch": [], "frequency": []}
cfg["audio"]["enabled"] = False
json.dump(cfg, open(cfgp, "w"))
os.remove(mine.STATE_FILE)
check(run("--dry-run") == 0, "dry run with a missing dictionary")

print("\nALL PASSED" if not fails else f"\n{fails} FAILED")
if os.path.isdir(os.path.join(mine.HERE, "dictionaries")) and not os.listdir(os.path.join(mine.HERE, "dictionaries")):
    os.rmdir(os.path.join(mine.HERE, "dictionaries"))
sys.exit(1 if fails else 0)
