// text_spot.hpp - vita-vn-ocr's text spotter (textspot8, vocr_textspot6.c): a small network that
// finds the text lines on a whole screen, each with its kind (dialogue, a speaker's name, a
// choice, UI text, scene text), in reading order. It takes the place of the text-box search and
// the line finder in front of the recognizer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"  // kt::Reader

namespace kt {

struct SpotBox {
    unsigned x = 0, y = 0, w = 0, h = 0;  // screen pixels, padded as the recognizer was trained on
    unsigned tag = 0;                     // kSpotDialogue ...
    unsigned conf = 0;                    // 1..127: how sure the spotter is that this is text
    unsigned block = 0;                   // lines of one block (one text box) share it
    bool dark = false;                    // text darker than its background
    bool vertical = false;                // a vertical column
};
enum { kSpotDialogue = 0, kSpotName = 1, kSpotChoice = 2, kSpotUi = 3, kSpotScene = 4 };

class TextSpotter {
public:
    TextSpotter() = default;
    ~TextSpotter();
    TextSpotter(const TextSpotter &) = delete;
    TextSpotter &operator=(const TextSpotter &) = delete;

    // Loads a .vtsp file (52 KB, kept until the plugin ends).
    bool load(Reader &file, std::string &error);
    bool loaded() const { return model_ != nullptr; }
    void unload();
    // The text lines of a grey w x h screen, in reading order. Its working memory (about 210 KB
    // for the top screen) is taken for the call only. False with error on failure.
    bool find(const uint8_t *grey, unsigned w, unsigned h, std::vector<SpotBox> &boxes, std::string &error) const;
    // Working memory a find() on a w x h screen needs.
    size_t arenaBytes(unsigned w, unsigned h) const;
    // Its rows computed by `workers` threads at once (the same boxes): runner(n, job, arg) must run job(arg) on
    // n threads at the same time and return when all have finished (LocalOcr's runner). The threads wait for
    // each row's blocks by spinning (a row's block is ~50 us of work: a wake-up would cost as much).
    typedef void (*Runner)(unsigned n, void (*job)(void *), void *arg);
    void setParallel(unsigned workers, Runner runner) { workers_ = workers ? workers : 1; runner_ = runner; }
    // Diagnostic (alpha): called when find() can't get its working memory, right there (after what it did get is
    // freed again): need = the arena's bytes; arenaOk / setOk = which of its two blocks malloc gave.
    typedef void (*MemFail)(size_t need, bool arenaOk, bool setOk);
    void setMemFail(MemFail fn) { memFail_ = fn; }

private:
    void *blob_ = nullptr;
    void *model_ = nullptr;
    unsigned workers_ = 1;
    Runner runner_ = nullptr;
    MemFail memFail_ = nullptr;
    mutable signed char *scratch_ = nullptr;  // the extra workers' scratch, taken at the first parallel find
    mutable unsigned scratchFor_ = 0;
};

}  // namespace kt
