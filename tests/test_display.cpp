// Version 3 dictionary files (dicts/) against dict.bin built from the same sources: the same entries, keys and
// search answers, the same senses (worked out from the display runs), and display runs for every entry of both.
//   test_display dict.bin jitendex.ktd
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>

#include "core/dict.hpp"
#include "core/entry_layout.hpp"
#include "core/kana.hpp"
#include "core/ui_text.hpp"

using namespace kt;

static int fails = 0;
static void check(bool ok, const std::string &what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}

struct FileReader : Reader {
    FILE *f = nullptr;
    uint32_t sz = 0;
    explicit FileReader(const char *p) { f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); } }
    ~FileReader() { if (f) fclose(f); }
    bool read(uint32_t o, void *d, uint32_t n) override { return f && !fseek(f, o, SEEK_SET) && fread(d, 1, n, f) == n; }
    uint32_t size() override { return sz; }
};

// "1. (v1,vt) to eat" -> "(v1,vt) to eat"
static std::string gloss(const std::string &s) {
    size_t p = s.find(". ");
    return p != std::string::npos && p < 4 ? s.substr(p + 2) : s;
}

// 24x30 cells like the 3DS font (as test_ui's): full-width characters 24 wide, ASCII 12
struct BoxFont : GlyphSource {
    int cellWidth() const override { return 24; }
    int cellHeight() const override { return 30; }
    bool metrics(uint32_t cp, GlyphInfo &gi) override {
        bool wide = cp >= 0x2E80;
        gi.left = 1;
        gi.width = wide ? 22 : 10;
        gi.advance = wide ? 24 : 12;
        return true;
    }
    bool pixels(uint32_t cp, uint8_t *out) override {
        GlyphInfo gi;
        metrics(cp, gi);
        memset(out, 0, 24 * 30);
        for (int y = 4; y < 26; y++)
            for (int x = 0; x < gi.width; x++) out[y * 24 + x] = 255;
        return true;
    }
};

