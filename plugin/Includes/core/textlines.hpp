// textlines.hpp - groups recognized text lines into blocks (one per text box), drops
// furigana and leftover specks. No OCR or drawing here, so it is unit-tested natively.
#pragma once
#include <string>
#include <vector>

namespace kt {

struct TextLine {
    std::string text;
    float height = 0;   // line box height (fraction of the screen height)
    float centerY = 0;
    float centerX = 0, width = 0;
    int paragraph = 0;
};

// One string per block, furigana dropped (kana-only lines much shorter than the text around
// them, or right above kanji they could be the reading of), lines of a block joined. Lines that
// continue straight below the previous one (same text size, same left edge) join it.
// Empty blocks removed.
std::vector<std::string> textParagraphs(const std::vector<TextLine> &lines);

// Tidies OCR output for the player: drops a stray one- or two-character piece after a block's
// final punctuation (the "next" marker or a sprite at the box edge read as text: 「！ク」), and
// drops blocks with hardly any Japanese when a real line is there too (「ー」, 「1AAS」).
void cleanParagraphs(std::vector<std::string> &paragraphs);

}  // namespace kt
