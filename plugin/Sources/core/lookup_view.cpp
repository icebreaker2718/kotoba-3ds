#include "core/lookup_view.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/kana.hpp"

namespace kt {

// ---------------------------------------------------------------------------
// Look (after Rubi for Vita: dark panel, white text, the word in yellow, its reading in blue)
// ---------------------------------------------------------------------------
static const Rgb kBg(15, 18, 22), kText(237, 237, 237), kDim(138, 144, 153), kWord(245, 197, 66),
    kWordBg(43, 49, 60), kReading(142, 197, 255), kConj(127, 178, 240), kSavedC(123, 216, 143),
    kRule(58, 63, 71), kButton(34, 40, 48), kButtonEdge(70, 78, 90);

// touchscreen
// kTextSize is the line height the font's 30-px cell is scaled to: 27 makes full-width characters
// about 22 px wide, 13 to a row (big enough to tap with a finger)
static const int kMargin = 12, kTextSize = 27, kRowH = 34, kTextTop = 8, kTextBottom = 182;
static const int kStatusY = 186, kStatusSize = 14, kButtonY = 206, kButtonH = 30;
static const int kBlockGap = 12;
// top screen
static const int kTopMargin = 10, kHeadSize = 24, kSmall = 15, kSenseSize = 17, kSenseLine = 22;

static bool isWordChar(uint32_t cp) {
    return isKanaCp(cp) || isKanjiCp(cp) || cp == 0x30FC || cp == 0x3005 || (cp >= '0' && cp <= '9') ||
           (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xFF10 && cp <= 0xFF19) ||
           (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A);
}

// characters that must not start a row (kinsoku): they stay at the end of the row before
static bool noRowStart(uint32_t cp) {
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case ',': case '.': case '!': case '?':
    case 0xFF01: case 0xFF1F: case 0x300D: case 0x300F: case 0xFF09: case ')': case 0x2026:
    case 0x30FC: case 0x3063: case 0x30C3: case 0x3083: case 0x3085: case 0x3087: case 0xFF5E: case 0x301C:
        return true;
    default:
        return false;
    }
}

void LookupView::layout() {
    cells_.clear();
    rows_.clear();
    const int maxW = kBottomW - 2 * kMargin;
    int y = 0;
    for (size_t b = 0; b < blocks_.size(); b++) {
        if (b > 0) y += kBlockGap;
        const std::string &t = blocks_[b];
        Row row;
        row.block = (int)b;
        row.y = y;
        row.first = (int)cells_.size();
        int x = 0;
        for (size_t i = 0; i < t.size();) {
            Cell c;
            c.block = (int)b;
            c.byte = i;
            c.cp = utf8Next(t, i);
            c.len = i - c.byte;
            c.w = tr_.advance(c.cp, kTextSize);
            // a new row when this one is full (closing punctuation may hang into the margin)
            if (x > 0 && x + c.w > maxW && !(noRowStart(c.cp) && x + c.w <= maxW + kMargin)) {
                // punctuation that can't hang takes the character before it along to the new row
                if (noRowStart(c.cp) && (int)cells_.size() - row.first > 1) {
                    Cell prev = cells_.back();
                    cells_.pop_back();
                    row.end = (int)cells_.size();
                    rows_.push_back(row);
                    y += kRowH;
                    row.y = y;
                    row.first = (int)cells_.size();
                    prev.x = kMargin;
                    prev.row = (int)rows_.size();
                    x = prev.w;
                    cells_.push_back(prev);
                    c.x = kMargin + x;
                    c.row = (int)rows_.size();
                    x += c.w;
                    cells_.push_back(c);
                    continue;
                }
                row.end = (int)cells_.size();
                rows_.push_back(row);
                y += kRowH;
                row.y = y;
                row.first = (int)cells_.size();
                x = 0;
            }
            c.x = kMargin + x;
            c.row = (int)rows_.size();
            x += c.w;
            cells_.push_back(c);
        }
        row.end = (int)cells_.size();
        rows_.push_back(row);
        y += kRowH;
    }
    contentH_ = y;
}

