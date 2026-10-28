// Built for speed (the plugin's C++ is otherwise built for size): this runs on every read.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "core/textbox.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace kt {

static const int kTolerance = 20;        // per channel, around the box colour
static const unsigned kMinArea = 1500;   // pixels of box colour
static const unsigned kColours = 8;      // most common colours tried
static const int kGrowStep = 14;       // per channel, between neighbouring pixels of a shaded box
static const int kGrowTolerance = 96;  // per channel, how far the shading may drift from the box colour
static const unsigned kSpanCap = 8192;   // fill stack entries (32 KB); a whole screen of one colour needs ~500

namespace {
// Working memory, from malloc so that running short gives "no box" (the whole screen is read
// then) instead of the abort a failed std::vector allocation is in the plugin.
struct Buf {
    void *p = nullptr;
    explicit Buf(size_t bytes) : p(malloc(bytes ? bytes : 1)) {}
    ~Buf() { free(p); }
    Buf(const Buf &) = delete;
    Buf &operator=(const Buf &) = delete;
};

// Scanline flood fill from (sx, sy) over the pixels ok(x, y) accepts: mark(x0, x1, y) gets each
// run of them (which ok must then refuse). The same pixels as a pixel-by-pixel fill, with a small
// stack: one entry per run start, (y << 16 | x). False if the stack ran out.
template <class Ok, class Mark>
bool scanFill(unsigned sx, unsigned sy, unsigned W, unsigned H, uint32_t *stk, Ok ok, Mark mark) {
    unsigned top = 0;
    stk[top++] = (sy << 16) | sx;
    while (top) {
        const uint32_t v = stk[--top];
        const unsigned y = v >> 16;
        unsigned x0 = v & 0xFFFF, x1 = x0;
        if (!ok(x0, y)) continue;
        while (x0 > 0 && ok(x0 - 1, y)) x0--;
        while (x1 + 1 < W && ok(x1 + 1, y)) x1++;
        mark(x0, x1, y);
        for (int d = -1; d <= 1; d += 2) {  // the rows above and below: each run under this one
            if ((d < 0 && y == 0) || (d > 0 && y + 1 >= H)) continue;
            const unsigned ny = y + d;
            bool in = false;
            for (unsigned x = x0; x <= x1; x++) {
                const bool o = ok(x, ny);
                if (o && !in) {
                    if (top == kSpanCap) return false;
                    stk[top++] = (ny << 16) | x;
                }
                in = o;
            }
        }
    }
    return true;
}
}  // namespace

