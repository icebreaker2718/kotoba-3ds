// Tests of the lookup screens (core/lookup_view, core/ui_text) with a made-up font of plain
// boxes: layout, tapping, moving between words and rows, paging, buttons, drawing.
//   test_ui dict.bin
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/bmp.hpp"
#include "core/kana.hpp"
#include "core/lookup_view.hpp"
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

// 24x30 cells like the 3DS font: full-width characters are 24 wide, ASCII 12, icons 25
struct BoxFont : GlyphSource {
    int cellWidth() const override { return 24; }
    int cellHeight() const override { return 30; }
    bool metrics(uint32_t cp, GlyphInfo &gi) override {
        if (cp == 0x2603) return false;  // a character the font lacks (☃)
        bool wide = cp >= 0x2E80;
        gi.left = 1;
        gi.width = wide ? 22 : 10;
        gi.advance = (cp >= 0xE000 && cp < 0xF000) ? 25 : (wide ? 24 : 12);
        return true;
    }
    bool pixels(uint32_t cp, uint8_t *out) override {
        GlyphInfo gi;
        if (!metrics(cp, gi)) return false;
        memset(out, 0, 24 * 30);
        for (int y = 4; y < 26; y++)
            for (int x = 0; x < gi.width; x++) out[y * 24 + x] = 255;
        return true;
    }
};

struct RgbCanvas : Canvas {
    std::vector<uint8_t> px;
    RgbCanvas(int w, int h) { width = w; height = h; px.assign((size_t)w * h * 3, 0); }
    void fill(int x, int y, int w, int h, Rgb c) override {
        for (int yy = std::max(0, y); yy < std::min(height, y + h); yy++)
            for (int xx = std::max(0, x); xx < std::min(width, x + w); xx++) set(xx, yy, c);
    }
    void blend(int x, int y, Rgb c, uint8_t a) override {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        uint8_t *p = &px[((size_t)y * width + x) * 3];
        p[0] = (uint8_t)((c.r * a + p[0] * (255 - a)) / 255);
        p[1] = (uint8_t)((c.g * a + p[1] * (255 - a)) / 255);
        p[2] = (uint8_t)((c.b * a + p[2] * (255 - a)) / 255);
    }
    void set(int x, int y, Rgb c) { uint8_t *p = &px[((size_t)y * width + x) * 3]; p[0] = c.r; p[1] = c.g; p[2] = c.b; }
    Rgb at(int x, int y) const { const uint8_t *p = &px[((size_t)y * width + x) * 3]; return Rgb(p[0], p[1], p[2]); }
};

static int cellOf(const LookupView &v, const char *ch, int nth = 0) {
    std::string s = ch;
    size_t i = 0;
    uint32_t cp = utf8Next(s, i);
    for (size_t k = 0; k < v.cells().size(); k++)
        if (v.cells()[k].cp == cp && nth-- == 0) return (int)k;
    return -1;
}

static bool tapCell(LookupView &v, int c) {
    const LookupView::Cell &cl = v.cells()[c];
    LookupView::Button b;
    // rows start at y = 8 on the touchscreen, 34 px each (no scrolling in these tests)
    return v.tap(cl.x + cl.w / 2, 8 + v.rows()[cl.row].y + 17, b);
}

