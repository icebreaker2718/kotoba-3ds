#!/usr/bin/env python3
"""
mine.py - turns the words you saved with Kotoba3DS into Anki cards.

It reads SD:/3ds/kotoba3ds/mined.tsv (over FTP from the ftpd homebrew app, or from the SD
card in a card reader), crops each word's screenshot to the top screen, optionally OCRs the
sentence with manga-ocr, and adds a note per word through AnkiConnect.

  python mine.py setup            create config.json, check the note type/deck, index dictionaries
  python mine.py                  import new words (Anki must be open)
  python mine.py --dry-run        show what would be added
  python mine.py --clear          after importing, remove imported words from the 3DS log

Needs: Anki with the AnkiConnect add-on (2055492159), `pip install pillow`.
Optional: `pip install manga-ocr` for sentence OCR.
"""
import argparse
import base64
import ftplib
import hashlib
import io
import json
import os
import re
import sys
import html
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import enrich  # noqa: E402
import yomitan  # noqa: E402
DEFAULT_CONFIG = {
    "source": {
        "_comment": "Use ftp_host (ftpd app on the 3DS) OR sd_root (path where the SD card is mounted).",
        "ftp_host": "192.168.1.50",
        "ftp_port": 5000,
        "sd_root": "",
    },
    "anki": {
        "_comment": "Field values are templates; see README for every {placeholder}.",
        "url": "http://127.0.0.1:8765",
        "deck": "Mining Deck",
        "note_type": "Lapis",
        "fields": {
            "Expression": "{expression}",
            "ExpressionFurigana": "{furigana}",
            "ExpressionReading": "{reading}",
            "ExpressionAudio": "{audio}",
            "SelectionText": "",
            "MainDefinition": "{definition_html}",
            "DefinitionPicture": "",
            "Sentence": "{sentence}",
            "SentenceFurigana": "",
            "SentenceAudio": "",
            "Picture": "{picture}",
            "Glossary": "{definition_html}",
            "Hint": "",
            "IsWordAndSentenceCard": "",
            "IsClickCard": "",
            "IsSentenceCard": "",
            "IsAudioCard": "",
            "PitchPosition": "{pitch_position}",
            "PitchCategories": "{pitch_categories}",
            "Frequency": "{frequency}",
            "FreqSort": "{freq_sort}",
            "MiscInfo": "{game} ({time})",
        },
        "tags": ["{game_tag}"],
        "allow_duplicates": False,
        "_comment_flag": "Cards whose word isn't in any of your dictionaries, or that got no word audio, get "
                         "these tags (\"\" for none) and this flag (1 = red, 0 = off), to fix in the Anki browser.",
        "missing_definition_tag": "no_definition",
        "missing_audio_tag": "no_audio",
        "flag_missing": 1,
    },
    "dictionaries": {
        "_comment": "Put Yomitan .zip dictionaries in the folder below and they are found automatically "
                    "(definitions, pitch accent, frequency). To choose explicitly, list paths in "
                    "glossary / pitch / frequency instead; for definitions the first match wins.",
        "folder": "dictionaries",
        "glossary": [],
        "pitch": [],
        "frequency": [],
    },
    "audio": {
        "_comment": "Tried in order until one has the word. local = Local Audio Server for Yomitan "
                    "(skipped when it isn't running), jpod101 / jpod101-alt = JapanesePod101, "
                    "lingualibre = native-speaker recordings on Wikimedia Commons, forvo = Forvo "
                    "(needs forvo_key from api.forvo.com). A URL or {\"name\": ..., \"url\": ...} "
                    "with {term} and {reading} also works.",
        "enabled": True,
        "sources": ["local", "jpod101", "jpod101-alt", "lingualibre", "forvo"],
        "forvo_key": "",
    },
    "ocr": {
        "_comment": "engine: manga-ocr or none. crop: [left, top, right, bottom] in top-screen pixels "
                    "(400x240); set it to your game's text box for much better OCR. "
                    "Per-game crops go in game_crops keyed by title id.",
        "engine": "manga-ocr",
        "crop": [0, 0, 400, 240],
        "game_crops": {},
    },
    "picture": {"format": "webp", "scale": 2},
    "known": {
        "_comment": "Words that already have Anki cards show in green on the 3DS (mine.py known, or "
                    "Select > Known words with ocr_server.py running). query: an Anki search for the "
                    "notes to take words from (\"\" = all notes). fields: the fields that hold the word, "
                    "first found wins ([] = the {expression} field of anki.fields, then the usual names).",
        "query": "",
        "fields": [],
    },
    "games": {"_comment": "title id -> game name for MiscInfo and the tag", "0004000000000000": "Example Game"},
}
KOTOBA_FIELDS = {
    "Word": "{expression}", "Reading": "{reading}", "Meaning": "{definition_html}",
    "Sentence": "{sentence}", "Picture": "{picture}", "Game": "{game}",
}
STATE_FILE = os.path.join(HERE, "imported.json")
REMOTE_DIR = "/3ds/kotoba3ds"
COLUMNS = ["time", "title_id", "expression", "reading", "definition", "screenshot", "query",
           "inflection", "entry", "sentence"]

