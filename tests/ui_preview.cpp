// Draws the plugin's lookup screens on a PC, as the console would: both screens stacked in one BMP.
//   ui_preview atlas.bin dict.bin shot.bmp model.vocr out.bmp [action ...]
//   ui_preview atlas.bin dict.bin shot.bmp --text "テキスト|二つ目" out.bmp [action ...]
// atlas.bin: make_font_atlas.py. Actions: right left up down L R scroll save hide tap:x,y
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/bmp.hpp"
#include "core/correct.hpp"
#include "core/local_ocr.hpp"
#include "core/lookup_view.hpp"
#include "core/textbox.hpp"
#include "core/ui_text.hpp"

using namespace kt;

struct FileReader : Reader {
    FILE *f = nullptr;
    uint32_t sz = 0;
    explicit FileReader(const char *p) { f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); } }
    ~FileReader() { if (f) fclose(f); }
    bool read(uint32_t o, void *d, uint32_t n) override { return f && !fseek(f, o, SEEK_SET) && fread(d, 1, n, f) == n; }
    uint32_t size() override { return sz; }
};

// make_font_atlas.py's file
struct AtlasFont : GlyphSource {
    int cw = 0, chh = 0;
    struct G { GlyphInfo info; std::vector<uint8_t> px; };
    std::vector<std::pair<uint32_t, G>> glyphs;  // sorted by code point
    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        char magic[4];
        uint16_t w, h;
        uint32_t n;
        if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "KTFA", 4) || fread(&w, 2, 1, f) != 1 || fread(&h, 2, 1, f) != 1 ||
            fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
        cw = w; chh = h;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t cp; int16_t left; uint16_t width, adv;
            if (fread(&cp, 4, 1, f) != 1 || fread(&left, 2, 1, f) != 1 || fread(&width, 2, 1, f) != 1 || fread(&adv, 2, 1, f) != 1) break;
            G g;
            g.info.left = left; g.info.width = width; g.info.advance = adv;
            g.px.resize((size_t)w * h);
            if (fread(g.px.data(), 1, g.px.size(), f) != g.px.size()) break;
            glyphs.push_back({cp, std::move(g)});
        }
        fclose(f);
        std::sort(glyphs.begin(), glyphs.end(), [](const std::pair<uint32_t, G> &a, const std::pair<uint32_t, G> &b) { return a.first < b.first; });
        return !glyphs.empty();
    }
    const G *find(uint32_t cp) const {
        size_t lo = 0, hi = glyphs.size();
        while (lo < hi) { size_t m = (lo + hi) / 2; if (glyphs[m].first < cp) lo = m + 1; else hi = m; }
        return lo < glyphs.size() && glyphs[lo].first == cp ? &glyphs[lo].second : nullptr;
    }
    int cellWidth() const override { return cw; }
    int cellHeight() const override { return chh; }
    bool metrics(uint32_t cp, GlyphInfo &info) override { const G *g = find(cp); if (!g) return false; info = g->info; return true; }
    bool pixels(uint32_t cp, uint8_t *out) override {
        const G *g = find(cp);
        if (!g) return false;
        // the glyph's pixels start at column 0 of the cell, as in the 3DS font's sheets
        memset(out, 0, (size_t)cw * chh);
        for (int y = 0; y < chh; y++) memcpy(out + (size_t)y * cw, g->px.data() + (size_t)y * cw, cw);
        return true;
    }
};

struct RgbCanvas : Canvas {
    std::vector<uint8_t> px;
    RgbCanvas(int w, int h) { width = w; height = h; px.assign((size_t)w * h * 3, 0); }
    void fill(int x, int y, int w, int h, Rgb c) override {
        for (int yy = std::max(0, y); yy < std::min(height, y + h); yy++)
            for (int xx = std::max(0, x); xx < std::min(width, x + w); xx++) {
                uint8_t *p = &px[((size_t)yy * width + xx) * 3];
                p[0] = c.r; p[1] = c.g; p[2] = c.b;
            }
    }
    void blend(int x, int y, Rgb c, uint8_t a) override {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        uint8_t *p = &px[((size_t)y * width + x) * 3];
        p[0] = (uint8_t)((c.r * a + p[0] * (255 - a)) / 255);
        p[1] = (uint8_t)((c.g * a + p[1] * (255 - a)) / 255);
        p[2] = (uint8_t)((c.b * a + p[2] * (255 - a)) / 255);
    }
};

