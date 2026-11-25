// Built for speed (the plugin's C++ is otherwise built for size): laid out on every lookup.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "core/entry_layout.hpp"

#include "core/kana.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace kt {

static uint32_t next(const char *s, size_t n, size_t &i) { return utf8Next(s, n, i); }

// characters that must not start a row (kinsoku), as the text panel's
static bool noRowStart(uint32_t cp) {
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case ',': case '.': case '!': case '?': case ';': case ':':
    case 0xFF01: case 0xFF1F: case 0x300D: case 0x300F: case 0xFF09: case ')': case 0x2026: case 0x30FB:
    case 0x30FC: case 0x3063: case 0x30C3: case 0x3083: case 0x3085: case 0x3087: case 0xFF5E: case 0x301C:
    case 0x3011: case 0xFF3D: case ']':
        return true;
    default:
        return false;
    }
}
static bool noRowEnd(uint32_t cp) {  // opening brackets stay with what follows
    return cp == '(' || cp == '[' || cp == 0x300C || cp == 0x300E || cp == 0xFF08 || cp == 0x3010;
}
static bool wide(uint32_t cp) { return cp >= 0x2E80; }

EntryLayout::~EntryLayout() {
    free(pool_);
    free(spans_);
    free(rows_);
}

bool EntryLayout::reserve() {
    if (pool_) return true;
    pool_ = (char *)malloc(kPoolBytes);
    spans_ = (Span *)malloc(sizeof(Span) * kMaxSpans);
    rows_ = (Row *)malloc(sizeof(Row) * kMaxRows);
    if (!pool_ || !spans_ || !rows_) {
        free(pool_);
        free(spans_);
        free(rows_);
        pool_ = nullptr;
        spans_ = nullptr;
        rows_ = nullptr;
        return false;
    }
    return true;
}

void EntryLayout::clear(int left, int width) {
    poolUsed_ = spanCount_ = rowCount_ = 0;
    left_ = left;
    width_ = width;
    truncated_ = open_ = false;
}

uint32_t EntryLayout::copy(const char *text, size_t len) {
    if (!pool_ || poolUsed_ + len > kPoolBytes) {
        truncated_ = true;
        return ~0u;
    }
    memcpy(pool_ + poolUsed_, text, len);
    poolUsed_ += (uint32_t)len;
    return poolUsed_ - (uint32_t)len;
}

EntryLayout::Span *EntryLayout::newSpan() {
    if (!spans_ || spanCount_ >= kMaxSpans) {
        truncated_ = true;
        return nullptr;
    }
    spans_[spanCount_] = Span();
    return &spans_[spanCount_++];
}

void EntryLayout::openRow(int x) {
    if (open_) closeRow();
    open_ = true;
    x_ = rowStartX_ = x;
    rowFirst_ = spanCount_;
    rowFixedHeight_ = 0;
    brkValid_ = false;
}

// Where the row could last have broken: at span `span`, `cut` bytes into it (0: at its start), at x_ now.
void EntryLayout::markBreak(uint32_t span, uint32_t cut) {
    brkSpan_ = span;
    brkCut_ = cut;
    brkX_ = x_;
    brkValid_ = x_ > rowStartX_;
}

// Something that can't start a row doesn't fit at its end: the row ends where it last could, and what came after
// that point (the word the punctuation belongs to) starts the next row. Nothing to do when there was no such point
// (a word longer than the row): then it runs over, as before.
void EntryLayout::breakAtLast() {
    if (!brkValid_ || brkSpan_ < rowFirst_ || brkSpan_ > spanCount_) return;
    uint32_t m = brkSpan_;
    if (brkCut_ > 0) {  // the break is inside a text span: split it
        if (m >= spanCount_ || brkCut_ >= spans_[m].len) return;
        if (!newSpan()) return;
        memmove(&spans_[m + 2], &spans_[m + 1], sizeof(Span) * (spanCount_ - 1 - (m + 1)));
        Span b = spans_[m];
        b.off += brkCut_;
        b.len = (uint16_t)(b.len - brkCut_);
        b.x = (int16_t)brkX_;
        spans_[m].len = (uint16_t)brkCut_;
        spans_[m + 1] = b;
        m++;
    }
    const uint32_t end = spanCount_;
    const int dx = hangX_ - brkX_;
    spanCount_ = m;
    closeRow();
    if (truncated_) return;
    spanCount_ = end;
    open_ = true;
    rowFirst_ = m;
    rowStartX_ = hangX_;
    rowFixedHeight_ = 0;
    brkValid_ = false;
    for (uint32_t k = m; k < end; k++) spans_[k].x = (int16_t)(spans_[k].x + dx);
    x_ += dx;
}

