// Native tests for the platform-independent plugin core.
//   make -C tests DICT=/path/to/dict.bin
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/deinflect.hpp"
#include "core/dict.hpp"
#include "core/kana.hpp"
#include "core/search.hpp"

using namespace kt;

class FileReader : public Reader {
public:
    explicit FileReader(const char *path) { f_ = fopen(path, "rb"); if (f_) { fseek(f_, 0, SEEK_END); sz_ = ftell(f_); } }
    ~FileReader() { if (f_) fclose(f_); }
    bool read(uint32_t off, void *dst, uint32_t len) override {
        reads++;
        return f_ && fseek(f_, off, SEEK_SET) == 0 && fread(dst, 1, len, f_) == len;
    }
    uint32_t size() override { return sz_; }
    int reads = 0;
private:
    FILE *f_ = nullptr;
    uint32_t sz_ = 0;
};

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void testRomaji() {
    struct { const char *in, *out; } cases[] = {
        {"taberu", "たべる"}, {"tabesaserarenakatta", "たべさせられなかった"},
        {"gakkou", "がっこう"}, {"shinbun", "しんぶん"}, {"konnichiha", "こんにちは"},
        {"kon'ya", "こんや"}, {"kinyuu", "きにゅう"}, {"kinnyuu", "きんにゅう"}, {"shinnen", "しんねん"}, {"nn", "ん"}, {"kanji", "かんじ"},
        {"matcha", "まっちゃ"}, {"chotto", "ちょっと"}, {"jibun", "じぶん"}, {"zyuu", "じゅう"},
        {"tsukue", "つくえ"}, {"fu-do", "ふーど"}, {"ONNA", "おんな"}, {"hon", "ほん"},
        {"食beta", "食べた"}, {"xtu", "っ"}, {"kyouto", "きょうと"}, {"dezain", "でざいん"},
        {"wo", "を"}, {"vu", "ゔ"}, {"ryokou", "りょこう"}, {"sshi", "っし"},
    };
    for (auto &c : cases) {
        std::string got = romajiToKana(c.in);
        CHECK(got == c.out, "romaji %s -> %s (want %s)", c.in, got.c_str(), c.out);
    }
    CHECK(romajiToKana("tabek", true) == "たべk", "partial input kept");
    CHECK(romajiToKana("hon", false) == "ほn", "final n deferred while typing");
    CHECK(normalizeKey("カタカナ・ラーメン") == "かたかな・らーめん", "katakana -> hiragana");
    std::string s = "食べた";
    utf8PopBack(s);
    CHECK(s == "食べ", "utf8PopBack");
}

static bool hasCand(const std::vector<Candidate> &v, const char *text) {
    for (auto &c : v) if (c.text == text) return true;
    return false;
}

static void testDeinflect() {
    struct { const char *in, *base; } cases[] = {
        {"たべられなかった", "たべる"}, {"かかれていました", "かく"}, {"いった", "いく"},
        {"よんでいる", "よむ"}, {"たかくなかった", "たかい"}, {"べんきょうした", "べんきょうする"},
        {"こなかった", "くる"}, {"来なかった", "来る"}, {"しずかな", "しずか"},
        {"かかされた", "かく"}, {"たべちゃった", "たべる"}, {"のんじゃう", "のむ"},
        {"たべたかった", "たべる"}, {"はなせる", "はなす"}, {"ください", "くださる"}, {"いらっしゃい", "いらっしゃる"},
    };
    for (auto &c : cases) {
        auto v = deinflect(c.in);
        CHECK(hasCand(v, c.base), "deinflect %s should reach %s (%zu candidates)", c.in, c.base, v.size());
    }
}

static const Result *findResult(const SearchOutput &so, const char *expr, Dictionary &d, Entry *out) {
    for (auto &r : so.results) {
        Entry e;
        if (d.entry(r.entryId, e) && expressionFor(e, r.matchedKey) == expr) {
            if (out) *out = e;
            return &r;
        }
    }
    return nullptr;
}

