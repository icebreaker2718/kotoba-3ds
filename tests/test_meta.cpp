// Pitch accent and frequency files (.ktm): every record found through the index, keys that aren't there not found,
// lookups take no memory, pitch patterns split into morae as Yomitan shows them, and the lookup screen shows them.
//   test_meta dict.ktd file.ktm [file.ktm ...]
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
#include "core/meta_dict.hpp"
#include "core/ui_text.hpp"

using namespace kt;

static unsigned long g_news = 0;
void *operator new(size_t n) {
    g_news++;
    if (void *p = malloc(n ? n : 1)) return p;
    abort();
}
void *operator new[](size_t n) {
    g_news++;
    if (void *p = malloc(n ? n : 1)) return p;
    abort();
}
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
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

static std::string pieces(const std::string &reading, int pos) {
    PitchPiece p[16];
    const int n = pitchPieces(reading, pos, p, 16);
    std::string out;
    for (int i = 0; i < n; i++)
        out += (p[i].high ? "^" : "_") + reading.substr(p[i].off, p[i].len) + (p[i].drop ? "\\" : "") + " ";
    return out;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_meta dict.ktd file.ktm [file.ktm ...]\n");
        return 2;
    }
    // pitch patterns: _ low, ^ high, \ the drop after it
    check(pieces("たべる", 2) == "_た ^べ\\ _る ", "たべる [2]: " + pieces("たべる", 2));
    check(pieces("さかな", 0) == "_さ ^かな ", "さかな [0]: " + pieces("さかな", 0));
    check(pieces("いのち", 1) == "^い\\ _のち ", "いのち [1]: " + pieces("いのち", 1));
    check(pieces("おとうと", 4) == "_お ^とうと\\ ", "おとうと [4]: " + pieces("おとうと", 4));
    check(pieces("きゃく", 0) == "_きゃ ^く ", "きゃく [0] (small kana in the mora): " + pieces("きゃく", 0));
    check(pieces("しゃかい", 1) == "^しゃ\\ _かい ", "しゃかい [1]: " + pieces("しゃかい", 1));
    check(moraCount("きょう") == 2 && moraCount("がっこう") == 4, "morae: きょう 2, がっこう 4");
    check(groupDigits("1234567") == "1,234,567" && groupDigits("123") == "123" && groupDigits("12㋕") == "12㋕" && groupDigits("13390㋕") == "13,390㋕" && groupDigits("x123456") == "x123456",
          "digits grouped");

    FileReader dr;
    Dictionary d;
    std::string err;
    check(dr.open(argv[1]) && d.open(&dr, err), std::string("dictionary opens ") + err);
    std::vector<FileReader *> readers;
    MetaSet set;
    for (int a = 2; a < argc; a++) {
        FileReader *r = new FileReader();
        check(r->open(argv[a]) && set.add(r, err), std::string("opens ") + argv[a] + " " + err);
        readers.push_back(r);
    }
    // every record, read straight from the blocks, found through the index; a key just past each not found
    for (int i = 0; i < set.count(); i++) {
        FileReader &r = *readers[i];
        uint8_t h[64];
        r.read(0, h, 64);
        auto u32 = [&](int o) { return (uint32_t)(h[o] | (h[o + 1] << 8) | (h[o + 2] << 16) | ((uint32_t)h[o + 3] << 24)); };
        const uint32_t blocks = u32(20), dataOff = u32(24);
        std::vector<char> b(4096);
        char out[256];
        size_t records = 0, found = 0, missing = 0, wrongly = 0;
        unsigned long news = 0;
        for (uint32_t k = 0; k < blocks; k++) {
            r.read(dataOff + k * 4096, b.data(), 4096);
            for (size_t p = 0; p < 4096 && b[p];) {
                const std::string key(&b[p]);
                const std::string val(&b[p + key.size() + 1]);
                p += key.size() + val.size() + 2;
                records++;
                const size_t tab = key.find('\t');
                const std::string expr = key.substr(0, tab), rd = key.substr(tab + 1);
                // through MetaFile::find with exactly this key (MetaSet::find also tries the expression alone)
                char seg[64 * 1024], blk[4096];
                const unsigned long n0 = g_news;
                const bool ok = const_cast<MetaFile &>(set.file(i)).find(key.data(), key.size(), out, sizeof(out), seg,
                                                                           sizeof(seg), blk);
                news += g_news - n0;
                if (ok && val.compare(0, sizeof(out) - 1, out) == 0) found++;
                else if (missing++ < 3) printf("     not found: %s\n", key.c_str());
                const std::string past = key + "\x01";
                if (const_cast<MetaFile &>(set.file(i)).find(past.data(), past.size(), out, sizeof(out), seg, sizeof(seg), blk))
                    wrongly++;
                (void)expr;
                (void)rd;
            }
        }
        printf("     %s (%s): %zu records\n", set.file(i).title().c_str(), set.file(i).kind() == MetaFile::PITCH ? "pitch" : "freq", records);
        check(records == set.file(i).records() && found == records,
              set.file(i).title() + ": every record found (" + std::to_string(found) + " of " + std::to_string(records) + ")");
        check(wrongly == 0, set.file(i).title() + ": keys that aren't there not found");
        check(news == 0, set.file(i).title() + ": lookups take no memory (" + std::to_string(news) + " allocations)");
    }

    // the lookup screen: 食べる with its pitch and frequencies
    {
        BoxFont font;
        TextRenderer tr(font);
        LookupView v(d, tr);
        v.setMeta(&set);
        v.setText({"食べる"});
        const EntryLayout &l = v.entryLayout();
        std::string all, deco;
        for (int row = 0; row < l.rowCount() && row < 8; row++) {
            for (int k = l.row(row).first; k < l.row(row).first + l.row(row).count; k++) {
                const EntryLayout::Span &s = l.span(k);
                const std::string t(l.text(s), s.len);
                all += t;
                if (s.deco) deco += (s.deco & EntryLayout::D_OVER ? "^" : "") + t + (s.deco & EntryLayout::D_DROP ? "\\" : "") + " ";
            }
            all += "\n";
        }
        printf("%s", all.c_str());
        const bool pitch = set.count(MetaFile::PITCH) > 0, freq = set.count(MetaFile::FREQ) > 0;
        if (pitch) check(deco.find("^べ\\") != std::string::npos && all.find("[2]") != std::string::npos,
                         "食べる: たべる with べ high and the drop after it, [2] (" + deco + ")");
        std::string freqTitle;
        for (int i = 0; i < set.count(); i++)
            if (set.file(i).kind() == MetaFile::FREQ) freqTitle = set.file(i).title();
        if (freq) check(all.find(freqTitle + ": ") != std::string::npos && all.find(" #") == std::string::npos,
                        "食べる: the frequency line, no built-in #rank");
    }
    // as the plugin does it: the lookup screen opens on the word first, then gets the files (RunLookupView). The
    // entry must be laid out again with them (on the console it wasn't, until Select: 2026-11-26)
    if (set.count()) {
        BoxFont font;
        TextRenderer tr(font);
        LookupView v(d, tr);
        v.setText({"食べる"});
        auto shown = [&]() {
            std::string all;
            const EntryLayout &l = v.entryLayout();
            for (int row = 0; row < l.rowCount() && row < 8; row++)
                for (int k = l.row(row).first; k < l.row(row).first + l.row(row).count; k++)
                    all += std::string(l.text(l.span(k)), l.span(k).len);
            return all;
        };
        const std::string before = shown();
        v.setMeta(&set);
        const std::string after = shown();
        check(after != before && after.find(set.file(0).title()) != std::string::npos,
              "files given after the word was shown: the entry shows them at once");
    }
    for (FileReader *r : readers) delete r;
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
