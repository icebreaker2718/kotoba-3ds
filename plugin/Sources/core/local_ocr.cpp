#include "core/local_ocr.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_refine.h"

namespace kt {

void *LocalOcr::alignedAlloc(size_t bytes) {  // 16-byte aligned; freed by the destructor (the plugin never runs it)
    void *raw = malloc(bytes + 16);
    if (!raw) return nullptr;
    raw_.push_back(raw);
    uintptr_t p = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    return (void *)p;
}

void LocalOcr::unload() {
    for (void *p : raw_) free(p);
    std::vector<void *>().swap(raw_);
    blob_ = arena_ = nullptr;
    model_ = ctx_ = nullptr;
    info_.clear();
}

LocalOcr::~LocalOcr() { unload(); }

bool LocalOcr::load(Reader &file, std::string &error) {
    if (blob_) return true;
    bool ok = loadInner(file, error);
    if (!ok) unload();  // give back anything allocated before the failure
    return ok;
}

bool LocalOcr::loadInner(Reader &file, std::string &error) {
    uint32_t n = file.size();
    if (n < 64 || n > 8 * 1024 * 1024) { error = "the OCR model file looks wrong"; return false; }
    unsigned char *blob = (unsigned char *)alignedAlloc(n);
    if (!blob) { error = "not enough memory for the OCR model"; return false; }
    for (uint32_t off = 0; off < n;) {  // in pieces: SD reads of a few hundred KB at a time
        uint32_t piece = n - off > 256 * 1024 ? 256 * 1024 : n - off;
        if (!file.read(off, blob + off, piece)) { error = "could not read the OCR model"; return false; }
        off += piece;
    }
    vocr_model *m = (vocr_model *)alignedAlloc(sizeof(vocr_model));
    vocr_ctx *c = (vocr_ctx *)alignedAlloc(sizeof(vocr_ctx));
    if (!m || !c) { error = "not enough memory for the OCR model"; return false; }
    int r = vocr_load(m, blob, n);
    if (r == VOCR_OK && vocr_check_crc(blob, n) != VOCR_OK) r = VOCR_E_FORMAT;
    if (r != VOCR_OK) {
        char b[96];
        snprintf(b, sizeof(b), "the OCR model file is damaged or not a VOCR model (%d)", r);
        error = b;
        return false;
    }
    vocr_opts o;
    memset(&o, 0, sizeof(o));
    o.mode = m->int8 ? VOCR_MODE_W8I : VOCR_MODE_F32;
    size_t need = vocr_arena_bytes(m, &o);
    unsigned char *arena = (unsigned char *)alignedAlloc(need);
    if (!arena) { error = "not enough memory for the OCR"; return false; }
    r = vocr_init(c, m, &o, arena, need);
    if (r != VOCR_OK) { error = "could not start the OCR"; return false; }
    blob_ = blob;
    arena_ = arena;
    model_ = m;
    ctx_ = c;
    char b[96];
    snprintf(b, sizeof(b), "%u characters, %u KB", m->n_classes, (unsigned)(n / 1024));
    info_ = b;
    return true;
}

// Recognizes one line; a line too long for the model is read in pieces.
static int runLine(vocr_ctx *c, const vocr_model *m, const uint8_t *grey, unsigned stride, const VocrLine &L,
                   std::string &out) {
    out.clear();
    int fmt = VOCR_GREY8 | (L.dark ? VOCR_INVERT : 0);
    unsigned pieces = 1;
    // the model squeezes anything wider than max_width after scaling to its height: split instead
    while ((unsigned long)((L.w + pieces - 1) / pieces) * m->height > (unsigned long)m->max_width * L.h && pieces < 8)
        pieces++;
    for (unsigned p = 0; p < pieces; p++) {
        unsigned x0 = L.x + L.w * p / pieces, x1 = L.x + L.w * (p + 1) / pieces;
        vocr_image im;
        im.pixels = grey + (size_t)L.y * stride + x0;
        im.width = x1 - x0;
        im.height = L.h;
        im.stride = stride;
        im.format = fmt;
        char text[512];
        int r = vocr_run(c, &im, text, sizeof(text), nullptr);
        if (r != VOCR_OK && r != VOCR_E_TEXT) return r;
        out += text;
    }
    return VOCR_OK;
}

bool LocalOcr::read(const uint8_t *grey, unsigned w, unsigned h, std::vector<LensLine> &lines, std::string &error,
                    void (*progress)(unsigned, unsigned)) {
    lines.clear();
    if (!blob_) { error = "no OCR model loaded"; return false; }
    VocrLinesImage im;
    im.pixels = grey;
    im.width = w;
    im.height = h;
    im.stride = w;
    im.bpp = VOCR_LINES_GREY;
    size_t la = vocr_lines_arena_bytes(w, h);
    void *larena = malloc(la);
    if (!larena) { error = "not enough memory for the OCR"; return false; }
    VocrLineSet set;
    int r = vocr_find_lines(&im, VOCR_LINES_GRAD, larena, la, &set);
    if (r == VOCR_LINES_OK) vocr_refine_lines(&im, VOCR_REFINE_DEFAULT, larena, la, &set, nullptr);
    free(larena);
    if (r != VOCR_LINES_OK) { error = "could not find text lines"; return false; }
    // furigana and other small print: not worth the time to recognize (they'd be dropped anyway)
    // (the reference height comes from line-shaped boxes: wide for their height, not icons/blobs)
    unsigned tallest = 0;
    for (unsigned i = 0; i < set.count; i++)
        if (set.lines[i].w >= 3 * set.lines[i].h && set.lines[i].h > tallest) tallest = set.lines[i].h;
    if (!tallest)
        for (unsigned i = 0; i < set.count; i++) if (set.lines[i].h > tallest) tallest = set.lines[i].h;
    std::vector<unsigned> todo;
    for (unsigned i = 0; i < set.count; i++)
        if (set.lines[i].h * 100 >= tallest * 55 && set.lines[i].w >= 8) todo.push_back(i);
#ifndef __3DS__
    if (getenv("LO_DEBUG")) for (unsigned i = 0; i < set.count; i++) fprintf(stderr, "found %u,%u %ux%u dark=%u\n", set.lines[i].x, set.lines[i].y, set.lines[i].w, set.lines[i].h, set.lines[i].dark);
#endif
    vocr_ctx *c = (vocr_ctx *)ctx_;
    const vocr_model *m = (const vocr_model *)model_;
    for (size_t k = 0; k < todo.size(); k++) {
        if (progress) progress((unsigned)k, (unsigned)todo.size());
        const VocrLine &L = set.lines[todo[k]];
        std::string text;
        r = runLine(c, m, grey, w, L, text);
#ifndef __3DS__
        if (getenv("LO_DEBUG")) fprintf(stderr, "run r=%d [%s]\n", r, text.c_str());
#endif
        if (r != VOCR_OK) continue;
        if (text.empty()) continue;
        LensLine line;
        line.text = text;
        line.paragraph = (int)k;  // each line its own block; lensParagraphs() joins a text box's lines
        line.centerX = (L.x + L.w / 2.0f) / w;
        line.centerY = (L.y + L.h / 2.0f) / h;
        line.width = (float)L.w / w;
        line.height = (float)L.h / h;
        lines.push_back(line);
    }
    if (lines.empty()) { error = "no text found on the screen"; return false; }
    return true;
}

uint8_t *topScreenGrey(Reader &bmp, unsigned &outW, unsigned &outH, std::string &error) {
    uint8_t hd[54];
    if (bmp.size() < sizeof(hd) || !bmp.read(0, hd, sizeof(hd)) || hd[0] != 'B' || hd[1] != 'M') {
        error = "the screenshot is not a BMP file";
        return nullptr;
    }
    auto rd32 = [](const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); };
    uint32_t dataOff = rd32(hd + 10);
    int32_t width = (int32_t)rd32(hd + 18), height = (int32_t)rd32(hd + 22);
    unsigned bpp = hd[28] | (hd[29] << 8);
    if (rd32(hd + 30) != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0) {
        error = "unsupported screenshot format";
        return nullptr;
    }
    bool bottomUp = height > 0;
    uint32_t H = bottomUp ? (uint32_t)height : (uint32_t)(-height), W = (uint32_t)width;
    outW = W < 400 ? W : 400;
    outH = H < 240 ? H : 240;
    uint32_t bytesPP = bpp / 8, stride = (W * bytesPP + 3) & ~3u;
    uint8_t *grey = (uint8_t *)malloc((size_t)outW * outH);
    uint8_t *row = (uint8_t *)malloc(stride);
    if (!grey || !row) {
        free(grey);
        free(row);
        error = "not enough memory for the picture";
        return nullptr;
    }
    for (uint32_t y = 0; y < outH; y++) {
        uint32_t fileRow = bottomUp ? (H - 1 - y) : y;
        if (!bmp.read(dataOff + fileRow * stride, row, stride)) {
            free(grey);
            free(row);
            error = "could not read the screenshot";
            return nullptr;
        }
        for (uint32_t x = 0; x < outW; x++) {
            const uint8_t *p = row + x * bytesPP;  // B, G, R
            grey[y * outW + x] = (uint8_t)((77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8);
        }
    }
    free(row);
    return grey;
}

}  // namespace kt
