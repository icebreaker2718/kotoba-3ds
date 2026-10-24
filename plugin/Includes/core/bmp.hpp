// bmp.hpp - screenshots: reading the top screen out of a .bmp, writing one, framebuffer pixels.
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

// Framebuffer pixels (GSP format: 0 RGBA8, 1 BGR8, 2 RGB565, 3 RGB5A1, 4 RGBA4; bpp bytes
// each) -> R, G, B bytes. Same channel layout as CTRPF's own pixel reader.
void framebufferToRGB(const uint8_t *px, unsigned count, int format, unsigned bpp, uint8_t *rgb);

// A 24-bit BMP file (bottom-up rows) of w x h R, G, B pixels.
std::string bmpFromRGB(const uint8_t *rgb, unsigned w, unsigned h);

}  // namespace kt
