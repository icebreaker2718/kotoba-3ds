"""
yomitan.py - reads Yomitan dictionary .zip files (Jitendex, JMdict-yomitan, monolingual
dictionaries, pitch-accent and frequency dictionaries).

Used by build_dict.py (to build the 3DS dictionary) and mine.py (to put the full, styled
definition, pitch accent and frequencies on the Anki card).

Format reference: https://github.com/yomidevs/yomitan/tree/master/ext/data/schemas
  term_bank_N.json       [expression, reading, defTags, rules, score, glossary[], sequence, termTags]
  term_meta_bank_N.json  [expression, "freq"|"pitch"|"ipa", data]
"""
import html
import json
import re
import zipfile

# ---------------------------------------------------------------------------
# zip access
# ---------------------------------------------------------------------------
_bank_num = re.compile(r"(\d+)\.json$")


def _read(z, name):
    """Reads a zip member without checking its CRC. Some popular dictionaries (e.g. the NHK2016
    pitch zip) ship with wrong checksums; Yomitan ignores them, so we do too."""
    with z.open(name) as f:
        f._expected_crc = None
        return f.read()


def _banks(z, prefix):
    names = [n for n in z.namelist() if n.split("/")[-1].startswith(prefix) and n.endswith(".json")]
    return sorted(names, key=lambda n: int(_bank_num.search(n).group(1)) if _bank_num.search(n) else 0)


def read_index(path):
    with zipfile.ZipFile(path) as z:
        name = next((n for n in z.namelist() if n.split("/")[-1] == "index.json"), None)
        return json.loads(_read(z, name)) if name else {}


def read_styles(path):
    with zipfile.ZipFile(path) as z:
        name = next((n for n in z.namelist() if n.split("/")[-1] == "styles.css"), None)
        return _read(z, name).decode("utf-8", "replace") if name else ""


def iter_terms(path):
    """Yields term rows: (expression, reading, defTags, rules, score, glossary, sequence, termTags)."""
    with zipfile.ZipFile(path) as z:
        for name in _banks(z, "term_bank_"):
            for row in json.loads(_read(z, name)):
                row = list(row) + [None] * (8 - len(row))
                yield row[:8]


def read_tags(path):
    """The dictionary's tag banks: {name: (category, notes)}."""
    out = {}
    with zipfile.ZipFile(path) as z:
        for name in _banks(z, "tag_bank_"):
            for row in json.loads(_read(z, name)):
                if isinstance(row, list) and row and isinstance(row[0], str):
                    row = row + [""] * (3 - len(row))
                    out[row[0]] = (str(row[1] or ""), str(row[3] if len(row) > 3 and row[3] else ""))
    return out


def iter_meta(path):
    """Yields term meta rows: (expression, mode, data)."""
    with zipfile.ZipFile(path) as z:
        for name in _banks(z, "term_meta_bank_"):
            for row in json.loads(_read(z, name)):
                if len(row) >= 3:
                    yield row[0], row[1], row[2]


def dictionary_kind(path):
    """'terms', 'pitch', 'freq' or 'unknown' - by peeking at the banks."""
    with zipfile.ZipFile(path) as z:
        if _banks(z, "term_bank_"):
            return "terms"
        metas = _banks(z, "term_meta_bank_")
        if metas:
            rows = json.loads(_read(z, metas[0]))
            modes = {r[1] for r in rows[:200] if len(r) >= 2}
            if "pitch" in modes:
                return "pitch"
            if "freq" in modes:
                return "freq"
    return "unknown"


# ---------------------------------------------------------------------------
# structured content helpers
# ---------------------------------------------------------------------------
def _items(glossary):
    """Normalizes a glossary list into structured-content nodes / strings."""
    out = []
    for g in glossary or []:
        if isinstance(g, str):
            out.append(g)
        elif isinstance(g, dict):
            t = g.get("type")
            if t == "structured-content":
                out.append(g.get("content"))
            elif t == "text":
                out.append(g.get("text", ""))
            # images and other types are skipped
        elif isinstance(g, list):  # deinflection-style glossary [term, [rules]]
            continue
    return out


def _data(node, key):
    d = node.get("data") if isinstance(node, dict) else None
    return d.get(key) if isinstance(d, dict) else None


