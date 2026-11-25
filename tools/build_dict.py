#!/usr/bin/env python3
"""
build_dict.py - builds kotoba3ds' compact dictionary file (dict.bin) for the SD card.

Sources (pick one):
  --jmdict JMdict_e.gz        the official JMdict XML from EDRDG (freshest data)
  --jamdict-db jamdict.db[.xz] the SQLite DB shipped in the `jamdict-data` pip package
  (no source flag)             auto-detect an installed `jamdict-data` package

Radicals (for the "search by kanji components" screen):
  --radkfile radkfile[.gz]     EDRDG RADKFILE (EUC-JP). Auto-detected from an installed
                               `jamdict` package when omitted.

Output: dict.bin  ->  copy to SD:/3ds/kotoba3ds/dict.bin

Dictionary files for the dicts/ folder (--dicts-out; plugin 1.3+): version 3, below, one file per dictionary.
dict.bin (-o) stays version 2 for older plugins.

File format (little endian), version 2 (the plugin also reads version 1: 8-byte key records, no fences):
  header (64 bytes)
    0  "KTB1"
    4  u32 version
    8  u32 entryCount
   12  u32 entryIndexOff   (entryCount+1) x {u32 dataOff, u32 posMask}  (last = sentinel)
   16  u32 keyCount
   20  u32 keyIndexOff     keyCount x {u32 strOff, u32 entryId, u32 posMask of that entry},
                           sorted by (key bytes, rank)
   24  u32 radCount
   28  u32 radTableOff     radCount x {char utf8[8], u16 strokes, u16 count, u32 listOff,
                                       char label[16]}  (32 bytes)
   32  u32 kanjiCount
   36  u32 kanjiTableOff   kanjiCount x u32 codepoint, most common kanji first
   40  u32 fenceStep       a fence every fenceStep keys (keys 0, step, 2 step, ...)
   44  u32 fenceCount
   48  u32 fenceOff        fenceCount x {u16 len, key bytes}
   52..63 reserved
  radical lists: u16 kanji ids, ascending (== most common first)
  key strings: NUL-terminated UTF-8
  entry blobs (UTF-8, see ENTRY BLOB below)

Version 3 (a dictionary file in dicts/): as version 2, plus
   52  u32 tagTableOff     tagCount x u32 offset of {u8 category, code\\0, label\\0, description\\0}
   56  u32 tagCount
   60  u32 titleOff        the dictionary's name, NUL-terminated UTF-8
  and each entry blob is the machine line (line 0 below), '\\n', then DISPLAY RUNS (no headline or sense
  lines: the plugin derives the plain senses for saved words from the runs), then a NUL.

DISPLAY RUNS: what the 3DS draws for the entry, styled on the PC (yomitan.display_paragraphs); the 3DS only
  wraps them to the panel's width. UTF-8 text, never a byte below 0x20, between these codes:
    0x01 kind indent+1   a paragraph starts (kinds: yomitan.P_*; indent in levels)
    0x02 style           the text after it is in this style (yomitan.S_*, + S_SMALL for the small size)
    0x03 hi lo           a tag chip: tag id (hi - 1) * 255 + (lo - 1) in the tag table
    0x04                 rows after the first start here (a hanging indent, after a sense's number)
    0x05 base 0x06 reading 0x07   furigana over base, in the current style
"""
import argparse
import gzip
import json
import io
import lzma
import os
import re
import sqlite3
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xml.etree.ElementTree as ET
from collections import defaultdict

