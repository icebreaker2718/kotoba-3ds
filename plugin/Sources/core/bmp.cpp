#include "core/bmp.hpp"

#include <cstdio>
#include <cstdlib>
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

uint8_t *topScreenRGB888(Reader &bmp, unsigned &w, unsigned &h, std::string &error) {
    uint32_t ww = 0, hh = 0;
    uint8_t *rgb = topScreenRGB(bmp, ww, hh, 1, error);
    w = ww;
    h = hh;
    return rgb;
}

void framebufferToRGB(const uint8_t *px, unsigned count, int format, unsigned bpp, uint8_t *rgb) {
    for (unsigned i = 0; i < count; i++, px += bpp, rgb += 3) {
        uint16_t v = (uint16_t)(px[0] | (px[1] << 8));
        switch (format) {
        case 0: rgb[0] = px[3]; rgb[1] = px[2]; rgb[2] = px[1]; break;  // RGBA8: A, B, G, R
        case 1: rgb[0] = px[2]; rgb[1] = px[1]; rgb[2] = px[0]; break;  // BGR8: B, G, R
        case 2:                                                          // RGB565
            rgb[0] = (uint8_t)((v >> 8) & 0xF8);
            rgb[1] = (uint8_t)((v >> 3) & 0xFC);
            rgb[2] = (uint8_t)((v << 3) & 0xF8);
            break;
        case 3:                                                          // RGB5A1
            rgb[0] = (uint8_t)((v >> 8) & 0xF8);
            rgb[1] = (uint8_t)((v >> 3) & 0xF8);
            rgb[2] = (uint8_t)((v << 2) & 0xF8);
            break;
        default:                                                         // RGBA4
            rgb[0] = (uint8_t)((v >> 8) & 0xF0);
            rgb[1] = (uint8_t)((v >> 4) & 0xF0);
            rgb[2] = (uint8_t)(v & 0xF0);
            break;
        }
    }
}

std::string bmpFromRGB(const uint8_t *rgb, unsigned w, unsigned h) {
    const unsigned stride = (w * 3 + 3) & ~3u, dataSize = stride * h;
    std::string out(54 + (size_t)dataSize, '\0');
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) out[at + i] = (char)((v >> (8 * i)) & 0xFF); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, 54 + dataSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);         // bottom-up
    out[26] = 1;          // planes
    out[28] = 24;         // bits per pixel
    put32(34, dataSize);
    for (unsigned y = 0; y < h; y++) {
        char *row = &out[54 + (size_t)(h - 1 - y) * stride];
        const uint8_t *src = rgb + (size_t)y * w * 3;
        for (unsigned x = 0; x < w; x++) {
            row[x * 3] = (char)src[x * 3 + 2];  // B, G, R
            row[x * 3 + 1] = (char)src[x * 3 + 1];
            row[x * 3 + 2] = (char)src[x * 3];
        }
    }
    return out;
}

}  // namespace kt