static void testSearch(Dictionary &d, FileReader &fr) {
    struct { const char *q, *expr; int maxRank; } cases[] = {
        {"taberu", "食べる", 0},
        {"taberarenakatta", "食べる", 2},
        {"tabeta", "食べる", 2},
        {"kakareteimashita", "書く", 3},
        {"itta", "行く", 6},
        {"benkyoushita", "勉強", 2},
        {"takakunakatta", "高い", 3},
        {"shizukana", "静か", 2},
        {"kita", "来る", 6},
        {"食べた", "食べる", 0},
        {"勉", "勉強", 3},
        {"ra-men", "ラーメン", 1},
        {"nihongo", "日本語", 0},
        {"yomeru", "読む", 3},
    };
    for (auto &c : cases) {
        fr.reads = 0;
        clock_t t0 = clock();
        SearchOutput so = search(d, c.q);
        double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
        int rank = -1;
        for (size_t i = 0; i < so.results.size(); i++) {
            Entry e;
            d.entry(so.results[i].entryId, e);
            if (expressionFor(e, so.results[i].matchedKey) == c.expr) { rank = (int)i; break; }
        }
        CHECK(rank >= 0 && rank <= c.maxRank, "search %s: %s at rank %d (want <= %d)", c.q, c.expr, rank, c.maxRank);
        printf("  %-20s -> %-8s rank %2d  %3zu results  %4d disk reads  %.1f ms\n", c.q, c.expr, rank,
               so.results.size(), fr.reads, ms);
        if (rank > c.maxRank || rank < 0) {
            for (size_t i = 0; i < so.results.size() && i < 8; i++) {
                Entry e;
                d.entry(so.results[i].entryId, e);
                printf("      %zu: %s  [%s]\n", i, summaryLine(e, so.results[i]).c_str(), so.results[i].reasons.c_str());
            }
        }
    }

    // details / log formatting
    SearchOutput so = search(d, "taberarenakatta");
    Entry e;
    const Result *r = findResult(so, "食べる", d, &e);
    CHECK(r != nullptr, "found 食べる");
    if (r) {
        printf("\n--- detail box ---\n%s", detailText(e, *r).c_str());
        printf("--- list line ---\n%s\n", summaryLine(e, *r).c_str());
        std::string line = mineLogLine("2026-10-19 08:30:00", "0004000000123400", e, *r, "shot - 003.bmp", "taberarenakatta");
        printf("--- mined.tsv line ---\n%s\n", line.c_str());
        int tabs = 0;
        for (char c : line) tabs += c == '\t';
        CHECK(tabs == 9, "10 columns (got %d tabs)", tabs);
        CHECK(r->reasons.find("passive") != std::string::npos, "reasons mention passive: %s", r->reasons.c_str());
    }

    // kanji typed as an alternative spelling keeps that spelling on the card
    so = search(d, "喰べる");
    r = findResult(so, "喰べる", d, &e);
    CHECK(r != nullptr, "alt kanji spelling kept as expression");
    if (r) {
        std::string line = mineLogLine("t", "tid", e, *r, "", "喰べる");
        CHECK(line.find("\t喰べる\t") != std::string::npos, "card keeps a kanji spelling that was looked up");
    }

    // a word looked up in kana goes on the card with its kanji spelling (when it has one)
    struct { const char *query, *card; } kana[] = {
        {"たべる", "食べる"}, {"ちょっと", "一寸"}, {"ありがとう", "有難う"}, {"これ", "此れ"}, {"する", "為る"}, {"ひらがな", "平仮名"},
    };
    for (auto &c : kana) {
        SearchOutput k = search(d, c.query);
        Entry ke;
        bool ok = false;
        std::string got;
        for (const Result &kr : k.results) {
            if (!d.entry(kr.entryId, ke)) continue;
            std::string col = mineLogLine("t", "tid", ke, kr, "", c.query);
            size_t a = col.find('\t', col.find('\t') + 1) + 1;
            got = col.substr(a, col.find('\t', a) - a);
            if (got == c.card) { ok = true; break; }
        }
        CHECK(ok, "%s -> card says %s (top result gave %s)", c.query, c.card, got.c_str());
    }
    // no kanji form: stays kana
    SearchOutput kk = search(d, "ここ");
    for (const Result &kr : kk.results) {
        Entry ke;
        if (d.entry(kr.entryId, ke) && ke.kanjiForms.empty()) {
            CHECK(cardExpressionFor(ke, kr.matchedKey) == ke.expression, "kana-only word stays kana: %s", ke.expression.c_str());
            break;
        }
    }
}

