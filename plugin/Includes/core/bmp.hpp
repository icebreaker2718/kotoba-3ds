// bmp.hpp - turns a CTRPF screenshot (.bmp, top screen above bottom screen) into a compact
// 16-bit top-screen image for uploading: ~190 KB instead of ~570 KB.
#pragma once
#include <string>

#include "core/dict.hpp"  // kt::Reader

namespace kt {

// Output: "K3RGB565 <w> <h>\n" followed by w*h little-endian RGB565 pixels, rows top to bottom.
// Reads at most the top 400x240 of the picture (the top screen).
bool topScreenRGB565(Reader &bmp, std::string &out, std::string &error);

}  // namespace kt