void LookupView::setText(const std::vector<std::string> &blocks) {
    blocks_ = blocks;
    saved_.clear();
    status_.clear();
    scrollY_ = 0;
    hidden_ = false;
    selBlock_ = -1;
    so_ = SearchOutput();
    entry_.clear();
    layout();
    stops_.assign(blocks_.size(), {});
    for (size_t b = 0; b < blocks_.size(); b++) {
        size_t at = 0;
        for (const std::string &w : chunkText(dict_, blocks_[b])) {
            size_t i = 0;
            if (isWordChar(utf8Next(w, i))) stops_[b].push_back(at);
            at += w.size();
        }
    }
    // start on the first word that has a dictionary entry
    for (size_t b = 0; b < blocks_.size() && selBlock_ < 0; b++)
        for (size_t s : stops_[b]) {
            selectAt((int)b, s);
            if (hasResult()) break;
        }
    if (!hasResult() && !stops_.empty() && !stops_[0].empty()) selectAt(0, stops_[0][0]);
}

const std::string &LookupView::sentence() const {
    static const std::string none;
    return selBlock_ >= 0 ? blocks_[selBlock_] : none;
}

std::string LookupView::selectedText() const {
    if (selBlock_ < 0) return "";
    return blocks_[selBlock_].substr(selByte_, selLen_);
}

int LookupView::selectedCell() const {
    for (size_t i = 0; i < cells_.size(); i++)
        if (cells_[i].block == selBlock_ && cells_[i].byte == selByte_) return (int)i;
    return -1;
}

bool LookupView::selectAt(int block, size_t byte) {
    if (block < 0 || block >= (int)blocks_.size() || byte >= blocks_[block].size()) return false;
    selBlock_ = block;
    selByte_ = byte;
    so_ = scanAt(dict_, blocks_[block], byte);
    trimResults(so_);
    resultIdx_ = 0;
    buildEntry();
    keepVisible();
    return true;
}

// L/R pages through what is worth seeing: every match of the longest word here (homographs),
// then at most two of each shorter length; no "words starting with…" guesses or one-kana matches
// when a real word is there.
void LookupView::trimResults(SearchOutput &so) {
    size_t longest = 0;
    bool anyFull = false;
    for (const Result &r : so.results)
        if (!r.prefixOnly) { anyFull = true; longest = std::max(longest, r.surface.size()); }
    if (!anyFull) return;
    std::vector<Result> kept;
    size_t lastLen = 0;
    unsigned ofLen = 0;
    for (const Result &r : so.results) {
        if (r.prefixOnly) continue;
        size_t len = r.surface.size();
        if (len < longest) {
            size_t i = 0;
            uint32_t cp = utf8Next(r.surface, i);
            if (i == len && isKanaCp(cp)) continue;  // a lone kana inside a longer word
            if (len != lastLen) { lastLen = len; ofLen = 0; }
            if (++ofLen > 2) continue;
        } else if (kept.size() >= 12) {
            continue;
        }
        kept.push_back(r);
    }
    // longest first (scanAt's order), shorter after
    std::stable_sort(kept.begin(), kept.end(),
                     [](const Result &a, const Result &b) { return a.surface.size() > b.surface.size(); });
    so.results.swap(kept);
}