static void testScan(Dictionary &d, FileReader &fr) {
    struct { const char *text; int chunkByte; const char *expr; const char *surface; } cases[] = {
        {"もう何も食べられなかった。", 12, "食べる", "食べられなかった"},
        {"もう何も食べられなかった。", 6, "何も", "何も"},
        {"この先に宝箱があるらしいぞ。", 12, "宝箱", "宝箱"},
        {"この先に宝箱があるらしいぞ。", 21, "ある", "ある"},
        {"勉強しなければならない", 0, "勉強", "勉強しなければ"},
        {"読んでおいた本", 0, "読む", "読んでおいた"},
        {"見せてください", 0, "見せる", "見せて"},
        {"「ちょっと待って！」", 3, "ちょっと", "ちょっと"},
        {"キレイなお花ですね", 0, "綺麗", "キレイな"},
    };
    for (auto &c : cases) {
        fr.reads = 0;
        clock_t t0 = clock();
        SearchOutput so = scanAt(d, c.text, c.chunkByte);
        double ms = 1000.0 * (clock() - t0) / CLOCKS_PER_SEC;
        int rank = -1;
        std::string surf;
        for (size_t i = 0; i < so.results.size(); i++) {
            Entry e;
            d.entry(so.results[i].entryId, e);
            if (expressionFor(e, so.results[i].matchedKey) == c.expr) { rank = (int)i; surf = so.results[i].surface; break; }
        }
        printf("  scan %-28s @%-2d -> %-6s rank %2d surface %-14s %3zu results %4d reads %.1f ms\n", c.text,
               c.chunkByte, c.expr, rank, surf.c_str(), so.results.size(), fr.reads, ms);
        CHECK(rank >= 0 && rank <= 2, "scan %s @%d finds %s (rank %d)", c.text, c.chunkByte, c.expr, rank);
        if (rank >= 0) {
            CHECK(surf == c.surface, "surface %s want %s", surf.c_str(), c.surface);
        } else {
            for (size_t i = 0; i < so.results.size() && i < 6; i++) {
                Entry e; d.entry(so.results[i].entryId, e);
                printf("      %zu: %s [%s] %s\n", i, summaryLine(e, so.results[i]).c_str(), so.results[i].surface.c_str(), so.results[i].reasons.c_str());
            }
        }
    }
}

static void testRadicals(Dictionary &d) {
    const auto &rads = d.radicals();
    CHECK(rads.size() > 200, "radicals loaded (%zu)", rads.size());
    int water = -1, blue = -1;
    for (size_t i = 0; i < rads.size(); i++) {
        if (rads[i].glyph == "汁") water = (int)i;
        if (rads[i].glyph == "青") blue = (int)i;
    }
    CHECK(water >= 0 && blue >= 0, "found water/blue radicals");
    if (water >= 0 && blue >= 0) {
        std::vector<std::string> ks;
        uint32_t total;
        d.kanjiWithRadicals({water, blue}, 50, ks, total);
        std::string all;
        for (auto &k : ks) all += k;
        printf("  water+blue -> %s (%u total), label '%s'\n", all.c_str(), total, rads[water].label.c_str());
        CHECK(all.find("清") != std::string::npos, "清 contains water+blue");
        CHECK(!ks.empty() && ks[0] == "清", "most common first (got %s)", ks.empty() ? "" : ks[0].c_str());
    }
}

int main(int argc, char **argv) {
    const char *dictPath = argc > 1 ? argv[1] : "dict.bin";
    testRomaji();
    testDeinflect();
    FileReader fr(dictPath);
    Dictionary d;
    std::string err;
    if (!d.open(&fr, err)) {
        printf("cannot open %s: %s\n", dictPath, err.c_str());
        return 1;
    }
    printf("dict: %u entries, %u keys\n", d.entryCount(), d.keyCount());
    testSearch(d, fr);
    testScan(d, fr);
    testRadicals(d);
    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
