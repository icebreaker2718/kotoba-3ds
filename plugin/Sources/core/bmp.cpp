#include "core/bmp.hpp"

#include <cstdio>
#include <vector>

namespace kt {

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool topScreenRGB565(Reader &bmp, std::string &out, std::string &error) {
    uint8_t h[54];
    if (bmp.size() < sizeof(h) || !bmp.read(0, h, sizeof(h)) || h[0] != 'B' || h[1] != 'M') {
        error = "the screenshot is not a BMP file";
        return false;
    }
    uint32_t dataOff = rd32(h + 10);
    int32_t width = (int32_t)rd32(h + 18), height = (int32_t)rd32(h + 22);
    uint16_t bpp = rd16(h + 28);
    uint32_t compression = rd32(h + 30);
    if (compression != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0) {
        error = "unsupported screenshot format";
        return false;
    }
    bool bottomUp = height > 0;
    uint32_t H = bottomUp ? (uint32_t)height : (uint32_t)(-height);
    uint32_t W = (uint32_t)width;
    uint32_t outW = W < 400 ? W : 400, outH = H < 240 ? H : 240;
    uint32_t bytesPP = bpp / 8;
    uint32_t stride = (W * bytesPP + 3) & ~3u;

    char head[40];
    snprintf(head, sizeof(head), "K3RGB565 %lu %lu\n", (unsigned long)outW, (unsigned long)outH);
    out.assign(head);
    size_t base = out.size();
    out.resize(base + (size_t)outW * outH * 2);

    std::vector<uint8_t> row(stride);
    for (uint32_t y = 0; y < outH; y++) {
        uint32_t fileRow = bottomUp ? (H - 1 - y) : y;
        if (!bmp.read(dataOff + fileRow * stride, row.data(), stride)) {
            error = "could not read the screenshot";
            return false;
        }
        char *dst = &out[base + (size_t)y * outW * 2];
        for (uint32_t x = 0; x < outW; x++) {
            const uint8_t *p = &row[x * bytesPP];  // stored as B, G, R
            uint16_t v = (uint16_t)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
            dst[x * 2] = (char)(v & 0xFF);
            dst[x * 2 + 1] = (char)(v >> 8);
        }
    }
    return true;
}

}  // namespace kt
