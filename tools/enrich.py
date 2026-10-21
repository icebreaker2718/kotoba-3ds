"""
enrich.py - everything mine.py adds to a card beyond what the 3DS saved:
full dictionary definitions (as Yomitan-style HTML), pitch accent, frequencies, word audio
and furigana.

Dictionaries are Yomitan .zip files. The first time one is used it is indexed into a small
SQLite file in tools/cache/ so later runs start instantly.
"""
import hashlib
import json
import urllib.error
import html
import socket
import os
import re
import sqlite3
import urllib.parse
import urllib.request

import yomitan

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE_DIR = os.path.join(HERE, "cache")
INDEX_VERSION = 2


def _to_hira(s):
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in s or "")


# ---------------------------------------------------------------------------
# index cache
# ---------------------------------------------------------------------------
def _cache_path(zip_path):
    st = os.stat(zip_path)
    tag = hashlib.md5(f"{os.path.abspath(zip_path)}|{st.st_size}|{int(st.st_mtime)}|{INDEX_VERSION}"
                      .encode()).hexdigest()[:10]
    base = re.sub(r"[^\w.-]+", "_", os.path.splitext(os.path.basename(zip_path))[0])
    return os.path.join(CACHE_DIR, f"{base}-{tag}.sqlite")


def _open_index(zip_path, build, log=print):
    os.makedirs(CACHE_DIR, exist_ok=True)
    path = _cache_path(zip_path)
    if os.path.exists(path):
        return sqlite3.connect(path)
    log(f"  indexing {os.path.basename(zip_path)} (first time only, may take a minute)...")
    tmp = path + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    db = sqlite3.connect(tmp)
    build(db)
    db.commit()
    db.close()
    os.replace(tmp, path)
    return sqlite3.connect(path)


class TermDictionary:
    """Yomitan term dictionary: definitions as HTML."""

    def __init__(self, zip_path, log=print):
        self.path = zip_path
        idx = yomitan.read_index(zip_path)
        self.title = idx.get("title") or os.path.splitext(os.path.basename(zip_path))[0]
        self.css = yomitan.read_styles(zip_path)
        self.db = _open_index(zip_path, self._build, log)

    def _build(self, db):
        db.execute("create table t (expr text, reading text, seq integer, tags text, redirect integer, gloss text)")
        rows = []
        for expr, reading, def_tags, rules, score, glossary, seq, term_tags in yomitan.iter_terms(self.path):
            rows.append((expr, _to_hira(reading or ""), seq if isinstance(seq, int) else 0, def_tags or "",
                         1 if yomitan.is_redirect(glossary) else 0, json.dumps(glossary, ensure_ascii=False)))
            if len(rows) >= 20000:
                db.executemany("insert into t values (?,?,?,?,?,?)", rows)
                rows = []
        db.executemany("insert into t values (?,?,?,?,?,?)", rows)
        db.execute("create index t_expr on t(expr)")
        db.execute("create index t_seq on t(seq)")

    def _rows(self, where, args):
        return self.db.execute(f"select expr, reading, seq, tags, redirect, gloss from t where {where}", args).fetchall()

    def lookup(self, expression, reading):
        """Best entry for the word -> (def_tags, glossary) or None."""
        r = _to_hira(reading)
        rows = self._rows("expr = ?", (expression,))
        exact = [x for x in rows if x[1] == r or (not x[1] and r == _to_hira(expression))] or rows
        if not exact and r:
            exact = self._rows("expr = ?", (reading,))
        if not exact:
            return None
        best = sorted(exact, key=lambda x: (x[4], -len(x[5])))[0]
        if best[4] and best[2]:  # redirect: follow to the real entry
            target = [x for x in self._rows("seq = ?", (abs(best[2]),)) if not x[4]]
            if target:
                best = max(target, key=lambda x: len(x[5]))
        return best[3], json.loads(best[5])

    def html(self, expression, reading):
        hit = self.lookup(expression, reading)
        if not hit:
            return ""
        tags, glossary = hit
        return yomitan.glossary_html(glossary, self.title, tags, self.css)


