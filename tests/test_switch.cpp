// Switching dictionaries (Select on the lookup screen): one Dictionary object opened on one file, then another, back
// and forth, must answer exactly as a Dictionary opened fresh on each file, and after the first round the switch
// must take no memory (the plugin's heap stays as it is over a long session). The lookup view searches again in the
// new dictionary from the same character.
//   test_switch first.ktd second.ktd
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "core/dict.hpp"
#include "core/entry_layout.hpp"
#include "core/kana.hpp"
#include "core/lookup_view.hpp"
#include "core/search.hpp"
#include "core/ui_text.hpp"

using namespace kt;

static unsigned long g_news = 0;
void *operator new(size_t n) {
    g_news++;
    if (void *p = malloc(n ? n : 1)) return p;
    abort();
}
void *operator new(size_t n, const std::nothrow_t &) noexcept {
    g_news++;
    return malloc(n ? n : 1);
}
void *operator new[](size_t n) {
    g_news++;
    if (void *p = malloc(n ? n : 1)) return p;
    abort();
}
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

static int fails = 0;
static void check(bool ok, const std::string &what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}

struct FileReader : Reader {
    FILE *f = nullptr;
    uint32_t sz = 0;
    bool open(const char *p) {
        if (f) fclose(f);
        f = fopen(p, "rb");
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); }
        return f != nullptr;
    }
    ~FileReader() { if (f) fclose(f); }
    bool read(uint32_t o, void *d, uint32_t n) override { return f && !fseek(f, o, SEEK_SET) && fread(d, 1, n, f) == n; }
    uint32_t size() override { return sz; }
};

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
    bool pixels(uint32_t, uint8_t *out) override { memset(out, 0, 24 * 30); return true; }
};

// What a lookup of each text gives: entry ids, surfaces, and the first entry's senses.
static std::string answers(Dictionary &d, const std::vector<std::string> &texts) {
    std::string out;
    for (const std::string &t : texts) {
        SearchOutput so = scanAt(d, t, 0);
        out += t + ":";
        for (const Result &r : so.results) out += " " + std::to_string(r.entryId) + "/" + r.surface;
        if (!so.results.empty()) {
            Entry e;
            if (d.entry(so.results[0].entryId, e))
                for (const std::string &s : e.senses) out += " | " + s;
        }
        out += "\n";
    }
    return out;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_switch first.ktd second.ktd\n");
        return 2;
    }
    const std::vector<std::string> texts = {"食べる", "田中さん", "一ノ瀬", "東京タワー", "ながら", "鈴木", "ナルト", "ありがとう"};
    std::string want[2];
    for (int k = 0; k < 2; k++) {  // each file opened fresh
        FileReader r;
        Dictionary d;
        std::string err;
        check(r.open(argv[1 + k]) && d.open(&r, err), std::string("opens fresh: ") + argv[1 + k] + " " + err);
        want[k] = answers(d, texts);
        printf("     %s (%u entries)\n", d.title().c_str(), d.entryCount());
    }
    check(want[0] != want[1], "the two dictionaries answer differently");

    // one object, switched 200 times, with lookups in between
    FileReader r;
    Dictionary d;
    std::string err;
    bool same = true, opened = true;
    unsigned long newsInOpen = 0;
    for (int i = 0; i < 200; i++) {
        const int k = i % 2;
        const unsigned long n0 = g_news;
        opened &= r.open(argv[1 + k]) && d.open(&r, err);
        if (i >= 2) newsInOpen += g_news - n0;
        const std::string got = answers(d, texts);
        if (got != want[k]) {
            if (same) printf("     switch %d differs:\n%s---\n%s", i, got.c_str(), want[k].c_str());
            same = false;
        }
    }
    check(opened, "opens every time " + err);
    check(same, "after every switch, the same answers as a fresh open");
    check(newsInOpen == 0, "switching takes no memory after the first round (" + std::to_string(newsInOpen) +
                               " allocations in 198 opens)");

    // the lookup view: the same character looked up again in the other dictionary
    {
        BoxFont font;
        TextRenderer tr(font);
        FileReader rv;
        Dictionary dv;
        check(rv.open(argv[1]) && dv.open(&rv, err), "view's dictionary opens");
        LookupView v(dv, tr);
        v.setText({"一ノ瀬さんが食べる"});
        auto nameLine = [&](const LookupView &lv) {
            const EntryLayout &l = lv.entryLayout();
            std::string all;
            for (int row = 0; row < l.rowCount(); row++) {
                for (int s = l.row(row).first; s < l.row(row).first + l.row(row).count; s++)
                    all += std::string(l.text(l.span(s)), l.span(s).len);
                all += "\n";
            }
            return all;
        };
        v.setDictionaryPlace(0, 2);
        const std::string first = nameLine(v);
        check(first.find(dv.title() + "   1/2") != std::string::npos, "the name line shows 1/2 and Select");
        check(rv.open(argv[2]) && dv.open(&rv, err), "switched");
        v.setDictionaryPlace(1, 2);
        v.dictionaryChanged();
        const std::string second = nameLine(v);
        check(second.find("2/2") != std::string::npos && second != first, "the entry is the other dictionary's");
        check(v.selectedText().size() > 0, "a word still selected");
        printf("     first:\n%s     second:\n%s", first.substr(0, 300).c_str(), second.substr(0, 300).c_str());
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
