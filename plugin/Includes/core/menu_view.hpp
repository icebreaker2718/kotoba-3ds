// menu_view.hpp - the plugin's own screens for its settings and lists, in the lookup screens'
// look: a list on the touchscreen (tap, or D-pad / Circle Pad and A), the highlighted item's
// explanation (or a saved word's entry) on the top screen, buttons along the bottom.
// Platform independent, like lookup_view: the plugin feeds it input and two screens to draw on.
#pragma once
#include <string>
#include <vector>

#include "core/ui_text.hpp"

namespace kt {

struct MenuItem {
    std::string label;
    std::string value;  // shown at the right of the row (a setting's current choice), or empty
    std::string help;   // shown on the top screen while the item is highlighted ("\n": new line)
    bool dim = false;   // greyed out (nothing to do there right now)
};

class MenuView {
public:
    static constexpr int kW = 320, kH = 240, kTopW = 400;

    explicit MenuView(TextRenderer &text) : tr_(text) {}

    void setTitle(const std::string &title) { title_ = title; }
    // The rows. The highlight stays on the same index (clamped) unless select() moves it.
    void setItems(const std::vector<MenuItem> &items);
    const std::vector<MenuItem> &items() const { return items_; }
    // Shown on the top screen under the title when there are no rows, or after the row's help.
    void setHelp(const std::string &help) { help_ = help; }
    // Shown in the list's place when there are no rows (a question, a progress message).
    void setMessage(const std::string &text) { message_ = text; }
    // The buttons along the bottom, left to right, e.g. {" Choose", " Back"}.
    void setButtons(const std::vector<std::string> &labels) { buttons_ = labels; }
    // A line above the buttons (a result, a warning); empty to clear.
    void setStatus(const std::string &text, Rgb color) { status_ = text; statusColor_ = color; }

    int selected() const { return items_.empty() ? -1 : sel_; }
    void select(int i);
    bool move(int dir);  // up / down; false at either end
    // A touch at (x, y): returns the row touched (-1 if none); `button` = the button touched or -1.
    int tap(int x, int y, int &button) const;

    void drawTop(Canvas &c);
    void drawBottom(Canvas &c);

    // Layout, for the tests.
    static constexpr int kRowTop = 6, kRowH = 28, kListBottom = 182, kButtonY = 206, kButtonH = 30;
    int scroll() const { return scroll_; }

private:
    TextRenderer &tr_;
    std::string title_, help_, message_, status_;
    Rgb statusColor_;
    std::vector<MenuItem> items_;
    std::vector<std::string> buttons_;
    int sel_ = 0, scroll_ = 0;  // scroll_: the first row shown
    int rowsFit() const;
    void keepVisible();
};

}  // namespace kt