void LookupView::buildEntry() {
    entry_.clear();
    topScroll_ = 0;
    const std::string &t = blocks_[selBlock_];
    size_t i = selByte_;
    utf8Next(t, i);
    selLen_ = i - selByte_;  // one character when nothing matches
    if (so_.results.empty()) {
        Line l;
        l.size = kSenseSize;
        l.height = kSenseLine;
        l.spans.push_back({"Nothing in the dictionary starts at", kDim});
        entry_.push_back(l);
        l.spans = {{"「" + t.substr(selByte_, selLen_) + "」", kText}};
        entry_.push_back(l);
        return;
    }
    const Result &r = so_.results[resultIdx_];
    if (!r.prefixOnly && !r.surface.empty()) selLen_ = r.surface.size();
    Entry e;
    if (!dict_.entry(r.entryId, e)) {
        Line l;
        l.spans.push_back({"Could not read this entry.", kDim});
        entry_.push_back(l);
        return;
    }
    const int maxW = kTopW - 2 * kTopMargin - 34;  // room for the 1/3 counter
    // headword (reading)
    std::string expr = expressionFor(e, r.matchedKey);
    Line head;
    head.size = kHeadSize;
    head.height = kHeadSize + 8;
    head.spans.push_back({expr, kWord});
    if (!e.reading.empty() && e.reading != expr) head.spans.push_back({" (" + e.reading + ")", kReading});
    if (tr_.width(expr + " (" + e.reading + ")", kHeadSize) > maxW) {  // too long: reading on its own line
        head.spans.resize(1);
        entry_.push_back(head);
        if (!e.reading.empty() && e.reading != expr) {
            Line rd;
            rd.size = kSenseSize + 2;
            rd.height = kSenseLine + 2;
            rd.spans.push_back({e.reading, kReading});
            entry_.push_back(rd);
        }
    } else {
        entry_.push_back(head);
    }
    auto small = [&](const std::string &text, Rgb c) {
        for (const std::string &w : tr_.wrap(text, kSmall, kTopW - 2 * kTopMargin)) {
            Line l;
            l.size = kSmall;
            l.height = kSmall + 5;
            l.spans.push_back({w, c});
            entry_.push_back(l);
        }
    };
    if (r.prefixOnly) small("(a longer word starting here)", kDim);
    if (!r.reasons.empty()) {
        std::string conj = r.reasons;
        for (size_t p; (p = conj.find(" > ")) != std::string::npos;) conj.replace(p, 3, " → ");
        small("Conjugation: " + conj, kConj);
    }
    std::string others;
    for (const std::string &k : e.kanjiForms)
        if (k != expr && others.size() < 60) others += (others.empty() ? "" : "・") + k;
    if (!others.empty()) small("Also written " + others, kDim);
    // senses: "1. (v1,vt) to eat" with the number and the part of speech in grey
    for (const std::string &sense : e.senses) {
        std::vector<std::string> lines = tr_.wrap(sense, kSenseSize, kTopW - 2 * kTopMargin);
        for (size_t k = 0; k < lines.size(); k++) {
            Line l;
            l.size = kSenseSize;
            l.height = kSenseLine;
            const std::string &w = lines[k];
            size_t grey = 0;
            if (k == 0) {
                size_t d = 0;
                while (d < w.size() && w[d] >= '0' && w[d] <= '9') d++;
                if (d > 0 && w.compare(d, 2, ". ") == 0) grey = d + 2;
                if (w.compare(grey, 1, "(") == 0) {
                    size_t close = w.find(") ", grey);
                    if (close != std::string::npos) grey = close + 2;
                }
            }
            if (grey) l.spans.push_back({w.substr(0, grey), kDim});
            l.spans.push_back({w.substr(grey), kText});
            entry_.push_back(l);
        }
    }
}

void LookupView::keepVisible() {
    int c = selectedCell();
    if (c < 0) return;
    const int areaH = kTextBottom - kTextTop;
    int top = rows_[cells_[c].row].y;
    // the whole selection (it may run onto the next row)
    int lastRow = cells_[c].row;
    for (size_t i = c; i < cells_.size() && cells_[i].block == selBlock_ && cells_[i].byte < selByte_ + selLen_; i++)
        lastRow = cells_[i].row;
    int bottom = rows_[lastRow].y + kRowH;
    if (bottom - scrollY_ > areaH) scrollY_ = bottom - areaH;
    if (top < scrollY_) scrollY_ = top;
    if (scrollY_ < 0) scrollY_ = 0;
}

int LookupView::charAt(int x, int y) const {
    if (y < kTextTop || y >= kTextBottom) return -1;
    int cy = y - kTextTop + scrollY_;
    for (const Row &r : rows_) {
        if (cy < r.y || cy >= r.y + kRowH) continue;
        int best = -1, bestD = 1 << 30;
        for (int i = r.first; i < r.end; i++) {
            const Cell &c = cells_[i];
            int d = x < c.x ? c.x - x : (x >= c.x + c.w ? x - (c.x + c.w) + 1 : 0);
            if (d < bestD) { bestD = d; best = i; }
        }
        return bestD <= 8 ? best : -1;
    }
    return -1;
}

