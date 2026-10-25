// bmp.hpp - screenshots: reading the top screen out of a .bmp, writing one, framebuffer pixels.
#pragma once
#include <cstdint>
#include <string>

#include "core/dict.hpp"  // kt::Reader

namespace kt {

// Output: "K3RGB565 <w> <h>\n" followed by w*h little-endian RGB565 pixels, rows top to bottom.
// Reads at most 400x480 (a screenshot of one screen, or of both).
bool screenshotRGB565(Reader &bmp, std::string &out, std::string &error);

// The top screen (up to 400x240) as R, G, B bytes, malloc'd (free() it). nullptr on failure.
uint8_t *topScreenRGB888(Reader &bmp, unsigned &w, unsigned &h, std::string &error);

// Framebuffer pixels (GSP format: 0 RGBA8, 1 BGR8, 2 RGB565, 3 RGB5A1, 4 RGBA4; bpp bytes
// each) -> R, G, B bytes. Same channel layout as CTRPF's own pixel reader.
void framebufferToRGB(const uint8_t *px, unsigned count, int format, unsigned bpp, uint8_t *rgb);

// A 24-bit BMP file (bottom-up rows) of w x h R, G, B pixels.
// A framebuffer copy stored the screen's way (w columns of h pixels, each column bottom to top)
// -> R, G, B rows from the top (w*h*3 bytes). The format test is done once, not per pixel.
void framebufferColumnsToRGB(const uint8_t *px, unsigned w, unsigned h, int format, unsigned bpp, uint8_t *rgb);

// The 54-byte header of a 24-bit bottom-up BMP (rows padded to 4 bytes), for writing one in pieces.
std::string bmpHeader(unsigned w, unsigned h);

// One R, G, B pixel -> framebuffer bytes (the inverse of framebufferToRGB, opaque).
void rgbToFramebuffer(const uint8_t *rgb, int format, uint8_t *px);

std::string bmpFromRGB(const uint8_t *rgb, unsigned w, unsigned h);

// A screen's picture kept while a lookup is open, in the framebuffer's column order (each
// column bottom to top), packed as R, G, B (picBytes 3) or RGB565 (picBytes 2, when both
// screens are kept). n pixels of one column at a time.
void framebufferColumnToPicture(const uint8_t *fb, unsigned n, int format, unsigned bpp, unsigned picBytes, uint8_t *pic);
// Back to the framebuffer: tone 0 as it was, 1 dimmed to ~19% (behind the lookup), 2 to 50%
// (see-through); pixels from shadeFrom on (the screen's top rows) also darkened, c * keep >> 8.
void pictureColumnToFramebuffer(const uint8_t *pic, unsigned n, unsigned picBytes, int tone, unsigned shadeFrom,
                                unsigned keep, int format, unsigned bpp, uint8_t *fb);
// One picture pixel -> R, G, B.
void picturePixelRGB(const uint8_t *pic, unsigned picBytes, uint8_t *rgb);

// A QOI image (qoiformat.org: lossless, about a third of a BMP's size for game screens), written
// a pixel at a time, rows from the top; the bytes come out through `sink` in pieces of up to
// `cap` bytes, gathered in `buf` (the caller's: a CTRPF task has only 4 KB of stack).
class QoiEncoder {
public:
    using Sink = bool (*)(const uint8_t *data, size_t n, void *user);  // false: stop (write failed)
    QoiEncoder(unsigned w, unsigned h, Sink sink, void *user, uint8_t *buf, size_t cap);
    void push(uint8_t r, uint8_t g, uint8_t b);
    bool finish();  // the end marker, and what is still buffered; false if the sink failed
private:
    Sink sink_;
    void *user_;
    bool ok_ = true;
    uint8_t *buf_;
    size_t cap_, n_ = 0;
    uint32_t index_[64] = {};
    uint32_t prev_ = 0xFF;  // 0xRRGGBBAA: opaque black
    unsigned run_ = 0;
    void put(uint8_t b) { buf_[n_++] = b; if (n_ == cap_) flush(); }
    void flush();
};

}  // namespace kt
