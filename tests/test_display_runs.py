"""Version 3 dictionary files (dicts/): Yomitan structured content -> display paragraphs -> display runs.
Builds a small Yomitan zip (Jitendex-style and plain entries), writes dict.bin and a dicts/ file from it, and
checks the paragraphs, the encoding and the file. No network, no real dictionary needed.
    python3 tests/test_display_runs.py
"""
import json
import os
import struct
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import build_dict as b  # noqa: E402
import yomitan as y  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails += 1


def chip(code, label, title, kind="part-of-speech-info"):
    return {"tag": "span", "title": title, "data": {"class": "tag", "code": code, "content": kind}, "content": label}


def sense(marker, glosses, extra=None):
    node = {"tag": "li", "data": {"content": "sense"},
            "content": [{"tag": "ul", "data": {"content": "glossary"}, "content": [{"tag": "li", "content": g} for g in glosses]}]}
    if marker:
        node["style"] = {"listStyleType": f'"{marker}"'}
    if extra:
        node["content"].append({"tag": "div", "data": {"content": "extra-info"}, "content": extra})
    return node


EXAMPLE = {"tag": "div", "data": {"class": "extra-box", "content": "example-sentence"}, "content": [
    {"tag": "div", "data": {"content": "example-sentence-a"}, "content": {"tag": "span", "lang": "ja", "content": [
        {"tag": "ruby", "content": ["果", {"tag": "rt", "content": "くだ"}]}, "を",
        {"tag": "span", "data": {"content": "example-keyword"}, "content": [{"tag": "ruby", "content": ["食", {"tag": "rt", "content": "た"}]}, "べる"]}]}},
    {"tag": "div", "data": {"content": "example-sentence-b"}, "content": [
        {"tag": "span", "lang": "en", "content": "Eat fruit."}, {"tag": "span", "data": {"content": "attribution-footnote"}, "content": "[1]"}]}]}
NOTE = {"tag": "div", "data": {"class": "extra-box", "content": "sense-note"}, "content": [
    {"tag": "div", "data": {"content": "sense-note-label"}, "content": "Note"},
    {"tag": "div", "data": {"content": "sense-note-content"}, "content": "a note\twith a tab"}]}
XREF = {"tag": "div", "data": {"class": "extra-box", "content": "xref"}, "content": [
    {"tag": "div", "data": {"content": "xref-content"}, "content": [
        {"tag": "span", "data": {"content": "reference-label"}, "content": "See also"},
        {"tag": "a", "href": "?query=x", "content": "喰う"}]},
    {"tag": "div", "data": {"content": "xref-glossary"}, "content": "① to eat"}]}
JITENDEX = [{"type": "structured-content", "content": [
    {"tag": "ul", "data": {"content": "sense-groups"}, "content": [
        {"tag": "li", "data": {"content": "sense-group"}, "content": [
            chip("v1", "1-dan", "Ichidan verb"), chip("vt", "transitive", "transitive verb"),
            {"tag": "ol", "content": [sense("①", ["to eat"], [EXAMPLE, NOTE]),
                                      sense("②", ["to live on", "to subsist on"], [XREF])]}]}]},
    {"tag": "div", "data": {"content": "attribution"}, "content": "JMdict"}]}]
PLAIN = [{"type": "structured-content", "content": [
    {"tag": "ol", "content": [{"tag": "li", "content": ["一つ目", {"tag": "span", "style": {"fontWeight": "bold"}, "content": "強"}]},
                              {"tag": "li", "content": "二つ目"}]},
    {"tag": "img", "path": "x.png"}]}, "and a plain line"]

# --- paragraphs
paras = y.display_paragraphs(JITENDEX)
kinds = [p.kind for p in paras]
check(kinds == [y.P_GROUP, y.P_SENSE, y.P_EX_JA, y.P_EX_EN, y.P_NOTE, y.P_SENSE, y.P_XREF],
      f"Jitendex: group, senses, example, note, cross-reference in order ({kinds})")
check([i[1][1] for i in paras[0].items] == ["v1", "vt"], "the group's tags as chips")
check(paras[1].plain() == "1. to eat" and ("hang",) in paras[1].items, "a sense: its number (①: 1.), a hang, the glosses")
check(paras[5].plain() == "2. to live on; to subsist on", "glosses joined with ;")
ex = paras[2].items
check(("ruby", y.S_EXAMPLE, "果", "くだ") in ex and ("ruby", y.S_KEYWORD, "食", "た") in ex and ("text", y.S_KEYWORD, "べる") in ex,
      "example: furigana kept, the keyword marked")