# ----------------------------------------------------------------------------
# Tag tables
# ----------------------------------------------------------------------------
# jamdict stores JMdict entities expanded to their descriptions; map them back
# to the short codes JMdict uses (and that we show on screen).
DESC2CODE = {
    "'ku' adjective (archaic)": "adj-ku",
    "'shiku' adjective (archaic)": "adj-shiku",
    "'taru' adjective": "adj-t",
    "Godan verb - -aru special class": "v5aru",
    "Godan verb - Iku/Yuku special class": "v5k-s",
    "Godan verb - Uru old class verb (old form of Eru)": "v5uru",
    "Godan verb with 'bu' ending": "v5b",
    "Godan verb with 'gu' ending": "v5g",
    "Godan verb with 'ku' ending": "v5k",
    "Godan verb with 'mu' ending": "v5m",
    "Godan verb with 'nu' ending": "v5n",
    "Godan verb with 'ru' ending": "v5r",
    "Godan verb with 'ru' ending (irregular verb)": "v5r-i",
    "Godan verb with 'su' ending": "v5s",
    "Godan verb with 'tsu' ending": "v5t",
    "Godan verb with 'u' ending": "v5u",
    "Godan verb with 'u' ending (special class)": "v5u-s",
    "Ichidan verb": "v1",
    "Ichidan verb - kureru special class": "v1-s",
    "Ichidan verb - zuru verb (alternative form of -jiru verbs)": "vz",
    "Kuru verb - special class": "vk",
    "adjectival nouns or quasi-adjectives (keiyodoshi)": "adj-na",
    "adjective (keiyoushi)": "adj-i",
    "adjective (keiyoushi) - yoi/ii class": "adj-ix",
    "adverb (fukushi)": "adv",
    "adverb taking the 'to' particle": "adv-to",
    "adverbial noun (fukushitekimeishi)": "n-adv",
    "archaic/formal form of na-adjective": "adj-nari",
    "auxiliary": "aux",
    "auxiliary adjective": "aux-adj",
    "auxiliary verb": "aux-v",
    "conjunction": "conj",
    "copula": "cop",
    "counter": "ctr",
    "expressions (phrases, clauses, etc.)": "exp",
    "interjection (kandoushi)": "int",
    "intransitive verb": "vi",
    "irregular nu verb": "vn",
    "irregular ru verb, plain form ends with -ri": "vr",
    "noun (common) (futsuumeishi)": "n",
    "noun (temporal) (jisoumeishi)": "n-t",
    "noun or participle which takes the aux. verb suru": "vs",
    "noun or verb acting prenominally": "adj-f",
    "noun, used as a prefix": "n-pref",
    "noun, used as a suffix": "n-suf",
    "nouns which may take the genitive case particle 'no'": "adj-no",
    "numeric": "num",
    "particle": "prt",
    "pre-noun adjectival (rentaishi)": "adj-pn",
    "prefix": "pref",
    "pronoun": "pn",
    "su verb - precursor to the modern suru": "vs-c",
    "suffix": "suf",
    "suru verb - included": "vs-i",
    "suru verb - special class": "vs-s",
    "transitive verb": "vt",
    "unclassified": "unc",
    "verb unspecified": "v-unspec",
    # misc
    "word usually written using kana alone": "uk",
    "abbreviation": "abbr",
    "archaism": "arch",
    "archaic": "arch",
    "yojijukugo": "yoji",
    "obscure term": "obsc",
    "onomatopoeic or mimetic word": "on-mim",
    "colloquialism": "col",
    "colloquial": "col",
    "slang": "sl",
    "idiomatic expression": "id",
    "proverb": "proverb",
    "honorific or respectful (sonkeigo) language": "hon",
    "obsolete term": "obs",
    "derogatory": "derog",
    "polite (teineigo) language": "pol",
    "humble (kenjougo) language": "hum",
    "sensitive": "sens",
    "historical term": "hist",
    "jocular, humorous term": "joc",
    "vulgar expression or word": "vulg",
    "familiar language": "fam",
    "children's language": "chn",
    "Internet slang": "net-sl",
    "dated term": "dated",
    "literary or formal term": "form",
    "formal or literary term": "form",
    "female term or language": "fem",
    "male term or language": "male",
    "poetical term": "poet",
    "manga slang": "m-sl",
    "quotation": "quote",
    "rare": "rare",
    "full name of a particular person": "person",
    "place name": "place",
    "work of art, literature, music, etc. name": "work",
    # kanji/kana form info
    "ateji (phonetic) reading": "ateji",
    "irregular okurigana usage": "io",
    "word containing irregular kana usage": "ik",
    "word containing irregular kanji usage": "iK",
    "word containing out-dated kanji": "oK",
    "word containing out-dated kanji or kanji usage": "oK",
    "out-dated or obsolete kana usage": "ok",
    "rarely-used kanji form": "rK",
    "search-only kanji form": "sK",
    "search-only kana form": "sk",
    "rarely used kana form": "rk",
}

def desc_code(d):
    if d in DESC2CODE:
        return DESC2CODE[d]
    if d.startswith("Yodan verb"):
        return "v4"
    if d.startswith("Nidan verb"):
        return "v2"
    if d.startswith("gikun"):
        return "gikun"
    return d


# Bits used by the on-device deinflector (must match core/deinflect.hpp)
POS_V1, POS_V5, POS_VK, POS_VSI, POS_VSN, POS_ADJ, POS_VZ, POS_ADJNA = (1 << i for i in range(8))

# Forms we never pick as the card's headword (still searchable)
BAD_FORM_TAGS = {"iK", "ik", "oK", "ok", "io", "rK", "sK", "sk", "rk"}
# Forms that should not even be searchable keys? (search-only forms ARE searchable)

MAX_SENSES = 12
MAX_FORMS_SHOWN = 6

KATA_START, KATA_END = 0x30A1, 0x30F6  # ァ..ヶ -> ぁ..ゖ