// The row's height and each span's height in it: text on a common baseline (a smaller span sits on the line's
// baseline), chips centred, furigana above their base (the row then has room for them).
void EntryLayout::closeRow() {
    if (!open_) return;
    open_ = false;
    if (spanCount_ == rowFirst_ && !rowFixedHeight_) return;  // nothing in it
    if (!rows_ || rowCount_ >= kMaxRows) {
        truncated_ = true;
        spanCount_ = rowFirst_;
        return;
    }
    int maxSize = 0;
    bool ruby = false;
    for (uint32_t i = rowFirst_; i < spanCount_; i++) {
        const Span &s = spans_[i];
        if (s.kind == K_RUBY) ruby = true;
        else maxSize = std::max<int>(maxSize, s.kind == K_CHIP ? kChip + 3 : s.size);
    }
    if (!maxSize) maxSize = kText;
    const int content = rowFixedHeight_ ? rowFixedHeight_ : maxSize + (maxSize >= kText ? 5 : 5);
    const int top = ruby ? kRuby + kRubyGap : 0;
    for (uint32_t i = rowFirst_; i < spanCount_; i++) {
        Span &s = spans_[i];
        if (s.kind == K_CHIP) s.y = (int16_t)(top + (content - (kChip + 4)) / 2);
        else if (s.kind == K_TEXT) s.y = (int16_t)(top + (content - maxSize) / 2 + (maxSize - s.size) * 4 / 5);
    }
    for (uint32_t i = rowFirst_; i < spanCount_; i++)  // furigana: over the span before them (their base)
        if (spans_[i].kind == K_RUBY) spans_[i].y = (int16_t)(i > rowFirst_ ? spans_[i - 1].y - kRuby - kRubyGap : 0);
    Row &r = rows_[rowCount_++];
    r.first = (uint16_t)rowFirst_;
    r.count = (uint16_t)(spanCount_ - rowFirst_);
    r.height = (int16_t)(top + content);
}

void EntryLayout::beginRow(int height, int size) {
    (void)size;
    openRow(left_);
    rowFixedHeight_ = height;
}

void EntryLayout::add(const char *text, size_t len, int size, Color c) {
    if (!open_) openRow(left_);
    const uint32_t off = copy(text, len);
    if (off == ~0u) return;
    Span *s = newSpan();
    if (!s) return;
    s->off = off;
    s->len = (uint16_t)len;
    s->x = (int16_t)x_;
    s->size = (uint8_t)size;
    s->color = c;
    for (size_t i = 0; i < len;) x_ += tr_.advance(next(text, len, i), size);
}

void EntryLayout::endRow() { closeRow(); }

void EntryLayout::addWrapped(const char *text, size_t len, int size, int rowHeight, Color c) {
    const uint32_t off = copy(text, len);
    if (off == ~0u) return;
    openRow(left_);
    hangX_ = left_;
    prev_ = 0;
    placeText(off, len, size, c);
    // each row of it the same height
    if (open_) closeRow();
    (void)rowHeight;
}

