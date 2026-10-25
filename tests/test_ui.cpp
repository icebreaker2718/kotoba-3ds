#include <algorithm>
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
#include "core/known_words.hpp"
#include "core/lookup_view.hpp"
#include "core/menu_view.hpp"
#include "core/hotkeys.hpp"
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
        v.drawTop(t2, LookupView::Panel::Hidden);
        bool blank = true;
        for (uint8_t p : t2.px) blank &= p == 0;
        check(blank, "hidden: nothing drawn over the game");
        RgbCanvas t3(400, 240);
        v.drawTop(t3, LookupView::Panel::SeeThrough);
        check(t3.at(0, 0).r == 0 && t3.at(0, 0).g == 0, "see-through: no band behind the entry");
        bool text = false;
        for (uint8_t p : t3.px) text |= p > 100;
        check(text, "see-through: the entry is still drawn");
        RgbCanvas b3(320, 240);
        v.drawBottom(b3, LookupView::Panel::Hidden);
        blank = true;
        for (uint8_t p : b3.px) blank &= p == 0;
        check(blank, "a hidden touchscreen panel: nothing drawn, not even the buttons");
        v.setHidden(false);
    }
    {   // the panels swapped: the text on the top screen (400 wide), the entry and buttons below
        v.setText({"この先に宝箱があるらしいぞ。気をつけて進むんだ。"});
        v.setTextOnTop(true);
        int widest = 0;
        for (const LookupView::Cell &cl : v.cells()) widest = std::max(widest, cl.x + cl.w);
        check(widest > 320 && widest <= 400, "the text is laid out for the top screen");
        LookupView::Button b;
        check(!v.tap(60, 30, b) && b == LookupView::kNone, "a tap on the entry doesn't select text");
        v.tap(300, 215, b);
        check(b == LookupView::kClose, "the buttons are still on the touchscreen");
        check(v.moveWord(1) && v.hasResult(), "the D-pad moves through the text");
        RgbCanvas top(400, 240), bottom(320, 240);
        v.drawTop(top);
        v.drawBottom(bottom);
        check(top.at(399, 120).r == 15 && top.at(399, 120).g == 18, "the text panel fills the top screen");
        bool entry = false;
        for (int y = 10; y < 190 && !entry; y++)
            for (int x = 10; x < 310 && !entry; x++) entry = bottom.at(x, y).r > 200;
        check(entry, "the entry is drawn on the touchscreen");
        v.setTextOnTop(false);
        widest = 0;
        for (const LookupView::Cell &cl : v.cells()) widest = std::max(widest, cl.x + cl.w);
        check(widest <= 320, "and back on the touchscreen");
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
    {   // the game's line breaks: rows start there, and the text shrinks so the lines fit
        LookupView v(dict, tr);
        v.setText({"ドリームストーンはパジャマウンテンの\nどこかにあるらしいが…"});
        check(v.blocks()[0] == "ドリームストーンはパジャマウンテンのどこかにあるらしいが…", "line breaks out of the text");
        int d = cellOf(v, "ど");
        check(d >= 0 && v.cells()[d].row == 1 && v.cells()[d].x == 12, "the game's second line starts a row");
        check(v.rows().size() == 2 && v.textSize() < 27, "smaller text: 18 characters fit a row");
        LookupView w(dict, tr);
        w.setText({"今日は\nいい天気"});
        check(w.textSize() == 27 && w.rows().size() == 2, "short lines keep the big text");
        w.setText({"ああああああああああああああああああああ"});
        int r2 = -1;
        for (size_t i = 0; i < w.cells().size() && r2 < 0; i++) if (w.cells()[i].row == 1) r2 = (int)i;
        check(r2 > 0 && w.cells()[r2].x == 12 + w.textSize() / 2, "a game line too long for a row goes on indented");
        w.setText({"Press the\nbutton"});
        check(w.blocks()[0] == "Press the button", "English words broken across lines keep their space");
    }
    {   // known words: green wherever they are
        KnownWords kw;
        const char list[] = "# words with cards\n天気\r\n食べる\n  \n天気\n";
        check(kw.load(list, sizeof(list) - 1) && kw.size() == 2 && kw.has("天気") && !kw.has("今日"), "the known words list");
        LookupView v(dict, tr);
        v.setKnown(&kw);
        v.setText({"今日はいい天気だから\nケーキを食べた"});
        int guard = 0;
        while (v.prefetch() && guard++ < 1000) {}
        check(v.cellKnown(cellOf(v, "天")) && v.cellKnown(cellOf(v, "気")) && !v.cellKnown(cellOf(v, "今")), "a known word is green");
        check(v.cellKnown(cellOf(v, "食")), "a known word conjugated (食べた: 食べる) is green");
        check(v.takeRedraw() && !v.takeRedraw(), "the screens are redrawn once for it");
        LookupView s(dict, tr);
        s.setKnown(&kw);
        s.setText({"ケーキとケーキ"});
        guard = 0;
        while (s.prefetch() && guard++ < 1000) {}
        check(!s.cellKnown(cellOf(s, "ケ", 1)), "not known before it is saved");
        tapCell(s, cellOf(s, "ケ"));
        s.markSaved();
        check(s.cellKnown(cellOf(s, "ケ", 1)) && kw.has("ケーキ"), "saving a word turns it green elsewhere too");
    }
    {   // hotkeys: names, and one that is part of another waits for it
        check(keyComboName(keys::ZL | keys::ZR) == "ZL + ZR" && keyComboName(0) == "Off" &&
                  keyComboName(keys::Y) == "Y", "hotkey names: " + keyComboName(keys::L | keys::R | keys::Down));
        HotkeyDispatcher d;
        d.set(0, keys::ZL | keys::ZR);
        d.set(1, keys::ZL);
        d.set(2, keys::Y);
        check(d.update(keys::Y, keys::Y, 0) == 2, "a hotkey part of no other: at once");
        check(d.update(keys::ZL, keys::ZL, 1000) == -1, "ZL (part of ZL + ZR) waits");
        check(d.update(keys::ZL | keys::ZR, keys::ZR, 1100) == 0, "then ZR: ZL + ZR");
        check(d.update(keys::ZL, keys::ZL, 2000) == -1 && d.update(0, 0, 2080) == 1, "ZL let go quickly: ZL");
        check(d.update(keys::ZL, keys::ZL, 3000) == -1 && d.update(keys::ZL, 0, 3100) == -1 &&
                  d.update(keys::ZL, 0, 3300) == 1, "ZL held: ZL after a moment");
        check(d.update(keys::ZL | keys::ZR, keys::ZL | keys::ZR, 4000) == 0, "both at once: the longer one");
        check(d.update(keys::A, keys::A, 5000) == -1, "other buttons: nothing");
    }
    {   // the menu screens
        MenuView m(tr);
        m.setTitle("Kotoba3DS");
        std::vector<MenuItem> items;
        for (int i = 0; i < 12; i++) {
            MenuItem it;
            it.label = "Item " + std::to_string(i);
            it.value = i % 2 ? "on" : "";
            it.help = "Help for item " + std::to_string(i);
            items.push_back(it);
        }
        m.setItems(items);
        m.setButtons({"\uE000 Choose", "\uE001 Close"});
        int b = -1;
        check(m.tap(100, MenuView::kRowTop + MenuView::kRowH + 5, b) == 1 && b == -1, "a tap on a row");
        check(m.tap(250, 220, b) == -1 && b == 1, "a tap on the second button");
        while (m.move(1)) {}
        check(m.selected() == 11 && m.scroll() > 0, "the list scrolls to the last item");
        check(m.tap(100, MenuView::kRowTop + 2, b) == m.scroll(), "taps count from the first row shown");
        RgbCanvas top(400, 240), bottom(320, 240);
        m.drawTop(top);
        m.drawBottom(bottom);
        bool help = false;
        for (int y = 50; y < 80 && !help; y++)
            for (int x = 14; x < 200 && !help; x++) help = top.at(x, y).r > 200;
        check(help, "the highlighted item's help is on the top screen");
        MenuView q(tr);
        q.setMessage("No saved words yet.");
        q.setButtons({"\uE001 Back"});
        RgbCanvas qb(320, 240);
        q.drawBottom(qb);
        check(q.selected() == -1, "a message screen has no rows");
    }
    printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}