bool LookupView::tap(int x, int y, Button &button) {
    button = kNone;
    if (y >= kButtonY - 4) {
        int w = (kBottomW - 16) / 3;
        int i = (x - 8) / w;
        if (x < 8 || i > 2) return false;
        button = i == 0 ? kSave : (i == 1 ? kHide : kClose);
        return false;
    }
    int c = charAt(x, y);
    if (c < 0 || !isWordChar(cells_[c].cp)) return false;
    if (cells_[c].block == selBlock_ && cells_[c].byte == selByte_) return false;  // already shown
    status_.clear();
    return selectAt(cells_[c].block, cells_[c].byte);
}

bool LookupView::moveWord(int dir) {
    if (selBlock_ < 0) return false;
    if (dir > 0) {
        size_t from = selByte_ + std::max<size_t>(selLen_, 1);
        for (size_t b = selBlock_; b < blocks_.size(); b++)
            for (size_t s : stops_[b])
                if (b > (size_t)selBlock_ || s >= from) { status_.clear(); return selectAt((int)b, s); }
    } else {
        for (int b = selBlock_; b >= 0; b--)
            for (size_t k = stops_[b].size(); k-- > 0;)
                if (b < selBlock_ || stops_[b][k] < selByte_) { status_.clear(); return selectAt(b, stops_[b][k]); }
    }
    return false;
}

