// lookup_view.hpp - the lookup screens, after Rubi for Vita's overlay: the text that was read on
// the touchscreen, where a tap (or the D-pad) picks a word, and the word's dictionary entry on
// the top screen over the dimmed game. Platform independent: the plugin feeds it input and gives
// it the two screens to draw on; the tests and previews do the same on a PC.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"
#include "core/known_words.hpp"
#include "core/search.hpp"
#include "core/ui_text.hpp"

namespace kt {

class LookupView {
public:
    // Touchscreen 320x240, top screen 400x240.
    static constexpr int kBottomW = 320, kBottomH = 240, kTopW = 400, kTopH = 240;

    LookupView(Dictionary &dict, TextRenderer &text) : dict_(dict), tr_(text) {}

    // The text blocks that were read. A '\n' in a block is where the game broke the line: the text
    // starts a new row there too (and is shrunk, down to a point, so the game's lines fit), but
    // words and the sentence run on across it. Selects the first word.
    void setText(const std::vector<std::string> &blocks);
    const std::vector<std::string> &blocks() const { return blocks_; }

    // ---- input (each returns true when the screens need redrawing) ----
    enum Button { kNone, kSave, kHide, kClose };
    // A touch at (x, y) on the touchscreen: a character (looks up the word starting there), or a
    // button (returned through `button`).
    bool tap(int x, int y, Button &button);
    bool moveWord(int dir);     // Circle Pad left/right: previous/next word
    bool moveRow(int dir);      // Circle Pad up/down: the row above/below, same position
    // Looks up one of the words the next press could go to, or checks one word against the
    // known words (call while idle). False when there is nothing left to do.
    bool prefetch();
    bool page(int dir);         // L/R, D-pad left/right: the other dictionary matches at this spot
    bool scroll(int lines);     // D-pad up/down, C-stick: the definition up/down
    // Words the player has already (Anki cards, saved words) show in green; prefetch() finds them.
    // markSaved() adds the saved word to it. nullptr: none.
    void setKnown(KnownWords *known) { known_ = known; }
    // True once since prefetch() found a known word (the screens should be drawn again).
    bool takeRedraw() { bool r = redraw_; redraw_ = false; return r; }
    void setRedraw() { redraw_ = true; }  // ask again (not drawn yet)
    void setHidden(bool hidden) { hidden_ = hidden; }  // the Hide button then reads "Show"
    bool hidden() const { return hidden_; }

    // Where the panels go: the text on the touchscreen and the entry on the top screen (the
    // default), or the other way round (then taps only reach the buttons, which stay on the
    // touchscreen). Call before setText() or after: the layout follows.
    void setTextOnTop(bool onTop);
    bool textOnTop() const { return textOnTop_; }

    // ---- what is selected ----
    bool hasResult() const { return !so_.results.empty(); }
    const Result *result() const { return hasResult() ? &so_.results[resultIdx_] : nullptr; }
    const std::string &query() const { return so_.query; }
    const std::string &sentence() const;          // the block the selection is in
    std::string selectedText() const;             // the highlighted characters
    // Shows the selection (and the word elsewhere) as saved (green); `tag`: the caller's note of
    // what was saved (the line written), for taking it back.
    void markSaved(const std::string &tag = "");
    // The selected word was saved in this lookup: its tag (the Save button then reads Undo).
    const std::string *savedTag() const;
    void unmarkSaved();                           // takes the selection's save back
    bool anySaved() const { return !saved_.empty(); }
    // The spellings the selected word is known under (for marking it known by hand); empty if
    // there is no word here.
    std::vector<std::string> selectedForms();
    // The known words changed: every word checked already is shown again as known or not.
    void refreshKnown();
    void setStatus(const std::string &s, Rgb c) { status_ = s; statusColor_ = c; }

