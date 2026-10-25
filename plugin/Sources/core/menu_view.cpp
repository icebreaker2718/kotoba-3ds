#include "core/menu_view.hpp"

#include <algorithm>

namespace kt {

// the lookup screens' colours (lookup_view.cpp)
static const Rgb kBg(15, 18, 22), kText(237, 237, 237), kDim(138, 144, 153), kWord(245, 197, 66),
    kSelBg(43, 49, 60), kValue(142, 197, 255), kRule(58, 63, 71), kButton(34, 40, 48), kButtonEdge(70, 78, 90);
static const int kMargin = 14, kLabelSize = 17, kValueSize = 15, kTitleSize = 22, kHelpSize = 16, kHelpLine = 21;

void MenuView::setItems(const std::vector<MenuItem> &items) {
    items_ = items;
    if (sel_ >= (int)items_.size()) sel_ = (int)items_.size() - 1;
    if (sel_ < 0) sel_ = 0;
    keepVisible();
}

void MenuView::select(int i) {
    if (items_.empty()) return;
    sel_ = std::max(0, std::min(i, (int)items_.size() - 1));
    keepVisible();
}

bool MenuView::move(int dir) {
    int to = sel_ + (dir > 0 ? 1 : -1);
    if (items_.empty() || to < 0 || to >= (int)items_.size()) return false;
    sel_ = to;
    keepVisible();
    return true;
}

int MenuView::rowsFit() const {
    int bottom = status_.empty() ? kListBottom + 18 : kListBottom;
    return std::max(1, (bottom - kRowTop) / kRowH);
}

void MenuView::keepVisible() {
    const int n = rowsFit();
    if (sel_ < scroll_) scroll_ = sel_;
    if (sel_ >= scroll_ + n) scroll_ = sel_ - n + 1;
    scroll_ = std::max(0, std::min(scroll_, std::max(0, (int)items_.size() - n)));
}

int MenuView::tap(int x, int y, int &button) const {
    button = -1;
    if (y >= kButtonY - 4) {
        if (buttons_.empty() || x < 8 || x >= kW - 8) return -1;
        const int bw = (kW - 16) / (int)buttons_.size();
        button = std::min((x - 8) / bw, (int)buttons_.size() - 1);
        return -1;
    }
    if (y < kRowTop) return -1;
    const int row = scroll_ + (y - kRowTop) / kRowH;
    if ((y - kRowTop) / kRowH >= rowsFit() || row >= (int)items_.size()) return -1;
    return row;
}

static void frame(Canvas &c, int x, int y, int w, int h, Rgb fill, Rgb edge) {
    c.fill(x, y, w, h, edge);
    c.fill(x + 1, y + 1, w - 2, h - 2, fill);
}

void MenuView::drawTop(Canvas &c) {
    c.fill(0, 0, kTopW, kH, kBg);
    tr_.draw(c, kMargin, 10, title_, kTitleSize, kWord);
    c.fill(kMargin, 40, kTopW - 2 * kMargin, 1, kRule);
    std::string text = help_;
    if (!items_.empty() && !items_[sel_].help.empty()) text = items_[sel_].help + (help_.empty() ? "" : "\n\n" + help_);
    int y = 50;
    size_t start = 0;
    while (start <= text.size() && y + kHelpLine <= kH - 4) {  // paragraph by paragraph
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (para.empty()) {
            y += kHelpLine / 2;
        } else {
            for (const std::string &line : tr_.wrap(para, kHelpSize, kTopW - 2 * kMargin)) {
                if (y + kHelpLine > kH - 4) break;
                tr_.draw(c, kMargin, y + 2, line, kHelpSize, kText);
                y += kHelpLine;
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

void MenuView::drawBottom(Canvas &c) {
    c.fill(0, 0, kW, kH, kBg);
    const int n = rowsFit();
    for (int r = 0; r < n && scroll_ + r < (int)items_.size(); r++) {
        const MenuItem &it = items_[scroll_ + r];
        const bool sel = scroll_ + r == sel_;
        const int y = kRowTop + r * kRowH;
        if (sel) c.fill(4, y, kW - 8, kRowH - 2, kSelBg);
        else if (r > 0) c.fill(kMargin, y - 1, kW - 2 * kMargin, 1, kRule);
        Rgb lc = it.dim ? kDim : (sel ? kWord : kText);
        int valueW = it.value.empty() ? 0 : tr_.width(it.value, kValueSize);
        tr_.draw(c, kMargin, y + (kRowH - 2 - kLabelSize) / 2, it.label, kLabelSize, lc);
        if (valueW) tr_.draw(c, kW - kMargin - valueW, y + (kRowH - 2 - kValueSize) / 2, it.value, kValueSize, it.dim ? kDim : kValue);
    }
    if (scroll_ > 0) tr_.draw(c, kW - 12, kRowTop - 4, "▲", 10, kDim);
    if (scroll_ + n < (int)items_.size()) tr_.draw(c, kW - 12, kRowTop + n * kRowH - 10, "▼", 10, kDim);
    if (items_.empty() && !message_.empty()) {  // the message, centred in the list's place
        std::vector<std::string> lines;
        size_t start = 0;
        while (true) {
            size_t nl = message_.find('\n', start);
            std::string para = message_.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (para.empty()) lines.push_back("");
            for (const std::string &l : tr_.wrap(para, kLabelSize, kW - 2 * kMargin)) lines.push_back(l);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        const int lineH = kLabelSize + 6;
        int y = std::max(8, (kListBottom - (int)lines.size() * lineH) / 2);
        for (const std::string &l : lines) {
            if (y + lineH > kListBottom + 16) break;
            int w = tr_.width(l, kLabelSize);
            tr_.draw(c, (kW - w) / 2, y, l, kLabelSize, kText);
            y += lineH;
        }
    }
    if (!status_.empty()) {
        std::vector<std::string> s = tr_.wrap(status_, 14, kW - 2 * kMargin);
        if (!s.empty()) tr_.draw(c, kMargin, kListBottom + 4, s[0], 14, statusColor_);
    }
    if (!buttons_.empty()) {
        const int bw = (kW - 16) / (int)buttons_.size();
        for (size_t i = 0; i < buttons_.size(); i++) {
            int x = 8 + (int)i * bw;
            frame(c, x + 2, kButtonY, bw - 4, kButtonH, kButton, kButtonEdge);
            int w = tr_.width(buttons_[i], 16);
            tr_.draw(c, x + (bw - w) / 2, kButtonY + (kButtonH - 16) / 2, buttons_[i], 16, kText);
        }
    }
}

}  // namespace kt