// Text from the pool, broken into pieces that can start a row (a word of English, a Japanese character, never
// before closing punctuation), placed one after another; pieces of one style next to each other stay one span.
void EntryLayout::placeText(uint32_t off, size_t len, int size, uint8_t color) {
    const char *t = pool_ + off;
    size_t i = 0;
    while (i < len) {
        // one piece
        const size_t start = i;
        size_t j = i;
        uint32_t cp = next(t, len, j);
        const uint32_t first = cp;
        int w = tr_.advance(cp, size);
        const bool space = cp == ' ';
        // a row may start after a space, or between Japanese characters (not before closing punctuation, not
        // after an opening bracket)
        const bool canBreak = !space && prev_ != 0 &&
                              (prev_ == ' ' || ((wide(cp) || wide(prev_)) && !noRowStart(cp) && !noRowEnd(prev_)));
        if (space) {
            while (j < len && t[j] == ' ') { j++; w += tr_.advance(' ', size); }
        } else {
            // the rest of an English word; closing punctuation after the piece; after an opening bracket, what
            // follows it
            bool word = !wide(cp);
            while (j < len) {
                size_t k = j;
                uint32_t c2 = next(t, len, k);
                const bool take = c2 != ' ' && ((word && !wide(c2)) || noRowStart(c2) || noRowEnd(cp));
                if (!take) break;
                if (noRowEnd(cp)) word = !wide(c2);
                w += tr_.advance(c2, size);
                cp = c2;
                j = k;
            }
        }
        (void)first;
        prev_ = space ? ' ' : cp;
        i = j;
        if (!open_) openRow(hangX_);
        if (x_ + w > left_ + width_ && x_ > rowStartX_) {
            if (canBreak) {
                closeRow();
                openRow(hangX_);
            } else if (!space) {
                breakAtLast();  // closing punctuation after a word that is already placed: the word goes down too
            }
        }
        if (space && x_ == rowStartX_ && rowCount_ > 0 && spanCount_ == rowFirst_) continue;  // no space opening a row
        Span *last = spanCount_ > rowFirst_ ? &spans_[spanCount_ - 1] : nullptr;
        const bool merge = last && last->kind == K_TEXT && last->color == color && last->size == size &&
                           last->off + last->len == off + start;
        if (canBreak) markBreak(merge ? spanCount_ - 1 : spanCount_, merge ? last->len : 0);
        if (merge) {
            last->len = (uint16_t)(last->len + (j - start));
        } else {
            Span *s = newSpan();
            if (!s) return;
            s->off = off + (uint32_t)start;
            s->len = (uint16_t)(j - start);
            s->x = (int16_t)x_;
            s->size = (uint8_t)size;
            s->color = color;
        }
        x_ += w;
    }
}

static EntryLayout::Color styleColor(uint8_t style) {
    switch (style & ~S_SMALL) {
    case S_DIM: return EntryLayout::C_DIM;
    case S_KEYWORD: return EntryLayout::C_WORD;
    case S_LINK: return EntryLayout::C_LINK;
    case S_EXAMPLE: return EntryLayout::C_EXAMPLE;
    default: return EntryLayout::C_TEXT;
    }
}

static EntryLayout::Color chipColor(uint8_t category) {
    switch (category) {
    case TAG_POS: return EntryLayout::C_CHIP_POS;
    case TAG_MISC: return EntryLayout::C_CHIP_MISC;
    case TAG_FIELD: return EntryLayout::C_CHIP_FIELD;
    case TAG_DIALECT: return EntryLayout::C_CHIP_DIALECT;
    default: return EntryLayout::C_CHIP_OTHER;
    }
}

bool EntryLayout::addEntry(Dictionary &d, uint32_t entryId) {
    if (!pool_) return false;
    uint32_t len = 0;
    const uint32_t off = poolUsed_;
    if (!d.display(entryId, pool_ + off, kPoolBytes - off, len)) {
        truncated_ = true;
        return false;
    }
    poolUsed_ += len;
    layoutPool(off, len, d);
    return !truncated_;
}

void EntryLayout::addRuns(const char *runs, size_t len, const Dictionary &d) {
    const uint32_t off = copy(runs, len);
    if (off != ~0u) layoutPool(off, len, d);
}

