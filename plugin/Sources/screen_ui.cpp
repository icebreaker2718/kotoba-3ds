#include "screen_ui.hpp"

#include <3ds.h>

#include <cstring>

#include "core/bmp.hpp"

namespace CTRPluginFramework {

// ---------------------------------------------------------------------------
// System font (CTRPF has mapped the shared font; libctru's font functions read it)
// ---------------------------------------------------------------------------
SystemFont::SystemFont() {
    TGLP_s *tglp = fontGetGlyphInfo(nullptr);
    if (!tglp || !tglp->sheetData) return;
    if (tglp->sheetFmt != GPU_A4 && tglp->sheetFmt != GPU_A8) return;
    cellW_ = tglp->cellWidth;
    cellH_ = tglp->cellHeight;
    fmt_ = tglp->sheetFmt;
    sheetW_ = tglp->sheetWidth;
    sheetH_ = tglp->sheetHeight;
    perRow_ = tglp->nRows;
    perSheet_ = tglp->nRows * tglp->nLines;
    ok_ = cellW_ > 0 && cellH_ > 0 && perRow_ > 0 && perSheet_ > 0 && (sheetW_ & 7) == 0;
}

std::string SystemFont::describe() const {
    return Utils::Format("font %s: cell %dx%d, format %d, sheet %dx%d, %d per sheet", ok_ ? "ok" : "unusable", cellW_,
                         cellH_, fmt_, sheetW_, sheetH_, perSheet_);
}

int SystemFont::glyphIndex(uint32_t cp) {
    FINF_s *finf = fontGetInfo(nullptr);
    int idx = fontGlyphIndexFromCodePoint(nullptr, cp);
    if (idx < 0 || idx == 0xFFFF) return -1;
    if (finf && idx == finf->alterCharIndex && cp != '?') return -1;  // the font's "missing" glyph
    return idx;
}

bool SystemFont::metrics(uint32_t cp, kt::GlyphInfo &info) {
    if (!ok_) return false;
    int idx = glyphIndex(cp);
    if (idx < 0) return false;
    charWidthInfo_s *cwi = fontGetCharWidthInfo(nullptr, idx);
    if (!cwi) return false;
    info.left = cwi->left;
    info.width = cwi->glyphWidth;
    info.advance = cwi->charWidth;
    return true;
}

bool SystemFont::pixels(uint32_t cp, uint8_t *out) {
    if (!ok_) return false;
    int idx = glyphIndex(cp);
    if (idx < 0) return false;
    const u8 *sheet = (const u8 *)fontGetGlyphSheetTex(nullptr, idx / perSheet_);
    if (!sheet) return false;
    int inSheet = idx % perSheet_;
    // glyph cells are laid out in rows with a 1-pixel border; the sheet is 8x8 tiles, each in
    // Morton (Z) order
    int x0 = (inSheet % perRow_) * (cellW_ + 1) + 1, y0 = (inSheet / perRow_) * (cellH_ + 1) + 1;
    for (int y = 0; y < cellH_; y++)
        for (int x = 0; x < cellW_; x++) {
            int px = x0 + x, py = y0 + y;
            uint8_t a = 0;
            if (px < sheetW_ && py < sheetH_) {
                int tile = (py >> 3) * (sheetW_ >> 3) + (px >> 3);
                int tx = px & 7, ty = py & 7;
                int m = (tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2) | ((tx & 4) << 2) | ((ty & 4) << 3);
                int pos = tile * 64 + m;
                if (fmt_ == GPU_A4) a = (uint8_t)(((sheet[pos >> 1] >> ((pos & 1) * 4)) & 0xF) * 0x11);
                else a = sheet[pos];
            }
            out[y * cellW_ + x] = a;
        }
    return true;
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------
ScreenCanvas::ScreenCanvas(const Screen &s) : s_(s), fmt_((int)s.Format), bpp_(s.BytesPerPixel) {
    width = s.IsTop ? 400 : 320;
    height = 240;
}

void ScreenCanvas::fill(int x, int y, int w, int h, kt::Rgb c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) return;
    uint8_t rgb[3] = {c.r, c.g, c.b}, px[4];
    kt::rgbToFramebuffer(rgb, fmt_, px);
    for (int xx = x; xx < x + w; xx++)
        for (int yy = y; yy < y + h; yy++) memcpy(s_.GetFramebuffer(xx, yy), px, bpp_);
}

void ScreenCanvas::blend(int x, int y, kt::Rgb c, uint8_t a) {
    if (x < 0 || y < 0 || x >= width || y >= height || !a) return;
    u8 *p = s_.GetFramebuffer(x, y);
    uint8_t rgb[3];
    kt::framebufferToRGB(p, 1, fmt_, bpp_, rgb);
    rgb[0] = (uint8_t)((c.r * a + rgb[0] * (255 - a)) / 255);
    rgb[1] = (uint8_t)((c.g * a + rgb[1] * (255 - a)) / 255);
    rgb[2] = (uint8_t)((c.b * a + rgb[2] * (255 - a)) / 255);
    kt::rgbToFramebuffer(rgb, fmt_, p);
}

void ScreenCanvas::shade(int x, int y, int w, int h, uint8_t a) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    for (int xx = x; xx < x + w; xx++)
        for (int yy = y; yy < y + h; yy++) {
            u8 *p = s_.GetFramebuffer(xx, yy);
            uint8_t rgb[3];
            kt::framebufferToRGB(p, 1, fmt_, bpp_, rgb);
            for (int k = 0; k < 3; k++) rgb[k] = (uint8_t)(rgb[k] * (255 - a) / 255);
            kt::rgbToFramebuffer(rgb, fmt_, p);
        }
}

}  // namespace CTRPluginFramework