// Walks display runs; false if they don't parse.
static bool parses(const char *r, size_t n, const Dictionary &d, size_t &paras) {
    paras = 0;
    for (size_t i = 0; i < n;) {
        const uint8_t c = (uint8_t)r[i];
        if (c == RUN_PARA) {
            if (i + 2 >= n || r[i + 1] < P_PLAIN || r[i + 1] > P_XREF || r[i + 2] < 1) return false;
            paras++;
        } else if (c == RUN_CHIP) {
            if (i + 2 >= n || !d.tag(((uint8_t)r[i + 1] - 1u) * 255u + ((uint8_t)r[i + 2] - 1u))) return false;
        } else if (c == RUN_RUBY) {
            const char *mid = (const char *)memchr(r + i, RUN_RUBY_MID, n - i);
            const char *end = (const char *)memchr(r + i, RUN_RUBY_END, n - i);
            if (!mid || !end || mid > end) return false;
        } else if (c == RUN_RUBY_MID || c == RUN_RUBY_END || (c < 0x20 && c > RUN_RUBY_END)) {
            return false;
        } else if (c >= 0x20 && !paras) {
            return false;  // text before any paragraph
        }
        i = runEnd(r, i, n);
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_display dict.bin file.ktd\n");
        return 2;
    }
    FileReader r2(argv[1]), r3(argv[2]);
    Dictionary d2, d3;
    std::string err;
    check(d2.open(&r2, err) && d2.version() == 2, "dict.bin opens (version 2) " + err);
    check(d3.open(&r3, err) && d3.version() == 3, "dicts/ file opens (version 3) " + err);
    if (fails) return 1;
    printf("     %s: %u entries, %u keys, %u tags\n", d3.title().c_str(), d3.entryCount(), d3.keyCount(), d3.tagCount());
    check(!d3.title().empty() && d3.tagCount() > 0, "title and tags read");
    check(d2.entryCount() == d3.entryCount() && d2.keyCount() == d3.keyCount(), "same entries and keys");

    // every entry: the machine fields, part-of-speech masks, senses, and display runs
    static char buf[16384];
    size_t fieldsDiffer = 0, maskDiffer = 0, senseCountDiffer = 0, glossDiffer = 0, glossSame = 0, noRuns2 = 0,
           noRuns3 = 0, badRuns = 0, paras = 0, biggest = 0;
    int shown = 0;
    for (uint32_t id = 0; id < d3.entryCount(); id++) {
        Entry a, b;
        if (!d2.entry(id, a) || !d3.entry(id, b)) { fieldsDiffer++; continue; }
        if (a.expression != b.expression || a.reading != b.reading || a.pos != b.pos || a.kanjiForms != b.kanjiForms ||
            a.kanaForms != b.kanaForms || a.freqRank != b.freqRank)
            fieldsDiffer++;
        if (d2.posMask(id) != d3.posMask(id)) maskDiffer++;
        // dict.bin keeps 12 senses (and a "(+N more senses)" line). Its senses lost every note but "Note" ones
        // (explanations, language of origin) and senses with only such a note: here they're kept as "; (note)" after
        // the glosses or a sense of their own. So each dict.bin sense must be the next one here with those extra notes
        // left out (and the part of speech shown again after a sense dict.bin didn't have).
        size_t n2 = a.senses.size();
        if (n2 && a.senses.back().rfind("(+", 0) == 0) n2--;
        size_t j = 0;
        bool countOk = true;
        for (size_t i = 0; i < n2; i++) {
            const std::string g2 = gloss(a.senses[i]);
            bool same = false;
            for (; j < b.senses.size() && !same; j++) {
                std::string g3 = gloss(b.senses[j]);
                same = g2 == g3;
                for (size_t cut = g3.size(); !same && (cut = g3.rfind("; (", cut - 1)) != std::string::npos && cut > 0;)
                    same = g2 == g3.substr(0, cut) || (g2.compare(0, cut, g3, 0, cut) == 0 && g2.compare(cut, 3, "; (") == 0);
                if (!same && g3.find(") (") != std::string::npos) {  // "(tags) (note)": a note-only sense
                    continue;
                }
                if (!same) break;
            }
            if (same) glossSame++;
            else {
                glossDiffer++;
                countOk = false;
                if (shown++ < 10) {
                    printf("     senses of %s differ:\n", a.expression.c_str());
                    for (auto &x : a.senses) printf("        2: %s\n", x.c_str());
                    for (auto &x : b.senses) printf("        3: %s\n", x.c_str());
                }
                break;
            }
        }
        if (!countOk) senseCountDiffer++;
        uint32_t len = 0;
        if (!d2.display(id, buf, sizeof(buf), len)) noRuns2++;
        size_t p = 0;
        if (!d3.display(id, buf, sizeof(buf), len)) noRuns3++;
        else if (!parses(buf, len, d3, p)) badRuns++;
        paras += p;
        biggest = std::max<size_t>(biggest, len);
    }
    printf("     senses alike %zu, differ %zu; %zu paragraphs; largest runs %zu bytes\n", glossSame, glossDiffer, paras,
           biggest);
    check(fieldsDiffer == 0, "every entry's forms, reading, word class, rank alike (" + std::to_string(fieldsDiffer) + " differ)");
    check(maskDiffer == 0, "every part-of-speech mask alike");
    check(senseCountDiffer * 1000 < d3.entryCount(),
          "every dict.bin sense found here, in order (" + std::to_string(senseCountDiffer) + " entries differ)");
    check(noRuns2 == 0, "display runs made for every dict.bin entry");
    check(noRuns3 == 0 && badRuns == 0, "display runs read and parse for every entry (" + std::to_string(badRuns) + " bad)");

    // every entry laid out at both panel widths (top screen 376, touchscreen 296): nothing cut short, every span
    // inside the panel (but for a word too long for a row), rows in order; the most memory any entry needs
    {
        BoxFont font;
        TextRenderer tr(font);
        EntryLayout lay(tr);
        check(lay.reserve(), "the layout's memory");
        size_t cut = 0, outside = 0, maxRows = 0, maxSpans = 0, maxPool = 0, rows = 0;
        double worst = 0, total = 0;
        uint32_t worstId = 0;
        for (int w : {376, 296}) {
            for (uint32_t id = 0; id < d3.entryCount(); id++) {
                auto t0 = std::chrono::steady_clock::now();
                lay.clear(12, w);
                const bool ok = lay.addEntry(d3, id);
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                total += us;
                if (us > worst) { worst = us; worstId = id; }
                if (!ok) cut++;
                rows += lay.rowCount();
                maxRows = std::max<size_t>(maxRows, lay.rowCount());
                maxPool = std::max<size_t>(maxPool, lay.poolUsed());
                if (lay.rowCount()) {
                    const EntryLayout::Row &last = lay.row(lay.rowCount() - 1);
                    maxSpans = std::max<size_t>(maxSpans, last.first + last.count);
                }
                for (int r = 0; r < lay.rowCount(); r++) {
                    const EntryLayout::Row &row = lay.row(r);
                    for (int k = row.first; k < row.first + row.count; k++) {
                        const EntryLayout::Span &sp = lay.span(k);
                        int sw = sp.kind == EntryLayout::K_CHIP ? sp.w : 0;
                        if (sp.kind != EntryLayout::K_CHIP)
                            for (size_t i = 0; i < sp.len;) sw += tr.advance(utf8Next(lay.text(sp), sp.len, i), sp.size);
                        // a span may run over only when it is the row's only piece (a word longer than the row)
                        if (sp.x < 12 || (sp.x + sw > 12 + w + (sp.kind == EntryLayout::K_TEXT ? 24 : 0) && row.count > 1)) {
                            if (outside++ < 3) {
                                printf("     outside: entry %u width %d row %d x %d w %d \"%.*s\"\n", id, w, r, sp.x, sw,
                                       (int)sp.len, lay.text(sp));
                            }
                        }
                    }
                }
            }
        }
        printf("     laid out %u entries x 2 widths: %zu rows; at most %zu rows, %zu spans, %zu bytes of text; "
               "mean %.1f us, slowest %.0f us (entry %u)\n", d3.entryCount(), rows, maxRows, maxSpans, maxPool,
               total / (2.0 * d3.entryCount()), worst, worstId);
        check(cut == 0, "every entry fits the layout's memory (" + std::to_string(cut) + " cut short)");
        // closing punctuation right after furigana can't start a row and may run over the edge (6 spans, all
        // in example sentences, Jitendex 2026-11-23)
        check(outside * 100000 < rows, "spans inside the panel (" + std::to_string(outside) + " outside)");
    }

    // search: the same answers for every 7th key
    size_t keysDiffer = 0, keysChecked = 0;
    {
        // the keys themselves aren't public; look up each entry's forms instead
        std::vector<uint32_t> i2, i3;
        for (uint32_t id = 0; id < d3.entryCount(); id += 7) {
            Entry e;
            if (!d3.entry(id, e, false)) continue;
            for (const std::string &f : e.kanaForms) {
                std::string k;
                for (size_t i = 0; i < f.size();) {
                    uint32_t cp = utf8Next(f, i);
                    utf8Append(k, cp >= 0x30A1 && cp <= 0x30F6 ? cp - 0x60 : cp);
                }
                d2.exact(k, i2);
                d3.exact(k, i3);
                keysChecked++;
                if (i2 != i3) keysDiffer++;
            }
        }
    }
    check(keysChecked > 10000 && keysDiffer == 0,
          "the same search answers (" + std::to_string(keysChecked) + " keys, " + std::to_string(keysDiffer) + " differ)");
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
