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
// The framebuffer is stored column by column, bottom to top: pixel (x, y) is at
// base + (x * column + 239 - y) * bpp, `column` = Stride / bpp pixels (240 plus any gap).
ScreenCanvas::ScreenCanvas(const Screen &s)
    : base_((u8 *)s.LeftFramebuffer), fmt_((int)s.Format), bpp_(s.BytesPerPixel),
      column_(s.BytesPerPixel ? s.Stride / s.BytesPerPixel : 240) {
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
    if (bpp_ == 4 && !((uintptr_t)base_ & 3)) {  // RGBA8: a word per pixel
        const u32 v = (u32)px[0] | (u32)px[1] << 8 | (u32)px[2] << 16 | (u32)px[3] << 24;
        for (int xx = x; xx < x + w; xx++) {
            u32 *q = (u32 *)(void *)at(xx, y + h - 1);
            for (int k = 0; k < h; k++) q[k] = v;
        }
        return;
    }
    for (int xx = x; xx < x + w; xx++) {
        u8 *p = at(xx, y + h - 1);  // the column runs upwards in memory
        for (int k = 0; k < h; k++, p += bpp_)
            for (unsigned b = 0; b < bpp_; b++) p[b] = px[b];
    }
}

void ScreenCanvas::blend(int x, int y, kt::Rgb c, uint8_t a) {
    if (x < 0 || y < 0 || x >= width || y >= height || !a) return;
    u8 *p = at(x, y);
    if (a == 255) {
        uint8_t rgb[3] = {c.r, c.g, c.b};
        kt::rgbToFramebuffer(rgb, fmt_, p);
        return;
    }
    uint8_t rgb[3];
    kt::framebufferToRGB(p, 1, fmt_, bpp_, rgb);
    rgb[0] = (uint8_t)((c.r * a + rgb[0] * (255 - a)) / 255);
    rgb[1] = (uint8_t)((c.g * a + rgb[1] * (255 - a)) / 255);
    rgb[2] = (uint8_t)((c.b * a + rgb[2] * (255 - a)) / 255);
    kt::rgbToFramebuffer(rgb, fmt_, p);
}

// blend() of a glyph, a column at a time (one column of the framebuffer is one run of memory,
// bottom to top): no call per pixel, and 8-bit formats read and written in place, a 32-bit pixel
// as one word.
void ScreenCanvas::blendBlock(int x, int y, int w, int h, const uint8_t *alpha, kt::Rgb c) {
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + w > width ? width - x : w, y1 = y + h > height ? height - y : h;
    if (x0 >= x1 || y0 >= y1) return;
    if (bpp_ < 3) {  // 16-bit formats: as blend()
        kt::Canvas::blendBlock(x, y, w, h, alpha, c);
        return;
    }
    uint8_t full[4];
    {
        uint8_t rgb[3] = {c.r, c.g, c.b};
        kt::rgbToFramebuffer(rgb, fmt_, full);
    }
    const unsigned R = bpp_ - 1, G = bpp_ - 2, B = bpp_ - 3;  // RGBA8: A B G R, BGR8: B G R
    const bool words = bpp_ == 4 && !((uintptr_t)base_ & 3);   // RGBA8 pixels start word-aligned
    for (int dx = x0; dx < x1; dx++) {
        u8 *p = at(x + dx, y + y0);
        const uint8_t *a = alpha + (size_t)y0 * w + dx;
        for (int dy = y0; dy < y1; dy++, p -= bpp_, a += w) {
            const unsigned v = *a;
            if (!v) continue;
            if (v == 255) {
                for (unsigned b = 0; b < bpp_; b++) p[b] = full[b];
                continue;
            }
            const unsigned k = 255 - v;
            if (bpp_ == 4 && words) {
                const u32 old = *(const u32 *)(const void *)p;
                const u32 nr = (c.r * v + (old >> 24) * k) / 255, ng = (c.g * v + ((old >> 16) & 0xFF) * k) / 255,
                          nb = (c.b * v + ((old >> 8) & 0xFF) * k) / 255;
                *(u32 *)(void *)p = (nr << 24) | (ng << 16) | (nb << 8) | full[0];
            } else {
                p[R] = (u8)((c.r * v + p[R] * k) / 255);
                p[G] = (u8)((c.g * v + p[G] * k) / 255);
                p[B] = (u8)((c.b * v + p[B] * k) / 255);
            }
        }
    }
}

void ScreenCanvas::shade(int x, int y, int w, int h, uint8_t a) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) return;
    const unsigned keep = 255 - a;
    const bool words = bpp_ == 4 && fmt_ == 0 && !((uintptr_t)base_ & 3);
    for (int xx = x; xx < x + w; xx++) {
        u8 *p = at(xx, y + h - 1);
        if (words) {  // RGBA8 a word at a time: the same bytes, one read and one write per pixel
            u32 *q = (u32 *)(void *)p;
            for (int k = 0; k < h; k++, q++) {
                const u32 v = *q;
                *q = ((((v >> 24) * keep) >> 8) << 24) | (((((v >> 16) & 0xFF) * keep) >> 8) << 16) |
                     (((((v >> 8) & 0xFF) * keep) >> 8) << 8) | (v & 0xFF);
            }
            continue;
        }
        for (int k = 0; k < h; k++, p += bpp_) {
            if (bpp_ >= 3) {  // 8 bits a channel (RGBA8 keeps its alpha byte)
                for (unsigned b = (fmt_ == 0 ? 1 : 0); b < bpp_; b++) p[b] = (u8)((p[b] * keep) >> 8);
            } else {
                uint8_t rgb[3];
                kt::framebufferToRGB(p, 1, fmt_, bpp_, rgb);
                for (int q = 0; q < 3; q++) rgb[q] = (uint8_t)((rgb[q] * keep) >> 8);
                kt::rgbToFramebuffer(rgb, fmt_, p);
            }
        }
    }
}

}  // namespace CTRPluginFramework