def _children(node):
    c = node.get("content") if isinstance(node, dict) else None
    if c is None:
        return []
    return c if isinstance(c, list) else [c]


def text_of(node, skip_rt=True):
    """Plain text of a node (ruby readings dropped)."""
    if node is None:
        return ""
    if isinstance(node, str):
        return node
    if isinstance(node, list):
        return "".join(text_of(n, skip_rt) for n in node)
    if isinstance(node, dict):
        tag = node.get("tag")
        if tag in ("rt", "rp") and skip_rt:
            return ""
        if tag == "img":
            return ""
        if tag == "br":
            return "\n"
        return "".join(text_of(c, skip_rt) for c in _children(node))
    return ""


def find_all(node, pred, out=None):
    if out is None:
        out = []
    if isinstance(node, list):
        for n in node:
            find_all(n, pred, out)
    elif isinstance(node, dict):
        if pred(node):
            out.append(node)
            return out
        for c in _children(node):
            find_all(c, pred, out)
    return out


def is_jitendex_like(glossary):
    """True when the entry uses Jitendex's data-content markers (sense / glossary)."""
    items = _items(glossary)
    return bool(find_all(items, lambda n: _data(n, "content") in ("sense", "glossary")))


def is_redirect(glossary):
    items = _items(glossary)
    return bool(find_all(items, lambda n: _data(n, "content") == "redirect-glossary")) and \
        not find_all(items, lambda n: _data(n, "content") == "glossary")


def jitendex_senses(glossary):
    """-> list of (pos_codes, misc_codes, [glosses]) like JMdict senses."""
    senses = []
    items = _items(glossary)
    groups = find_all(items, lambda n: _data(n, "content") == "sense-group") or [{"content": items}]
    tag_kinds = ("part-of-speech-info", "misc-info", "field-info", "dialect-info")
    for g in groups:
        # tags that sit on the group (before the senses) apply to every sense in it
        group_pos, group_misc = [], []
        for c in _children(g):
            if isinstance(c, dict) and _data(c, "content") in tag_kinds:
                (group_pos if _data(c, "content") == "part-of-speech-info" else group_misc).append(
                    _data(c, "code") or text_of(c))
        sense_nodes = find_all(g, lambda n: _data(n, "content") == "sense")
        for s in sense_nodes:
            pos, misc = list(group_pos), list(group_misc)
            for t in find_all(s, lambda n: _data(n, "content") in tag_kinds):
                code = _data(t, "code") or text_of(t)
                (pos if _data(t, "content") == "part-of-speech-info" else misc).append(code)
            glosses = []
            for gl in find_all(s, lambda n: _data(n, "content") == "glossary"):
                lis = [c for c in _children(gl) if isinstance(c, dict) and c.get("tag") == "li"]
                glosses += [text_of(li).strip() for li in lis] if lis else [text_of(gl).strip()]
            notes = [text_of(n).strip() for n in find_all(s, lambda n: _data(n, "content") == "sense-note-content")]
            glosses = [x for x in glosses if x]
            if notes:
                glosses.append("(" + "; ".join(notes) + ")")
            if glosses:
                senses.append((pos, misc, glosses))
    return senses


# ---------------------------------------------------------------------------
# display paragraphs (what the 3DS draws: build_dict.py encodes them, see its DISPLAY RUNS)
# ---------------------------------------------------------------------------
# paragraph kinds (the 3DS spaces and indents by kind; dict.cpp derives the plain senses from SENSE / GROUP)
P_PLAIN, P_GROUP, P_SENSE, P_EX_JA, P_EX_EN, P_NOTE, P_XREF = 1, 2, 3, 4, 5, 6, 7
# text styles (colour on the 3DS); S_SMALL is added for the smaller size
S_TEXT, S_DIM, S_KEYWORD, S_LINK, S_EXAMPLE = 1, 2, 3, 4, 5
S_SMALL = 0x40
# tag (chip) categories
TAG_POS, TAG_MISC, TAG_FIELD, TAG_DIALECT, TAG_OTHER = 1, 2, 3, 4, 5
_TAG_KINDS = {"part-of-speech-info": TAG_POS, "misc-info": TAG_MISC, "field-info": TAG_FIELD,
              "dialect-info": TAG_DIALECT}
