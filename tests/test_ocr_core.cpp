// On-device OCR plumbing (natively):
//   test_ocr_core dict.bin                    text blocks, furigana, clean-up, chunking, pixels/BMP
//   test_ocr_core --local model.vocr shot.bmp the full read (also on 2 threads); see test_local_ocr.py
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstring>

#include "core/bmp.hpp"
#include "core/dict.hpp"
#include "core/textbox.hpp"
#include "core/textlines.hpp"
#include "core/correct.hpp"
#include "core/local_ocr.hpp"
#include "core/net.hpp"
#include "core/search.hpp"

using namespace kt;
static int fails = 0;
static void check(bool ok, const std::string &what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}
class FileReader : public Reader {
public:
    explicit FileReader(const char *p) { f_ = fopen(p, "rb"); if (f_) { fseek(f_, 0, SEEK_END); sz_ = ftell(f_); } }
    ~FileReader() { if (f_) fclose(f_); }
    bool read(uint32_t off, void *d, uint32_t n) override { return f_ && !fseek(f_, off, SEEK_SET) && fread(d, 1, n, f_) == n; }
    uint32_t size() override { return sz_; }
private:
    FILE *f_ = nullptr; uint32_t sz_ = 0;
};
// stands in for the 3DS's CTRPF tasks: n threads at once, wait for all
static void threadRunner(unsigned n, void (*job)(void *), void *arg) {
    std::vector<std::thread> t;
    for (unsigned i = 0; i < n; i++) t.emplace_back(job, arg);
    for (auto &x : t) x.join();
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--local")) {  // --local model.vocr shot.bmp: on-device OCR
        FileReader mf(argv[2]), bmp(argv[3]);
        std::string e;
        unsigned w = 0, h = 0;
        uint8_t *rgb = topScreenRGB888(bmp, w, h, e);
        if (!rgb) { printf("BMP FAILED: %s\n", e.c_str()); return 3; }
        std::vector<TextBox> boxes = findTextBoxes(rgb, w, h);
        uint8_t *grey = greyFromRGB(rgb, w, h);
        free(rgb);
        for (auto &b : boxes) printf("BOX: %u,%u %ux%u colour %u,%u,%u score %.0f\n", b.x, b.y, b.w, b.h, b.r, b.g, b.b, b.score);
        LocalOcr ocr;
        if (!ocr.load(mf, e)) { printf("MODEL FAILED: %s\n", e.c_str()); return 3; }
        std::vector<std::string> paras;
        int used = -1;
        bool ok = localReadScreen(ocr, grey, w, h, boxes, paras, e, nullptr, &used);
        // the plugin frees the model after every read and loads it again next time
        ocr.unload();
        std::vector<std::string> again;
        FileReader mf2(argv[2]);
        bool ok2 = !ocr.loaded() && ocr.load(mf2, e) && localReadScreen(ocr, grey, w, h, boxes, again, e);
        free(grey);
        if (!ok) { printf("OCR FAILED: %s\n", e.c_str()); return 3; }
        if (!ok2 || again != paras) { printf("RELOAD FAILED: %s\n", e.c_str()); return 3; }
        // the same with lines read on 2 threads (as on a New 3DS: 2 cores)
        {
            uint8_t *rgb3 = nullptr;
            FileReader bmp3(argv[3]);
            rgb3 = topScreenRGB888(bmp3, w, h, e);
            uint8_t *grey3 = greyFromRGB(rgb3, w, h);
            free(rgb3);
            LocalOcr par;
            FileReader mf3(argv[2]);
            par.setParallel(2, threadRunner);
            std::vector<std::string> threaded;
            bool ok3 = par.load(mf3, e) && localReadScreen(par, grey3, w, h, boxes, threaded, e);
            free(grey3);
            if (!ok3 || threaded != paras) { printf("PARALLEL DIFFERS: %s\n", e.c_str()); return 3; }
            printf("WORKERS: %u\n", par.lastWorkers());
        }
        printf("READ: %s\n", used >= 0 ? "text box" : "whole screen");
        for (auto &p : paras) printf("PARAGRAPH: %s\n", p.c_str());
        return 0;
    }
    std::string err;
    {   // a 2-line text box read as 2 blocks; a menu elsewhere stays separate
        auto L = [](const char *t, int p, float cx, float cy, float w, float h) {
            TextLine l; l.text = t; l.paragraph = p; l.centerX = cx; l.centerY = cy; l.width = w; l.height = h; return l;
        };
        std::vector<TextLine> ls = {L("メニュー", 0, 0.10f, 0.05f, 0.15f, 0.05f),
                                    L("ゆめせかい", 1, 0.25f, 0.66f, 0.12f, 0.02f),
                                    L("また夢世界へ行く時は", 2, 0.36f, 0.72f, 0.60f, 0.07f),
                                    L("声をかけるんじゃぞ!", 3, 0.35f, 0.83f, 0.58f, 0.07f),
                                    L("はい", 4, 0.85f, 0.95f, 0.08f, 0.07f)};
        std::vector<std::string> ps = textParagraphs(ls);
        check(ps.size() == 3 && ps[1] == "また夢世界へ行く時は声をかけるんじゃぞ!",
              "lines of one text box merged" + (ps.size() > 1 ? ": " + ps[1] : std::string()));
        check(ps.size() == 3 && ps[0] == "メニュー" && ps[2] == "はい", "menu and a far-off choice stay separate");
    }

    {   // clean-up of what the OCR read
        std::vector<std::string> p = {"ご名答！スルドイですね～！ク", "ー", "1AAS", "とにかく探しましょ!子",
                                      "発見したんです。クる、", "そうです。はい", "お誕生日なら攻撃力が4倍",
                                      "何かお買い求めでしょうか？"};
        cleanParagraphs(p);
        check(p.size() == 6, "near-empty blocks dropped (ー, 1AAS)");
        if (p.size() == 6) {
            check(p[0] == "ご名答！スルドイですね～！", "speck after ！ dropped: " + p[0]);
            check(p[1] == "とにかく探しましょ!", "speck after ! dropped: " + p[1]);
            check(p[2] == "発見したんです。", "speck + 、 after 。 dropped: " + p[2]);
            check(p[3] == "そうです。はい", "real words after 。 kept: " + p[3]);
            check(p[4] == "お誕生日なら攻撃力が4倍" && p[5] == "何かお買い求めでしょうか？", "normal lines untouched");
        }
        std::vector<std::string> only = {"長が"};
        cleanParagraphs(only);
        check(only.size() == 1, "a lone short block is kept (nothing better to offer)");
    }
    {   // framebuffer pixels -> RGB, and the screenshot BMP written from them
        const uint8_t rgba8[4] = {0xFF, 0x30, 0x20, 0x10}, bgr8[3] = {0x30, 0x20, 0x10};
        uint8_t o[3];
        framebufferToRGB(rgba8, 1, 0, 4, o);
        check(o[0] == 0x10 && o[1] == 0x20 && o[2] == 0x30, "RGBA8 framebuffer pixel");
        framebufferToRGB(bgr8, 1, 1, 3, o);
        check(o[0] == 0x10 && o[1] == 0x20 && o[2] == 0x30, "BGR8 framebuffer pixel");
        const uint16_t c565 = (uint16_t)((0x1F << 11) | (0x00 << 5) | 0x10);
        uint8_t px[2] = {(uint8_t)(c565 & 0xFF), (uint8_t)(c565 >> 8)};
        framebufferToRGB(px, 1, 2, 2, o);
        check(o[0] == 0xF8 && o[1] == 0 && o[2] == 0x80, "RGB565 framebuffer pixel");
        std::vector<uint8_t> img(400 * 240 * 3);
        for (size_t i = 0; i < img.size(); i++) img[i] = (uint8_t)(i * 7);
        std::string bmp = bmpFromRGB(img.data(), 400, 240);
        MemReader r(bmp);
        unsigned w = 0, h = 0;
        std::string e2;
        uint8_t *back = topScreenRGB888(r, w, h, e2);
        check(back && w == 400 && h == 240 && !memcmp(back, img.data(), img.size()), "screenshot BMP reads back the same");
        free(back);
        check(bmpHeader(400, 240) == bmp.substr(0, 54), "BMP header written alone = the whole file's");
        // the snapshot's layout (columns bottom to top) -> RGB rows, every format, = pixel by pixel
        const unsigned W = 7, H = 5;
        for (int fmt = 0; fmt < 5; fmt++) {
            unsigned bpp = fmt == 0 ? 4 : (fmt == 1 ? 3 : 2);
            std::vector<uint8_t> cols(W * H * bpp), fast(W * H * 3), slow(W * H * 3);
            for (size_t i = 0; i < cols.size(); i++) cols[i] = (uint8_t)(i * 37 + fmt);
            framebufferColumnsToRGB(cols.data(), W, H, fmt, bpp, fast.data());
            for (unsigned x = 0; x < W; x++)
                for (unsigned k = 0; k < H; k++)
                    framebufferToRGB(&cols[(x * H + k) * bpp], 1, fmt, bpp, &slow[((H - 1 - k) * W + x) * 3]);
            check(fast == slow, "framebuffer columns -> rows, format " + std::to_string(fmt));
        }
    }

    if (argc > 1) {
        FileReader dr(argv[1]);
        Dictionary d;
        if (d.open(&dr, err)) {
            for (const char *t : {"また夢世界へ行く時は声をかけるんじゃぞ！", "この先に宝箱があるらしいぞ。",
                                  "もう何も食べられなかった。", "ABCあいう"}) {
                std::vector<std::string> c = chunkText(d, t);
                std::string joined, shown;
                for (auto &x : c) { joined += x; shown += x + "|"; }
                check(joined == t, std::string("chunks join back: ") + shown);
            }
            std::vector<std::string> c = chunkText(d, "この先に宝箱があるらしいぞ。");
            bool hasGa = false, hasTakarabako = false;
            for (auto &x : c) { hasGa |= x == "が"; hasTakarabako |= x == "宝箱"; }
            check(hasGa && hasTakarabako, "particle split off, compound kept");
            // misread kanji fixed by the dictionary from the recognizer's other guesses
            auto alt = [](uint32_t byte, std::vector<std::string> o) { OcrAlt a; a.byte = byte; a.len = 3; a.options = o; return a; };
            std::string t = "造跡の壁にこんな一文を";
            check(fixOcrLine(d, t, {alt(0, {"遺", "遠"}), alt(24, {"ー"})}) == 1 && t == "遺跡の壁にこんな一文を",
                  "造跡 -> 遺跡: " + t);
            t = "この島には造跡がたくさん";
            fixOcrLine(d, t, {alt(15, {"遺"})});
            check(t == "この島には遺跡がたくさん", "fixed mid-line: " + t);
            t = "建物を造る";  // 造 is part of a word already: left alone
            check(fixOcrLine(d, t, {alt(9, {"遺"})}) == 0 && t == "建物を造る", "a kanji inside a word is trusted");
            t = "人に会う";   // no guess makes a longer word: left alone
            check(fixOcrLine(d, t, {alt(0, {"入"})}) == 0 && t == "人に会う", "nothing better: unchanged");
            t = "ここは門の造跡";  // offsets after a fix still line up
            fixOcrLine(d, t, {alt(15, {"遺"}), alt(18, {"跡"})});
            check(t == "ここは門の遺跡", "later alternatives after a fix: " + t);
            check(wordCover(d, "遺跡が", 0, 3) == 2 && wordCover(d, "造跡が", 0, 3) <= 1, "wordCover");
        }
    }
    printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
