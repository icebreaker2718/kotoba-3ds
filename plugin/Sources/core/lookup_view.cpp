// Built for speed (the plugin's C++ is otherwise built for size): the lookup screens' drawing.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
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
// about 22 px wide, 13 to a row (big enough to tap with a finger). Text whose game lines are
// longer is shrunk so they fit, down to kMinTextSize (about 18 to a row); rows are 7 px taller.
static const int kMargin = 12, kTextSize = 27, kMinTextSize = 20, kRowGap = 7, kTextTop = 8, kTextBottom = 182;
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

int LookupView::textW() const { return textOnTop_ ? kTopW : kBottomW; }
int LookupView::textBottom() const { return textOnTop_ ? kTopH - 26 : kTextBottom; }
int LookupView::statusY() const { return textOnTop_ ? kTopH - 22 : kStatusY; }
int LookupView::entryW() const { return textOnTop_ ? kBottomW : kTopW; }
int LookupView::entryEnd() const { return textOnTop_ ? kButtonY - 4 : kTopH - 6; }

void LookupView::setTextOnTop(bool onTop) {
    if (onTop == textOnTop_) return;
    textOnTop_ = onTop;
    layout();
    if (selBlock_ >= 0) {
        buildEntry();
        keepVisible();
    }
}

void LookupView::layout() {
    cells_.clear();
    rows_.clear();
    const int maxW = textW() - 2 * kMargin;
    // the biggest text size that fits each of the game's lines on one row (lines too long even
    // at the smallest size don't count: they carry on in the next row, indented, at any size)
    struct Span { size_t block, from, to; };
    std::vector<Span> lines;
    auto width = [&](const Span &l, int size) {
        int x = 0;
        for (size_t i = l.from; i < l.to;) x += tr_.advance(utf8Next(blocks_[l.block], i), size);
        return x;
    };
    for (size_t b = 0; b < blocks_.size(); b++) {
        size_t from = 0;
        for (size_t k = 0; k <= breaks_[b].size(); k++) {
            Span l = {b, from, k < breaks_[b].size() ? breaks_[b][k] : blocks_[b].size()};
            if (width(l, kMinTextSize) <= maxW) lines.push_back(l);
            from = l.to;
        }
    }
    textSize_ = kTextSize;
    for (bool fits = false; !fits && textSize_ > kMinTextSize;) {
        fits = true;
        for (const Span &l : lines)
            if (width(l, textSize_) > maxW) { fits = false; textSize_--; break; }
    }
    rowH_ = textSize_ + kRowGap;
    const int indent = textSize_ / 2;  // rows that carry on a game line too long for one
    int y = 0;
    for (size_t b = 0; b < blocks_.size(); b++) {
        if (b > 0) y += kBlockGap;
        const std::string &t = blocks_[b];
        Row row;
        row.block = (int)b;
        row.y = y;
        row.first = (int)cells_.size();
        int x = 0;
        size_t k = 0;  // the next game line break
        for (size_t i = 0; i < t.size();) {
            Cell c;
            c.block = (int)b;
            c.byte = i;
            c.cp = utf8Next(t, i);
            c.len = i - c.byte;
            c.w = tr_.advance(c.cp, textSize_);
            if (k < breaks_[b].size() && breaks_[b][k] == c.byte) {  // the game's next line: a new row
                k++;
                if (x > 0) {
                    row.end = (int)cells_.size();
                    rows_.push_back(row);
                    y += rowH_;
                    row.y = y;
                    row.first = (int)cells_.size();
                    x = 0;
                }
            }
            // a new row when this one is full (closing punctuation may hang into the margin)
            if (x > 0 && x + c.w > maxW && !(noRowStart(c.cp) && x + c.w <= maxW + kMargin)) {
                // punctuation that can't hang takes the character before it along to the new row
                if (noRowStart(c.cp) && (int)cells_.size() - row.first > 1) {
                    Cell prev = cells_.back();
                    cells_.pop_back();
                    row.end = (int)cells_.size();
                    rows_.push_back(row);
                    y += rowH_;
                    row.y = y;
                    row.first = (int)cells_.size();
                    prev.x = kMargin + indent;
                    prev.row = (int)rows_.size();
                    x = indent + prev.w;
                    cells_.push_back(prev);
                    c.x = kMargin + x;
                    c.row = (int)rows_.size();
                    x += c.w;
                    cells_.push_back(c);
                    continue;
                }
                row.end = (int)cells_.size();
                rows_.push_back(row);
                y += rowH_;
                row.y = y;
                row.first = (int)cells_.size();
                x = indent;  // a game line too long for a row goes on, indented
            }
            c.x = kMargin + x;
            c.row = (int)rows_.size();
            x += c.w;
            cells_.push_back(c);
        }
        row.end = (int)cells_.size();
        rows_.push_back(row);
        y += rowH_;
    }
    contentH_ = y;
}

