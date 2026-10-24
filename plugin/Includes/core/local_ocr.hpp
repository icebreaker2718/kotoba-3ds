// local_ocr.hpp - reads the screen on the console itself with VOCR (vita-vn-ocr's recognizer,
// plugin/Sources/vocr/): finds the text lines, recognizes them, returns them as LensLine so
// lensParagraphs() can group them into blocks and drop furigana.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"  // kt::Reader
#include "core/lens.hpp"  // kt::LensLine
#include "core/textbox.hpp"

namespace kt {

class LocalOcr {
public:
    LocalOcr() = default;
    ~LocalOcr();
    LocalOcr(const LocalOcr &) = delete;
    LocalOcr &operator=(const LocalOcr &) = delete;

    // Loads a .vocr model file (read whole into memory, kept until the plugin ends).
    bool load(Reader &file, std::string &error);
    bool loaded() const { return blob_ != nullptr; }
    // Frees the model and its working memory (load() again to use it).
    void unload();

    // Lines can be read in parallel: runner(n, job, arg) must run job(arg) on n threads at once
    // and return when all have finished. Each extra worker needs its own ~180 KB of memory.
    typedef void (*Runner)(unsigned n, void (*job)(void *), void *arg);
    void setParallel(unsigned workers, Runner runner) { workers_ = workers ? workers : 1; runner_ = runner; }
    unsigned lastWorkers() const { return lastWorkers_; }  // how many the last read used
    const std::string &modelInfo() const { return info_; }

    // grey: w*h bytes (row-major), grey = (77 R + 150 G + 29 B) >> 8 as the model expects.
    // Reads the region (rx, ry, rw, rh), the whole image when rw or rh is 0. Line positions are
    // relative to the whole image. progress(i, n) is called before each line (may be null).
    bool read(const uint8_t *grey, unsigned w, unsigned h, std::vector<LensLine> &lines, std::string &error,
              void (*progress)(unsigned i, unsigned n) = nullptr, unsigned rx = 0, unsigned ry = 0,
              unsigned rw = 0, unsigned rh = 0);

private:
    unsigned char *blob_ = nullptr;   // 16-byte aligned
    unsigned char *arena_ = nullptr;  // recognizer arena, 16-byte aligned
    void *model_ = nullptr;           // vocr_model
    void *ctx_ = nullptr;             // vocr_ctx
    std::string info_;
    std::vector<void *> raw_;  // every allocation, for the destructor
    std::vector<void *> ctxs_; // vocr_ctx per worker (the first is ctx_)
    size_t arenaBytes_ = 0;
    int mode_ = 0;
    unsigned workers_ = 1, lastWorkers_ = 1;
    Runner runner_ = nullptr;
    void *alignedAlloc(size_t bytes);
    bool loadInner(Reader &file, std::string &error);
};

// The top 400x240 of a CTRPF screenshot as grey bytes (malloc'd; caller frees with free()).
uint8_t *topScreenGrey(Reader &bmp, unsigned &w, unsigned &h, std::string &error);
// R, G, B bytes -> grey bytes as the model expects (malloc'd; nullptr if out of memory).
uint8_t *greyFromRGB(const uint8_t *rgb, unsigned w, unsigned h);

// Reads the best text box (findTextBoxes) and falls back to the whole screen when it holds no
// Japanese. Returns the text blocks (lensParagraphs); `usedBox` = index of the box read, -1 = screen.
bool localReadScreen(LocalOcr &ocr, const uint8_t *grey, unsigned w, unsigned h, const std::vector<TextBox> &boxes,
                     std::vector<std::string> &paragraphs, std::string &error,
                     void (*progress)(unsigned i, unsigned n) = nullptr, int *usedBox = nullptr);

}  // namespace kt
