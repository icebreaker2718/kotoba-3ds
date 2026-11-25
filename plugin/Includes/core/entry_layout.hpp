// entry_layout.hpp - a dictionary entry laid out for the lookup screen: rows of styled spans (text, tag chips,
// furigana) at their positions. The PC styled the entry (display runs, see tools/build_dict.py); this only breaks
// it into rows of the panel's width. All its memory is taken once (reserve()): a lookup allocates nothing, so the
// heap stays as it is over a long session. Platform independent.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/dict.hpp"
#include "core/ui_text.hpp"

namespace kt {

class EntryLayout {
public:
    // Colours by role; the lookup view gives each its Rgb.
    enum Color : uint8_t {
        C_TEXT, C_DIM, C_WORD, C_READING, C_CONJ, C_LINK, C_EXAMPLE,
        C_CHIP_POS, C_CHIP_MISC, C_CHIP_FIELD, C_CHIP_DIALECT, C_CHIP_OTHER, C_COUNT
    };
    enum SpanKind : uint8_t { K_TEXT, K_CHIP, K_RUBY };  // a chip is drawn on a box; ruby: furigana (small, above)
    struct Span {
        uint32_t off = 0;    // its text in the pool
        uint16_t len = 0;
        int16_t x = 0, y = 0;  // y: the text cell's top, from the row's top
        uint8_t size = 0, color = 0, kind = K_TEXT;
        int16_t w = 0;       // chips: the box's width
    };
    struct Row {
        uint16_t first = 0, count = 0;  // spans [first, first + count)
        int16_t height = 0;
    };

    // Sizes (pixels: the text's line height, as TextRenderer takes it)
    static constexpr int kText = 17, kSmall = 14, kRuby = 10, kChip = 13, kIndent = 16;
    static constexpr int kTextRow = 22, kSmallRow = 19, kChipPad = 4, kChipGap = 4, kRubyGap = 1;
    // Memory, taken once: the text (headline, the entry's runs), the spans, the rows (~42 KB)
    static constexpr uint32_t kPoolBytes = 24 * 1024;
    static constexpr uint32_t kMaxSpans = 1024, kMaxRows = 384;

    explicit EntryLayout(TextRenderer &tr) : tr_(tr) {}
    ~EntryLayout();
    EntryLayout(const EntryLayout &) = delete;
    EntryLayout &operator=(const EntryLayout &) = delete;
    bool reserve();  // false: no memory (nothing is laid out then)

    void clear(int left, int width);  // rows start at x = left, at most width wide

    // A row of spans side by side, no wrapping (the headword line); spans are added with add().
    void beginRow(int height, int size);
    void add(const char *text, size_t len, int size, Color c);
    void endRow();
    // Text wrapped to the width, one row per line, in one size and colour.
    void addWrapped(const char *text, size_t len, int size, int rowHeight, Color c);
    // The entry: the dictionary writes its display runs into the pool (Dictionary::display), they're laid out.
    // False if the entry can't be read (nothing added) or didn't fit (truncated() then).
    bool addEntry(Dictionary &d, uint32_t entryId);
    // Display runs already in hand (tests).
    void addRuns(const char *runs, size_t len, const Dictionary &d);

    int rowCount() const { return (int)rowCount_; }
    const Row &row(int i) const { return rows_[i]; }
    const Span &span(int i) const { return spans_[i]; }
    const char *text(const Span &s) const { return pool_ + s.off; }
    bool truncated() const { return truncated_; }
    bool ready() const { return pool_ != nullptr; }
    uint32_t poolUsed() const { return poolUsed_; }

private:
    TextRenderer &tr_;
    char *pool_ = nullptr;
    Span *spans_ = nullptr;
    Row *rows_ = nullptr;
    uint32_t poolUsed_ = 0, spanCount_ = 0, rowCount_ = 0;
    int left_ = 0, width_ = 0;
    bool truncated_ = false;
    // the row being built
    bool open_ = false;
    int x_ = 0, rowStartX_ = 0, hangX_ = 0, rowFixedHeight_ = 0;
    uint32_t rowFirst_ = 0;
    uint32_t prev_ = 0;  // the character before the next piece (0: a paragraph's start)

    uint32_t copy(const char *text, size_t len);  // into the pool: its offset (or ~0u when full)
    Span *newSpan();
    void openRow(int x);
    void closeRow();
    void placeText(uint32_t off, size_t len, int size, uint8_t color);
    void layoutPool(uint32_t off, size_t len, const Dictionary &d);
};

// Display runs as text: chips as [label], furigana as base(reading), a paragraph a line (tests, logs).
void runsAsText(const char *runs, size_t len, const Dictionary &d, char *out, size_t cap);

}  // namespace kt