void LookupView::setText(const std::vector<std::string> &blocks) {
    // the game's line breaks out of the text, remembered as where rows start
    blocks_.clear();
    breaks_.clear();
    for (const std::string &raw : blocks) {
        std::string t;
        std::vector<size_t> br;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] != '\n') { t += raw[i]; continue; }
            if (t.empty() || i + 1 >= raw.size() || raw[i + 1] == '\n') continue;
            // English words broken across lines get their space back
            unsigned char before = (unsigned char)t.back(), after = (unsigned char)raw[i + 1];
            if (before < 0x80 && after < 0x80 && before != ' ' && after != ' ') t += ' ';
            br.push_back(t.size());
        }
        blocks_.push_back(t);
        breaks_.push_back(br);
    }
    marks_.assign(blocks_.size(), {});
    redraw_ = false;
    saved_.clear();
    status_.clear();
    scrollY_ = 0;
    hidden_ = false;
    selBlock_ = -1;
    so_ = SearchOutput();
    entry_.clear();
    cache_.clear();
    layout();
    // where words start is worked out later (stops(), from prefetch() while the player reads):
    // splitting all the text takes ~100 dictionary reads, and only the first word is needed now
    stops_.assign(blocks_.size(), {});
    stopsReady_.assign(blocks_.size(), 0);
    // start on the first word that has a dictionary entry (looking a few characters in)
    int first = -1, tries = 0;
    for (size_t i = 0; i < cells_.size() && tries < 6; i++) {
        if (!isWordChar(cells_[i].cp)) continue;
        if (first < 0) first = (int)i;
        tries++;
        selectAt(cells_[i].block, cells_[i].byte);
        if (hasResult()) break;
    }
    if (!hasResult() && first >= 0) selectAt(cells_[first].block, cells_[first].byte);
}