NOTE_CSS = """.card { font-family: "Hiragino Kaku Gothic ProN", "Noto Sans JP", "Yu Gothic", sans-serif;
  font-size: 22px; text-align: center; color: #222; background: #fafafa; }
.word { font-size: 48px; margin: 12px 0; }
.sentence { font-size: 26px; margin: 10px auto; max-width: 720px; }
.sentence b { color: #c0392b; }
.reading { font-size: 28px; color: #555; }
.meaning { text-align: left; max-width: 640px; margin: 12px auto; font-size: 18px; }
.picture img { max-width: 100%; image-rendering: pixelated; border-radius: 6px; }
.game { font-size: 13px; color: #999; margin-top: 8px; }
.nightMode .card, .card.nightMode { color: #eee; background: #222; }
"""
NOTE_FRONT = '<div class="word">{{Word}}</div>\n<div class="sentence">{{Sentence}}</div>'
NOTE_BACK = ('{{FrontSide}}\n<hr id="answer">\n<div class="reading">{{Reading}}</div>\n'
             '<div class="meaning">{{Meaning}}</div>\n<div class="picture">{{Picture}}</div>\n'
             '<div class="game">{{Game}}</div>')


# ---------------------------------------------------------------------------
# config / state
# ---------------------------------------------------------------------------
def load_config(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        cfg = json.load(f)
    # older versions only had jpod101: give unchanged configs the newer fallbacks
    au = cfg.get("audio")
    if isinstance(au, dict) and au.get("sources") == ["jpod101"]:
        au["sources"] = list(DEFAULT_CONFIG["audio"]["sources"])
    # the flag used to mark sentences without the word (no longer checked): keep its value
    an = cfg.get("anki")
    if isinstance(an, dict) and "flag_missing_word" in an:
        an.setdefault("flag_missing", an["flag_missing_word"])
        for old in ("flag_missing_word", "missing_word_tag", "_comment_flag"):
            an.pop(old, None)
    # fill in keys added in newer versions
    for k, v in DEFAULT_CONFIG.items():
        if isinstance(v, dict):
            cfg.setdefault(k, {})
            for kk, vv in v.items():
                cfg[k].setdefault(kk, vv)
        else:
            cfg.setdefault(k, v)
    return cfg


def load_state():
    if os.path.exists(STATE_FILE):
        with open(STATE_FILE, encoding="utf-8") as f:
            return set(json.load(f))
    return set()


def save_state(done):
    with open(STATE_FILE, "w", encoding="utf-8") as f:
        json.dump(sorted(done), f, ensure_ascii=False, indent=0)


def row_key(row):
    return f"{row['time']}|{row['title_id']}|{row['expression']}"


# ---------------------------------------------------------------------------
# reading files from the 3DS
# ---------------------------------------------------------------------------
class Source:
    def read(self, rel):  # -> bytes or None
        raise NotImplementedError

    def write(self, rel, data):
        raise NotImplementedError

    def close(self):
        pass


class LocalSource(Source):
    def __init__(self, root):
        self.root = root

    def _p(self, rel):
        return os.path.join(self.root, *REMOTE_DIR.strip("/").split("/"), *rel.split("/"))

    def read(self, rel):
        try:
            with open(self._p(rel), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def write(self, rel, data):
        with open(self._p(rel), "wb") as f:
            f.write(data)


class FtpSource(Source):
    def __init__(self, host, port):
        self.ftp = ftplib.FTP()
        self.ftp.connect(host, int(port), timeout=15)
        self.ftp.login()  # ftpd accepts any login

    def read(self, rel):
        buf = io.BytesIO()
        try:
            self.ftp.retrbinary(f"RETR {REMOTE_DIR}/{rel}", buf.write)
        except ftplib.error_perm:
            return None
        return buf.getvalue()

    def write(self, rel, data):
        self.ftp.storbinary(f"STOR {REMOTE_DIR}/{rel}", io.BytesIO(data))

    def close(self):
        try:
            self.ftp.quit()
        except Exception:
            pass


def open_source(cfg, args):
    src = cfg["source"]
    if args.sd:
        return LocalSource(args.sd)
    if args.ftp:
        host, _, port = args.ftp.partition(":")
        return FtpSource(host, port or 5000)
    if src.get("sd_root"):
        return LocalSource(src["sd_root"])
    return FtpSource(src["ftp_host"], src.get("ftp_port", 5000))


def parse_log(data):
    rows = []
    for line in data.decode("utf-8", errors="replace").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        cols += [""] * (len(COLUMNS) - len(cols))
        rows.append(dict(zip(COLUMNS, cols)))
    return rows


# ---------------------------------------------------------------------------
# AnkiConnect
# ---------------------------------------------------------------------------
class AnkiError(Exception):
    pass


def anki(cfg, action, **params):
    req = json.dumps({"action": action, "version": 6, "params": params}).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(cfg["anki"]["url"], req), timeout=30) as r:
            resp = json.loads(r.read())
    except OSError as e:
        raise AnkiError(f"Can't reach AnkiConnect at {cfg['anki']['url']} - is Anki open "
                        f"with the AnkiConnect add-on installed? ({e})")
    if resp.get("error"):
        raise AnkiError(resp["error"])
    return resp.get("result")


# ---------------------------------------------------------------------------
# words that already have cards (green on the 3DS)
# ---------------------------------------------------------------------------
KNOWN_FIELD_NAMES = ["Expression", "Word", "Vocab", "Vocabulary", "Term", "Kanji", "Japanese", "Front"]
_JAPANESE = re.compile(r"[\u3040-\u30ff\u3400-\u9fff\uf900-\ufaff\u3005]")


def clean_known_word(value):
    """A card field -> the plain word: no HTML, no furigana (食[た]べる, <ruby>), no spaces."""
    v = re.sub(r"<rt>.*?</rt>|<rp>.*?</rp>", "", value, flags=re.S)
    v = re.sub(r"<[^>]*>", "", v)
    v = v.replace("&nbsp;", " ")
    v = re.sub(r"\[[^\]]*\]", "", v)
    v = re.sub(r"\s+", "", v)
    if not v or len(v) > 30 or not _JAPANESE.search(v):
        return ""
    return v


def known_words(cfg, log=print):
    """The words of the user's Anki cards, sorted by their UTF-8 bytes (as the 3DS searches them)."""
    k = cfg.get("known", {})
    query = k.get("query") or "deck:*"
    fields = list(k.get("fields") or [])
    if not fields:
        fields = [f for f, t in cfg["anki"]["fields"].items() if t.strip() == "{expression}"]
        fields += [f for f in KNOWN_FIELD_NAMES if f not in fields]
    ids = anki(cfg, "findNotes", query=query)
    words = set()
    for i in range(0, len(ids), 1000):
        for note in anki(cfg, "notesInfo", notes=ids[i:i + 1000]):
            have = note.get("fields", {})
            for f in fields:
                if f in have:
                    w = clean_known_word(have[f].get("value", ""))
                    if w:
                        words.add(w)
                    break
    log(f"{len(words)} words in {len(ids)} Anki notes.")
    return sorted(words, key=lambda w: w.encode("utf-8"))


def known_text(words):
    return "# Kotoba3DS: words that have Anki cards (green in lookups)\n" + "".join(w + "\n" for w in words)


def run_known(cfg, args):
    data = known_text(known_words(cfg)).encode("utf-8")
    try:
        src = open_source(cfg, args)
    except (OSError, ftplib.Error) as e:
        path = os.path.join(HERE, "known.txt")
        with open(path, "wb") as f:
            f.write(data)
        print(f"Couldn't reach the 3DS ({e}). Wrote {path}: copy it to SD:{REMOTE_DIR}/known.txt.")
        return 1
    try:
        src.write("known.txt", data)
    finally:
        src.close()
    print(f"Put the list on the 3DS ({REMOTE_DIR}/known.txt): those words show in green in lookups.")
    return 0


def setup_anki(cfg):
    a = cfg["anki"]
    fields = list(a["fields"].keys())
    if a["note_type"] not in anki(cfg, "modelNames"):
        if a["note_type"] == "Kotoba3DS":
            fields = list(KOTOBA_FIELDS)
            a["fields"] = dict(KOTOBA_FIELDS)
            anki(cfg, "createModel", modelName="Kotoba3DS", inOrderFields=fields, css=NOTE_CSS,
                 isCloze=False, cardTemplates=[{"Name": "Recognition", "Front": NOTE_FRONT, "Back": NOTE_BACK}])
            print('Created note type "Kotoba3DS".')
        elif a["note_type"] == "Lapis":
            sys.exit('Note type "Lapis" not found in Anki. Import the Lapis deck (.apkg) from '
                     "https://github.com/donkuri/lapis/releases first, then run setup again.")
        else:
            sys.exit(f'Note type "{a["note_type"]}" not found in Anki. Check anki.note_type in config.json.')
    else:
        have = anki(cfg, "modelFieldNames", modelName=a["note_type"])
        missing = [f for f in fields if f not in have]
        if missing:
            sys.exit(f'Note type "{a["note_type"]}" has no field(s) {missing}. '
                     f"Its fields are {have}; edit anki.fields in config.json.")
        print(f'Note type "{a["note_type"]}" found, all {len(fields)} fields match.')
    if a["deck"] in anki(cfg, "deckNames"):
        print(f'Deck "{a["deck"]}" found.')
    else:
        anki(cfg, "createDeck", deck=a["deck"])
        print(f'Deck "{a["deck"]}" did not exist - created it.')
    Enricher(cfg).describe()


# ---------------------------------------------------------------------------
# dictionaries, pitch, frequency, audio
# ---------------------------------------------------------------------------
def _resolve(path):
    p = os.path.expanduser(path)
    return p if os.path.isabs(p) else os.path.join(HERE, p)


class Enricher:
    def __init__(self, cfg, log=print):
        self.cfg, self.log = cfg, log
        d = cfg.get("dictionaries", {})
        self.glossaries, self.pitches, self.freqs = [], [], []
        found = self._scan(d.get("folder"))
        for kind, cls, dest in (("glossary", enrich.TermDictionary, self.glossaries),
                                ("pitch", enrich.PitchDictionary, self.pitches),
                                ("frequency", enrich.FrequencyDictionary, self.freqs)):
            for path in (d.get(kind) or found.get(kind, [])):
                full = _resolve(path)
                if not os.path.exists(full):
                    log(f"  ({kind} dictionary {path} not found - skipping)")
                    continue
                try:
                    dest.append(cls(full, log))
                except Exception as e:  # a broken zip shouldn't stop mining
                    log(f"  ({kind} dictionary {path} could not be read: {e})")

    def _scan(self, folder):
        """Sorts the Yomitan zips in `folder` by kind. Jitendex/JMdict go first for definitions."""
        out = {"glossary": [], "pitch": [], "frequency": []}
        if not folder:
            return out
        full = _resolve(folder)
        if not os.path.isdir(full):
            return out
        for name in sorted(os.listdir(full)):
            if not name.lower().endswith(".zip"):
                continue
            path = os.path.join(full, name)
            try:
                kind = yomitan.dictionary_kind(path)
            except Exception as e:
                self.log(f"  (could not read {name}: {e})")
                continue
            if kind == "unknown":
                self.log(f"  ({name} is not a Yomitan dictionary zip - if it contains other .zip files, "
                         "extract those into this folder)")
            key = {"terms": "glossary", "pitch": "pitch", "freq": "frequency"}.get(kind)
            if key:
                out[key].append(path)
        out["glossary"].sort(key=lambda p: (0 if re.search(r"jitendex|jmdict", os.path.basename(p), re.I) else 1, p))
        return out

    def describe(self):
        for label, items in (("Definitions", self.glossaries), ("Pitch accent", self.pitches),
                             ("Frequency", self.freqs)):
            print(f"{label}: " + (", ".join(x.title for x in items) if items else "none"))
        au = self.cfg.get("audio", {})
        names = []
        for s in au.get("sources", []):
            n = s if isinstance(s, str) else s.get("name") or s.get("type") or s.get("url", "?")
            if n == "forvo" and not (au.get("forvo_key") or (isinstance(s, dict) and s.get("key"))):
                n = "forvo (no key, skipped)"
            names.append(n)
        print("Audio: " + (", ".join(names) if au.get("enabled") else "off"))

    def definition_html(self, expression, reading, fallback_text):
        return self.lookup_definition(expression, reading, fallback_text)[0]

    def lookup_definition(self, expression, reading, fallback_text):
        """-> (definition HTML, whether one of the dictionaries had the word). Without, the 3DS's
        own definition (fallback_text) is used."""
        for d in self.glossaries:
            h = d.html(expression, reading)
            if h:
                return h, True
        return self._fallback_html(fallback_text), False

    @staticmethod
    def _fallback_html(fallback_text):
        if not fallback_text:
            return ""
        senses = [re.sub(r"^\d+\. ", "", line) for line in fallback_text.split("<br>")]
        lines = "".join("<li>" + html.escape(x) + "</li>" for x in senses)
        return ('<div style="text-align: left;" class="yomitan-glossary"><ol><li data-dictionary="JMdict">'
                f'<i>(JMdict)</i> <span><ol>{lines}</ol></span></li></ol></div>')

    def pitch(self, expression, reading, pos_codes):
        for d in self.pitches:
            p = d.positions(expression, reading)
            if p:
                return enrich.pitch_fields(p, reading, pos_codes)
        return "", ""

    def frequency(self, expression, reading):
        found = []
        for d in self.freqs:
            v = d.value(expression, reading)
            if v:
                found.append((d.title, v[0], v[1]))
        if not found:
            # like Yomitan: 9999999 sorts unknown words last; blank when no frequency list is set up
            return "", ("9999999" if self.freqs else "")
        items = "".join(f"<li>{html.escape(t)}: {html.escape(str(disp))}</li>" for t, _, disp in found)
        harmonic = round(len(found) / sum(1.0 / max(1, v) for _, v, _ in found))
        return f'<ul style="text-align: left;">{items}</ul>', str(harmonic)

    def audio(self, expression, reading):
        au = self.cfg.get("audio", {})
        if not au.get("enabled"):
            return None
        return enrich.fetch_audio(au.get("sources", []), expression, reading, self.log,
                                  forvo_key=au.get("forvo_key", ""))


def pos_codes_of(definition):
    codes = []
    for line in (definition or "").split("<br>"):
        m = re.match(r"\d+\. \(([^)]*)\)", line)
        if m:
            codes += [c.strip() for c in m.group(1).split(",")]
    return codes


# ---------------------------------------------------------------------------
# screenshots + OCR
# ---------------------------------------------------------------------------
def _decode_qoi(data):
    """A QOI image (the plugin's screenshots) -> RGB image, for Pillow versions that can't read QOI."""
    from PIL import Image
    w, h = int.from_bytes(data[4:8], "big"), int.from_bytes(data[8:12], "big")
    out = bytearray(w * h * 3)
    index = [(0, 0, 0, 0)] * 64
    r, g, b, a = 0, 0, 0, 255
    p, o, n = 14, 0, w * h * 3
    while o < n:
        op = data[p]; p += 1
        if op == 0xFE:
            r, g, b = data[p], data[p + 1], data[p + 2]; p += 3
        elif op == 0xFF:
            r, g, b, a = data[p], data[p + 1], data[p + 2], data[p + 3]; p += 4
        elif op >> 6 == 0:
            r, g, b, a = index[op]
        elif op >> 6 == 1:
            r = (r + ((op >> 4) & 3) - 2) & 255; g = (g + ((op >> 2) & 3) - 2) & 255; b = (b + (op & 3) - 2) & 255
        elif op >> 6 == 2:
            dg = (op & 63) - 32; x = data[p]; p += 1
            r = (r + dg + (x >> 4) - 8) & 255; g = (g + dg) & 255; b = (b + dg + (x & 15) - 8) & 255
        else:
            for _ in range((op & 63) + 1):
                if o >= n:
                    break
                out[o:o + 3] = bytes((r, g, b)); o += 3
            continue
        index[(r * 3 + g * 5 + b * 7 + a * 11) % 64] = (r, g, b, a)
        out[o:o + 3] = bytes((r, g, b)); o += 3
    return Image.frombytes("RGB", (w, h), bytes(out))


def open_screenshot(data):
    """A screenshot the plugin saved (QOI, or BMP from older versions) as an RGB image: the
    screen(s) it read: top 400x240, touchscreen 320x240, or both 400x480."""
    from PIL import Image
    try:
        return Image.open(io.BytesIO(data)).convert("RGB")
    except Exception:
        if data[:4] == b"qoif":
            return _decode_qoi(data)
        raise


def top_screen(bmp_bytes):
    return open_screenshot(bmp_bytes)


def encode_picture(img, cfg):
    from PIL import Image
    pc = cfg["picture"]
    scale = max(1, int(pc.get("scale", 1)))
    if scale > 1:
        img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    fmt = pc.get("format", "webp").lower()
    buf = io.BytesIO()
    if fmt == "png":
        img.save(buf, "PNG", optimize=True)
    elif fmt in ("jpg", "jpeg"):
        img.save(buf, "JPEG", quality=88)
        fmt = "jpg"
    else:
        img.save(buf, "WEBP", quality=85)
        fmt = "webp"
    return buf.getvalue(), fmt


_ocr_model = None


def ocr_sentence(img, cfg, title_id):
    oc = cfg["ocr"]
    if oc.get("engine", "none") != "manga-ocr":
        return ""
    global _ocr_model
    if _ocr_model is None:
        try:
            from manga_ocr import MangaOcr
        except ImportError:
            print("  (manga-ocr not installed: pip install manga-ocr - leaving Sentence empty)")
            oc["engine"] = "none"
            return ""
        print("  loading manga-ocr (first time takes a while)...")
        _ocr_model = MangaOcr()
    import ocrprep
    box = ocrprep.crop_for(cfg, title_id, img)
    return ocrprep.read_box(_ocr_model, img.crop(tuple(box))).strip()


KANJI = re.compile(r"[\u3400-\u9fff\u3005]")
HIRAGANA = re.compile(r"[\u3041-\u309f]")
STOP = set("のはをがもへねよや")


def sentence_from_device(row):
    text, tapped = row["sentence"], row["query"]
    if tapped and tapped in text:
        i = text.index(tapped)
        return text[:i] + "<b>" + tapped + "</b>" + text[i + len(tapped):]
    return highlight(text, row["expression"], row["reading"])


def highlight(sentence, expression, reading):
    """Bold the word in the OCR'd sentence. A conjugated word is found by the longest
    prefix it shares with the dictionary form (食べる -> 食べ in 食べなかった), and the
    hiragana that follow are bolded with it."""
    if not sentence:
        return sentence
    for word in (expression, reading):
        if not word:
            continue
        for k in range(len(word), 0, -1):
            stem = word[:k]
            if k == 1 and not KANJI.match(stem):
                break  # one kana on its own matches too much
            i = sentence.find(stem)
            if i < 0:
                continue
            j = i + k
            if k < len(word):  # conjugated form: take the inflection too
                while (j < len(sentence) and HIRAGANA.match(sentence[j]) and sentence[j] not in STOP
                       and j - i < len(word) + 6):
                    j += 1
            return sentence[:i] + "<b>" + sentence[i:j] + "</b>" + sentence[j:]
    return sentence


def flag_note(cfg, note_id, log=print):
    """Flags the new note's cards (AnkiConnect setSpecificValueOfCard)."""
    flag = int(cfg["anki"].get("flag_missing", 1) or 0)
    if not flag or not note_id:
        return
    try:
        for card in anki(cfg, "findCards", query=f"nid:{note_id}") or []:
            anki(cfg, "setSpecificValueOfCard", card=card, keys=["flags"], newValues=[str(flag)],
                 warning_check=True)
    except AnkiError as e:
        log(f"    (couldn't flag the card - update AnkiConnect? {e})")


# ---------------------------------------------------------------------------
# main import
# ---------------------------------------------------------------------------
class _Blank(dict):
    def __missing__(self, key):
        return "{" + key + "}"


def build_fields(cfg, row, sentence, picture_html, extra=None):
    game = cfg["games"].get(row["title_id"], row["title_id"])
    values = {
        "expression": row["expression"],
        "reading": row["reading"],
        "furigana": enrich.furigana_plain(row["expression"], row["reading"]),
        "definition": row["definition"],
        "definition_html": row["definition"],
        "sentence": sentence,
        "picture": picture_html,
        "audio": "",
        "pitch_position": "",
        "pitch_categories": "",
        "frequency": "",
        "freq_sort": "",
        "game": game,
        "game_tag": re.sub(r"\s+", "_", game.strip()),
        "time": row["time"],
        "inflection": row["inflection"],
        "query": row["query"],
        "title_id": row["title_id"],
    }
    values.update(extra or {})
    return {field: tmpl.format_map(_Blank(values)) for field, tmpl in cfg["anki"]["fields"].items()}


def build_tags(cfg, row):
    game = cfg["games"].get(row["title_id"], row["title_id"])
    values = _Blank({"game": game, "game_tag": re.sub(r"\s+", "_", game.strip()), "title_id": row["title_id"]})
    tags = [t.format_map(values).strip() for t in cfg["anki"].get("tags", [])]
    return [t for t in tags if t]


def enrich_row(enricher, row, log=print):
    """-> (extra field values, list of (filename, bytes) media, what was missing: a list of
    "definition" (no dictionary had the word) and "audio" (no recording found), for those set up)"""
    expr, reading = row["expression"], row["reading"]
    pos, cats = enricher.pitch(expr, reading, pos_codes_of(row["definition"]))
    freq_html, freq_sort = enricher.frequency(expr, reading)
    definition, found = enricher.lookup_definition(expr, reading, row["definition"])
    missing = [] if found or not enricher.glossaries else ["definition"]
    extra = {
        "definition_html": definition,
        "pitch_position": pos,
        "pitch_categories": cats,
        "frequency": freq_html,
        "freq_sort": freq_sort,
    }
    media = []
    got = enricher.audio(expr, reading)
    if got:
        data, ext, source = got
        name = f"kotoba3ds_{source}_{hashlib.md5((expr + '|' + reading).encode()).hexdigest()[:12]}.{ext}"
        media.append((name, data))
        extra["audio"] = f"[sound:{name}]"
    elif enricher.cfg.get("audio", {}).get("enabled") and enricher.cfg.get("audio", {}).get("sources"):
        missing.append("audio")
    return extra, media, missing


def make_card(cfg, enricher, row, img=None, dry_run=False, log=print):
    """Builds and adds one note. img: top-screen PIL image or None.
    -> "added" | "duplicate" | "failed" | "unreachable" | "dry"."""
    label = f"{row['expression']}【{row['reading']}】"
    sentence, picture_html, media = "", "", None
    if img is not None:
        if not row["sentence"]:
            sentence = highlight(ocr_sentence(img, cfg, row["title_id"]), row["expression"], row["reading"])
        pic, ext = encode_picture(img, cfg)
        stamp = re.sub(r"\D", "", row["time"])
        fname = f"kotoba3ds_{stamp}_{row['title_id'][-8:] or 'game'}.{ext}"
        media = (fname, pic)
        picture_html = f'<img src="{fname}">'
    if row["sentence"]:  # already read on the console ("Read screen"): bold what was tapped
        sentence = sentence_from_device(row)
    extra, more_media, missing = enrich_row(enricher, row, log)
    fields = build_fields(cfg, row, sentence, picture_html, extra)
    tags = build_tags(cfg, row)
    # tagged (and flagged) for fixing in Anki: no dictionary had the word, or no audio was found
    for what in missing:
        tag = cfg["anki"].get(f"missing_{what}_tag", "")
        if tag:
            tags.append(tag)
    note = "  ⚑ no " + " and no ".join("dictionary entry" if m == "definition" else "audio" for m in missing) if missing else ""
    if dry_run:
        filled = [k for k, v in fields.items() if v]
        log(f"  would add {label}: fills {', '.join(filled)}" + (note + " - would be flagged" if note else ""))
        return "dry"
    try:
        for name, blob in ([media] if media else []) + more_media:
            anki(cfg, "storeMediaFile", filename=name, data=base64.b64encode(blob).decode())
        note_id = anki(cfg, "addNote", note={
            "deckName": cfg["anki"]["deck"],
            "modelName": cfg["anki"]["note_type"],
            "fields": fields,
            "tags": tags,
            "options": {"allowDuplicate": bool(cfg["anki"].get("allow_duplicates")), "duplicateScope": "deck"},
        })
    except AnkiError as e:
        if "duplicate" in str(e).lower():
            log(f"  {label}: already in the deck, skipped")
            return "duplicate"
        log(f"  {label}: {e}")
        return "unreachable" if "Can't reach" in str(e) else "failed"
    if missing:
        flag_note(cfg, note_id, log)
    bits = [x for x, ok in (("audio", extra.get("audio")), ("pitch", extra.get("pitch_position")),
                            ("picture", picture_html)) if ok]
    log(f"  added {label}" + (f"  「{re.sub('<.*?>', '', sentence)}」" if sentence else "")
        + (f"  [{', '.join(bits)}]" if bits else "")
        + (note + " - flagged" if note else ""))
    return "added"


def mark_done(row):
    done = load_state()
    done.add(row_key(row))
    save_state(done)


def run_import(cfg, args):
    done = load_state()
    src = open_source(cfg, args)
    try:
        data = src.read("mined.tsv")
        if data is None:
            print("No mined.tsv on the 3DS yet - save a word in the plugin first.")
            return 0
        rows = parse_log(data)
        new = [r for r in rows if row_key(r) not in done]
        print(f"{len(rows)} saved words, {len(new)} new" +
              (f" ({len(rows) - len(new)} already made into cards)." if len(rows) != len(new) else "."))
        enricher = Enricher(cfg) if new else None
        added = skipped = failed = 0
        for row in new:
            img = None
            if row["screenshot"]:
                bmp = src.read(f"shots/{row['screenshot']}")
                if bmp is None:
                    print(f"  {row['expression']}: screenshot {row['screenshot']} not found")
                else:
                    img = top_screen(bmp)
            result = make_card(cfg, enricher, row, img, args.dry_run)
            if result == "dry":
                continue
            if result in ("failed", "unreachable"):
                failed += 1
                if result == "unreachable":
                    break
                continue
            added += result == "added"
            skipped += result == "duplicate"
            done.add(row_key(row))
            save_state(done)
        if not args.dry_run:
            print(f"Done: {added} added, {skipped} duplicates, {failed} failed.")
            if args.clear and failed == 0:
                keep = [l for l in data.decode("utf-8").splitlines()
                        if l.startswith("#") or (l.strip() and row_key(parse_log(l.encode())[0]) not in done)]
                src.write("mined.tsv", ("\n".join(keep) + "\n").encode())
                print("Cleared imported words from the 3DS log.")
        return 1 if failed else 0
    finally:
        src.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="import", choices=["import", "setup", "known"],
                    help="import: make cards from the saved words (default); setup: check Anki; "
                         "known: put the words of your Anki cards on the 3DS (green in lookups)")
    ap.add_argument("--config", default=os.path.join(HERE, "config.json"))
    ap.add_argument("--ftp", help="HOST[:PORT] of ftpd on the 3DS (overrides config)")
    ap.add_argument("--sd", help="path where the SD card is mounted (overrides config)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-ocr", action="store_true")
    ap.add_argument("--clear", action="store_true", help="remove imported words from the 3DS log afterwards")
    args = ap.parse_args()

    cfg = load_config(args.config)
    if cfg is None:
        with open(args.config, "w", encoding="utf-8") as f:
            json.dump(DEFAULT_CONFIG, f, ensure_ascii=False, indent=2)
        print(f"Wrote {args.config} - put your 3DS's IP address (shown in ftpd) in it.")
        if args.command != "setup":
            return 0
        cfg = load_config(args.config)
    if args.no_ocr:
        cfg["ocr"]["engine"] = "none"
    try:
        if args.command == "setup":
            setup_anki(cfg)
            return 0
        if args.command == "known":
            return run_known(cfg, args)
        return run_import(cfg, args)
    except AnkiError as e:
        print(e)
        return 1
    except (OSError, ftplib.Error) as e:
        print(f"Could not read from the 3DS: {e}\n"
              "Is ftpd running on the 3DS, and is the IP/port in config.json right?")
        return 1


if __name__ == "__main__":
    sys.exit(main())
