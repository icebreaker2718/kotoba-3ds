"""Checks that the JMdict XML loader and the jamdict DB loader produce identical entries.
Writes a JMdict-format XML (with DTD entities, like the real file) for a sample of entries
taken from the DB, then loads it back through load_jmdict_xml().
    python3 tests/test_builder.py path/to/jamdict.db
"""
import gzip, os, random, sys, tempfile
from xml.sax.saxutils import escape
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import build_dict as b

db = sys.argv[1]
entries = {e.idseq: e for e in b.load_jamdict_db(db)}
random.seed(1)
sample = random.sample(sorted(entries), 3000) + [1358280]  # + 食べる
code2desc = {}
for d, c in b.DESC2CODE.items():
    code2desc.setdefault(c, d)
def ent(c):
    if c not in code2desc:
        code2desc[c] = c
    return f"&{c};"

body = []
for i in sample:
    e = entries[i]
    x = [f"<entry>\n<ent_seq>{i}</ent_seq>"]
    for t, inf, pri in e.kanji:
        x.append("<k_ele><keb>%s</keb>%s%s</k_ele>" % (escape(t), "".join(f"<ke_inf>{ent(c)}</ke_inf>" for c in inf),
                                                       "".join(f"<ke_pri>{p}</ke_pri>" for p in pri)))
    for t, inf, pri, nok in e.kana:
        x.append("<r_ele><reb>%s</reb>%s%s%s</r_ele>" % (escape(t), "<re_nokanji/>" if nok else "",
                 "".join(f"<re_inf>{ent(c)}</re_inf>" for c in inf), "".join(f"<re_pri>{p}</re_pri>" for p in pri)))
    prev = None
    for pos, misc, gl in e.senses:
        x.append("<sense>%s%s%s</sense>" % ("".join(f"<pos>{ent(c)}</pos>" for c in pos) if pos != prev else "",
                 "".join(f"<misc>{ent(c)}</misc>" for c in misc), "".join(f"<gloss>{escape(g)}</gloss>" for g in gl)))
        prev = pos
    x.append("</entry>")
    body.append("\n".join(x))
dtd = "\n".join(f'<!ENTITY {c} "{escape(d)}">' for c, d in code2desc.items())
xml = f'<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE JMdict [\n<!ELEMENT JMdict (entry*)>\n{dtd}\n]>\n<JMdict>\n' + "\n".join(body) + "\n</JMdict>\n"
path = os.path.join(tempfile.mkdtemp(), "JMdict_e.gz")
with gzip.open(path, "wt", encoding="utf-8") as f:
    f.write(xml)
got = {e.idseq: e for e in b.load_jmdict_xml(path)}
bad = 0
for i in sample:
    want, have = b.entry_blob(entries[i]), b.entry_blob(got[i]) if i in got else None
    if want != have:
        bad += 1
        if bad <= 3:
            print("MISMATCH", i, "\n", want, "\n---\n", have)
print(f"{len(sample) - bad}/{len(sample)} entries identical via XML path")
print(b.entry_blob(got[1358280]))
sys.exit(1 if bad else 0)
