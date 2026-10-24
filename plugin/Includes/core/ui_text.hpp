// ui_text.hpp - anti-aliased text at any size from a bitmap font (on the 3DS: the system font's
// glyph sheets, 4-bit alpha, ~24 px), drawn onto any surface. Platform independent so the
// screens can be rendered on a PC for tests and previews exactly as the console draws them.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kt {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    constexpr Rgb() = default;
    constexpr Rgb(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
};

struct GlyphInfo {
    int left = 0;     // blank columns before the glyph's pixels
    int width = 0;    // columns of pixels
    int advance = 0;  // pen movement
};

// A bitmap font at its native size.
class GlyphSource {
public:
    virtual ~GlyphSource() {}
    virtual int cellWidth() const = 0;
    virtual int cellHeight() const = 0;
    // Metrics of cp; false if the font has no glyph for it.
    virtual bool metrics(uint32_t cp, GlyphInfo &info) = 0;
    // Alpha (0-255) of cp's cell into out (cellWidth x cellHeight bytes, row-major).
    virtual bool pixels(uint32_t cp, uint8_t *out) = 0;
};

// Something to draw on: a screen's framebuffer, or a picture in the tests.
class Canvas {
public:
    virtual ~Canvas() {}
    int width = 0, height = 0;
    virtual void fill(int x, int y, int w, int h, Rgb c) = 0;  // clipped by the implementation
    virtual void blend(int x, int y, Rgb c, uint8_t alpha) = 0;  // 255 = c, 0 = unchanged
    // Darkens a rectangle (alpha 255 = black).
    virtual void shade(int x, int y, int w, int h, uint8_t alpha) {
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++) blend(xx, yy, Rgb(0, 0, 0), alpha);
    }
};

class TextRenderer {
public:
    explicit TextRenderer(GlyphSource &font) : font_(font) {}
    GlyphSource &font() { return font_; }

    // `size` is the line's height in pixels (the font's cell scaled to it).
    int advance(uint32_t cp, int size);
    int width(const std::string &text, int size);
    // Draws text with its cell's top at y. Returns the x after the last character.
    int draw(Canvas &c, int x, int y, const std::string &text, int size, Rgb color);
    int drawChar(Canvas &c, int x, int y, uint32_t cp, int size, Rgb color);
    // Splits text into lines no wider than maxWidth (at spaces in English, anywhere in Japanese).
    std::vector<std::string> wrap(const std::string &text, int size, int maxWidth);

private:
    GlyphSource &font_;
    std::vector<uint8_t> cell_;
    struct Scaled {
        int advance = 0, ox = 0, w = 0, h = 0;
        std::vector<uint8_t> alpha;  // w x h
    };
    static const size_t kCacheBytes = 160 * 1024;
    std::unordered_map<uint64_t, Scaled> cache_;
    size_t cacheBytes_ = 0;
    uint32_t fallback(uint32_t cp);
    const Scaled *scaled(uint32_t cp, int size);
};

}  // namespace kt
