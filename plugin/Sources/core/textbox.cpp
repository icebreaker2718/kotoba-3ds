#include "core/textbox.hpp"

#include <algorithm>
#include <cstdlib>

namespace kt {

static const int kTolerance = 20;        // per channel, around the box colour
static const unsigned kMinArea = 1500;   // pixels of box colour
static const unsigned kColours = 8;      // most common colours tried

std::vector<TextBox> findTextBoxes(const uint8_t *rgb, unsigned w, unsigned h, unsigned maxBoxes) {
    std::vector<TextBox> out;
    const unsigned n = w * h;
    if (!rgb || n == 0) return out;
    // 1. the most common colours (5 bits per channel)
    std::vector<uint32_t> hist(32768, 0);
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = rgb + i * 3;
        hist[((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3)]++;
    }
    // the most common colours, in one pass over the histogram (ties: the lower key first, as a
    // repeated "largest not yet taken" scan picks them)
    std::vector<uint32_t> top;
    {
        uint32_t cnt[kColours], keys[kColours];
        unsigned m = 0;
        for (uint32_t key = 0; key < 32768; key++) {
            uint32_t c = hist[key];
            if (!c || (m == kColours && c <= cnt[m - 1])) continue;
            unsigned at = m < kColours ? m++ : kColours - 1;
            while (at > 0 && cnt[at - 1] < c) { cnt[at] = cnt[at - 1]; keys[at] = keys[at - 1]; at--; }
            cnt[at] = c;
            keys[at] = key;
        }
        for (unsigned k = 0; k < m; k++) {
            if (cnt[k] < kMinArea / 5) break;  // a shaded box spreads over several neighbouring colours
            top.push_back(keys[k]);
        }
    }
    // which of those colours each pixel is close to: one bit per colour, all in one pass
    std::vector<uint8_t> near(n, 0);
    int cr[kColours], cg[kColours], cb[kColours];
    for (size_t k = 0; k < top.size(); k++) {
        cr[k] = (int)((top[k] >> 10) & 31) * 8 + 4;
        cg[k] = (int)((top[k] >> 5) & 31) * 8 + 4;
        cb[k] = (int)(top[k] & 31) * 8 + 4;
    }
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = rgb + i * 3;
        uint8_t bits = 0;
        for (size_t k = 0; k < top.size(); k++)
            if (abs(p[0] - cr[k]) <= kTolerance && abs(p[1] - cg[k]) <= kTolerance && abs(p[2] - cb[k]) <= kTolerance)
                bits |= (uint8_t)(1u << k);
        near[i] = bits;
    }
    // Component numbers: a colour's components are numbered above `base`, so the label array is
    // cleared only when the numbers run out, not once per colour. Positions on the fill stacks are
    // packed (y << 16 | x): no division per pixel (the 3DS's CPU has no divide instruction).
    std::vector<uint16_t> label(n, 0);
    unsigned last = 0;
    std::vector<uint32_t> stack;
    std::vector<uint8_t> outside;
    for (size_t ki = 0; ki < top.size(); ki++) {
        const uint8_t bit = (uint8_t)(1u << ki);
        if (last > 65535 - 60000) {  // not enough numbers left for this colour: start over
            std::fill(label.begin(), label.end(), 0);
            last = 0;
        }
        const unsigned base = last;
        unsigned next = base;
        // 2. connected areas of that colour
        for (unsigned start = 0; start < n && next - base < 60000; start++) {
            if (!(near[start] & bit) || label[start] > base) continue;
            ++next;
            unsigned area = 0, l = w, t = h, r = 0, b = 0;
            stack.clear();
            stack.push_back(((start / w) << 16) | (start % w));
            label[start] = (uint16_t)next;
            while (!stack.empty()) {
                uint32_t v = stack.back();
                stack.pop_back();
                area++;
                unsigned x = v & 0xFFFF, y = v >> 16;
                if (x < l) l = x;
                if (x > r) r = x;
                if (y < t) t = y;
                if (y > b) b = y;
                uint32_t i = y * w + x;
                if (x > 0 && (near[i - 1] & bit) && label[i - 1] <= base) { label[i - 1] = (uint16_t)next; stack.push_back(v - 1); }
                if (x + 1 < w && (near[i + 1] & bit) && label[i + 1] <= base) { label[i + 1] = (uint16_t)next; stack.push_back(v + 1); }
                if (y > 0 && (near[i - w] & bit) && label[i - w] <= base) { label[i - w] = (uint16_t)next; stack.push_back(v - 0x10000); }
                if (y + 1 < h && (near[i + w] & bit) && label[i + w] <= base) { label[i + w] = (uint16_t)next; stack.push_back(v + 0x10000); }
            }
            if (area < kMinArea) continue;
            unsigned bw = r - l + 1, bh = b - t + 1;
            float fill = (float)area / (bw * bh);
            // a box: mostly its colour, text-box sized, not the whole screen
            if (fill < 0.45f || bw < 80 || bh < 24 || (bw >= w - 20 && bh >= h * 5 / 6)) continue;
            // 3. the letters: pixels inside the box that aren't its colour and aren't connected to
            //    the outside of the box (a flood fill from the edge of its rectangle)
            outside.assign(bw * bh, 0);
            stack.clear();
            const uint16_t me = (uint16_t)next;
            auto inBox = [&](unsigned x, unsigned y) { return label[(t + y) * w + l + x] == me; };
            auto seed = [&](unsigned x, unsigned y) {
                if (!inBox(x, y) && !outside[y * bw + x]) { outside[y * bw + x] = 1; stack.push_back((y << 16) | x); }
            };
            for (unsigned x = 0; x < bw; x++) { seed(x, 0); seed(x, bh - 1); }
            for (unsigned y = 0; y < bh; y++) { seed(0, y); seed(bw - 1, y); }
            while (!stack.empty()) {
                uint32_t v = stack.back();
                stack.pop_back();
                unsigned x = v & 0xFFFF, y = v >> 16;
                if (x > 0) seed(x - 1, y);
                if (x + 1 < bw) seed(x + 1, y);
                if (y > 0) seed(x, y - 1);
                if (y + 1 < bh) seed(x, y + 1);
            }
            unsigned ink = 0;
            for (unsigned y = 0; y < bh; y++)
                for (unsigned x = 0; x < bw; x++)
                    if (!inBox(x, y) && !outside[y * bw + x]) ink++;
            float inkFrac = (float)ink / (bw * bh);
            if (inkFrac < 0.02f || inkFrac > 0.6f) continue;
            TextBox tb;
            tb.x = l;
            tb.y = t;
            tb.w = bw;
            tb.h = bh;
            tb.r = (uint8_t)cr[ki];
            tb.g = (uint8_t)cg[ki];
            tb.b = (uint8_t)cb[ki];
            tb.score = area * std::min(1.0f, inkFrac * 10);
            out.push_back(tb);
        }
        last = next;
    }
    std::sort(out.begin(), out.end(), [](const TextBox &a, const TextBox &b) { return a.score > b.score; });
    // neighbouring colours find the same box again: keep the best of each
    std::vector<TextBox> unique;
    for (const TextBox &b : out) {
        bool dup = false;
        for (const TextBox &u : unique) {
            unsigned ix0 = std::max(b.x, u.x), iy0 = std::max(b.y, u.y);
            unsigned ix1 = std::min(b.x + b.w, u.x + u.w), iy1 = std::min(b.y + b.h, u.y + u.h);
            if (ix1 > ix0 && iy1 > iy0 && (ix1 - ix0) * (iy1 - iy0) * 10 > std::min(b.w * b.h, u.w * u.h) * 7) dup = true;
        }
        if (!dup) unique.push_back(b);
        if (unique.size() >= maxBoxes) break;
    }
    return unique;
}

}  // namespace kt