_CTRL = re.compile(r"[\x00-\x1f]+")
# ①..⑳ (Jitendex's sense numbers) as 1. .. 20.: the 3DS's system font may not have them
_CIRCLED = re.compile("[\u2460-\u2473]")


def _clean(s):
    return _CIRCLED.sub(lambda m: f"{ord(m.group(0)) - 0x245F}.", _CTRL.sub(" ", s))


class Para:
    """One paragraph: kind, indent level, items. Items: ("text", style, str), ("ruby", style, base, reading),
    ("chip", (category, code, label, description)), ("hang",) = later rows start where this item is."""
    __slots__ = ("kind", "indent", "items")

    def __init__(self, kind, indent=0):
        self.kind, self.indent, self.items = kind, indent, []

    def text(self, style, s):
        s = _clean(s)
        if not s:
            return
        if self.items and self.items[-1][0] == "text" and self.items[-1][1] == style:
            self.items[-1] = ("text", style, self.items[-1][2] + s)
        else:
            self.items.append(("text", style, s))

    def plain(self):
        return "".join(i[2] if i[0] == "text" else i[2] if i[0] == "ruby" else "" for i in self.items)

    def __repr__(self):
        return f"Para({self.kind},{self.indent},{self.items!r})"


def _chip(node):
    d = node.get("data") or {}
    code = d.get("code") or text_of(node)
    return (_TAG_KINDS.get(d.get("content"), TAG_OTHER), _clean(code), _clean(text_of(node) or code),
            _clean(node.get("title") or ""))


def _is_chip(node):
    return isinstance(node, dict) and _data(node, "content") in _TAG_KINDS


def _inline(node, p, style):
    """Appends a node's text to paragraph p: ruby kept, example keywords and links styled, chips as chips."""
    if node is None:
        return
    if isinstance(node, str):
        p.text(style, node)
        return
    if isinstance(node, list):
        for n in node:
            _inline(n, p, style)
        return
    if not isinstance(node, dict):
        return
    tag = node.get("tag")
    kind = _data(node, "content")
    if tag in ("rt", "rp", "img") or kind in ("attribution", "attribution-footnote", "graphic"):
        return
    if _is_chip(node):
        p.items.append(("chip", _chip(node)))
        return
    if tag == "br":
        p.text(style, " ")
        return
    if tag == "ruby":
        base = "".join(text_of(c) for c in _children(node) if not (isinstance(c, dict) and c.get("tag") in ("rt", "rp")))
        reading = "".join(text_of(c, skip_rt=False) for c in _children(node)
                          if isinstance(c, dict) and c.get("tag") == "rt")
        if base and reading:
            p.items.append(("ruby", style, _clean(base), _clean(reading)))
        else:
            p.text(style, base)
        return
    small = style & S_SMALL
    if kind == "example-keyword":
        style = S_KEYWORD | small
    elif tag == "a" and not str(node.get("href", "")).startswith(("http://", "https://")):
        style = S_LINK | small
    elif isinstance(node.get("style"), dict) and node["style"].get("fontWeight") in ("bold", 700, "700"):
        style = S_KEYWORD | small
    for c in _children(node):
        _inline(c, p, style)


def _extra_box(box, out, indent):
    """Jitendex's extra boxes under a sense: examples, notes, cross-references."""
    kind = _data(box, "content")
    if kind == "example-sentence":
        a = find_all(box, lambda n: _data(n, "content") == "example-sentence-a")
        b = find_all(box, lambda n: _data(n, "content") == "example-sentence-b")
        if a:
            p = Para(P_EX_JA, indent)
            _inline(_children(a[0]), p, S_EXAMPLE)
            out.append(p)
        if b:
            p = Para(P_EX_EN, indent)
            en = find_all(b[0], lambda n: n.get("lang") == "en") or _children(b[0])
            _inline(en, p, S_DIM | S_SMALL)
            out.append(p)
    elif kind in ("xref", "antonym"):
        p = Para(P_XREF, indent)
        label = find_all(box, lambda n: _data(n, "content") == "reference-label")
        content = find_all(box, lambda n: _data(n, "content") in ("xref-content", "antonym-content"))
        gloss = find_all(box, lambda n: _data(n, "content") in ("xref-glossary", "antonym-glossary"))
        if label:
            p.text(S_DIM | S_SMALL, text_of(label[0]).strip() + " ")
        for c in (_children(content[0]) if content else []):
            if not (isinstance(c, dict) and _data(c, "content") == "reference-label"):
                _inline(c, p, S_LINK)
        if gloss:
            p.text(S_DIM | S_SMALL, " · " + text_of(gloss[0]).strip())
        out.append(p)
    elif kind in ("sense-note", "lang-source", "info-gloss") or kind:
        if kind in ("graphic",):
            return
        label = find_all(box, lambda n: str(_data(n, "content") or "").endswith("-label"))
        content = find_all(box, lambda n: str(_data(n, "content") or "").endswith("-content"))
        p = Para(P_NOTE, indent)
        if label:
            p.text(S_DIM | S_SMALL, text_of(label[0]).strip() + ": ")
        _inline(content[0] if content else box, p, S_TEXT | S_SMALL)
        if p.items:
            out.append(p)


