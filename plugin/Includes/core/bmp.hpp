// bmp.hpp - turns a CTRPF screenshot (.bmp, top screen above bottom screen) into a compact
// 16-bit top-screen image for uploading: ~190 KB instead of ~570 KB.
#pragma once
#include <cstdint>
#include <string>

#include "core/dict.hpp"  // kt::Reader

namespace kt {

// Output: "K3RGB565 <w> <h>\n" followed by w*h little-endian RGB565 pixels, rows top to bottom.
// Reads at most the top 400x240 of the picture (the top screen).
bool topScreenRGB565(Reader &bmp, std::string &out, std::string &error);

// The top screen (up to 400x240) as R, G, B bytes, malloc'd (free() it). nullptr on failure.
uint8_t *topScreenRGB888(Reader &bmp, unsigned &w, unsigned &h, std::string &error);

// The top screen as a JPEG, enlarged `scale` times (pixel-doubled: small game fonts read
// better). Falls back to scale 1 if there isn't enough memory.
bool topScreenJpeg(Reader &bmp, int scale, int quality, std::string &out, int &width, int &height,
                   std::string &error);

}  // namespace kt