// Lays out runs that are in the pool (the spans point into them).
void EntryLayout::layoutPool(uint32_t off, size_t len, const Dictionary &d) {
    const char *r = pool_ + off;
    size_t i = 0;
    int size = kText;
    uint8_t color = C_TEXT;
    bool first = true;
    while (i < len) {
        const uint8_t c = (uint8_t)r[i];
        const size_t end = runEnd(r, i, len);
        if (c == RUN_PARA && i + 2 < len) {
            const uint8_t kind = (uint8_t)r[i + 1];
            const int indent = (uint8_t)r[i + 2] - 1;
            closeRow();
            const int x = left_ + std::min(indent, 4) * kIndent;
            hangX_ = x;
            prev_ = 0;
            openRow(x);
            // a little room above a group's tags and each sense (not at the top)
            if (!first && (kind == P_GROUP || kind == P_SENSE) && rowCount_ > 0) rows_[rowCount_ - 1].height += 3;
            first = false;
            size = kText;
            color = C_TEXT;
        } else if (c == RUN_STYLE && i + 1 < len) {
            const uint8_t st = (uint8_t)r[i + 1];
            size = st & S_SMALL ? kSmall : kText;
            color = styleColor(st);
        } else if (c == RUN_CHIP && i + 2 < len) {
            const Tag *t = d.tag(((uint8_t)r[i + 1] - 1u) * 255u + ((uint8_t)r[i + 2] - 1u));
            if (t) {
                const size_t n = strlen(t->label);
                int w = 2 * kChipPad;
                for (size_t k = 0; k < n;) w += tr_.advance(next(t->label, n, k), kChip);
                if (!open_) openRow(hangX_);
                if (x_ + w > left_ + width_ && x_ > rowStartX_) {
                    closeRow();
                    openRow(hangX_);
                }
                markBreak(spanCount_, 0);
                const uint32_t lo = copy(t->label, n);
                Span *s = lo == ~0u ? nullptr : newSpan();
                if (s) {
                    s->off = lo;
                    s->len = (uint16_t)n;
                    s->x = (int16_t)x_;
                    s->size = kChip;
                    s->color = chipColor(t->category);
                    s->kind = K_CHIP;
                    s->w = (int16_t)w;
                }
                x_ += w + kChipGap;
                prev_ = ' ';
            }
        } else if (c == RUN_HANG) {
            if (!open_) openRow(hangX_);
            hangX_ = x_;
        } else if (c == RUN_RUBY) {
            const char *mid = (const char *)memchr(r + i, RUN_RUBY_MID, end - i);
            if (mid) {
                const uint32_t bOff = off + (uint32_t)(i + 1), rOff = off + (uint32_t)(mid + 1 - r);
                const size_t bLen = mid - (r + i + 1), rLen = (r + end - 1) - (mid + 1);
                int bw = 0, rw = 0;
                for (size_t k = 0; k < bLen;) bw += tr_.advance(next(r + i + 1, bLen, k), size);
                for (size_t k = 0; k < rLen;) rw += tr_.advance(next(mid + 1, rLen, k), kRuby);
                const int w = std::max(bw, rw);
                if (!open_) openRow(hangX_);
                const bool canBreak = prev_ == 0 || !noRowEnd(prev_);  // not right after an opening bracket
                if (x_ + w > left_ + width_ && x_ > rowStartX_) {
                    if (canBreak) {
                        closeRow();
                        openRow(hangX_);
                    } else {
                        breakAtLast();
                    }
                }
                if (canBreak) markBreak(spanCount_, 0);
                Span *b = newSpan();
                Span *rd = b ? newSpan() : nullptr;
                if (b && rd) {
                    b->off = bOff;
                    b->len = (uint16_t)bLen;
                    b->x = (int16_t)(x_ + (w - bw) / 2);
                    b->size = (uint8_t)size;
                    b->color = color;
                    rd->off = rOff;
                    rd->len = (uint16_t)rLen;
                    rd->x = (int16_t)(x_ + (w - rw) / 2);
                    rd->size = kRuby;
                    rd->color = color;
                    rd->kind = K_RUBY;
                }
                x_ += w;
                for (size_t k = 0; k < bLen;) prev_ = next(r + i + 1, bLen, k);
            }
        } else if (c >= 0x20) {
            placeText(off + (uint32_t)i, end - i, size, color);
        }
        if (truncated_) break;
        i = end;
    }
    closeRow();
}

void runsAsText(const char *runs, size_t n, const Dictionary &d, char *out, size_t cap) {
    size_t o = 0;
    auto put = [&](const char *s, size_t len) {
        if (o + len + 1 > cap) len = o + 1 < cap ? cap - o - 1 : 0;
        memcpy(out + o, s, len);
        o += len;
    };
    for (size_t i = 0; i < n;) {
        const uint8_t c = (uint8_t)runs[i];
        const size_t end = runEnd(runs, i, n);
        if (c == RUN_PARA && i + 2 < n) {
            if (o) put("\n", 1);
            for (int k = 1; k < (uint8_t)runs[i + 2]; k++) put("  ", 2);
        } else if (c == RUN_CHIP && i + 2 < n) {
            const Tag *t = d.tag(((uint8_t)runs[i + 1] - 1u) * 255u + ((uint8_t)runs[i + 2] - 1u));
            put("[", 1);
            if (t) put(t->label, strlen(t->label));
            put("]", 1);
        } else if (c == RUN_RUBY) {
            const char *mid = (const char *)memchr(runs + i, RUN_RUBY_MID, end - i);
            if (mid) {
                put(runs + i + 1, mid - (runs + i + 1));
                put("(", 1);
                put(mid + 1, runs + end - 1 - (mid + 1));
                put(")", 1);
            }
        } else if (c >= 0x20) {
            put(runs + i, end - i);
        }
        i = end;
    }
    if (cap) out[o < cap ? o : cap - 1] = '\0';
}

}  // namespace kt