def normalize_key(s: str) -> str:
    """Katakana -> hiragana, full-width ASCII left alone. Must match core/kana.cpp."""
    out = []
    for ch in s:
        cp = ord(ch)
        if KATA_START <= cp <= KATA_END:
            out.append(chr(cp - 0x60))
        else:
            out.append(ch)
    return "".join(out)


def pos_mask(codes):
    m = 0
    for c in codes:
        if c in ("v1", "v1-s"):
            m |= POS_V1
        elif c.startswith("v5"):
            m |= POS_V5
        elif c == "vk":
            m |= POS_VK
        elif c in ("vs-i", "vs-s"):
            m |= POS_VSI
        elif c == "vs":
            m |= POS_VSN
        elif c in ("adj-i", "adj-ix"):
            m |= POS_ADJ
        elif c == "vz":
            m |= POS_VZ
        elif c == "adj-na":
            m |= POS_ADJNA
    return m


def pri_score(tags):
    """Lower = more common."""
    best = 100
    for t in tags:
        if t.startswith("nf") and t[2:].isdigit():
            best = min(best, int(t[2:]))
        elif t in ("ichi1", "news1", "spec1", "gai1"):
            best = min(best, 50)
        elif t in ("ichi2", "news2", "spec2", "gai2"):
            best = min(best, 70)
    return best


class Entry:
    __slots__ = ("idseq", "kanji", "kana", "senses", "freq", "paras")

    def __init__(self, idseq):
        self.idseq = idseq
        self.freq = 0     # frequency rank (1 = most common), 0 = not in the list
        self.kanji = []   # (text, info_codes, pri_tags)
        self.kana = []    # (text, info_codes, pri_tags, nokanji)
        self.senses = []  # (pos_codes, misc_codes, glosses)
        self.paras = None  # [yomitan.Para] to show (version 3 files); None: made from senses


# ----------------------------------------------------------------------------
# Loaders
# ----------------------------------------------------------------------------
class _EntityStrippingReader(io.RawIOBase):
    """Feeds JMdict XML to the parser with the DTD removed and `&ent;` -> `ent`,
    so tags come out as short codes instead of long descriptions."""
    _ent = re.compile(rb"&([A-Za-z0-9][A-Za-z0-9_\-]*);")
    _keep = {b"amp", b"lt", b"gt", b"quot", b"apos"}

    def __init__(self, f):
        self._lines = iter(f)
        self._buf = b""
        self._in_dtd = False

    def readable(self):
        return True

    def _next_chunk(self):
        for line in self._lines:
            if self._in_dtd:
                if line.strip().startswith(b"]>"):
                    self._in_dtd = False
                continue
            if line.lstrip().startswith(b"<!DOCTYPE"):
                if b"]>" not in line:
                    self._in_dtd = True
                continue
            return self._ent.sub(lambda m: m.group(0) if m.group(1) in self._keep else m.group(1), line)
        return None

    def readinto(self, b):
        while len(self._buf) < len(b):
            chunk = self._next_chunk()
            if chunk is None:
                break
            self._buf += chunk
        n = min(len(b), len(self._buf))
        b[:n] = self._buf[:n]
        self._buf = self._buf[n:]
        return n


