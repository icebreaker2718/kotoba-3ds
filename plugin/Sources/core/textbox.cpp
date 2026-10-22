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
    std::vector<uint32_t> top;
    for (unsigned k = 0; k < kColours; k++) {
        uint32_t best = 0, bestKey = 0;
        for (uint32_t key = 0; key < 32768; key++)
            if (hist[key] > best && std::find(top.begin(), top.end(), key) == top.end()) { best = hist[key]; bestKey = key; }
        if (best < kMinArea / 5) break;  // a shaded box spreads over several neighbouring colours
        top.push_back(bestKey);
    }
    std::vector<uint8_t> mask(n);
    std::vector<uint16_t> label(n);
    std::vector<uint32_t> stack;
    std::vector<uint8_t> outside;
    for (uint32_t key : top) {
        int cr = (int)((key >> 10) & 31) * 8 + 4, cg = (int)((key >> 5) & 31) * 8 + 4, cb = (int)(key & 31) * 8 + 4;
        for (unsigned i = 0; i < n; i++) {
            const uint8_t *p = rgb + i * 3;
            mask[i] = abs(p[0] - cr) <= kTolerance && abs(p[1] - cg) <= kTolerance && abs(p[2] - cb) <= kTolerance;
        }
        // 2. connected areas of that colour
        std::fill(label.begin(), label.end(), 0);
        uint16_t next = 0;
        for (unsigned start = 0; start < n && next < 60000; start++) {
            if (!mask[start] || label[start]) continue;
            ++next;
            unsigned area = 0, l = w, t = h, r = 0, b = 0;
            stack.clear();
            stack.push_back(start);
            label[start] = next;
            while (!stack.empty()) {
                uint32_t i = stack.back();
                stack.pop_back();
                area++;
                unsigned x = i % w, y = i / w;
                if (x < l) l = x;
                if (x > r) r = x;
                if (y < t) t = y;
                if (y > b) b = y;
                uint32_t nb[4] = {x > 0 ? i - 1 : UINT32_MAX, x + 1 < w ? i + 1 : UINT32_MAX,
                                  y > 0 ? i - w : UINT32_MAX, y + 1 < h ? i + w : UINT32_MAX};
                for (uint32_t j : nb)
                    if (j != UINT32_MAX && mask[j] && !label[j]) { label[j] = next; stack.push_back(j); }
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
            auto inBox = [&](unsigned x, unsigned y) { return label[y * w + x] == next; };
            for (unsigned x = 0; x < bw; x++)
                for (unsigned y : {0u, bh - 1})
                    if (!inBox(l + x, t + y) && !outside[y * bw + x]) { outside[y * bw + x] = 1; stack.push_back(y * bw + x); }
            for (unsigned y = 0; y < bh; y++)
                for (unsigned x : {0u, bw - 1})
                    if (!inBox(l + x, t + y) && !outside[y * bw + x]) { outside[y * bw + x] = 1; stack.push_back(y * bw + x); }
            while (!stack.empty()) {
                uint32_t i = stack.back();
                stack.pop_back();
                unsigned x = i % bw, y = i / bw;
                uint32_t nb[4] = {x > 0 ? i - 1 : UINT32_MAX, x + 1 < bw ? i + 1 : UINT32_MAX,
                                  y > 0 ? i - bw : UINT32_MAX, y + 1 < bh ? i + bw : UINT32_MAX};
                for (uint32_t j : nb) {
                    if (j == UINT32_MAX || outside[j]) continue;
                    if (inBox(l + j % bw, t + j / bw)) continue;
                    outside[j] = 1;
                    stack.push_back(j);
                }
            }
            unsigned ink = 0;
            for (unsigned y = 0; y < bh; y++)
                for (unsigned x = 0; x < bw; x++)
                    if (!inBox(l + x, t + y) && !outside[y * bw + x]) ink++;
            float inkFrac = (float)ink / (bw * bh);
            if (inkFrac < 0.02f || inkFrac > 0.6f) continue;
            TextBox tb;
            tb.x = l;
            tb.y = t;
            tb.w = bw;
            tb.h = bh;
            tb.r = (uint8_t)cr;
            tb.g = (uint8_t)cg;
            tb.b = (uint8_t)cb;
            tb.score = area * std::min(1.0f, inkFrac * 10);
            out.push_back(tb);
        }
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