check(paras[3].plain() == "Eat fruit.", "the translation without its footnote")
check(paras[4].plain() == "Note: a note with a tab", "a note with its label, control characters gone")
check(paras[6].plain() == "See also 喰う · 1. to eat" and paras[6].items[1][1] == y.S_LINK, "a cross-reference, its word a link")
gp = y.display_paragraphs(PLAIN)
check([p.plain() for p in gp] == ["1. 一つ目強", "2. 二つ目", "and a plain line"], f"other dictionaries: lists numbered ({[p.plain() for p in gp]})")
check(("text", y.S_KEYWORD, "強") in gp[0].items, "bold kept")

# --- encoding
tags = b.TagTable()
runs = b.encode_runs(paras, tags)
check(min(runs) >= 1, "no NUL in the runs")
dec = b.decode_runs(runs, tags.rows)
check(dec[0] == (y.P_GROUP, 0, "[1-dan][transitive]") and dec[2][2] == "果(くだ)を食(た)べる" and dec[4][1] == 1,
      f"runs decode to the paragraphs ({dec[:3]})")
many = b.TagTable()
for k in range(300):  # ids past 255 take both bytes
    many.id((1, f"c{k}", f"l{k}", ""))
p = y.Para(y.P_GROUP)
p.items = [("chip", (1, "c299", "l299", ""))]
check(b.decode_runs(b.encode_runs([p], many), many.rows)[0][2] == "[l299]", "a tag id over 255")

# --- files from a small Yomitan zip
with tempfile.TemporaryDirectory() as tmp:
    z = os.path.join(tmp, "test.zip")
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("index.json", json.dumps({"title": "Test Dict [2026-01-01]", "format": 3, "revision": "1"}))
        zf.writestr("term_bank_1.json", json.dumps([
            ["食べる", "たべる", "★", "v1", 200, JITENDEX, 100, ""],
            ["喰べる", "たべる", "", "v1", 0, [{"type": "structured-content", "content": {"tag": "div", "data": {"content": "redirect-glossary"}, "content": "食べる"}}], -100, ""],
            ["一つ", "ひとつ", "", "", 0, PLAIN, 0, ""]], ensure_ascii=False))
    entries = list(b.load_yomitan(z, keep_display=True))
    out2, out3 = os.path.join(tmp, "dict.bin"), os.path.join(tmp, "dicts", "test.ktd")
    os.makedirs(os.path.dirname(out3))
    b.build(entries, [], out2)
    b.build(entries, [], out3, version=3, title="Test Dict")
    d = open(out3, "rb").read()
    h = struct.unpack("<4s15I", d[:64])
    check(h[0] == b"KTB1" and h[1] == 3 and h[2] == 2, "version 3 header, 2 entries")
    tag_off, tag_n, title_off = h[13], h[14], h[15]
    check(d[title_off:d.index(b"\0", title_off)] == b"Test Dict", "the title")
    offs = struct.unpack(f"<{tag_n}I", d[tag_off:tag_off + 4 * tag_n])
    rows = [(d[o],) + tuple(x.decode() for x in d[o + 1:].split(b"\0")[:3]) for o in offs]
    check(rows[:2] == [(1, "v1", "1-dan", "Ichidan verb"), (1, "vt", "transitive", "transitive verb")], "the tag table")
    n, eoff = h[2], h[3]
    idx = struct.unpack(f"<{2 * (n + 1)}I", d[eoff:eoff + 8 * (n + 1)])
    rec = d[idx[0]:idx[2] - 1]
    machine, r = rec.split(b"\n", 1)
    check(machine.decode().split("\t")[:3] == ["食べる", "たべる", "v1,vt"] and "喰べる" in machine.decode(), "the machine line, redirect forms kept")
    check(b.decode_runs(r, rows)[1][2] == "1. to eat", "the record's runs")
    d2 = open(out2, "rb").read()
    check(d2[4:8] == struct.pack("<I", 2) and len(d2) > 64, "dict.bin from the same entries is still version 2")
    # JMdict-style entries (no structured content): paragraphs from the senses
    e = b.Entry(1)
    e.kana = [("は", [], [], False)]
    e.senses = [(["prt"], [], ["topic marker"]), (["prt"], ["uk"], ["contrast"])]
    dp = b.decode_runs(b.encode_runs(b.paras_from_senses(e), b.TagTable()), b.TagTable().rows or [(1, "prt", "prt", ""), (2, "uk", "uk", "")])
    check([k for k, _, _ in dp] == [y.P_GROUP, y.P_SENSE, y.P_SENSE], "JMdict senses as paragraphs")

print("FAILED" if fails else "all passed")
sys.exit(1 if fails else 0)
