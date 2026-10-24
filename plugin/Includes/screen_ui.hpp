// screen_ui.hpp - the console side of the lookup screens: the system font as a GlyphSource
// (drawn at any size, anti-aliased) and the screens' framebuffers as a Canvas.
#pragma once
#include <CTRPluginFramework.hpp>

#include "core/ui_text.hpp"

namespace CTRPluginFramework {

// The 3DS system font, read straight from its glyph sheets (4-bit alpha, ~24 px cells).
class SystemFont : public kt::GlyphSource {
public:
    SystemFont();
    bool ok() const { return ok_; }
    std::string describe() const;  // for the log
    int cellWidth() const override { return cellW_; }
    int cellHeight() const override { return cellH_; }
    bool metrics(uint32_t cp, kt::GlyphInfo &info) override;
    bool pixels(uint32_t cp, uint8_t *out) override;

private:
    bool ok_ = false;
    int cellW_ = 24, cellH_ = 30, fmt_ = 0, sheetW_ = 0, sheetH_ = 0, perSheet_ = 0, perRow_ = 0;
    int glyphIndex(uint32_t cp);
};

// A screen's current draw buffer.
class ScreenCanvas : public kt::Canvas {
public:
    explicit ScreenCanvas(const Screen &s);
    void fill(int x, int y, int w, int h, kt::Rgb c) override;
    void blend(int x, int y, kt::Rgb c, uint8_t alpha) override;
    void shade(int x, int y, int w, int h, uint8_t alpha) override;

private:
    const Screen &s_;
    int fmt_;
    unsigned bpp_;
};

}  // namespace CTRPluginFramework
