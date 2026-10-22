#include "core/bmp.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image_write.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

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

// Reads the top 400x240 of the screenshot as RGB888 (malloc'd: big, and failure must not abort).
static uint8_t *topScreenRGB(Reader &bmp, uint32_t &outW, uint32_t &outH, int scale, std::string &error) {
    uint8_t h[54];
    if (bmp.size() < sizeof(h) || !bmp.read(0, h, sizeof(h)) || h[0] != 'B' || h[1] != 'M') {
        error = "the screenshot is not a BMP file";
        return nullptr;
    }
    uint32_t dataOff = rd32(h + 10);
    int32_t width = (int32_t)rd32(h + 18), height = (int32_t)rd32(h + 22);
    uint16_t bpp = rd16(h + 28);
    if (rd32(h + 30) != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0) {
        error = "unsupported screenshot format";
        return nullptr;
    }
    bool bottomUp = height > 0;
    uint32_t H = bottomUp ? (uint32_t)height : (uint32_t)(-height), W = (uint32_t)width;
    uint32_t w = W < 400 ? W : 400, hh = H < 240 ? H : 240;
    uint32_t bytesPP = bpp / 8, stride = (W * bytesPP + 3) & ~3u;
    outW = w * scale;
    outH = hh * scale;
    uint8_t *rgb = (uint8_t *)malloc((size_t)outW * outH * 3);
    uint8_t *row = (uint8_t *)malloc(stride);
    if (!rgb || !row) {
        free(rgb);
        free(row);
        error = "not enough memory for the picture";
        return nullptr;
    }
    for (uint32_t y = 0; y < hh; y++) {
        uint32_t fileRow = bottomUp ? (H - 1 - y) : y;
        if (!bmp.read(dataOff + fileRow * stride, row, stride)) {
            free(rgb);
            free(row);
            error = "could not read the screenshot";
            return nullptr;
        }
        for (int sy = 0; sy < scale; sy++) {
            uint8_t *dst = rgb + ((size_t)(y * scale + sy) * outW) * 3;
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = row + x * bytesPP;  // B, G, R
                for (int sx = 0; sx < scale; sx++, dst += 3) { dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0]; }
            }
        }
    }
    free(row);
    return rgb;
}

static void appendBytes(void *ctx, void *data, int size) { ((std::string *)ctx)->append((const char *)data, size); }

bool topScreenJpeg(Reader &bmp, int scale, int quality, std::string &out, int &width, int &height,
                   std::string &error) {
    uint32_t w = 0, h = 0;
    uint8_t *rgb = topScreenRGB(bmp, w, h, scale, error);
    if (!rgb && scale > 1) {  // not enough memory for the bigger picture: send it as it is
        std::string e2;
        rgb = topScreenRGB(bmp, w, h, 1, e2);
    }
    if (!rgb) return false;
    out.clear();
    int ok = stbi_write_jpg_to_func(appendBytes, &out, (int)w, (int)h, 3, rgb, quality);
    free(rgb);
    if (!ok || out.empty()) {
        error = "could not encode the picture";
        return false;
    }
    width = (int)w;
    height = (int)h;
    return true;
}

}  // namespace kt
