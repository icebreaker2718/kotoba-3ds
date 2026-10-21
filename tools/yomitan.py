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