def _jitendex_sense(s, marker, out):
    p = Para(P_SENSE, 0)
    if marker:
        p.text(S_DIM, marker + " ")
        p.items.append(("hang",))
    for c in _children(s):  # the sense's own tags (misc, field, dialect)
        if _is_chip(c):
            p.items.append(("chip", _chip(c)))
    glosses = []
    for gl in find_all(s, lambda n: _data(n, "content") == "glossary"):
        lis = [c for c in _children(gl) if isinstance(c, dict) and c.get("tag") == "li"]
        glosses += lis or [gl]
    for i, g in enumerate(glosses):
        if i:
            p.text(S_TEXT, "; ")
        _inline(_children(g) if isinstance(g, dict) else g, p, S_TEXT)
    if p.items:
        out.append(p)
    for extra in find_all(s, lambda n: _data(n, "content") == "extra-info"):
        for box in find_all(extra, lambda n: isinstance(n, dict) and (_data(n, "class") == "extra-box" or
                                                                      _data(n, "content") == "graphic")):
            _extra_box(box, out, 1)


def jitendex_paragraphs(glossary):
    out = []
    items = _items(glossary)
    for g in find_all(items, lambda n: _data(n, "content") == "sense-group"):
        chips = [c for c in _children(g) if _is_chip(c)]
        if chips:
            p = Para(P_GROUP, 0)
            for c in chips:
                p.items.append(("chip", _chip(c)))
            out.append(p)
        for s in find_all(g, lambda n: _data(n, "content") == "sense"):
            st = s.get("style") if isinstance(s.get("style"), dict) else {}
            marker = str(st.get("listStyleType", "")).strip("\"'") if st else ""
            _jitendex_sense(s, marker, out)
    return out


def generic_paragraphs(glossary, max_paras=200):
    """Any dictionary's structured content: a paragraph per block element, list items numbered or bulleted
    (nested lists indented), tables a row per paragraph, ruby, bold and links kept."""
    out = []

    def walk(node, cur, indent):
        """cur: the open paragraph (or None). Returns the paragraph open after the node."""
        if node is None:
            return cur
        if isinstance(node, (str, list)) or (isinstance(node, dict) and node.get("tag") not in _BLOCK):
            if isinstance(node, list):
                for n in node:
                    cur = walk(n, cur, indent)
                return cur
            if cur is None:
                cur = Para(P_PLAIN, indent)
                out.append(cur)
            if isinstance(node, str):
                for k, part in enumerate(node.split("\n")):
                    if k:
                        cur = Para(P_PLAIN, indent)
                        out.append(cur)
                    cur.text(S_TEXT, part)
            else:
                _inline(node, cur, S_TEXT)
            return cur
        tag = node.get("tag")
        if _data(node, "content") in ("attribution",):
            return cur
        if tag == "br":
            return None
        if tag in ("ol", "ul"):
            n = 0
            for li in _children(node):
                if isinstance(li, dict) and li.get("tag") == "li":
                    n += 1
                    p = Para(P_PLAIN, indent)
                    st = li.get("style") if isinstance(li.get("style"), dict) else {}
                    mark = str(st.get("listStyleType", "")).strip("\"'") if st else ""
                    if not mark or mark in ("decimal", "disc", "circle", "square", "none"):
                        mark = f"{n}." if tag == "ol" else "•" if mark != "none" else ""
                    if mark:
                        p.text(S_DIM, mark + " ")
                        p.items.append(("hang",))
                    out.append(p)
                    walk(_children(li), p, indent + 1)
                else:
                    walk(li, None, indent)
            return None
        if tag == "tr":
            p = Para(P_PLAIN, indent)
            out.append(p)
            for k, cell in enumerate(_children(node)):
                if k:
                    p.text(S_TEXT, "  ")
                _inline(cell, p, S_DIM if isinstance(cell, dict) and cell.get("tag") == "th" else S_TEXT)
            return None
        cur = None  # div, p, table, details ...: a paragraph of their own
        for c in _children(node):
            cur = walk(c, cur, indent)
        return None

    for item in _items(glossary):
        walk(item, None, 0)
    return [p for p in out if p.items and p.plain().strip() or any(i[0] == "chip" for i in p.items)][:max_paras]


