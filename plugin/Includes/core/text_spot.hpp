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

private:
    void *blob_ = nullptr;
    void *model_ = nullptr;
};

}  // namespace kt
