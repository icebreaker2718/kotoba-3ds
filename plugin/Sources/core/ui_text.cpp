#include "core/ui_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

TextRenderer::~TextRenderer() {
    free(slots_);
    free(slab_);
}

bool TextRenderer::reserve() {
    if (slab_) return true;
    if (!slots_) slots_ = (Slot *)malloc(kSlots * sizeof(Slot));
    if (!slots_) return false;
    std::fill(slots_, slots_ + kSlots, Slot());
    for (size_t bytes : {(size_t)96 * 1024, (size_t)48 * 1024, (size_t)16 * 1024})  // as much as fits
        if ((slab_ = (uint8_t *)malloc(bytes)) != nullptr) {
            slabBytes_ = bytes;
            break;
        }
    slabUsed_ = 0;
    count_ = 0;
    return slab_ != nullptr;
}

// The glyph scaled to `size` (made once, then kept: the same characters are drawn on every redraw).
const TextRenderer::Scaled *TextRenderer::scaled(uint32_t cp, int size) {
    if (size <= 0 || size > 255) return nullptr;
    const uint32_t key = (cp << 8) | (uint32_t)size;
    if (!slab_ && !reserve()) return nullptr;
    unsigned at = (key * 2654435761u) >> 22;  // 10 bits: kSlots
    while (slots_[at].key && slots_[at].key != key) at = (at + 1) & (kSlots - 1);
    if (slots_[at].key == key) {
        const Slot &sl = slots_[at];
        found_.advance = sl.advance;
        found_.ox = sl.ox;
        found_.w = sl.w;
        found_.h = sl.h;
        found_.alpha = slab_ + sl.off;
        return &found_;
    }
    GlyphInfo gi;
    if (!font_.metrics(cp, gi)) return nullptr;
    const int cw = font_.cellWidth(), ch = font_.cellHeight();
    const float s = (float)size / ch;  // destination pixels per source pixel
    Slot g;
    g.key = key;
    g.advance = (int16_t)std::lround(gi.advance * s);
    g.ox = (int16_t)std::lround(gi.left * s);
    int w = (gi.width > 0 && cp != ' ' && cp != 0x3000) ? (int)std::ceil(gi.width * s) : 0;
    g.h = (uint16_t)size;
    const size_t bytes = (size_t)(w > 0 ? w : 0) * size;
    if (bytes > slabBytes_) w = 0;
    if (slabUsed_ + bytes > slabBytes_ || count_ + 1 > kSlots * 3 / 4) {  // full: start over
        std::fill(slots_, slots_ + kSlots, Slot());
        slabUsed_ = 0;
        count_ = 0;
        at = (key * 2654435761u) >> 22;
    }
    cell_.resize((size_t)cw * ch);
    uint8_t *alpha = slab_ + slabUsed_;
    if (w > 0 && font_.pixels(cp, cell_.data())) {
        // area-weighted resampling: each destination pixel averages the source area it covers;
        // the cell holds the glyph's pixels from column 0
        std::fill(alpha, alpha + (size_t)w * size, 0);
        const float inv = 1.0f / s;
        for (int dy = 0; dy < size; dy++) {
            float sy0 = dy * inv, sy1 = (dy + 1) * inv;
            for (int dx = 0; dx < w; dx++) {
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
                float a = sum / area * 1.25f;  // a slight boost: thin strokes stay solid when shrunk
                alpha[(size_t)dy * w + dx] = a > 255 ? 255 : (uint8_t)a;
            }
        }
    } else {
        w = 0;
    }
    g.w = (uint16_t)w;
    g.off = (uint32_t)slabUsed_;
    slabUsed_ += (size_t)w * size;
    while (slots_[at].key) at = (at + 1) & (kSlots - 1);
    slots_[at] = g;
    count_++;
    found_.advance = g.advance;
    found_.ox = g.ox;
    found_.w = g.w;
    found_.h = g.h;
    found_.alpha = alpha;
    return &found_;
}

int TextRenderer::drawChar(Canvas &c, int x, int y, uint32_t cp, int size, Rgb color) {
    const Scaled *g = scaled(fallback(cp), size);
    if (!g) return x;
    if (g->w > 0) c.blendBlock(x + g->ox, y, g->w, g->h, g->alpha, color);
    return x + g->advance;
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
