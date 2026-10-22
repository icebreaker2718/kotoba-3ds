// textbox.hpp - finds a game's dialogue box on the top screen without any setup: a large area
// of one flat colour (the box) with text-sized holes in it (the letters). Reading only that box
// keeps busy scenery out of the OCR's line finder.
#pragma once
#include <cstdint>
#include <vector>

namespace kt {

struct TextBox {
    unsigned x = 0, y = 0, w = 0, h = 0;
    uint8_t r = 0, g = 0, b = 0;  // the box's colour
    float score = 0;
};

// rgb: w*h*3 bytes (R, G, B), rows top to bottom. Best candidate first; empty if none.
std::vector<TextBox> findTextBoxes(const uint8_t *rgb, unsigned w, unsigned h, unsigned maxBoxes = 3);

}  // namespace kt