class PitchDictionary:
    def __init__(self, zip_path, log=print):
        self.path = zip_path
        self.title = yomitan.read_index(zip_path).get("title", os.path.basename(zip_path))
        self.db = _open_index(zip_path, self._build, log)

    def _build(self, db):
        db.execute("create table p (expr text, reading text, positions text)")
        rows = []
        for expr, mode, data in yomitan.iter_meta(self.path):
            if mode != "pitch" or not isinstance(data, dict):
                continue
            pos = []
            for p in data.get("pitches", []):
                v = p.get("position")
                if isinstance(v, int):
                    pos.append(v)
                elif isinstance(v, str) and re.fullmatch(r"[HL]+", v):  # "LHHL" pattern form
                    drop = next((i for i in range(1, len(v)) if v[i - 1] == "H" and v[i] == "L"), None)
                    pos.append(0 if drop is None else drop)
            if pos:
                rows.append((expr, _to_hira(data.get("reading", expr)), json.dumps(pos)))
        db.executemany("insert into p values (?,?,?)", rows)
        db.execute("create index p_expr on p(expr)")

    def positions(self, expression, reading):
        """All pitch positions for this word and reading (rows for one word can be split)."""
        r = _to_hira(reading or expression)
        out = []
        for e in (expression, reading):
            if not e:
                continue
            for expr, rd, pos in self.db.execute("select * from p where expr = ?", (e,)):
                if rd == r:
                    out += [p for p in json.loads(pos) if p not in out]
            if out:
                break
        return out


class FrequencyDictionary:
    def __init__(self, zip_path, log=print):
        self.path = zip_path
        self.title = yomitan.read_index(zip_path).get("title", os.path.splitext(os.path.basename(zip_path))[0])
        self.db = _open_index(zip_path, self._build, log)

    def _build(self, db):
        db.execute("create table f (expr text, reading text, value integer, display text)")
        rows = []
        for expr, mode, data in yomitan.iter_meta(self.path):
            if mode != "freq":
                continue
            reading = ""
            v = data
            if isinstance(v, dict) and "frequency" in v:
                reading = v.get("reading", "")
                v = v["frequency"]
            display = None
            if isinstance(v, dict):
                display = v.get("displayValue")
                v = v.get("value")
            if isinstance(v, (int, float)):
                rows.append((expr, _to_hira(reading), int(v), display or str(int(v))))
        db.executemany("insert into f values (?,?,?,?)", rows)
        db.execute("create index f_expr on f(expr)")

    def value(self, expression, reading):
        r = _to_hira(reading)
        rows = self.db.execute("select reading, value, display from f where expr = ?", (expression,)).fetchall()
        rows = [x for x in rows if not x[0] or x[0] == r] or []
        if not rows:
            return None
        best = min(rows, key=lambda x: x[1])
        return best[1], best[2]


# ---------------------------------------------------------------------------
# pitch helpers (same categories as Yomitan's {pitch-accent-categories})
# ---------------------------------------------------------------------------
_SMALL = set("ゃゅょぁぃぅぇぉゎャュョァィゥェォヮ")


def mora_count(kana):
    return sum(1 for c in kana if c not in _SMALL)


def pitch_category(position, reading, pos_codes):
    if position == 0:
        return "heiban"
    verb_or_adj = any(c.startswith(("v1", "v5", "vk", "vs", "vz", "adj-i")) for c in pos_codes)
    if verb_or_adj:
        return "kifuku"
    if position == 1:
        return "atamadaka"
    return "odaka" if position == mora_count(reading) else "nakadaka"


def pitch_fields(positions, reading, pos_codes):
    if not positions:
        return "", ""
    uniq = list(dict.fromkeys(positions))
    if len(uniq) == 1:
        pos_html = f'<span style="display:inline;"><span>[</span><span>{uniq[0]}</span><span>]</span></span>'
    else:
        pos_html = "<ol>" + "".join(
            f'<li><span style="display:inline;"><span>[</span><span>{p}</span><span>]</span></span></li>'
            for p in uniq) + "</ol>"
    cats = list(dict.fromkeys(pitch_category(p, reading, pos_codes) for p in uniq))
    return pos_html, ",".join(cats)


# ---------------------------------------------------------------------------
# furigana (Anki "plain" format: 食[た]べる, お 金[かね])
# ---------------------------------------------------------------------------
_KANJI = r"[㐀-鿿豈-﫿々ヶ]"


def furigana_plain(expression, reading):
    if not reading or expression == reading or not re.search(_KANJI, expression):
        return expression
    parts = re.findall(rf"{_KANJI}+|[^㐀-鿿豈-﫿々ヶ]+", expression)
    pattern = "".join("(.+?)" if re.match(_KANJI, p) else f"({re.escape(_to_hira(p))})" for p in parts)
    m = re.fullmatch(pattern, _to_hira(reading))
    if not m:
        return f"{expression}[{reading}]"
    out = ""
    for i, p in enumerate(parts):
        if re.match(_KANJI, p):
            out += (" " if out else "") + f"{p}[{m.group(i + 1)}]"
        else:
            out += p
    return out


# ---------------------------------------------------------------------------
# audio
# ---------------------------------------------------------------------------
JPOD_URL = "https://assets.languagepod101.com/dictionary/japanese/audiomp3.php"
# JapanesePod101 returns this "the audio for this clip is currently not available" clip
# instead of a 404 for unknown words.
JPOD_MISSING_MD5 = "7e2c2f954ef6051373ba916f000168dc"
JPOD_MISSING_SHA256 = "ae6398b5a27bc8c0a771df6c907ade794be15518174773c58c7c7ddd17098906"
_UA = {"User-Agent": "Mozilla/5.0 (kotoba3ds mine.py)"}


