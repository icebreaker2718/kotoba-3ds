#include "core/ui_text.hpp"

#include <algorithm>
#include <cmath>

#include "core/kana.hpp"
#include "core/wrap.hpp"

namespace kt {

uint32_t TextRenderer::fallback(uint32_t cp) {
    GlyphInfo gi;
    if (font_.metrics(cp, gi)) return cp;
    if (cp == 0xFF5E && font_.metrics(0x301C, gi)) return 0x301C;  // ～ -> 〜
    if (cp >= 0xFF01 && cp <= 0xFF5E && font_.metrics(cp - 0xFEE0, gi)) return cp - 0xFEE0;  // full-width ASCII
    return '?';
}

int TextRenderer::advance(uint32_t cp, int size) {
    GlyphInfo gi;
    if (!font_.metrics(fallback(cp), gi)) return 0;
    return (int)std::lround(gi.advance * (float)size / font_.cellHeight());
}

int TextRenderer::width(const std::string &text, int size) {
    int w = 0;
    for (size_t i = 0; i < text.size();) w += advance(utf8Next(text, i), size);
    return w;
}

int TextRenderer::drawChar(Canvas &c, int x, int y, uint32_t cp, int size, Rgb color) {
    cp = fallback(cp);
    GlyphInfo gi;
    if (!font_.metrics(cp, gi)) return x;
    const int cw = font_.cellWidth(), ch = font_.cellHeight();
    const float s = (float)size / ch;  // destination pixels per source pixel
    int adv = (int)std::lround(gi.advance * s);
    if (gi.width <= 0 || cp == ' ' || cp == 0x3000) return x + adv;
    cell_.resize((size_t)cw * ch);
    if (!font_.pixels(cp, cell_.data())) return x + adv;
    // area-weighted resampling: each destination pixel averages the source area it covers. The
    // cell holds the glyph's pixels from column 0; `left` is where they go.
    const int ox = x + (int)std::lround(gi.left * s);
    const int dw = (int)std::ceil(gi.width * s);
    const float inv = 1.0f / s;
    for (int dy = 0; dy < size; dy++) {
        float sy0 = dy * inv, sy1 = (dy + 1) * inv;
        int py = y + dy;
        if (py < 0 || py >= c.height) continue;
        for (int dx = 0; dx < dw; dx++) {
            int px = ox + dx;
            if (px < 0 || px >= c.width) continue;
            float sx0 = dx * inv, sx1 = (dx + 1) * inv;
            float sum = 0, area = 0;
            for (int sy = (int)sy0; sy < ch && sy < sy1; sy++) {
                float wy = std::min<float>(sy + 1, sy1) - std::max<float>(sy, sy0);
                if (wy <= 0) continue;
                for (int sx = (int)sx0; sx < cw && sx < sx1 && sx < gi.width; sx++) {
                    float wx = std::min<float>(sx + 1, sx1) - std::max<float>(sx, sx0);
                    if (wx <= 0) continue;
                    sum += wx * wy * cell_[(size_t)sy * cw + sx];
                    area += wx * wy;
                }
            }
            if (area <= 0) continue;
            // a slight boost: thin strokes stay solid when shrunk
            float a = sum / area * 1.25f;
            if (a >= 1) c.blend(px, py, color, a > 255 ? 255 : (uint8_t)a);
        }
    }
    return x + adv;
}

int TextRenderer::draw(Canvas &c, int x, int y, const std::string &text, int size, Rgb color) {
    for (size_t i = 0; i < text.size();) x = drawChar(c, x, y, utf8Next(text, i), size, color);
    return x;
}

static TextRenderer *g_wrapRenderer = nullptr;  // wrapText takes a plain function
static int g_wrapSize = 0;
static float wrapWidth(const std::string &s) { return (float)g_wrapRenderer->width(s, g_wrapSize); }

std::vector<std::string> TextRenderer::wrap(const std::string &text, int size, int maxWidth) {
    g_wrapRenderer = this;
    g_wrapSize = size;
    return wrapText(text, (float)maxWidth, wrapWidth);
}

}  // namespace kt