def load_jmdict_xml(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rb") as raw:
        reader = io.BufferedReader(_EntityStrippingReader(raw), 1 << 16)
        for _, el in ET.iterparse(reader, events=("end",)):
            if el.tag != "entry":
                continue
            e = Entry(int(el.findtext("ent_seq")))
            for k in el.findall("k_ele"):
                e.kanji.append((k.findtext("keb"), [x.text for x in k.findall("ke_inf")],
                                [x.text for x in k.findall("ke_pri")]))
            for r in el.findall("r_ele"):
                e.kana.append((r.findtext("reb"), [x.text for x in r.findall("re_inf")],
                               [x.text for x in r.findall("re_pri")], r.find("re_nokanji") is not None))
            last_pos = []
            for s in el.findall("sense"):
                pos = [x.text for x in s.findall("pos")] or last_pos  # JMdict: pos carries over
                last_pos = pos
                glosses = [g.text for g in s.findall("gloss")
                           if g.text and g.get("{http://www.w3.org/XML/1998/namespace}lang", "eng") == "eng"]
                if glosses:
                    e.senses.append((pos, [x.text for x in s.findall("misc")], glosses))
            el.clear()
            if e.senses and (e.kanji or e.kana):
                yield e


def _open_sqlite(path):
    if path.endswith(".xz"):
        out = path[:-3]
        if not os.path.exists(out):
            print(f"Decompressing {path} ...", file=sys.stderr)
            with lzma.open(path) as src, open(out, "wb") as dst:
                while True:
                    b = src.read(1 << 20)
                    if not b:
                        break
                    dst.write(b)
        path = out
    return sqlite3.connect(path)


def load_jamdict_db(path):
    db = _open_sqlite(path)
    code = desc_code
    kanji = defaultdict(list)
    kinfo, kpri = defaultdict(list), defaultdict(list)
    for kid, t in db.execute("select kid,text from KJI"):
        kinfo[kid].append(code(t))
    for kid, t in db.execute("select kid,text from KJP"):
        kpri[kid].append(t)
    for kid, idseq, text in db.execute("select ID,idseq,text from Kanji order by ID"):
        kanji[idseq].append((text, kinfo.get(kid, []), kpri.get(kid, [])))
    kana = defaultdict(list)
    rinfo, rpri = defaultdict(list), defaultdict(list)
    for kid, t in db.execute("select kid,text from KNI"):
        rinfo[kid].append(code(t))
    for kid, t in db.execute("select kid,text from KNP"):
        rpri[kid].append(t)
    for kid, idseq, text, nok in db.execute("select ID,idseq,text,nokanji from Kana order by ID"):
        kana[idseq].append((text, rinfo.get(kid, []), rpri.get(kid, []), bool(nok)))
    pos, misc, gloss = defaultdict(list), defaultdict(list), defaultdict(list)
    for sid, t in db.execute("select sid,text from pos"):
        pos[sid].append(code(t))
    for sid, t in db.execute("select sid,text from misc"):
        misc[sid].append(code(t))
    for sid, t in db.execute("select sid,text from SenseGloss where lang='eng' or lang is null"):
        if t:
            gloss[sid].append(t)
    senses = defaultdict(list)
    for sid, idseq in db.execute("select ID,idseq from Sense order by ID"):
        senses[idseq].append(sid)
    for (idseq,) in db.execute("select idseq from Entry order by idseq"):
        e = Entry(idseq)
        e.kanji = kanji.get(idseq, [])
        e.kana = kana.get(idseq, [])
        last_pos = []
        for sid in senses.get(idseq, []):
            p = pos.get(sid) or last_pos
            last_pos = p
            if gloss.get(sid):
                e.senses.append((p, misc.get(sid, []), gloss[sid]))
        if e.senses and (e.kanji or e.kana):
            yield e


# Jitendex/Yomitan term tags -> JMdict form codes
YOMI_FORM_TAGS = {
    "old kanji form": "oK", "irregular form": "iK", "obsolete reading": "ok",
    "rarely used form": "rK", "search-only form": "sK", "ateji form": "ateji",
    "special reading": "gikun",
}
_HAS_KANJI = re.compile(r"[\u3400-\u9fff\uf900-\ufaff\u3005]")


def _yomi_form_codes(tags):
    t = (tags or "").replace("\u00a0", " ")
    return [c for name, c in YOMI_FORM_TAGS.items() if name in t]


def load_yomitan(path, freq=None, keep_display=False):
    """Yomitan term dictionary (.zip) -> Entry objects. Sequenced dictionaries (Jitendex,
    JMdict-yomitan) are grouped into one entry per sequence; others into one entry per
    (expression, reading)."""
    import yomitan
    title = yomitan.read_index(path).get("title", os.path.basename(path))
    groups = {}
    order = []
    for expr, reading, def_tags, rules, score, glossary, seq, term_tags in yomitan.iter_terms(path):
        # redirect rows have no reading; they only contribute their spelling
        # Jitendex marks redirect rows (old spellings etc.) with the negated sequence of their entry
        key = abs(seq) if isinstance(seq, int) and seq != 0 else (expr, reading)
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append((expr, reading or "", def_tags or "", rules or "", score or 0, glossary, term_tags or ""))
    print(f"{title}: {len(order)} entries", file=sys.stderr)
    for n, key in enumerate(order):
        rows = groups[key]
        e = Entry(key if isinstance(key, int) else 10_000_000 + n)
        star = any("\u2605" in r[2] for r in rows)
        best_score = max(r[4] for r in rows)
        # forms
        for expr, reading, def_tags, rules, score, glossary, term_tags in rows:
            codes = _yomi_form_codes(def_tags)
            pri = ["ichi1"] if ("\u2605" in def_tags or "priority" in def_tags.replace("\u00a0", " ")) else []
            if freq is not None and expr in freq:
                pri.append("nf%02d" % min(48, max(1, freq[expr] // 500 + 1)))
            if _HAS_KANJI.search(expr):
                if expr not in [k[0] for k in e.kanji]:
                    e.kanji.append((expr, codes, pri))
            elif expr not in [r[0] for r in e.kana]:
                e.kana.append((expr, codes, pri, False))
            if reading and reading not in [r[0] for r in e.kana]:
                e.kana.append((reading, [c for c in codes if c in ("ok",)], pri, False))
        # senses: from the richest non-redirect row
        content_rows = [r for r in rows if not yomitan.is_redirect(r[5])] or rows
        main = max(content_rows, key=lambda r: len(json.dumps(r[5], ensure_ascii=False)))
        glossary, rules = main[5], main[3]
        rule_codes = rules.split()
        if keep_display:
            e.paras = yomitan.display_paragraphs(glossary)
        if yomitan.is_jitendex_like(glossary):
            e.senses = yomitan.jitendex_senses(glossary)
            if e.senses and rule_codes and not pos_mask(c for s in e.senses for c in s[0]):
                p0, m0, g0 = e.senses[0]  # word class only known from the rules field
                e.senses[0] = (p0 + rule_codes, m0, g0)
        else:
            lines = yomitan.plain_lines(glossary)
            if lines:
                e.senses = [(rule_codes, [], [lines[0]])] + [([], [], [l]) for l in lines[1:]]
        if not e.senses:
            continue
        yield e


class Frequency:
    """A Yomitan frequency dictionary (rank-based, e.g. JPDB): lower = more common.

    by_pair[(expression, reading)]: rows that name their reading (JPDB: a kanji word, with a
        second row marked ㋕ for how often it is written in kana)
    by_term[expression]: rows without a reading (JPDB: kana words, one row per homograph)
    """

    def __init__(self, path):
        import yomitan
        self.by_pair, self.by_term = {}, {}
        for expr, mode, data in yomitan.iter_meta(path):
            if mode != "freq":
                continue
            reading = data.get("reading") if isinstance(data, dict) else None
            v = data
            if isinstance(v, dict) and "frequency" in v:
                v = v["frequency"]
            if isinstance(v, dict):
                v = v.get("value")
            if not isinstance(v, (int, float)) or v <= 0:
                continue
            v = int(v)
            if reading:
                self.by_pair[(expr, reading)] = min(self.by_pair.get((expr, reading), 10 ** 9), v)
                self.by_term[expr] = min(self.by_term.get(expr, 10 ** 9), v)  # for the old pri tags
            else:
                self.by_term[expr] = min(self.by_term.get(expr, 10 ** 9), v)

    def __len__(self):
        return len(self.by_term)

    def __contains__(self, expr):
        return expr in self.by_term

    def __getitem__(self, expr):
        return self.by_term[expr]

    def rank(self, e):
        """The entry's rank: its best (kanji form, reading) pair, kanji or kana written; for a
        word with no kanji, its kana forms. A kanji word never takes a bare kana row's rank (that
        row could be any homograph's: しょうか is 消化 and 唱歌 and ...)."""
        readings = [r[0] for r in e.kana]
        if e.kanji:
            vals = [self.by_pair[(k[0], r)] for k in e.kanji for r in readings if (k[0], r) in self.by_pair]
        else:
            vals = [self.by_pair[(r, r)] for r in readings if (r, r) in self.by_pair]
            vals += [self.by_term[r] for r in readings if r in self.by_term]
        return min(vals) if vals else 0


def load_frequency(path):
    return Frequency(path)


# ----------------------------------------------------------------------------
# Entry blob
# ----------------------------------------------------------------------------
# Line 0 (machine fields, TAB separated):
#   expression \t reading \t posCodes(comma) \t kanjiForms(｜) \t kanaForms(｜) \t freqRank
#   (freqRank: 1 = most common in the --freq list, empty = not in it or no list)
# Line 1: headline, e.g. 食べる・喰べる【たべる】
# Lines 2..: numbered senses for display, e.g. "1. (v1,vt) to eat"
SEP = "｜"  # full-width vertical bar, never appears in headwords


def entry_blob(e: Entry) -> str:
    first_misc = e.senses[0][1]
    good_k = [k for k in e.kanji if not (set(k[1]) & BAD_FORM_TAGS)] or e.kanji
    good_r = [r for r in e.kana if not r[3] and not (set(r[1]) & BAD_FORM_TAGS)] or e.kana
    if not e.kanji or "uk" in first_misc:
        expr = good_r[0][0] if good_r else e.kana[0][0]
    else:
        expr = good_k[0][0]
    reading = good_r[0][0] if good_r else (e.kana[0][0] if e.kana else expr)
    all_pos = []
    for p, _, _ in e.senses:
        for c in p:
            if c not in all_pos:
                all_pos.append(c)
    shown_k = [k[0] for k in e.kanji if "sK" not in k[1]][:MAX_FORMS_SHOWN]
    shown_r = [r[0] for r in e.kana if "sk" not in r[1]][:MAX_FORMS_SHOWN]
    if shown_k:
        head = "・".join(shown_k) + "【" + "・".join(shown_r) + "】"
    else:
        head = "・".join(shown_r)
    lines = [
        "\t".join([expr, reading, ",".join(all_pos),
                   SEP.join(k[0] for k in e.kanji), SEP.join(r[0] for r in e.kana),
                   str(e.freq) if e.freq else ""]),
        head,
    ]
    prev_pos = None
    for i, (p, m, g) in enumerate(e.senses[:MAX_SENSES], 1):
        tags = []
        if p != prev_pos:
            tags += p
            prev_pos = p
        tags += m
        tag = "(" + ",".join(tags) + ") " if tags else ""
        lines.append(f"{i}. {tag}" + "; ".join(g))
    if len(e.senses) > MAX_SENSES:
        lines.append(f"(+{len(e.senses) - MAX_SENSES} more senses)")
    return "\n".join(lines)


# ----------------------------------------------------------------------------
# Version 3: display runs (see DISPLAY RUNS at the top)
# ----------------------------------------------------------------------------
class TagTable:
    """The tags the chips name, each once: (category, code, label, description) -> id."""

    def __init__(self):
        self.ids, self.rows = {}, []

    def id(self, tag):
        if tag not in self.ids:
            assert len(self.rows) < 255 * 255
            self.ids[tag] = len(self.rows)
            self.rows.append(tag)
        return self.ids[tag]


def paras_from_senses(e):
    """Display paragraphs for entries without structured content (JMdict XML / jamdict): the senses as the
    version 2 blob shows them, part of speech as chips over each run of senses that share it."""
    import yomitan as y
    out, prev = [], None
    many = len(e.senses) > 1
    for i, (p, m, g) in enumerate(e.senses[:MAX_SENSES], 1):
        if p and p != prev:
            gp = y.Para(y.P_GROUP)
            gp.items = [("chip", (y.TAG_POS, c, c, "")) for c in p]
            out.append(gp)
        prev = p
        sp = y.Para(y.P_SENSE)
        if many:
            sp.text(y.S_DIM, f"{i}. ")
            sp.items.append(("hang",))
        sp.items += [("chip", (y.TAG_MISC, c, c, "")) for c in m]
        sp.text(y.S_TEXT, "; ".join(g))
        out.append(sp)
    return out


def encode_runs(paras, tags):
    out = bytearray()
    for p in paras:
        out += bytes([1, p.kind, min(p.indent, 250) + 1])
        style = None
        for it in p.items:
            if it[0] == "chip":
                t = tags.id(it[1])
                out += bytes([3, t // 255 + 1, t % 255 + 1])
                continue
            if it[0] == "hang":
                out.append(4)
                continue
            if it[1] != style:
                style = it[1]
                out += bytes([2, style])
            if it[0] == "text":
                out += it[2].encode()
            else:  # ruby
                out += b"\x05" + it[2].encode() + b"\x06" + it[3].encode() + b"\x07"
    assert min(out, default=1) >= 1
    return bytes(out)


def entry_record(e: Entry, tags) -> bytes:
    """Version 3: the machine line, then the display runs."""
    machine = entry_blob(e).split("\n", 1)[0]
    paras = e.paras if e.paras is not None else paras_from_senses(e)
    return machine.encode() + b"\n" + encode_runs(paras, tags)


def decode_runs(data, tags):
    """For the tests: display runs -> [(kind, indent, text with chips as [label] and ruby as base(reading))]."""
    out, i, n = [], 0, len(data)
    while i < n:
        c = data[i]
        if c == 1:
            out.append([data[i + 1], data[i + 2] - 1, ""])
            i += 3
        elif c == 2:
            i += 2
        elif c == 3:
            t = (data[i + 1] - 1) * 255 + data[i + 2] - 1
            out[-1][2] += f"[{tags[t][2]}]"
            i += 3
        elif c == 4:
            i += 1
        elif c == 5:
            j = data.index(6, i)
            k = data.index(7, j)
            out[-1][2] += data[i + 1:j].decode() + "(" + data[j + 1:k].decode() + ")"
            i = k + 1
        else:
            j = i
            while j < n and data[j] >= 0x20:
                j += 1
            out[-1][2] += data[i:j].decode()
            i = j
    return [tuple(p) for p in out]


# ----------------------------------------------------------------------------
# Radicals
# ----------------------------------------------------------------------------
# radkfile uses ordinary kanji as stand-ins for some radical shapes; label them.
RAD_LABELS = {
    "化": "person 亻", "个": "hat 𠆢", "并": "horns 丷", "刈": "knife 刂", "込": "road 辶",
    "尚": "small ⺌", "忙": "heart 忄", "扎": "hand 扌", "汁": "water 氵", "犯": "dog 犭",
    "艾": "grass 艹", "邦": "village R 阝", "阡": "mound L 阝", "老": "old 耂", "杰": "fire 灬",
    "礼": "altar 礻", "疔": "sick 疒", "禹": "track 禸", "初": "cloth 衤", "買": "net 罒",
    "乞": "𠂉", "滴": "啇", "也": "也", "亡": "lid 亠", "冂": "box 冂",
}


def load_radkfile(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rb") as f:
        text = f.read().decode("euc_jp", errors="replace")
    rads = []  # (radical, strokes, [kanji...])
    cur = None
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        if line.startswith("$"):
            parts = line.split()
            cur = (parts[1], int(parts[2]), [])
            rads.append(cur)
        elif cur is not None:
            cur[2].extend(ch for ch in line.strip() if ord(ch) >= 0x3400 and ch != "�")
    return rads


# ----------------------------------------------------------------------------
# Writer
# ----------------------------------------------------------------------------
FENCE_STEP = 128  # keys per stretch between two fences (see build)


def build(entries, rads, out_path, freq=None, version=2, title=""):
    """Writes dict.bin (version 2) or a dicts/ file (version 3: display runs, tag table, title)."""
    entries = list(entries)
    tags = TagTable()
    print(f"{len(entries)} entries", file=sys.stderr)

    def entry_rank(e):
        return min([pri_score(k[2]) for k in e.kanji] + [pri_score(r[2]) for r in e.kana] + [100])

    if freq is not None:
        for e in entries:
            e.freq = freq.rank(e)
        print(f"frequency ranks: {sum(1 for e in entries if e.freq)} entries", file=sys.stderr)
    # entry ids in order of commonness: the plugin lists a spot's matches (homographs) by id, so
    # the most common comes first. Ranked entries by rank, then the rest by JMdict's priority tags.
    entries.sort(key=lambda e: ((0, e.freq) if e.freq else (1, entry_rank(e)), e.idseq))

    # keys
    keys = {}  # (keybytes, entryId) -> form score
    kanji_freq = defaultdict(int)
    for eid, e in enumerate(entries):
        for text, info, pri in e.kanji:
            k = normalize_key(text).encode()
            sc = pri_score(pri)
            keys[(k, eid)] = min(keys.get((k, eid), 999), sc)
            w = 20 if sc <= 50 else 6 if sc <= 70 else 1
            for ch in text:
                if 0x3400 <= ord(ch) <= 0x9FFF:
                    kanji_freq[ch] += w
        for text, info, pri, nok in e.kana:
            k = normalize_key(text).encode()
            keys[(k, eid)] = min(keys.get((k, eid), 999), pri_score(pri))
    key_list = sorted(keys.items(), key=lambda kv: (kv[0][0], kv[1], kv[0][1]))
    print(f"{len(key_list)} keys", file=sys.stderr)

    # radicals / kanji ids
    all_kanji = set()
    for _, _, ks in rads:
        all_kanji.update(ks)
    kanji_order = sorted(all_kanji, key=lambda c: (-kanji_freq.get(c, 0), ord(c)))
    kid = {c: i for i, c in enumerate(kanji_order)}
    assert len(kanji_order) < 65536

    blob = bytearray(b"\0" * 64)

    def align(n=4):
        while len(blob) % n:
            blob.append(0)

    # kanji table
    align()
    kanji_off = len(blob)
    for c in kanji_order:
        blob += struct.pack("<I", ord(c))
    # radical lists
    rad_records = []
    for r, strokes, ks in rads:
        ids = sorted({kid[c] for c in ks})
        align()
        off = len(blob)
        blob += struct.pack(f"<{len(ids)}H", *ids)
        rad_records.append((r, strokes, len(ids), off))
    align()
    rad_off = len(blob)
    for r, strokes, cnt, off in rad_records:
        rb = r.encode()[:8].ljust(8, b"\0")
        lab = RAD_LABELS.get(r, "").encode()[:15]
        lab = lab.decode("utf-8", "ignore").encode().ljust(16, b"\0")
        blob += rb + struct.pack("<HHI", strokes, min(cnt, 65535), off) + lab
    # key strings
    str_offs = {}
    for (k, _), _ in key_list:
        if k not in str_offs:
            str_offs[k] = len(blob)
            blob += k + b"\0"
    # entry blobs
    entry_offs = []
    masks = []
    for e in entries:
        entry_offs.append(len(blob))
        masks.append(pos_mask(c for s in e.senses for c in s[0]))
        blob += (entry_record(e, tags) if version >= 3 else entry_blob(e).encode()) + b"\0"
    end_off = len(blob)
    # entry index
    align()
    entry_idx_off = len(blob)
    for off, m in zip(entry_offs, masks):
        blob += struct.pack("<II", off, m)
    blob += struct.pack("<II", end_off, 0)
    # key index (version 2): the key's string, its entry, and that entry's part-of-speech mask
    # (the search filters deinflected matches by it: one read less per match)
    align()
    key_idx_off = len(blob)
    for (k, eid), _ in key_list:
        blob += struct.pack("<III", str_offs[k], eid, masks[eid])
    # fences: every FENCE_STEP-th key, loaded into memory by the plugin, so a search reads only
    # the stretch of FENCE_STEP keys around its key from the SD card
    align()
    fence_off = len(blob)
    fences = [key_list[i][0][0] for i in range(0, len(key_list), FENCE_STEP)]
    for k in fences:
        assert len(k) < 65536
        blob += struct.pack("<H", len(k)) + k

    extra = []
    if version >= 3:  # the tags' table and the dictionary's name
        align()
        tag_rows = []
        for cat, code, label, desc in tags.rows:
            tag_rows.append(len(blob))
            blob += bytes([cat]) + b"\0".join(x.encode() for x in (code, label, desc)) + b"\0"
        align()
        tag_off = len(blob)
        blob += struct.pack(f"<{len(tag_rows)}I", *tag_rows)
        title_off = len(blob)
        blob += title.encode() + b"\0"
        extra = [tag_off, len(tag_rows), title_off]
    header = struct.pack(f"<4s{12 + len(extra)}I", b"KTB1", version, len(entries), entry_idx_off, len(key_list),
                         key_idx_off, len(rad_records), rad_off, len(kanji_order), kanji_off, FENCE_STEP, len(fences),
                         fence_off, *extra)
    blob[:len(header)] = header
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"wrote {out_path} (version {version}): {len(blob) / 1e6:.1f} MB, {len(rad_records)} radicals, "
          f"{len(kanji_order)} kanji" + (f", {len(tags.rows)} tags" if version >= 3 else ""), file=sys.stderr)
    return tags


def find_installed(pkg, rel):
    try:
        import importlib.util
        spec = importlib.util.find_spec(pkg)
        if spec and spec.submodule_search_locations:
            for loc in spec.submodule_search_locations:
                p = os.path.join(loc, rel)
                if os.path.exists(p):
                    return p
    except Exception:
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jmdict", help="JMdict_e(.gz) XML from EDRDG")
    ap.add_argument("--jamdict-db", help="jamdict.db or jamdict.db.xz")
    ap.add_argument("--yomitan", action="append", metavar="ZIP",
                    help="Yomitan dictionary zip, e.g. jitendex-yomitan.zip (repeat to combine several)")
    ap.add_argument("--freq", metavar="ZIP", help="Yomitan frequency dictionary zip (e.g. JPDB) for ranking")
    ap.add_argument("--radkfile", help="radkfile or radkfilex (.gz ok)")
    ap.add_argument("-o", "--out", help="dict.bin to write (version 2, every plugin version); the default when "
                    "--dicts-out isn't given")
    ap.add_argument("--dicts-out", metavar="FILE",
                    help="dictionary file for SD:/3ds/kotoba3ds/dicts/ to write (version 3: full entries, plugin 1.3+), "
                         "e.g. dicts/jitendex.ktd; with -o both are written from one read of the sources")
    a = ap.parse_args()
    if not a.out and not a.dicts_out:
        a.out = "dict.bin"

    freq = None
    if a.freq:
        freq = load_frequency(a.freq)
        print(f"frequency list: {len(freq.by_pair)} word/reading pairs, {len(freq.by_term)} words",
              file=sys.stderr)
    title = ""
    if a.yomitan:
        import yomitan
        title = re.sub(r"\s*\[[^\]]*\]\s*$", "", yomitan.read_index(a.yomitan[0]).get("title", "")) or \
            os.path.splitext(os.path.basename(a.yomitan[0]))[0]

        def chained():
            for p in a.yomitan:
                yield from load_yomitan(p, freq, keep_display=bool(a.dicts_out))
        entries = chained()
    elif a.jmdict:
        entries = load_jmdict_xml(a.jmdict)
        title = "JMdict"
    else:
        db = a.jamdict_db or find_installed("jamdict_data", "jamdict.db") \
            or find_installed("jamdict_data", "jamdict.db.xz")
        if not db:
            sys.exit("No dictionary source. Pass --yomitan jitendex-yomitan.zip or --jmdict JMdict_e.gz, or run:\n"
                     "  pip install jamdict jamdict-data")
        entries = load_jamdict_db(db)
        title = "JMdict"

    rk = a.radkfile or find_installed("jamdict", "data/radkfile.gz")
    if not rk:
        print("warning: no radkfile found; kanji component search will be empty", file=sys.stderr)
        rads = []
    else:
        rads = load_radkfile(rk)
    if a.out and a.dicts_out:
        entries = list(entries)
    if a.out:
        build(entries, rads, a.out, freq)
    if a.dicts_out:
        if os.path.dirname(a.dicts_out):
            os.makedirs(os.path.dirname(a.dicts_out), exist_ok=True)
        build(entries, rads, a.dicts_out, freq, version=3, title=title)


if __name__ == "__main__":
    main()