static void fixLine(std::string &t, const std::vector<OcrAlt> &alts, void *d) { fixOcrLine(*(Dictionary *)d, t, alts); }

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: see the top of ui_preview.cpp\n"); return 2; }
    AtlasFont font;
    if (!font.load(argv[1])) { fprintf(stderr, "no atlas\n"); return 2; }
    FileReader dr(argv[2]), shot(argv[3]);
    Dictionary dict;
    std::string e;
    if (!dict.open(&dr, e)) { fprintf(stderr, "dict: %s\n", e.c_str()); return 2; }
    unsigned w = 0, h = 0;
    uint8_t *rgb = topScreenRGB888(shot, w, h, e);
    if (!rgb) { fprintf(stderr, "shot: %s\n", e.c_str()); return 2; }
    std::vector<std::string> blocks;
    int argi;
    if (!strcmp(argv[4], "--text")) {
        std::string t = argv[5];
        for (size_t s = 0, p; s <= t.size(); s = p + 1) {
            p = t.find('|', s);
            if (p == std::string::npos) p = t.size();
            blocks.push_back(t.substr(s, p - s));
        }
        argi = 7;
        if (argc < 7) return 2;
    } else {
        FileReader mf(argv[4]);
        LocalOcr ocr;
        if (!ocr.load(mf, e)) { fprintf(stderr, "model: %s\n", e.c_str()); return 2; }
        ocr.setFixer(fixLine, &dict);
        std::vector<TextBox> boxes = findTextBoxes(rgb, w, h);
        uint8_t *grey = greyFromRGB(rgb, w, h);
        if (!localReadScreen(ocr, grey, w, h, boxes, blocks, e)) { fprintf(stderr, "ocr: %s\n", e.c_str()); return 2; }
        free(grey);
        argi = 6;
    }
    const char *out = argv[argi - 1];
    TextRenderer tr(font);
    LookupView view(dict, tr);
    view.setText(blocks);
    for (int i = argi; i < argc; i++) {
        std::string a = argv[i];
        LookupView::Button b;
        if (a == "right") view.moveWord(1);
        else if (a == "left") view.moveWord(-1);
        else if (a == "down") view.moveRow(1);
        else if (a == "up") view.moveRow(-1);
        else if (a == "R") view.page(1);
        else if (a == "L") view.page(-1);
        else if (a == "scroll") view.scroll(3);
        else if (a == "hide") view.setHidden(!view.hidden());
        else if (a == "save") { view.markSaved(); view.setStatus("Saved " + view.selectedText() + " (12 words) · card sent to PC", Rgb(123, 216, 143)); }
        else if (!a.compare(0, 4, "tap:")) { int x, y; sscanf(a.c_str() + 4, "%d,%d", &x, &y); view.tap(x, y, b); }
    }
    // the console: top screen (the game dimmed, or as it is when hidden), bottom screen centred under it
    RgbCanvas top(400, 240), bottom(320, 240);
    for (size_t i = 0; i < (size_t)400 * 240 * 3; i++) {
        uint8_t v = rgb[i];
        top.px[i] = view.hidden() ? v : (uint8_t)((v >> 3) + (v >> 4));  // ~19%, as the plugin
    }
    view.drawTop(top);
    view.drawBottom(bottom);
    std::vector<uint8_t> both((size_t)400 * 480 * 3, 24);
    memcpy(both.data(), top.px.data(), top.px.size());
    for (int y = 0; y < 240; y++) memcpy(&both[((size_t)(240 + y) * 400 + 40) * 3], &bottom.px[(size_t)y * 320 * 3], 320 * 3);
    std::string bmp = bmpFromRGB(both.data(), 400, 480);
    FILE *f = fopen(out, "wb");
    fwrite(bmp.data(), 1, bmp.size(), f);
    fclose(f);
    printf("selected: %s  sentence: %s\n", view.selectedText().c_str(), view.sentence().c_str());
    free(rgb);
    return 0;
}
