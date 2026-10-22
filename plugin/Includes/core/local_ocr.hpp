// local_ocr.hpp - reads the screen on the console itself with VOCR (vita-vn-ocr's recognizer,
// plugin/Sources/vocr/): finds the text lines, recognizes them, returns them as LensLine so
// lensParagraphs() can group them into blocks and drop furigana.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"  // kt::Reader
#include "core/lens.hpp"  // kt::LensLine

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
    const std::string &modelInfo() const { return info_; }

    // grey: w*h bytes (row-major), grey = (77 R + 150 G + 29 B) >> 8 as the model expects.
    // progress(i, n) is called before each line is recognized (may be null).
    bool read(const uint8_t *grey, unsigned w, unsigned h, std::vector<LensLine> &lines, std::string &error,
              void (*progress)(unsigned i, unsigned n) = nullptr);

private:
    unsigned char *blob_ = nullptr;   // 16-byte aligned
    unsigned char *arena_ = nullptr;  // recognizer arena, 16-byte aligned
    void *model_ = nullptr;           // vocr_model
    void *ctx_ = nullptr;             // vocr_ctx
    std::string info_;
    std::vector<void *> raw_;  // every allocation, for the destructor
    void *alignedAlloc(size_t bytes);
};

// The top 400x240 of a CTRPF screenshot as grey bytes (malloc'd; caller frees with free()).
uint8_t *topScreenGrey(Reader &bmp, unsigned &w, unsigned &h, std::string &error);

}  // namespace kt