// Where words start in block b (greedy longest dictionary match, as chunkText splits it).
const std::vector<size_t> &LookupView::stops(size_t b) const {
    if (!stopsReady_[b]) {
        stopsReady_[b] = 1;
        size_t at = 0;
        std::vector<size_t> lens;
        for (const std::string &w : chunkText(dict_, blocks_[b])) {
            size_t i = 0;
            if (isWordChar(utf8Next(w, i))) {
                stops_[b].push_back(at);
                lens.push_back(w.size());
            }
            at += w.size();
        }
        // known words are looked for at every character, not only where the split starts words:
        // 天気 in いい天気, the second ケーキ in ケーキとケーキ
        const std::string &t = blocks_[b];
        size_t k = 0;
        for (size_t i = 0; i < t.size();) {
            Mark m;
            m.byte = i;
            bool word = isWordChar(utf8Next(t, i));
            while (k < stops_[b].size() && stops_[b][k] < m.byte) k++;
            m.stop = k < stops_[b].size() && stops_[b][k] == m.byte;
            if (m.stop) m.word = lens[k];
            m.first = i - m.byte;
            if (word) marks_[b].push_back(m);
        }
    }
    return stops_[b];
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
    so_ = lookup(block, byte);
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
    const int maxW = entryW() - 2 * kTopMargin - 34;  // room for the 1/3 counter
    // headword (reading)
    std::string expr = expressionFor(e, r.matchedKey);
    Line head;
    head.size = kHeadSize;
    head.height = kHeadSize + 8;
    head.spans.push_back({expr, kWord});
    const bool showReading = !e.reading.empty() && e.reading != expr;
    if (showReading) head.spans.push_back({" (" + e.reading + ")", kReading});
    // how common it is (JPDB rank, as Rubi shows it): "#1,234"
    std::string rank;
    if (e.freqRank) {
        std::string n = std::to_string(e.freqRank);
        for (int i = (int)n.size() - 3; i > 0; i -= 3) n.insert((size_t)i, ",");
        rank = " #" + n;
    }
    int headW = tr_.width(expr + (showReading ? " (" + e.reading + ")" : ""), kHeadSize) + tr_.width(rank, kSmall + 2);
    if (headW > maxW) {  // too long: reading (and rank) on a line of their own
        head.spans.resize(1);
        entry_.push_back(head);
        if (showReading || !rank.empty()) {
            Line rd;
            rd.size = kSenseSize + 2;
            rd.height = kSenseLine + 2;
            if (showReading) rd.spans.push_back({e.reading, kReading});
            if (!rank.empty()) rd.spans.push_back({showReading ? rank : rank.substr(1), kDim, kSmall + 2});
            entry_.push_back(rd);
        }
    } else {
        if (!rank.empty()) head.spans.push_back({rank, kDim, kSmall + 2});
        entry_.push_back(head);
    }
    auto small = [&](const std::string &text, Rgb c) {
        for (const std::string &w : tr_.wrap(text, kSmall, entryW() - 2 * kTopMargin)) {
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
        std::vector<std::string> lines = tr_.wrap(sense, kSenseSize, entryW() - 2 * kTopMargin);
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
    const int areaH = textBottom() - kTextTop;
    int top = rows_[cells_[c].row].y;
    // the whole selection (it may run onto the next row)
    int lastRow = cells_[c].row;
    for (size_t i = c; i < cells_.size() && cells_[i].block == selBlock_ && cells_[i].byte < selByte_ + selLen_; i++)
        lastRow = cells_[i].row;
    int bottom = rows_[lastRow].y + rowH_;
    if (bottom - scrollY_ > areaH) scrollY_ = bottom - areaH;
    if (top < scrollY_) scrollY_ = top;
    if (scrollY_ < 0) scrollY_ = 0;
}

int LookupView::charAt(int x, int y) const {
    if (textOnTop_ || y < kTextTop || y >= textBottom()) return -1;
    int cy = y - kTextTop + scrollY_;
    for (const Row &r : rows_) {
        if (cy < r.y || cy >= r.y + rowH_) continue;
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

// The dictionary matches at a spot, remembered: moving back and forth costs no disk reads, and
// prefetch() looks the neighbours up while the player reads.
SearchOutput LookupView::lookup(int block, size_t byte) {
    for (const Cached &c : cache_)
        if (c.block == block && c.byte == byte) return c.so;
    Cached c;
    c.block = block;
    c.byte = byte;
    c.so = scanAt(dict_, blocks_[block], byte);
    trimResults(c.so);
    if (!c.so.results.empty()) {  // read its entry too, so its disk blocks are in the cache
        Entry e;
        dict_.entry(c.so.results[0].entryId, e);
    }
    if (cache_.size() >= 64) cache_.erase(cache_.begin());
    cache_.push_back(c);
    return c.so;
}

bool LookupView::isCached(int block, size_t byte) const {
    for (const Cached &c : cache_)
        if (c.block == block && c.byte == byte) return true;
    return false;
}

bool LookupView::wordTarget(int dir, int &block, size_t &byte) const {
    if (selBlock_ < 0) return false;
    if (dir > 0) {
        size_t from = selByte_ + std::max<size_t>(selLen_, 1);
        for (size_t b = selBlock_; b < blocks_.size(); b++)
            for (size_t s : stops(b))
                if (b > (size_t)selBlock_ || s >= from) { block = (int)b; byte = s; return true; }
    } else {
        for (int b = selBlock_; b >= 0; b--)
            for (size_t k = stops(b).size(); k-- > 0;)
                if (b < selBlock_ || stops_[b][k] < selByte_) { block = b; byte = stops_[b][k]; return true; }
    }
    return false;
}

bool LookupView::rowTarget(int dir, int &block, size_t &byte) const {
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
    block = cells_[best].block;
    byte = cells_[best].byte;
    return true;
}

bool LookupView::moveWord(int dir) {
    int b;
    size_t at;
    if (!wordTarget(dir, b, at)) return false;
    status_.clear();
    return selectAt(b, at);
}

bool LookupView::moveRow(int dir) {
    int b;
    size_t at;
    if (!rowTarget(dir, b, at)) return false;
    status_.clear();
    return selectAt(b, at);
}

bool LookupView::prefetch() {
    if (selBlock_ < 0) return false;
    if (!stopsReady_[selBlock_]) { stops(selBlock_); return true; }  // split this text into words
    int b;
    size_t at;
    // where the next press goes: the next and previous words, the rows below and above
    for (int step = 0; step < 4; step++) {
        bool ok = step == 0 ? wordTarget(1, b, at) : step == 1 ? wordTarget(-1, b, at) : rowTarget(step == 2 ? 1 : -1, b, at);
        if (ok && !isCached(b, at)) { lookup(b, at); return true; }
    }
    // then a few words further on
    int ahead = 0;
    for (size_t bl = selBlock_; bl < blocks_.size() && ahead <= 6; bl++)
        for (size_t st : stops(bl)) {
            if (bl == (size_t)selBlock_ && st <= selByte_) continue;
            if (++ahead > 6) break;
            if (!isCached((int)bl, st)) { lookup((int)bl, st); return true; }
        }
    // then which words the player has already
    return checkKnown();
}

static bool hasKanji(const std::string &s) {
    for (size_t i = 0; i < s.size();)
        if (isKanjiCp(utf8Next(s, i))) return true;
    return false;
}

static bool hasKatakana(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8Next(s, i);
        if (cp >= 0x30A1 && cp <= 0x30FA) return true;
    }
    return false;
}

static std::string toKatakana(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8Next(s, i);
        utf8Append(out, cp >= 0x3041 && cp <= 0x3096 ? cp + 0x60 : cp);
    }
    return out;
}

// The spellings the word at a spot could have on a card, for the match a lookup there shows
// first (the others are mostly other words that happen to share the kana); len = its length.
// Written in kanji: the kanji spellings. Written in kana: the kana (a card for 帰る doesn't make
// every かえる green), and, for hiragana of two characters or more, the word's kanji spelling.
void LookupView::formsOf(const SearchOutput &so, std::vector<std::string> &forms, size_t &len, bool *kanjiSurface,
                         size_t word) {
    forms.clear();
    len = 0;
    const Result *r = nullptr;
    for (const Result &x : so.results)
        if (!x.prefixOnly && (word ? x.surface.size() == word && !r : !r || x.surface.size() > r->surface.size())) r = &x;
    if (kanjiSurface) *kanjiSurface = false;
    if (!r) return;
    len = r->surface.size();
    auto put = [&](const std::string &f) {
        if (!f.empty() && std::find(forms.begin(), forms.end(), f) == forms.end()) forms.push_back(f);
    };
    Entry e;
    const bool haveEntry = dict_.entry(r->entryId, e);
    if (hasKanji(r->surface)) {
        if (kanjiSurface) *kanjiSurface = true;
        if (haveEntry) {
            put(cardExpressionFor(e, r->matchedKey));
            put(expressionFor(e, r->matchedKey));
        }
        return;
    }
    put(r->matchedKey);  // the dictionary form in hiragana
    put(toKatakana(r->matchedKey));
    if (haveEntry && !hasKanji(e.expression)) put(e.expression);
    size_t i = 0, chars = 0;
    while (i < r->surface.size()) { utf8Next(r->surface, i); chars++; }
    if (haveEntry && chars >= 2 && !hasKatakana(r->surface)) put(cardExpressionFor(e, r->matchedKey));
}

// Checks one more word stop against the known words. False when all are checked.
bool LookupView::checkKnown() {
    if (!known_) return false;
    for (size_t b = 0; b < blocks_.size(); b++) {
        if (!stopsReady_[b]) { stops(b); return true; }
        for (Mark &m : marks_[b]) {
            if (m.state) continue;
            SearchOutput so;
            bool cached = false;
            for (const Cached &c : cache_)
                if (c.block == (int)b && c.byte == m.byte) { so = c.so; cached = true; break; }
            if (!cached) {  // looked up without taking a place in the cache the moves use
                so = scanAt(dict_, blocks_[b], m.byte);
                trimResults(so);
            }
            bool kanji = false;
            formsOf(so, m.forms, m.len, &kanji, m.stop ? m.word : 0);  // at a word: the word as split
            m.state = 1;
            // inside a word the split found, only words in kanji (天気 in いい天気, not した in 倒した)
            m.counts = m.stop || (kanji && m.len > m.first);
            if (m.counts)
                for (const std::string &f : m.forms)
                    if (known_->has(f)) { m.state = 2; redraw_ = true; break; }
            return true;
        }
    }
    return false;
}

std::vector<std::string> LookupView::selectedForms() {
    std::vector<std::string> forms;
    size_t len;
    if (selBlock_ >= 0) formsOf(so_, forms, len);
    return forms;
}

void LookupView::refreshKnown() {
    if (!known_) return;
    for (std::vector<Mark> &ms : marks_)
        for (Mark &m : ms) {
            if (!m.state) continue;
            m.state = 1;
            if (m.counts)
                for (const std::string &f : m.forms)
                    if (known_->has(f)) { m.state = 2; break; }
        }
}

bool LookupView::isKnown(const Cell &c) const {
    if ((size_t)c.block >= marks_.size()) return false;
    for (const Mark &m : marks_[c.block])
        if (m.state == 2 && c.byte >= m.byte && c.byte < m.byte + m.len) return true;
    return false;
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
        if (y + entry_[i].height > entryEnd()) break;
        y += entry_[i].height;
    }
    return n;
}

bool LookupView::scroll(int lines) {
    int before = topScroll_;
    // the furthest down that still fills the screen
    int maxScroll = (int)entry_.size();
    for (int h = 0; maxScroll > 0 && h + entry_[maxScroll - 1].height <= entryEnd() - kTopMargin;)
        h += entry_[--maxScroll].height;
    topScroll_ = std::max(0, std::min(topScroll_ + lines, maxScroll));
    return topScroll_ != before;
}

void LookupView::markSaved() {
    if (selBlock_ < 0) return;
    saved_.push_back({selBlock_, selByte_, selLen_});
    if (!known_) return;
    // the word is known now: wherever else it is in the text, it turns green too
    std::vector<std::string> forms;
    size_t len;
    formsOf(so_, forms, len);
    if (forms.empty()) return;
    for (const std::string &f : forms) known_->add(f);
    for (std::vector<Mark> &ms : marks_)
        for (Mark &m : ms)
            if (m.state == 1 && m.counts)
                for (const std::string &f : m.forms)
                    if (std::find(forms.begin(), forms.end(), f) != forms.end()) { m.state = 2; break; }
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

void LookupView::drawTop(Canvas &c, Panel p, bool bandShaded) {
    if (textOnTop_) drawTextPanel(c, p);
    else drawEntryPanel(c, p, bandShaded);
}

void LookupView::drawBottom(Canvas &c, Panel p, bool bandShaded) {
    if (p == Panel::Hidden) return;
    if (textOnTop_) drawEntryPanel(c, p, bandShaded);
    else drawTextPanel(c, p);
    drawButtons(c);
}

// The text that was read, a row at a time (whole rows only), the selection highlighted.
void LookupView::drawTextPanel(Canvas &c, Panel p) {
    if (p == Panel::Hidden) return;
    const int W = textW(), bottomY = textBottom(), areaH = bottomY - kTextTop;
    if (p == Panel::Shown) c.fill(0, 0, W, textOnTop_ ? kTopH : kBottomH, kBg);
    // block separators
    for (size_t r = 1; r < rows_.size(); r++)
        if (rows_[r].block != rows_[r - 1].block) {
            int y = kTextTop + rows_[r].y - scrollY_ - kBlockGap / 2 - 1;
            if (y >= kTextTop && y < bottomY) c.fill(kMargin, y, W - 2 * kMargin, 1, kRule);
        }
    for (const Row &r : rows_) {
        int y = r.y - scrollY_;
        if (y < 0 || y + rowH_ > areaH) continue;  // only whole rows
        y += kTextTop;
        for (int i = r.first; i < r.end; i++) {
            const Cell &cl = cells_[i];
            bool sel = cl.block == selBlock_ && cl.byte >= selByte_ && cl.byte < selByte_ + selLen_;
            if (sel) c.fill(cl.x - 1, y + 1, cl.w + 2, rowH_ - 2, kWordBg);
        }
        for (int i = r.first; i < r.end; i++) {
            const Cell &cl = cells_[i];
            bool sel = cl.block == selBlock_ && cl.byte >= selByte_ && cl.byte < selByte_ + selLen_;
            Rgb col = sel ? kWord : (isSaved(cl) || isKnown(cl) ? kSavedC : kText);
            tr_.drawChar(c, cl.x, y + (rowH_ - textSize_) / 2, cl.cp, textSize_, col);
        }
    }
    // more text above / below
    if (scrollY_ > 0) tr_.draw(c, W - 14, kTextTop - 4, "▲", 12, kDim);
    if (contentH_ - scrollY_ > areaH) tr_.draw(c, W - 14, bottomY - 10, "▼", 12, kDim);
    // status, or the controls
    if (!status_.empty()) {
        std::vector<std::string> s = tr_.wrap(status_, kStatusSize, W - 2 * kMargin);
        if (!s.empty()) tr_.draw(c, kMargin, statusY(), s[0], kStatusSize, statusColor_);
    } else {
        std::string hint = "\uE049 words  \uE079\uE07A scroll  \uE07B\uE07C\uE052\uE053 meanings  \uE003 known";
        int w = tr_.width(hint, 13);
        tr_.draw(c, (W - w) / 2, statusY() + 1, hint, 13, kDim);
    }
}

void LookupView::drawButtons(Canvas &c) {
    const int bw = (kBottomW - 16) / 3;
    const char *labels[3] = {"\uE000 Save", hidden_ ? "\uE002 Show" : "\uE002 Hide", "\uE001 Close"};
    for (int i = 0; i < 3; i++) {
        int x = 8 + i * bw;
        frame(c, x + 2, kButtonY, bw - 4, kButtonH, kButton, kButtonEdge);
        Rgb col = (i == 0 && !hasResult()) ? kDim : kText;
        int w = tr_.width(labels[i], 16);
        tr_.draw(c, x + (bw - w) / 2, kButtonY + (kButtonH - 16) / 2, labels[i], 16, col);
    }
}

int LookupView::entryBand() const {
    int h = kTopMargin;
    for (size_t i = topScroll_; i < entry_.size() && h + entry_[i].height <= entryEnd(); i++) h += entry_[i].height;
    return std::min(h + 6, textOnTop_ ? kButtonY - 4 : kTopH);
}

void LookupView::drawEntryPanel(Canvas &c, Panel p, bool bandShaded) {
    if (p == Panel::Hidden) return;
    const int W = entryW(), end = entryEnd();
    if (p == Panel::Shown) {
        // a darker band behind the entry, so it reads over a bright speech bubble too
        const int h = entryBand();
        if (!bandShaded) c.shade(0, 0, W, h, kBandShade);
        if (h < (textOnTop_ ? kBottomH : kTopH)) c.fill(0, h, W, 1, kRule);
    }
    int y = kTopMargin;
    for (size_t i = topScroll_; i < entry_.size(); i++) {
        const Line &l = entry_[i];
        if (y + l.height > end) break;
        int x = kTopMargin;
        for (const Span &s : l.spans) {
            int size = s.size ? s.size : l.size;  // a smaller span sits on the line's baseline
            x = tr_.draw(c, x, y + (l.height - l.size) / 2 + (l.size - size) * 4 / 5, s.text, size, s.color);
        }
        y += l.height;
    }
    // which match of how many (L/R for the others)
    if (so_.results.size() > 1) {
        char b[24];
        snprintf(b, sizeof(b), "%u/%u", (unsigned)resultIdx_ + 1, (unsigned)so_.results.size());
        std::string t = std::string("\uE052") + b + "\uE053";
        int w = tr_.width(t, kSmall);
        tr_.draw(c, W - kTopMargin - w, kTopMargin + 4, t, kSmall, kDim);
    }
    if (topScroll_ > 0) tr_.draw(c, W - 18, end - 34, "▲", 12, kDim);
    if (topScroll_ + topLinesFit() < (int)entry_.size()) tr_.draw(c, W - 18, end - 14, "▼", 12, kDim);
}

}  // namespace kt