std::vector<TextBox> findTextBoxes(const uint8_t *rgb, unsigned w, unsigned h, unsigned maxBoxes, bool growShaded) {
    std::vector<TextBox> out;
    const unsigned n = w * h;
    if (!rgb || n == 0 || w > 0xFFFF || h > 0xFFFF) return out;
    // 1. the most common colours (5 bits per channel). 16-bit counts, held at 65535: at most one
    //    colour of a screen can get there, and it is the most common either way.
    std::vector<uint32_t> top;
    {
        Buf hb(32768 * sizeof(uint16_t));
        uint16_t *H = (uint16_t *)hb.p;
        if (!H) return out;
        memset(H, 0, 32768 * sizeof(uint16_t));
        const uint8_t *p = rgb;
        for (unsigned i = 0; i < n; i++, p += 3) {
            uint16_t &c = H[((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3)];
            if (c != 0xFFFF) c++;
        }
        // the most common colours, in one pass over the histogram (ties: the lower key first, as
        // a repeated "largest not yet taken" scan picks them)
        uint32_t cnt[kColours], keys[kColours];
        unsigned m = 0;
        for (uint32_t key = 0; key < 32768; key++) {
            uint32_t c = H[key];
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
    Buf nearBuf(n), labelBuf((size_t)n * sizeof(uint16_t)), stackBuf(kSpanCap * sizeof(uint32_t));
    uint8_t *near = (uint8_t *)nearBuf.p;
    uint16_t *label = (uint16_t *)labelBuf.p;
    uint32_t *stk = (uint32_t *)stackBuf.p;
    if (!near || !label || !stk) return out;
    int cr[kColours], cg[kColours], cb[kColours];
    for (size_t k = 0; k < top.size(); k++) {
        cr[k] = (int)((top[k] >> 10) & 31) * 8 + 4;
        cg[k] = (int)((top[k] >> 5) & 31) * 8 + 4;
        cb[k] = (int)(top[k] & 31) * 8 + 4;
    }
    // per channel and value: the colours within the tolerance on that channel; a pixel is near the
    // colours all three of its channels are
    {
        Buf lb(3 * 256);  // R, G, B (on the heap: gcc then keeps its address in a register)
        uint8_t *L = (uint8_t *)lb.p;
        if (!L) return out;
        memset(L, 0, 3 * 256);
        for (int v = 0; v < 256; v++)
            for (size_t k = 0; k < top.size(); k++) {
                const uint8_t bit = (uint8_t)(1u << k);
                if (abs(v - cr[k]) <= kTolerance) L[v] |= bit;
                if (abs(v - cg[k]) <= kTolerance) L[256 + v] |= bit;
                if (abs(v - cb[k]) <= kTolerance) L[512 + v] |= bit;
            }
        const uint8_t *p = rgb;
        for (unsigned i = 0; i < n; i++, p += 3) near[i] = (uint8_t)(L[p[0]] & L[256 + p[1]] & L[512 + p[2]]);
    }
    // Component numbers: a colour's components are numbered above `base`, so the label array is
    // cleared only when the numbers run out, not once per colour.
    memset(label, 0, (size_t)n * sizeof(uint16_t));
    unsigned last = 0;
    for (size_t ki = 0; ki < top.size(); ki++) {
        const uint8_t bit = (uint8_t)(1u << ki);
        if (last > 65535 - 60000) {  // not enough numbers left for this colour: start over
            memset(label, 0, (size_t)n * sizeof(uint16_t));
            last = 0;
        }
        const unsigned base = last;
        unsigned next = base;
        // 2. connected areas of that colour
        const uint32_t bit4 = bit * 0x01010101u;
        for (unsigned start = 0; start < n && next - base < 60000; start++) {
            // most pixels aren't this colour: past them 4 at a time
            for (uint32_t q; start + 4 <= n; start += 4) {
                memcpy(&q, near + start, 4);
                if (q & bit4) break;
            }
            if (start >= n) break;
            if (!(near[start] & bit) || label[start] > base) continue;
            ++next;
            const uint16_t me = (uint16_t)next;
            unsigned area = 0, l = w, t = h, r = 0, b = 0;
            bool shaded = false;
            const unsigned sy = start / w, sx = start - sy * w;  // one division per area
            bool filled = scanFill(
                sx, sy, w, h, stk,
                [&](unsigned x, unsigned y) {
                    const unsigned i = y * w + x;
                    return (near[i] & bit) && label[i] <= base;
                },
                [&](unsigned x0, unsigned x1, unsigned y) {
                    uint16_t *row = label + y * w;
                    for (unsigned x = x0; x <= x1; x++) row[x] = me;
                    area += x1 - x0 + 1;
                    if (x0 < l) l = x0;
                    if (x1 > r) r = x1;
                    if (y < t) t = y;
                    if (y > b) b = y;
                });
            if (!filled) return std::vector<TextBox>();  // a screen this odd: no box, read it all
            // A shaded box (lighter at the top, bluer at the bottom: Pokémon's speech bubbles)
            // drifts out of the tolerance part way down. Let it grow into neighbouring pixels
            // whose colour changes only a little from pixel to pixel: a gradient goes on, the
            // box's outline and the letters stop it. A box is closed by its outline: growth that
            // reaches the edge of the area searched is smooth scenery instead, and is taken back.
            if (growShaded && area >= kMinArea && r - l + 1 >= 80 && b - t + 1 >= 16 &&
                next - base < 60000) {  // already box-like
                const uint16_t grown = (uint16_t)++next;  // its own number until it's kept
                const unsigned bw0 = r - l + 1, bh0 = b - t + 1;
                const unsigned mx = bw0 / 4 + 8, my = bh0 + 16;
                const unsigned gx0 = l > mx ? l - mx : 0, gx1 = std::min(w - 1, r + mx);
                const unsigned gy0 = t > my ? t - my : 0, gy1 = std::min(h - 1, b + my);
                auto px = [&](unsigned i) { return rgb + (size_t)i * 3; };
                auto close = [&](unsigned i, unsigned j) {
                    const uint8_t *a = px(i), *c = px(j);
                    return abs(a[0] - c[0]) <= kGrowStep && abs(a[1] - c[1]) <= kGrowStep && abs(a[2] - c[2]) <= kGrowStep;
                };
                auto can = [&](unsigned i) {
                    if (label[i] > base) return false;  // this box, or another of its colour
                    const uint8_t *a = px(i);
                    return abs(a[0] - cr[ki]) <= kGrowTolerance && abs(a[1] - cg[ki]) <= kGrowTolerance &&
                           abs(a[2] - cb[ki]) <= kGrowTolerance;
                };
                auto ours = [&](unsigned i) { return label[i] == me || label[i] == grown; };
                bool leaked = false, any = false;
                unsigned gl = l, gt = t, gr = r, gb = b, garea = 0;
                auto take = [&](unsigned x, unsigned y, unsigned i) {
                    label[i] = grown;
                    garea++;
                    any = true;
                    if (x == gx0 || x == gx1 || y == gy0 || y == gy1) leaked = true;
                    if (x < gl) gl = x;
                    if (x > gr) gr = x;
                    if (y < gt) gt = y;
                    if (y > gb) gb = y;
                };
                // Grown as a flood fill: from the pixels next to the box, then only the neighbours
                // of pixels just taken (the stack, free again, as a ring of pixels to look around).
                // When the ring is full, a sweep finds what it dropped: the same pixels either way.
                const unsigned cap = kSpanCap;
                unsigned head = 0, count = 0;
                bool dropped = false;
                auto push = [&](unsigned i) {
                    if (count == cap) { dropped = true; return; }
                    stk[(head + count++) % cap] = i;
                };
                // takes i (x, y) if it carries on from a pixel of ours next to it
                auto tryTake = [&](unsigned x, unsigned y, unsigned i) {
                    if (ours(i) || !can(i)) return;
                    if ((x > gx0 && ours(i - 1) && close(i, i - 1)) || (x < gx1 && ours(i + 1) && close(i, i + 1)) ||
                        (y > gy0 && ours(i - w) && close(i, i - w)) || (y < gy1 && ours(i + w) && close(i, i + w))) {
                        take(x, y, i);
                        push(i);
                    }
                };
                // The first pass looks only right around the box (a pixel it can start from is next
                // to one of the box's); a pass after the ring overflowed, around all that grew.
                unsigned sx0 = l > gx0 ? l - 1 : gx0, sx1 = std::min(gx1, r + 1);
                unsigned sy0 = t > gy0 ? t - 1 : gy0, sy1 = std::min(gy1, b + 1);
                bool sweep = true;
                while (sweep && !leaked) {
                    sweep = false;
                    dropped = false;
                    const uint64_t me4 = 0x0001000100010001ull * me;
                    for (unsigned y = sy0; y <= sy1 && !leaked; y++)
                        for (unsigned x = sx0; x <= sx1; x++) {
                            const unsigned i = y * w + x;
                            uint64_t q;  // inside the box: four of its own pixels at a time
                            if (x + 4 <= sx1 + 1 && (memcpy(&q, label + i, 8), q == me4)) {
                                x += 3;
                                continue;
                            }
                            tryTake(x, y, i);
                        }
                    while (count && !leaked) {
                        const unsigned i = stk[head];
                        head = (head + 1) % cap;
                        count--;
                        const unsigned y = i / w, x = i - y * w;
                        if (x > gx0) tryTake(x - 1, y, i - 1);
                        if (x < gx1) tryTake(x + 1, y, i + 1);
                        if (y > gy0) tryTake(x, y - 1, i - w);
                        if (y < gy1) tryTake(x, y + 1, i + w);
                    }
                    if (dropped) {  // the ring overflowed: look again, around everything ours
                        sweep = true;
                        sx0 = gl > gx0 ? gl - 1 : gx0, sx1 = std::min(gx1, gr + 1);
                        sy0 = gt > gy0 ? gt - 1 : gy0, sy1 = std::min(gy1, gb + 1);
                    }
                }
                // a text box grown this way is a wide band of lines, not a lump (a shaded cushion)
                if (any && (gr - gl + 1) * 2 < (gb - gt + 1) * 3) leaked = true;
                if (any)  // what grew lies within gl..gr, gt..gb
                    for (unsigned y = gt; y <= gb; y++)
                        for (unsigned x = gl; x <= gr; x++) {
                            uint16_t &v = label[y * w + x];
                            if (v == grown) v = leaked ? 0 : me;
                        }
                if (any && !leaked) {
                    shaded = true;
                    area += garea;
                    l = gl, t = gt, r = gr, b = gb;
                }
            }
            if (area < kMinArea) continue;
            unsigned bw = r - l + 1, bh = b - t + 1;
            float fill = (float)area / (bw * bh);
            // a box: mostly its colour, text-box sized, not the whole screen
            if (fill < 0.45f || bw < 80 || bh < 24 || (bw >= w - 20 && bh >= h * 5 / 6)) continue;
            // 3. the letters: pixels inside the box that aren't its colour and aren't connected to
            //    the outside of the box (a flood fill from the edge of its rectangle)
            Buf ob((size_t)bw * bh);
            uint8_t *outside = (uint8_t *)ob.p;
            if (!outside) return std::vector<TextBox>();
            memset(outside, 0, (size_t)bw * bh);
            auto inBox = [&](unsigned x, unsigned y) { return label[(t + y) * w + l + x] == me; };
            auto okOut = [&](unsigned x, unsigned y) { return !inBox(x, y) && !outside[y * bw + x]; };
            auto markOut = [&](unsigned x0, unsigned x1, unsigned y) { memset(outside + y * bw + x0, 1, x1 - x0 + 1); };
            for (unsigned x = 0; x < bw && filled; x++) {
                if (okOut(x, 0)) filled = scanFill(x, 0, bw, bh, stk, okOut, markOut);
                if (filled && okOut(x, bh - 1)) filled = scanFill(x, bh - 1, bw, bh, stk, okOut, markOut);
            }
            for (unsigned y = 0; y < bh && filled; y++) {
                if (okOut(0, y)) filled = scanFill(0, y, bw, bh, stk, okOut, markOut);
                if (filled && okOut(bw - 1, y)) filled = scanFill(bw - 1, y, bw, bh, stk, okOut, markOut);
            }
            if (!filled) return std::vector<TextBox>();
            unsigned ink = 0, il = bw, it = bh, ir = 0, ib = 0;
            for (unsigned y = 0; y < bh; y++)
                for (unsigned x = 0; x < bw; x++)
                    if (!inBox(x, y) && !outside[y * bw + x]) {
                        ink++;
                        if (x < il) il = x;
                        if (x > ir) ir = x;
                        if (y < it) it = y;
                        if (y > ib) ib = y;
                    }
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
            tb.inkX = l + il;
            tb.inkY = t + it;
            tb.inkW = ir - il + 1;
            tb.inkH = ib - it + 1;
            tb.shaded = shaded;
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
