// local_ocr.hpp - reads the screen on the console itself with VOCR (vita-vn-ocr's recognizer,
// plugin/Sources/vocr/): finds the text lines, recognizes them, returns them as TextLine so
// textParagraphs() can group them into blocks and drop furigana.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"  // kt::Reader
#include "core/textlines.hpp"
#include "core/textbox.hpp"

namespace kt {

// A character the recognizer was unsure of: text[byte, byte + len) could also be one of options
// (likeliest first).
struct OcrAlt {
    uint32_t byte = 0, len = 0;
    std::vector<std::string> options;
};
// Called for each recognized line with its unsure characters; may change the text.
typedef void (*LineFixer)(std::string &text, const std::vector<OcrAlt> &alts, void *user);

// internal: an unsure character as a worker thread records it (no allocation there)
struct RawAlt {
    uint16_t byte = 0, len = 0, n = 0;
    uint16_t cls[3] = {0, 0, 0};
};
enum { kTopK = 4, kMaxAlts = 48 };
static const float kAltMinShare = 0.02f;  // a guess with less than 2% of the top 4's weight is ignored

struct TopK {  // internal: per-worker buffers for the recognizer's top-K guesses
    float *logit = nullptr;
    uint16_t *cls = nullptr;
    unsigned cap = 0;  // frames
};

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
    // and return when all have finished. Each extra worker needs its own ~180 KB of memory, taken
    // at the next load() (call this first) or else at the first read that uses it.
    typedef void (*Runner)(unsigned n, void (*job)(void *), void *arg);
    void setParallel(unsigned workers, Runner runner) { workers_ = workers ? workers : 1; runner_ = runner; }
    unsigned lastWorkers() const { return lastWorkers_; }
    // VOCR's batch sizes (0 = its defaults): frames per tile and frames per output-head batch.
    // Bigger batches use more memory; the text is the same. Takes effect at the next load().
    void setOptions(unsigned tileFrames, unsigned headFrames) { tileFrames_ = tileFrames; headFrames_ = headFrames; }
    // With a fixer, each line's uncertain characters are passed to it (see fixOcrLine in correct.hpp).
    void setFixer(LineFixer fixer, void *user) { fixer_ = fixer; fixerUser_ = user; }  // how many the last read used
    const std::string &modelInfo() const { return info_; }

    // grey: w*h bytes (row-major), grey = (77 R + 150 G + 29 B) >> 8 as the model expects.
    // Reads the region (rx, ry, rw, rh), the whole image when rw or rh is 0. Line positions are
    // relative to the whole image. progress(i, n) is called before each line (may be null).
    bool read(const uint8_t *grey, unsigned w, unsigned h, std::vector<TextLine> &lines, std::string &error,
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
    unsigned tileFrames_ = 0, headFrames_ = 0;
    LineFixer fixer_ = nullptr;
    void *fixerUser_ = nullptr;
    std::vector<TopK> topk_;
    void *alignedAlloc(size_t bytes);
    bool loadInner(Reader &file, std::string &error);
    void ensureWorkers(unsigned n);
};

// Per-layer timing of recognition, for the speed test. While on, time between VOCR's phase marks
// is added to ticks[]: [0, n_ops) the layers (the last = the output head), [kProfInput] reading
// the picture's columns, [kProfOther] the rest. Single-threaded use only (one worker).
enum { kProfInput = 62, kProfOther = 63, kProfSlots = 64 };
void ocrProfile(bool on, uint64_t (*clock)());
const uint64_t *ocrProfileTicks();
// The ARM11 kernel that multiplies two positions at a time (default), or one at a time as
// before: the same text, for the speed test.
void ocrUsePairKernel(bool on);

// The top 400x240 of a CTRPF screenshot as grey bytes (malloc'd; caller frees with free()).
uint8_t *topScreenGrey(Reader &bmp, unsigned &w, unsigned &h, std::string &error);
// R, G, B bytes -> grey bytes as the model expects (malloc'd; nullptr if out of memory).
uint8_t *greyFromRGB(const uint8_t *rgb, unsigned w, unsigned h);

// Reads the best text box (findTextBoxes) and falls back to the whole screen when it holds no
// Japanese. Returns the text blocks (textParagraphs); `usedBox` = index of the box read, -1 = screen.
bool localReadScreen(LocalOcr &ocr, const uint8_t *grey, unsigned w, unsigned h, const std::vector<TextBox> &boxes,
                     std::vector<std::string> &paragraphs, std::string &error,
                     void (*progress)(unsigned i, unsigned n) = nullptr, int *usedBox = nullptr);

}  // namespace kt
