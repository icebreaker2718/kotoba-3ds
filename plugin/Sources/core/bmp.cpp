// Built for speed (the plugin's C++ is otherwise built for size): the screens are painted from here.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "core/bmp.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>


namespace kt {

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool screenshotRGB565(Reader &bmp, std::string &out, std::string &error) {
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
    uint32_t outW = W < 400 ? W : 400, outH = H < 480 ? H : 480;
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

void framebufferColumnToPicture(const uint8_t *fb, unsigned n, int format, unsigned bpp, unsigned picBytes, uint8_t *pic) {
    for (unsigned i = 0; i < n; i++, fb += bpp) {
        uint8_t rgb[3];
        framebufferToRGB(fb, 1, format, bpp, rgb);
        if (picBytes == 3) {
            *pic++ = rgb[0];
            *pic++ = rgb[1];
            *pic++ = rgb[2];
        } else {
            const uint16_t v = (uint16_t)(((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
            *pic++ = (uint8_t)(v & 0xFF);
            *pic++ = (uint8_t)(v >> 8);
        }
    }
}

void picturePixelRGB(const uint8_t *pic, unsigned picBytes, uint8_t *rgb) {
    if (picBytes == 3) {
        rgb[0] = pic[0];
        rgb[1] = pic[1];
        rgb[2] = pic[2];
        return;
    }
    const unsigned v = pic[0] | (pic[1] << 8), r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    rgb[0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[1] = (uint8_t)((g << 2) | (g >> 4));
    rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

void pictureColumnToFramebuffer(const uint8_t *pic, unsigned n, unsigned picBytes, int tone, unsigned shadeFrom,
                                unsigned keep, int format, unsigned bpp, uint8_t *fb) {
    if (picBytes == 3 && format == 0 && bpp == 4) {  // the common case, without the per-pixel tests
        for (unsigned i = 0; i < n; i++, pic += 3, fb += 4) {
            unsigned r = pic[0], g = pic[1], b = pic[2];
            if (tone == 1) r = (r >> 3) + (r >> 4), g = (g >> 3) + (g >> 4), b = (b >> 3) + (b >> 4);
            else if (tone == 2) r >>= 1, g >>= 1, b >>= 1;
            if (i >= shadeFrom) r = (r * keep) >> 8, g = (g * keep) >> 8, b = (b * keep) >> 8;
            fb[0] = 0xFF;
            fb[1] = (uint8_t)b;
            fb[2] = (uint8_t)g;
            fb[3] = (uint8_t)r;
        }
        return;
    }
    for (unsigned i = 0; i < n; i++, pic += picBytes, fb += bpp) {
        uint8_t c[3];
        picturePixelRGB(pic, picBytes, c);
        for (int k = 0; k < 3; k++) {
            unsigned v = c[k];
            if (tone == 1) v = (v >> 3) + (v >> 4);  // ~19%
            else if (tone == 2) v >>= 1;            // 50%
            if (i >= shadeFrom) v = (v * keep) >> 8;
            c[k] = (uint8_t)v;
        }
        if (format == 0 && bpp == 4) {  // RGBA8: A, B, G, R
            fb[0] = 0xFF;
            fb[1] = c[2];
            fb[2] = c[1];
            fb[3] = c[0];
        } else {
            rgbToFramebuffer(c, format, fb);
        }
    }
}

// ---------------------------------------------------------------------------
// QOI
// ---------------------------------------------------------------------------
QoiEncoder::QoiEncoder(unsigned w, unsigned h, Sink sink, void *user, uint8_t *buf, size_t cap)
    : sink_(sink), user_(user), buf_(buf), cap_(cap) {
    const uint8_t head[14] = {'q', 'o', 'i', 'f', (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                              (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 3, 0};
    for (uint8_t b : head) put(b);
}

void QoiEncoder::flush() {
    if (n_ && ok_) ok_ = sink_(buf_, n_, user_);
    n_ = 0;
}

void QoiEncoder::push(uint8_t r, uint8_t g, uint8_t b) {
    const uint32_t px = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
    if (px == prev_) {
        if (++run_ == 62) { put(0xC0 | 61); run_ = 0; }
        return;
    }
    if (run_) { put((uint8_t)(0xC0 | (run_ - 1))); run_ = 0; }
    const unsigned idx = (r * 3 + g * 5 + b * 7 + 255 * 11) % 64;
    if (index_[idx] == px) {
        put((uint8_t)idx);
    } else {
        index_[idx] = px;
        const int dr = (int)r - (int)(prev_ >> 24), dg = (int)g - (int)((prev_ >> 16) & 0xFF),
                  db = (int)b - (int)((prev_ >> 8) & 0xFF);
        const int dr_dg = dr - dg, db_dg = db - dg;
        if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
            put((uint8_t)(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
        } else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
            put((uint8_t)(0x80 | (dg + 32)));
            put((uint8_t)(((dr_dg + 8) << 4) | (db_dg + 8)));
        } else {
            put(0xFE);
            put(r);
            put(g);
            put(b);
        }
    }
    prev_ = px;
}

bool QoiEncoder::finish() {
    if (run_) { put((uint8_t)(0xC0 | (run_ - 1))); run_ = 0; }
    for (int i = 0; i < 7; i++) put(0);
    put(1);
    flush();
    return ok_;
}

}  // namespace kt