bool LookupView::moveRow(int dir) {
    int c = selectedCell();
    if (c < 0) return false;
    int target = cells_[c].row + dir;
    if (target < 0 || target >= (int)rows_.size()) return false;
    int cx = cells_[c].x + cells_[c].w / 2, best = -1, bestD = 1 << 30;
    for (int i = rows_[target].first; i < rows_[target].end; i++) {
        if (!isWordChar(cells_[i].cp)) continue;
        int d = std::abs(cells_[i].x + cells_[i].w / 2 - cx);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    status_.clear();
    return selectAt(cells_[best].block, cells_[best].byte);
}

bool LookupView::page(int dir) {
    size_t n = so_.results.size();
    if (n < 2) return false;
    resultIdx_ = (resultIdx_ + n + (dir > 0 ? 1 : n - 1)) % n;
    buildEntry();
    keepVisible();
    return true;
}

int LookupView::topLinesFit() const {  // from topScroll_
    int y = kTopMargin, n = 0;
    for (size_t i = topScroll_; i < entry_.size(); i++, n++) {
        if (y + entry_[i].height > kTopH - 6) break;
        y += entry_[i].height;
    }
    return n;
}

bool LookupView::scroll(int lines) {
    int before = topScroll_;
    // the furthest down that still fills the screen
    int maxScroll = (int)entry_.size();
    for (int h = 0; maxScroll > 0 && h + entry_[maxScroll - 1].height <= kTopH - 6 - kTopMargin;)
        h += entry_[--maxScroll].height;
    topScroll_ = std::max(0, std::min(topScroll_ + lines, maxScroll));
    return topScroll_ != before;
}

void LookupView::markSaved() {
    if (selBlock_ >= 0) saved_.push_back({selBlock_, selByte_, selLen_});
}

bool LookupView::isSaved(const Cell &c) const {
    for (const Saved &s : saved_)
        if (s.block == c.block && c.byte >= s.byte && c.byte < s.byte + s.len) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void frame(Canvas &c, int x, int y, int w, int h, Rgb fill, Rgb edge) {
    c.fill(x, y, w, h, edge);
    c.fill(x + 1, y + 1, w - 2, h - 2, fill);
}

void LookupView::drawBottom(Canvas &c) {
    c.fill(0, 0, kBottomW, kBottomH, kBg);
    const int areaH = kTextBottom - kTextTop;
    // block separators
    for (size_t r = 1; r < rows_.size(); r++)
        if (rows_[r].block != rows_[r - 1].block) {
            int y = kTextTop + rows_[r].y - scrollY_ - kBlockGap / 2 - 1;
            if (y >= kTextTop && y < kTextBottom) c.fill(kMargin, y, kBottomW - 2 * kMargin, 1, kRule);
        }
    for (const Row &r : rows_) {
        int y = r.y - scrollY_;
        if (y < 0 || y + kRowH > areaH) continue;  // only whole rows
        y += kTextTop;
        for (int i = r.first; i < r.end; i++) {
            const Cell &cl = cells_[i];
            bool sel = cl.block == selBlock_ && cl.byte >= selByte_ && cl.byte < selByte_ + selLen_;
            if (sel) c.fill(cl.x - 1, y + 1, cl.w + 2, kRowH - 2, kWordBg);
        }
        for (int i = r.first; i < r.end; i++) {
            const Cell &cl = cells_[i];
            bool sel = cl.block == selBlock_ && cl.byte >= selByte_ && cl.byte < selByte_ + selLen_;
            Rgb col = sel ? kWord : (isSaved(cl) ? kSavedC : kText);
            tr_.drawChar(c, cl.x, y + (kRowH - kTextSize) / 2, cl.cp, kTextSize, col);
        }
    }
    // more text above / below
    if (scrollY_ > 0) tr_.draw(c, kBottomW - 14, kTextTop - 4, "▲", 12, kDim);
    if (contentH_ - scrollY_ > areaH) tr_.draw(c, kBottomW - 14, kTextBottom - 10, "▼", 12, kDim);
    // status, or the controls
    if (!status_.empty()) {
        std::vector<std::string> s = tr_.wrap(status_, kStatusSize, kBottomW - 2 * kMargin);
        if (!s.empty()) tr_.draw(c, kMargin, kStatusY, s[0], kStatusSize, statusColor_);
    } else {
        std::string hint = " words   rows   meanings   scroll";
        int w = tr_.width(hint, 13);
        tr_.draw(c, (kBottomW - w) / 2, kStatusY + 1, hint, 13, kDim);
    }
    // buttons
    const int bw = (kBottomW - 16) / 3;
    const char *labels[3] = {" Save", hidden_ ? " Show" : " Hide", " Close"};
    for (int i = 0; i < 3; i++) {
        int x = 8 + i * bw;
        frame(c, x + 2, kButtonY, bw - 4, kButtonH, kButton, kButtonEdge);
        Rgb col = (i == 0 && !hasResult()) ? kDim : kText;
        int w = tr_.width(labels[i], 16);
        tr_.draw(c, x + (bw - w) / 2, kButtonY + (kButtonH - 16) / 2, labels[i], 16, col);
    }
}

void LookupView::drawTop(Canvas &c) {
    if (hidden_) return;
    // a darker band behind the entry, so it reads over a bright speech bubble too
    int h = kTopMargin;
    for (size_t i = topScroll_; i < entry_.size() && h + entry_[i].height <= kTopH - 6; i++) h += entry_[i].height;
    h = std::min(h + 6, kTopH);
    c.shade(0, 0, kTopW, h, 185);
    if (h < kTopH) c.fill(0, h, kTopW, 1, kRule);
    int y = kTopMargin;
    for (size_t i = topScroll_; i < entry_.size(); i++) {
        const Line &l = entry_[i];
        if (y + l.height > kTopH - 6) break;
        int x = kTopMargin;
        for (const Span &s : l.spans) x = tr_.draw(c, x, y + (l.height - l.size) / 2, s.text, l.size, s.color);
        y += l.height;
    }
    // which match of how many (L/R for the others)
    if (so_.results.size() > 1) {
        char b[24];
        snprintf(b, sizeof(b), "%u/%u", (unsigned)resultIdx_ + 1, (unsigned)so_.results.size());
        std::string t = std::string("") + b + "";
        int w = tr_.width(t, kSmall);
        tr_.draw(c, kTopW - kTopMargin - w, kTopMargin + 4, t, kSmall, kDim);
    }
    if (topScroll_ > 0) tr_.draw(c, kTopW - 18, kTopH - 40, "▲", 12, kDim);
    if (topScroll_ + topLinesFit() < (int)entry_.size()) tr_.draw(c, kTopW - 18, kTopH - 20, "▼", 12, kDim);
}

}  // namespace kt