def _get(url, timeout=12):
    req = urllib.request.Request(url, headers=_UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read(), r.headers.get("Content-Type", "")


def _is_kana(s):
    return bool(s) and all(0x3040 <= ord(c) <= 0x30FF for c in s)


def jpod_audio(expression, reading, base_url=JPOD_URL):
    params = {}
    if not (_is_kana(expression) and expression == reading):
        params["kanji"] = expression
    if reading:
        params["kana"] = reading
    data, ctype = _get(base_url + "?" + urllib.parse.urlencode(params))
    if len(data) < 1000 or "html" in ctype:
        return None
    if hashlib.md5(data).hexdigest() == JPOD_MISSING_MD5 or hashlib.sha256(data).hexdigest() == JPOD_MISSING_SHA256:
        return None
    return data, "mp3"


_AUDIO_EXTS = ("mp3", "ogg", "opus", "m4a", "aac", "wav", "flac")


def _ext_for(url, ctype):
    path = url.lower().split("?")[0]
    for ext in _AUDIO_EXTS:
        if path.endswith("." + ext):
            return ext
    for ext in _AUDIO_EXTS:
        if f"/{ext}" in (ctype or "") or (ext == "mp3" and "mpeg" in (ctype or "")):
            return ext
    return "mp3"


def custom_audio(template, expression, reading):
    """Custom URL like Yomitan's: either a direct audio file, or a JSON audio source list
    (the format used by "Local Audio Server for Yomitan")."""
    url = template.replace("{term}", urllib.parse.quote(expression)) \
                  .replace("{expression}", urllib.parse.quote(expression)) \
                  .replace("{reading}", urllib.parse.quote(reading or expression))
    data, ctype = _get(url)
    if "json" in ctype or data[:1] in (b"{", b"["):
        info = json.loads(data.decode("utf-8"))
        sources = info.get("audioSources", []) if isinstance(info, dict) else []
        for src in sources:
            u = src.get("url")
            if not u:
                continue
            u = urllib.parse.urljoin(url, u)
            try:
                d, ct = _get(u)
            except OSError:
                continue
            if d:
                return d, _ext_for(u, ct)
        return None
    if not data:
        return None
    return data, _ext_for(url, ctype)


JPOD_ALT_URL = "https://www.japanesepod101.com/learningcenter/reference/dictionary_post"
LOCAL_AUDIO_URL = "http://127.0.0.1:5050/?term={term}&reading={reading}"
COMMONS_API = "https://commons.wikimedia.org/w/api.php"
FORVO_API = "https://apifree.forvo.com"


def _good_jpod_clip(data, ctype):
    if not data or len(data) < 1000 or "html" in (ctype or ""):
        return False
    return not (hashlib.md5(data).hexdigest() == JPOD_MISSING_MD5
                or hashlib.sha256(data).hexdigest() == JPOD_MISSING_SHA256)


def jpod_alt_audio(expression, reading, url=JPOD_ALT_URL):
    """JapanesePod101's dictionary search page (Yomitan's "JapanesePod101 (Alternate)"): sometimes
    has words the plain audio URL doesn't. Picks the result row whose word and reading match."""
    body = urllib.parse.urlencode({"post": "dictionary_reference", "match_type": "exact",
                                   "search_query": expression, "vulgar": "true"}).encode()
    req = urllib.request.Request(url, data=body, headers=dict(_UA, **{
        "Content-Type": "application/x-www-form-urlencoded"}))
    with urllib.request.urlopen(req, timeout=12) as r:
        page = r.read().decode("utf-8", "replace")
    strip = lambda h: re.sub(r"<[^>]+>|\s+", "", html.unescape(h))
    for row in page.split('class="dc-result-row"')[1:]:
        src = re.search(r'<source[^>]*\ssrc="([^"]+)"', row) or re.search(r'<audio[^>]*\ssrc="([^"]+)"', row)
        if not src:
            continue
        kana = re.search(r'class="dc-vocab_kana"[^>]*>(.*?)</', row, re.S)
        word = re.search(r'class="dc-vocab"[^>]*>(.*?)</', row, re.S)
        kana = strip(kana.group(1)) if kana else ""
        word = strip(word.group(1)) if word else ""
        if reading and kana and kana != reading:
            continue
        if word and word != expression and not (kana and expression == kana):
            continue
        u = urllib.parse.urljoin(url, html.unescape(src.group(1)))
        data, ctype = _get(u)
        if _good_jpod_clip(data, ctype):
            return data, _ext_for(u, ctype)
    return None


def lingualibre_audio(expression, reading, api=COMMONS_API):
    """Native-speaker recordings from Lingua Libre on Wikimedia Commons
    (files named "LL-Q5287 (jpn)-<speaker>-<word>.wav")."""
    def query(params):
        params = dict(params, format="json")
        data, _ = _get(api + "?" + urllib.parse.urlencode(params))
        return json.loads(data.decode("utf-8"))
    term = expression
    esc = re.sub(r'([\\.^$|?*+()\[\]{}/"])', r"\\\1", term)
    titles = []
    # a title regex is exact; plain keywords are the fallback if regex search is unavailable
    for search in (f'intitle:/LL-Q5287 \\(jpn\\)-[^-]+-{esc}\\.wav/', f'"LL-Q5287 (jpn)" {term}'):
        try:
            found = query({"action": "query", "list": "search", "srnamespace": 6, "srlimit": 20,
                           "srsearch": search})
        except ValueError:
            continue
        titles = [h["title"] for h in found.get("query", {}).get("search", [])
                  if h.get("title", "").startswith("File:LL-Q5287 (jpn)-")
                  and h["title"].endswith(f"-{term}.wav")]
        if titles:
            break
    if titles:
        info = query({"action": "query", "prop": "imageinfo", "iiprop": "url", "titles": titles[0]})
        for page in info.get("query", {}).get("pages", {}).values():
            for ii in page.get("imageinfo", []):
                if ii.get("url"):
                    data, ctype = _get(ii["url"])
                    if data:
                        return data, _ext_for(ii["url"], ctype)
    return None


def forvo_audio(expression, reading, key, api=FORVO_API):
    """Forvo's official API (needs your own key from api.forvo.com). Best-rated recording first."""
    if not key:
        return None
    for term in dict.fromkeys(t for t in (expression, reading) if t):
        url = (f"{api}/key/{urllib.parse.quote(key)}/format/json/action/word-pronunciations/word/"
               f"{urllib.parse.quote(term)}/language/ja/order/rate-desc/limit/3")
        data, _ = _get(url)
        try:
            items = json.loads(data.decode("utf-8")).get("items", [])
        except ValueError:
            raise ValueError("Forvo did not return JSON (check the API key)")
        for it in items if isinstance(items, list) else []:
            u = it.get("pathmp3") or it.get("pathogg")
            if u:
                d, ctype = _get(u)
                if d:
                    return d, _ext_for(u, ctype) if not it.get("pathmp3") else "mp3"
        if term == expression and _is_kana(expression):
            break
    return None


# Sources that failed to connect (e.g. Local Audio Server not running) are skipped for the rest
# of the run instead of being retried for every word.
_unreachable = set()


def _source_name(src):
    if isinstance(src, str):
        return src
    return src.get("name") or src.get("type") or src.get("url") or "?"


def fetch_audio(sources, expression, reading, log=print, forvo_key=""):
    """Tries each source in order -> (bytes, ext, source_name) or None.
    Sources: "local" (Local Audio Server, skipped if it isn't running), "jpod101",
    "jpod101-alt", "lingualibre", "forvo" (needs a key), a URL, or {"name":..., "url":...}."""
    for src in sources:
        kind = src if isinstance(src, str) else src.get("type", "")
        url = src.get("url") if isinstance(src, dict) else None
        name = _source_name(src)
        if name in _unreachable:
            continue
        try:
            if kind == "jpod101":
                got = jpod_audio(expression, reading, url or JPOD_URL)
            elif kind in ("jpod101-alt", "jpod101-alternate"):
                got = jpod_alt_audio(expression, reading, url or JPOD_ALT_URL)
            elif kind in ("lingualibre", "lingua-libre"):
                got = lingualibre_audio(expression, reading, url or COMMONS_API)
            elif kind == "forvo":
                key = (src.get("key") if isinstance(src, dict) else "") or forvo_key
                if not key:
                    continue
                got = forvo_audio(expression, reading, key, url or FORVO_API)
            elif kind in ("local", "local-audio-server"):
                try:
                    got = custom_audio(url or LOCAL_AUDIO_URL, expression, reading)
                except urllib.error.HTTPError:
                    raise
                except (urllib.error.URLError, ConnectionError, socket.timeout):
                    _unreachable.add(name)  # not running: quietly skip it from now on
                    continue
            elif url:
                got = custom_audio(url, expression, reading)
            elif isinstance(src, str) and src.startswith(("http://", "https://")):
                got = custom_audio(src, expression, reading)
            else:
                log(f"    unknown audio source {name!r}")
                continue
        except (OSError, ValueError) as e:
            log(f"    audio source {name}: {e}")
            continue
        if got:
            return got[0], got[1], name
    return None
