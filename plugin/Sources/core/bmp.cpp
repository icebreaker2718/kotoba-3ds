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

template <int F>
static inline void pixelToRGB(const uint8_t *px, uint8_t *o) {
    if (F == 0) { o[0] = px[3]; o[1] = px[2]; o[2] = px[1]; return; }  // RGBA8: A, B, G, R
    if (F == 1) { o[0] = px[2]; o[1] = px[1]; o[2] = px[0]; return; }  // BGR8
    uint16_t v = (uint16_t)(px[0] | (px[1] << 8));
    if (F == 2) { o[0] = (uint8_t)((v >> 8) & 0xF8); o[1] = (uint8_t)((v >> 3) & 0xFC); o[2] = (uint8_t)((v << 3) & 0xF8); }
    else if (F == 3) { o[0] = (uint8_t)((v >> 8) & 0xF8); o[1] = (uint8_t)((v >> 3) & 0xF8); o[2] = (uint8_t)((v << 2) & 0xF8); }
    else { o[0] = (uint8_t)((v >> 8) & 0xF0); o[1] = (uint8_t)((v >> 4) & 0xF0); o[2] = (uint8_t)(v & 0xF0); }
}

template <int F>
static void columnsToRGB(const uint8_t *px, unsigned w, unsigned h, unsigned bpp, uint8_t *rgb) {
    const size_t rowBytes = (size_t)w * 3;
    for (unsigned x = 0; x < w; x++) {
        const uint8_t *src = px + (size_t)x * h * bpp;
        uint8_t *dst = rgb + (size_t)(h - 1) * rowBytes + x * 3;  // the column starts at the bottom
        for (unsigned k = 0; k < h; k++, src += bpp, dst -= rowBytes) pixelToRGB<F>(src, dst);
    }
}

void framebufferColumnsToRGB(const uint8_t *px, unsigned w, unsigned h, int format, unsigned bpp, uint8_t *rgb) {
    switch (format) {
    case 0: columnsToRGB<0>(px, w, h, bpp, rgb); break;
    case 1: columnsToRGB<1>(px, w, h, bpp, rgb); break;
    case 2: columnsToRGB<2>(px, w, h, bpp, rgb); break;
    case 3: columnsToRGB<3>(px, w, h, bpp, rgb); break;
    default: columnsToRGB<4>(px, w, h, bpp, rgb); break;
    }
}

std::string bmpHeader(unsigned w, unsigned h) {
    const unsigned stride = (w * 3 + 3) & ~3u, dataSize = stride * h;
    std::string out(54, '\0');
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) out[at + i] = (char)((v >> (8 * i)) & 0xFF); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, 54 + dataSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    out[26] = 1;
    out[28] = 24;
    put32(34, dataSize);
    return out;
}

void rgbToFramebuffer(const uint8_t *rgb, int format, uint8_t *px) {
    uint8_t r = rgb[0], g = rgb[1], b = rgb[2];
    uint16_t v;
    switch (format) {
    case 0: px[0] = 0xFF; px[1] = b; px[2] = g; px[3] = r; return;  // RGBA8: A, B, G, R
    case 1: px[0] = b; px[1] = g; px[2] = r; return;                 // BGR8
    case 2: v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); break;
    case 3: v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xF8) << 3) | ((b & 0xF8) >> 2) | 1); break;
    default: v = (uint16_t)(((r & 0xF0) << 8) | ((g & 0xF0) << 4) | (b & 0xF0) | 0xF); break;
    }
    px[0] = (uint8_t)(v & 0xFF);
    px[1] = (uint8_t)(v >> 8);
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