    // ---- drawing ----
    // How a screen's panel is shown: normally; see-through (the text panel without its
    // background, the entry without its darker band: the caller paints the game lighter
    // underneath); or hidden (nothing drawn: the game as it is).
    enum class Panel { Shown, SeeThrough, Hidden };
    // Each screen's panel. Under the entry the caller has drawn the dimmed game (or black), and,
    // with bandShaded, the darker band behind it too (entryBand() rows, darkened by kBandShade).
    // The touchscreen also gets the buttons (not when hidden).
    void drawTop(Canvas &c, Panel p = Panel::Shown, bool bandShaded = false);
    void drawBottom(Canvas &c, Panel p = Panel::Shown, bool bandShaded = false);
    int entryBand() const;  // rows of the darker band behind the entry, on its screen
    static constexpr uint8_t kBandShade = 185;

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
    int textSize() const { return textSize_; }
    bool cellKnown(int i) const { return isKnown(cells_[i]); }

private:
    Dictionary &dict_;
    TextRenderer &tr_;
    std::vector<std::string> blocks_;
    std::vector<std::vector<size_t>> breaks_;  // per block: byte offsets that start a game line
    std::vector<Cell> cells_;
    std::vector<Row> rows_;
    mutable std::vector<std::vector<size_t>> stops_;  // per block: byte offsets where words start
    mutable std::vector<char> stopsReady_;            // per block: stops_ worked out yet
    const std::vector<size_t> &stops(size_t block) const;
    int contentH_ = 0, scrollY_ = 0;
    int textSize_ = 27, rowH_ = 34;
    // selection
    int selBlock_ = -1;
    size_t selByte_ = 0, selLen_ = 0;
    SearchOutput so_;
    size_t resultIdx_ = 0;
    // the entry on the top screen
    struct Span { std::string text; Rgb color; int size = 0; };  // size 0 = the line's
    struct Line { std::vector<Span> spans; int size = 16, height = 21; };
    std::vector<Line> entry_;
    int topScroll_ = 0;
    bool hidden_ = false, textOnTop_ = false;
    // the layout's sizes, by where the text goes
    int textW() const;
    int textBottom() const;
    int statusY() const;
    int entryW() const;
    int entryEnd() const;  // entry lines end above this
    void drawTextPanel(Canvas &c, Panel p);
    void drawEntryPanel(Canvas &c, Panel p, bool bandShaded);
    void drawButtons(Canvas &c);
    struct Saved { int block; size_t byte, len; std::string tag; };
    std::vector<Saved> saved_;
    std::string status_;
    Rgb statusColor_;

    struct Cached { int block; size_t byte; SearchOutput so; };
    std::vector<Cached> cache_;
    // known words: per block, per word character
    // a word character: whether the longest word starting there is known (state 0 not checked
    // yet, 1 no, 2 yes); len its bytes, first the first character's; stop: a word starts there
    // (word: its length as the text is split)
    struct Mark { size_t byte = 0, len = 0, first = 0, word = 0; uint8_t state = 0; bool stop = false, counts = false; std::vector<std::string> forms; };
    mutable std::vector<std::vector<Mark>> marks_;  // filled in with stops()
    KnownWords *known_ = nullptr;
    bool redraw_ = false;
    bool checkKnown();
    // word: the length of the match to take (0: the longest)
    void formsOf(const SearchOutput &so, std::vector<std::string> &forms, size_t &len, bool *kanjiSurface = nullptr,
                 size_t word = 0);
    bool isKnown(const Cell &c) const;

    void layout();
    static void trimResults(SearchOutput &so);
    SearchOutput lookup(int block, size_t byte);
    bool isCached(int block, size_t byte) const;
    bool wordTarget(int dir, int &block, size_t &byte) const;
    bool rowTarget(int dir, int &block, size_t &byte) const;
    bool selectAt(int block, size_t byte);
    void buildEntry();
    void keepVisible();
    int topLinesFit() const;
    bool isSaved(const Cell &c) const;
};

}  // namespace kt
