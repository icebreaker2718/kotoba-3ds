// lookup_view.hpp - the lookup screens, after Rubi for Vita's overlay: the text that was read on
// the touchscreen, where a tap (or the D-pad) picks a word, and the word's dictionary entry on
// the top screen over the dimmed game. Platform independent: the plugin feeds it input and gives
// it the two screens to draw on; the tests and previews do the same on a PC.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"
#include "core/search.hpp"
#include "core/ui_text.hpp"

namespace kt {

class LookupView {
public:
    // Touchscreen 320x240, top screen 400x240.
    static constexpr int kBottomW = 320, kBottomH = 240, kTopW = 400, kTopH = 240;

    LookupView(Dictionary &dict, TextRenderer &text) : dict_(dict), tr_(text) {}

    // The text blocks that were read (the longest first is best). Selects the first word.
    void setText(const std::vector<std::string> &blocks);
    const std::vector<std::string> &blocks() const { return blocks_; }

    // ---- input (each returns true when the screens need redrawing) ----
    enum Button { kNone, kSave, kHide, kClose };
    // A touch at (x, y) on the touchscreen: a character (looks up the word starting there), or a
    // button (returned through `button`).
    bool tap(int x, int y, Button &button);
    bool moveWord(int dir);     // D-pad left/right: previous/next word
    bool moveRow(int dir);      // D-pad up/down: the row above/below, same position
    bool page(int dir);         // L/R: the other dictionary matches at this spot
    bool scroll(int lines);     // Circle Pad: the definition up/down
    void setHidden(bool hidden) { hidden_ = hidden; }
    bool hidden() const { return hidden_; }

    // ---- what is selected ----
    bool hasResult() const { return !so_.results.empty(); }
    const Result *result() const { return hasResult() ? &so_.results[resultIdx_] : nullptr; }
    const std::string &query() const { return so_.query; }
    const std::string &sentence() const;          // the block the selection is in
    std::string selectedText() const;             // the highlighted characters
    void markSaved();                             // shows the selection as saved (green)
    void setStatus(const std::string &s, Rgb c) { status_ = s; statusColor_ = c; }

    // ---- drawing ----
    void drawBottom(Canvas &c);
    // The entry panel; the caller has drawn the dimmed game (or black) underneath.
    void drawTop(Canvas &c);

    // Layout, for the tests: the character at (x, y) on the touchscreen, -1 if none.
    int charAt(int x, int y) const;
    struct Cell {
        int block = 0;
        size_t byte = 0, len = 0;
        uint32_t cp = 0;
        int row = 0, x = 0, w = 0;
    };
    struct Row {
        int block = 0, y = 0;  // content coordinates (before scrolling)
        int first = 0, end = 0;  // cells [first, end)
    };
    const std::vector<Cell> &cells() const { return cells_; }
    const std::vector<Row> &rows() const { return rows_; }
    int selectedCell() const;

private:
    Dictionary &dict_;
    TextRenderer &tr_;
    std::vector<std::string> blocks_;
    std::vector<Cell> cells_;
    std::vector<Row> rows_;
    std::vector<std::vector<size_t>> stops_;  // per block: byte offsets where words start
    int contentH_ = 0, scrollY_ = 0;
    // selection
    int selBlock_ = -1;
    size_t selByte_ = 0, selLen_ = 0;
    SearchOutput so_;
    size_t resultIdx_ = 0;
    // the entry on the top screen
    struct Span { std::string text; Rgb color; };
    struct Line { std::vector<Span> spans; int size = 16, height = 21; };
    std::vector<Line> entry_;
    int topScroll_ = 0;
    bool hidden_ = false;
    struct Saved { int block; size_t byte, len; };
    std::vector<Saved> saved_;
    std::string status_;
    Rgb statusColor_;

    void layout();
    static void trimResults(SearchOutput &so);
    bool selectAt(int block, size_t byte);
    void buildEntry();
    void keepVisible();
    int topLinesFit() const;
    bool isSaved(const Cell &c) const;
};

}  // namespace kt