int main(int argc, char **argv) {
    BoxFont font;
    TextRenderer tr(font);
    {   // measuring and drawing text
        check(tr.width("あいう", 30) == 72 && tr.width("abc", 30) == 36, "widths at the font's size");
        check(tr.width("あいう", 15) == 3 * 12, "widths scaled (24 * 15/30 = 12)");
        check(tr.advance(0x2603, 30) == tr.advance('?', 30), "a missing character falls back to ?");
        check(tr.advance(0xFF5E, 30) > 0, "～ is drawn (or its stand-in)");
        RgbCanvas c(100, 40);
        int end = tr.draw(c, 2, 0, "あa", 30, Rgb(255, 255, 255));
        check(end == 2 + 24 + 12, "draw returns the pen position");
        check(c.at(2 + 1 + 5, 15).r == 255 && c.at(2, 15).r == 0, "glyph pixels land after the left bearing");
        RgbCanvas s(60, 30);
        tr.draw(s, 0, 0, "あ", 15, Rgb(255, 255, 255));
        check(s.at(5, 8).r > 200 && s.at(5, 1).r == 0 && s.at(20, 8).r == 0, "scaled glyph covers half the cell");
        std::vector<std::string> w = tr.wrap("きょうはいいてんきですね", 30, 24 * 5);
        check(w.size() == 3 && w[0] == "きょうはい", "Japanese wraps by width");
        tr.draw(c, -20, -10, "あああああああ", 30, Rgb(1, 2, 3));  // clipped, no crash
    }
    {   // framebuffer pixel formats round trip (the console screens)
        const uint8_t rgb[3] = {0xF8, 0xFC, 0x08};
        for (int fmt = 0; fmt < 5; fmt++) {
            uint8_t px[4] = {0, 0, 0, 0}, back[3];
            rgbToFramebuffer(rgb, fmt, px);
            framebufferToRGB(px, 1, fmt, fmt == 0 ? 4 : (fmt == 1 ? 3 : 2), back);
            bool ok = back[0] == (fmt == 4 ? 0xF0 : 0xF8) && back[2] == (fmt == 4 ? 0x00 : 0x08);
            check(ok, "framebuffer format " + std::to_string(fmt) + " round trip");
        }
    }
    if (argc < 2) {
        printf(fails ? "%d FAILED\n" : "ALL PASSED (no dictionary: view tests skipped)\n", fails);
        return fails ? 1 : 0;
    }
    FileReader dr(argv[1]);
    Dictionary dict;
    std::string err;
    if (!dict.open(&dr, err)) { printf("dictionary: %s\n", err.c_str()); return 1; }
    LookupView v(dict, tr);
    {   // frequency ranks (dict.bin built with --freq): shown, and homographs listed most common first
        SearchOutput so = scanAt(dict, "しょうか", 0);
        std::vector<uint32_t> ranks;
        for (const Result &r : so.results) {
            if (r.surface != "しょうか") break;
            Entry e;
            if (dict.entry(r.entryId, e)) ranks.push_back(e.freqRank);
        }
        bool ranked = !ranks.empty() && ranks[0] > 0;
        if (ranked) {
            bool ordered = true;
            for (size_t i = 1; i < ranks.size(); i++)
                if (ranks[i] && ranks[i] < ranks[i - 1]) ordered = false;
            check(ordered, "homographs in frequency order (しょうか: #" + std::to_string(ranks[0]) + " first)");
            v.setText({"しょうか"});
            RgbCanvas top(400, 240);
            v.drawTop(top);  // the rank is drawn on the headword line
            check(v.result() != nullptr, "ranked entry shown");
        } else {
            printf("note: this dict.bin has no frequency ranks (built without --freq)\n");
        }
    }

    const char *line = "また明日、一緒に見に行こう！今度は友達も一緒に来るよ。楽しみだね。";
    v.setText({line});
    check(v.hasResult() && !v.selectedText().empty(), "a word is selected at the start: " + v.selectedText());
    if (getenv("UI_DEBUG"))
        for (const LookupView::Row &r : v.rows()) {
            std::string t;
            for (int i = r.first; i < r.end; i++) utf8Append(t, v.cells()[i].cp);
            printf("row y=%d [%s] last x+w=%d\n", r.y, t.c_str(), v.cells()[r.end - 1].x + v.cells()[r.end - 1].w);
        }
    check(v.rows().size() == 3 && v.rows()[0].end - v.rows()[0].first == 14,
          "13 wide characters to a row, ！ hanging at the end of the first");
    {   // tap 一 -> 一緒に
        int c = cellOf(v, "一");
        check(c >= 0 && tapCell(v, c), "tapping 一 changes the selection");
        check(v.selectedText() == "一緒に" || v.selectedText() == "一緒", "the longest word from there: " + v.selectedText());
        check(v.sentence() == line, "the sentence is the block");
        std::string first = v.result() ? std::to_string(v.result()->entryId) : "";
        check(v.page(1) && v.page(-1) && v.result() && std::to_string(v.result()->entryId) == first, "L/R page and come back");
        LookupView::Button b;
        const LookupView::Cell &cl = v.cells()[c];
        check(!v.tap(cl.x + cl.w / 2, 8 + 15, b) && b == LookupView::kNone, "tapping the shown word again: nothing to redraw");
        int comma = cellOf(v, "、");
        check(!tapCell(v, comma), "punctuation can't be selected");
    }
    {   // D-pad: words
        std::string at = v.selectedText();
        check(v.moveWord(1), "right: the next word");
        std::string next = v.selectedText();
        check(next != at && next.compare(0, 3, "見") == 0, "after 一緒に comes 見…: " + next);
        check(v.moveWord(-1) && v.selectedText().compare(0, 3, "一") == 0, "left goes back: " + v.selectedText());
        while (v.moveWord(-1)) {}
        check(v.selectedText().compare(0, 3, "ま") == 0, "left stops at the first word: " + v.selectedText());
        while (v.moveWord(1)) {}
        check(v.selectedCell() >= 0 && v.cells()[v.selectedCell()].row == 2 && !v.moveWord(1),
              "right stops at the last word: " + v.selectedText());
    }
    {   // looking up ahead while idle: finite, and moving still works afterwards
        v.setText({line});
        int n = 0;
        while (v.prefetch() && n < 100) n++;
        check(n >= 2 && n < 20, "words looked up ahead: " + std::to_string(n));
        std::string before = v.selectedText();
        check(v.moveWord(1) && v.selectedText() != before && v.moveWord(-1) && v.selectedText() == before,
              "moving over prefetched words");
        while (v.moveWord(1)) {}
    }
    {   // D-pad: rows
        check(v.moveRow(-1) && v.cells()[v.selectedCell()].row == 1, "up: the row above");
        check(v.moveRow(-1) && v.cells()[v.selectedCell()].row == 0 && !v.moveRow(-1), "up on the top row: nothing");
        check(v.moveRow(1) && v.cells()[v.selectedCell()].row == 1, "down: the row below");
    }
    {   // kinsoku: no row starts with 、。！ or a small kana
        v.setText({"ああああああああああああああ。ああああああああああああ、、あああああああっあああああ"});
        bool ok = true;
        for (const LookupView::Row &r : v.rows()) {
            uint32_t cp = v.cells()[r.first].cp;
            if (cp == 0x3002 || cp == 0x3001 || cp == 0xFF01 || cp == 0x3063) ok = false;
            const LookupView::Cell &last = v.cells()[r.end - 1];
            if (last.x + last.w > 320 - 2) ok = false;  // stays on the screen
        }
        check(ok, "rows never start with punctuation, and stay on the screen");
    }
    {   // several blocks: separate rows, words move across them
        v.setText({"ジャンプもふんわりと高く上がるのだな。", "はい"});
        check(v.rows().back().block == 1 && v.rows().back().y > v.rows()[v.rows().size() - 2].y + 34,
              "a second block below, with a gap");
        while (v.moveWord(1)) {}
        check(v.sentence() == "はい", "right goes on into the next block");
        int f = cellOf(v, "ふ");
        tapCell(v, f);
        check(v.selectedText() == "ふんわり", "ふんわり: " + v.selectedText());
        // L/R: every match of the longest word, at most two of each shorter length, no lone kana
        size_t n = 0;
        std::vector<std::string> surf;
        const Result *first = v.result();
        uint32_t firstId = first ? first->entryId : 0;
        do {
            surf.push_back(v.result()->surface);
            v.page(1);
            n++;
        } while (v.result()->entryId != firstId && n < 50);
        bool lone = false, prefix = false;
        for (const std::string &s : surf) lone |= utf8Length(s) == 1;
        check(n >= 1 && n <= 16 && !lone, "matches to page through: " + std::to_string(n));
        (void)prefix;
    }
    {   // buttons and the hide toggle
        LookupView::Button b;
        v.tap(60, 220, b);
        check(b == LookupView::kSave, "Save button");
        v.tap(160, 220, b);
        check(b == LookupView::kHide, "Hide button");
        v.tap(270, 225, b);
        check(b == LookupView::kClose, "Close button");
        v.tap(160, 190, b);
        check(b == LookupView::kNone, "the hint line is no button");
    }
    {   // drawing: the selection's background, saved words, a long entry that scrolls
        v.setText({"もう何も食べられなかった。"});
        int c = cellOf(v, "食");
        tapCell(v, c);
        check(v.selectedText() == "食べられなかった", "conjugated word selected whole: " + v.selectedText());
        check(v.result() && !v.result()->reasons.empty(), "with its conjugation: " + (v.result() ? v.result()->reasons : ""));
        RgbCanvas top(400, 240), bottom(320, 240);
        v.drawTop(top);
        v.drawBottom(bottom);
        const LookupView::Cell &cl = v.cells()[c];
        Rgb bg = bottom.at(cl.x, 8 + 2);  // inside the highlight, above the glyph
        check(bg.r == 43 && bg.g == 49 && bg.b == 60, "the selected word has its background");
        check(top.at(0, 0).r == 0, "the entry band is dark");
        v.markSaved();
        v.setStatus("Saved", Rgb(123, 216, 143));
        check(v.moveWord(-1), "move on after saving");
        RgbCanvas b2(320, 240);
        v.drawBottom(b2);
        Rgb saved = b2.at(cl.x + 6, 8 + 17);
        check(saved.g > saved.r && saved.g > 150, "a saved word is green");
        tapCell(v, cellOf(v, "何"));
        int scrolled = 0;
        while (v.scroll(1) && scrolled < 500) scrolled++;
        check(scrolled < 500, "scrolling stops at the end");
        check(!v.scroll(1) && (scrolled == 0 || v.scroll(-1000)), "and back to the top");
        v.setHidden(true);
        RgbCanvas t2(400, 240);
        v.drawTop(t2);
        bool blank = true;
        for (uint8_t p : t2.px) blank &= p == 0;
        check(blank, "hidden: nothing drawn over the game");
    }
    {   // text with nothing in the dictionary, and long text that scrolls on the touchscreen
        v.setText({"ABC"});
        RgbCanvas t(400, 240), b(320, 240);
        v.drawTop(t);
        v.drawBottom(b);
        std::string many;
        for (int i = 0; i < 12; i++) many += "今日は天気がいいですね。";
        v.setText({many});
        while (v.moveWord(1)) {}
        int sc = v.selectedCell();
        int y = v.rows()[v.cells()[sc].row].y;
        check(y > 174, "the last word is below the first screenful");
        LookupView::Button bt;
        const LookupView::Cell &last = v.cells()[sc];
        int c2 = -1;
        for (int ty = 8; ty < 182 && c2 != sc; ty += 5) c2 = v.charAt(last.x + last.w / 2, ty);
        check(c2 == sc, "the text scrolled so the selection is on screen");
        (void)bt;
    }
    printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}