# Yomitan tag categories -> chip colours (yomitan.TAG_*): partOfSpeech blue, name types and the rest grey, ...
_TAG_CATEGORY = {"partOfSpeech": TAG_POS, "pos": TAG_POS, "name": TAG_OTHER, "archaism": TAG_MISC, "popular": TAG_MISC,
                 "frequent": TAG_MISC, "expression": TAG_MISC, "dictionary": TAG_OTHER, "field": TAG_FIELD,
                 "dialect": TAG_DIALECT}


def tag_chips(names, tags):
    """A row's definition tags (space-separated names) as chips, described by the tag bank."""
    out = []
    for name in (names or "").replace("\u00a0", " ").split():
        cat, notes = tags.get(name, ("", ""))
        if name == "\u2605":
            continue
        out.append(("chip", (_TAG_CATEGORY.get(cat, TAG_OTHER), _clean(name), _clean(name), _clean(notes))))
    return out


def rows_paragraphs(rows, tags):
    """Several term rows of one entry (other dictionaries than Jitendex: a row per definition), each with its
    definition tags as chips over its content. rows: [(def_tags, glossary)]."""
    out, seen = [], set()
    for def_tags, glossary in rows:
        key = json.dumps(glossary, ensure_ascii=False, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        chips = tag_chips(def_tags, tags)
        paras = display_paragraphs(glossary)
        if chips:
            if paras and paras[0].kind in (P_PLAIN, P_SENSE) and not any(i[0] == "hang" for i in paras[0].items):
                paras[0].items[:0] = chips  # on the definition's first line
            else:
                g = Para(P_GROUP)
                g.items = chips
                out.append(g)
        out += paras
    return out


def display_paragraphs(glossary):
    """-> [Para]: what the 3DS shows for one entry's glossary. Several definitions of plain text are numbered, as
    Yomitan lists them."""
    if is_jitendex_like(glossary):
        return jitendex_paragraphs(glossary)
    items = _items(glossary)
    if len(items) > 1 and all(isinstance(i, str) and "\n" not in i for i in items):
        out = []
        for n, text in enumerate(items, 1):
            p = Para(P_SENSE)
            p.text(S_DIM, f"{n}. ")
            p.items.append(("hang",))
            p.text(S_TEXT, text)
            out.append(p)
        return out
    return generic_paragraphs(glossary)


_BLOCK = {"div", "li", "ol", "ul", "p", "tr", "table", "details", "summary", "br"}


def plain_lines(glossary, max_lines=14):
    """Generic flattening for any dictionary (monolingual etc.): one line per block element."""
    lines = []

    def walk(node, buf):
        if node is None:
            return
        if isinstance(node, str):
            buf.append(node)
            return
        if isinstance(node, list):
            for n in node:
                walk(n, buf)
            return
        if isinstance(node, dict):
            tag = node.get("tag")
            if tag in ("rt", "rp", "img"):
                return
            if tag in _BLOCK:
                flush(buf)
                for c in _children(node):
                    walk(c, buf)
                flush(buf)
            else:
                for c in _children(node):
                    walk(c, buf)

    def flush(buf):
        s = re.sub(r"\s+", " ", "".join(buf)).strip()
        if s:
            lines.append(s)
        buf.clear()

    for item in _items(glossary):
        buf = []
        if isinstance(item, str):
            for part in item.split("\n"):
                buf.append(part)
                flush(buf)
        else:
            walk(item, buf)
            flush(buf)
    return lines[:max_lines]


# ---------------------------------------------------------------------------
# HTML rendering (same markup Yomitan sends to Anki)
# ---------------------------------------------------------------------------
_HTML_TAGS = {"ruby", "rt", "rp", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "div", "span",
              "ol", "ul", "li", "details", "summary", "br", "a", "img"}
_STYLE_UNITLESS = {"fontWeight", "lineHeight", "opacity", "zIndex", "flexGrow", "flexShrink"}


def _css_name(k):
    return re.sub(r"[A-Z]", lambda m: "-" + m.group(0).lower(), k)


def _style(style):
    if not isinstance(style, dict):
        return ""
    parts = []
    for k, v in style.items():
        if isinstance(v, (int, float)) and k not in _STYLE_UNITLESS and v != 0:
            v = f"{v}em"
        if isinstance(v, list):
            v = " ".join(str(x) for x in v)
        parts.append(f"{_css_name(k)}:{v}")
    return ";".join(parts)


def render_html(node):
    if node is None:
        return ""
    if isinstance(node, str):
        return html.escape(node, quote=False).replace("\n", "<br>")
    if isinstance(node, list):
        return "".join(render_html(n) for n in node)
    if not isinstance(node, dict):
        return ""
    tag = node.get("tag")
    if tag not in _HTML_TAGS:
        return render_html(node.get("content"))
    if tag == "br":
        return "<br>"
    if tag == "img":
        return ""  # dictionary images aren't copied to Anki
    attrs = []
    if tag == "a":
        href = node.get("href", "")
        if not href.startswith(("http://", "https://")):
            # internal Yomitan search link: keep the text only
            return f'<span class="gloss-link">{render_html(node.get("content"))}</span>'
        attrs.append(f'href="{html.escape(href)}"')
    data = node.get("data")
    if isinstance(data, dict):
        for k, v in data.items():
            attrs.append(f'data-sc-{_css_name(k)}="{html.escape(str(v))}"')
    for k in ("lang", "title"):
        if node.get(k):
            attrs.append(f'{k}="{html.escape(str(node[k]))}"')
    st = _style(node.get("style"))
    if st:
        attrs.append(f'style="{html.escape(st)}"')
    for k in ("colSpan", "rowSpan"):
        if node.get(k):
            attrs.append(f'{k.lower()}="{node[k]}"')
    a = (" " + " ".join(attrs)) if attrs else ""
    return f"<{tag}{a}>{render_html(node.get('content'))}</{tag}>"


def scope_css(css, scope):
    """Prefixes every top-level selector with `scope` (keeps nested blocks intact)."""
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    out, i, depth, start = [], 0, 0, 0
    sel_start = 0
    while i < len(css):
        ch = css[i]
        if ch == "{":
            if depth == 0:
                sel = css[sel_start:i].strip()
                if sel.startswith("@"):
                    out.append(sel + "{")
                else:
                    out.append(",".join(f"{scope} {s.strip()}" for s in sel.split(",") if s.strip()) + "{")
                start = i + 1
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                out.append(css[start:i] + "}")
                sel_start = i + 1
        i += 1
    return "".join(out)


def glossary_html(glossary, dict_title, def_tags="", css=""):
    """Markup like Yomitan's single-glossary for one entry (what Lapis' MainDefinition expects)."""
    tags = ", ".join([t for t in (def_tags or "").split() if t] + [dict_title])
    tags = tags.replace(" ", " ")
    body = []
    for item in _items(glossary):
        body.append(render_html(item))
    content = "".join(body) if len(body) <= 1 else "<ul>" + "".join(f"<li>{b}</li>" for b in body) + "</ul>"
    style = ""
    if css:
        style = "<style>" + scope_css(css, f'.yomitan-glossary [data-dictionary="{dict_title}"]') + "</style>"
    return (f'<div style="text-align: left;" class="yomitan-glossary"><ol>'
            f'<li data-dictionary="{html.escape(dict_title)}"><i>({html.escape(tags)})</i> '
            f"<span>{content}</span></li></ol>{style}</div>")
