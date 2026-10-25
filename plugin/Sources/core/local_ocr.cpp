#include "core/local_ocr.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>

#include "core/kana.hpp"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_refine.h"

// VOCR's phase marks (vocr.c is built with -DVOCR_PROF=kt_vocr_prof): free unless profiling.
static bool g_prof = false;
static uint64_t (*g_profClock)() = nullptr;
static uint64_t g_profTicks[64], g_profT = 0;
static unsigned g_profLast = 63;
extern "C" void kt_vocr_prof(unsigned id) {
    if (!g_prof) return;
    uint64_t t = g_profClock();
    g_profTicks[g_profLast] += t - g_profT;
    g_profT = t;
    g_profLast = id < 62 ? id : (id == 101 ? 62 : 63);
}

namespace kt {

void ocrProfile(bool on, uint64_t (*clock)()) {
    if (on) {
        memset(g_profTicks, 0, sizeof(g_profTicks));
        g_profClock = clock;
        g_profT = clock();
        g_profLast = kProfOther;
    } else if (g_prof) {
        kt_vocr_prof(100);  // close the last phase
    }
    g_prof = on && clock;
}

const uint64_t *ocrProfileTicks() { return g_profTicks; }

void ocrUsePairKernel(bool on) { vocr_use_pairs = on ? 1 : 0; }

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
    ctxs_.clear();
    topk_.clear();
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
    unsigned char *blob = (unsigned char *)alignedAlloc(n + 32);  // + the bytes VOCR's ARM kernel may read past the end
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
    // the CRC pass costs ~0.1 s on the 3DS: once per file per session is enough
    static uint32_t checkedSize = 0;
    if (r == VOCR_OK && checkedSize != n) {
        if (vocr_check_crc(blob, n) != VOCR_OK) r = VOCR_E_FORMAT;
        else checkedSize = n;
    }
    if (r == VOCR_OK) vocr_pack_simd32(m);  // ARM11: weights reordered for 2-at-a-time multiplies
    if (r != VOCR_OK) {
        char b[96];
        snprintf(b, sizeof(b), "the OCR model file is damaged or not a VOCR model (%d)", r);
        error = b;
        return false;
    }
    vocr_opts o;
    memset(&o, 0, sizeof(o));
    o.mode = m->int8 ? VOCR_MODE_W8I : VOCR_MODE_F32;
    o.tile_frames = tileFrames_;
    o.head_frames = headFrames_;
    size_t need = vocr_arena_bytes(m, &o);
    unsigned char *arena = (unsigned char *)alignedAlloc(need);
    if (!arena) { error = "not enough memory for the OCR"; return false; }
    r = vocr_init(c, m, &o, arena, need);
    if (r != VOCR_OK) { error = "could not start the OCR"; return false; }
    arenaBytes_ = need;
    mode_ = o.mode;
    ctxs_.assign(1, c);
    blob_ = blob;
    arena_ = arena;
    model_ = m;
    ctx_ = c;
    char b[96];
    snprintf(b, sizeof(b), "%u characters, %u KB", m->n_classes, (unsigned)(n / 1024));
    info_ = b;
    return true;
}

// Recognizes one line into out (cap bytes, NUL-terminated); a line too long for the model is read
// in pieces. With tk, also fills alts[0..*nAlts) with the characters the recognizer was unsure of.
// Runs on worker threads: no allocation, little stack.
static int runLine(vocr_ctx *c, const vocr_model *m, const uint8_t *grey, unsigned stride, const VocrLine &L,
                   char *out, size_t cap, const TopK *tk, RawAlt *alts, unsigned *nAlts) {
    out[0] = 0;
    if (nAlts) *nAlts = 0;
    size_t used = 0;
    int fmt = VOCR_GREY8 | (L.dark ? VOCR_INVERT : 0);
    unsigned pieces = 1;
    // the model squeezes anything wider than max_width after scaling to its height: split instead
    while ((unsigned long)((L.w + pieces - 1) / pieces) * m->height > (unsigned long)m->max_width * L.h && pieces < 8)
        pieces++;
    for (unsigned p = 0; p < pieces && used + 1 < cap; p++) {
        unsigned x0 = L.x + L.w * p / pieces, x1 = L.x + L.w * (p + 1) / pieces;
        vocr_image im;
        im.pixels = grey + (size_t)L.y * stride + x0;
        im.width = x1 - x0;
        im.height = L.h;
        im.stride = stride;
        im.format = fmt;
        vocr_extra ex;
        memset(&ex, 0, sizeof(ex));
        bool withAlts = tk && tk->logit && alts && nAlts &&
                        vocr_resized_width(m, im.width, im.height) / m->stride + 1 <= tk->cap;
        if (withAlts) {
            ex.topk = kTopK;
            ex.topk_logit = tk->logit;
            ex.topk_class = tk->cls;
            ex.cap_frames = tk->cap;
        }
        int r = vocr_run(c, &im, out + used, cap - used, withAlts ? &ex : nullptr);
        if (r != VOCR_OK && r != VOCR_E_TEXT) return r;
        size_t len = strlen(out + used);
        if (withAlts && r == VOCR_OK) {
            // redo the greedy CTC over the frames' argmax, to know which frame wrote each character
            unsigned last = 0;
            size_t at = used;
            bool same = true;
            for (unsigned f = 0; f < ex.frames && same; f++) {
                const uint16_t *tc = tk->cls + (size_t)f * kTopK;
                const float *tv = tk->logit + (size_t)f * kTopK;
                unsigned best = tc[0];
                if (best && best != last) {
                    unsigned n;
                    const char *s = vocr_class_text(m, best, &n);
                    if (at + n > used + len || memcmp(out + at, s, n)) { same = false; break; }
                    // the other guesses for this character, with their share of the frame's top K
                    float sum = 1.0f;
                    for (unsigned j = 1; j < kTopK; j++) sum += expf(tv[j] - tv[0]);
                    RawAlt a;
                    a.byte = (uint16_t)at;
                    a.len = (uint16_t)n;
                    a.n = 0;
                    for (unsigned j = 1; j < kTopK && a.n < 3; j++)
                        if (tc[j] && expf(tv[j] - tv[0]) / sum >= kAltMinShare) a.cls[a.n++] = tc[j];
                    if (a.n && *nAlts < kMaxAlts) alts[(*nAlts)++] = a;
                    at += n;
                }
                last = best;
            }
            if (!same || at != used + len) *nAlts = 0;  // couldn't line them up: no alternatives
        }
        used += len;
    }
    return VOCR_OK;
}

namespace {
enum { kLineCap = 512 };
struct LineJob {
    const vocr_model *model = nullptr;
    void **ctxs = nullptr;
    const uint8_t *grey = nullptr;
    unsigned stride = 0;
    const VocrLine *lines = nullptr;
    unsigned count = 0;
    char *text = nullptr;   // count x kLineCap
    int *rc = nullptr;
    const TopK *tk = nullptr;  // per worker
    RawAlt *alts = nullptr;    // count x kMaxAlts
    unsigned *nAlts = nullptr;
    const unsigned *order = nullptr;  // the lines in the order they are handed out
    std::atomic<unsigned> next{0}, worker{0};
};

void lineWorker(void *arg) {
    LineJob *j = (LineJob *)arg;
    unsigned w = j->worker.fetch_add(1);
    vocr_ctx *c = (vocr_ctx *)j->ctxs[w];
    for (unsigned k; (k = j->next.fetch_add(1)) < j->count;) {
        const unsigned i = j->order[k];
        j->rc[i] = runLine(c, j->model, j->grey, j->stride, j->lines[i], j->text + (size_t)i * kLineCap, kLineCap,
                           j->tk ? &j->tk[w] : nullptr, j->alts + (size_t)i * kMaxAlts, &j->nAlts[i]);
    }
}
}  // namespace

bool LocalOcr::read(const uint8_t *grey, unsigned w, unsigned h, std::vector<TextLine> &lines, std::string &error,
                    void (*progress)(unsigned, unsigned), unsigned rx, unsigned ry, unsigned rw, unsigned rh) {
    lines.clear();
    if (!blob_) { error = "no OCR model loaded"; return false; }
    if (!rw || !rh || rx + rw > w || ry + rh > h) { rx = ry = 0; rw = w; rh = h; }
    VocrLinesImage im;
    im.pixels = grey + (size_t)ry * w + rx;
    im.width = rw;
    im.height = rh;
    im.stride = w;
    im.bpp = VOCR_LINES_GREY;
    size_t la = vocr_lines_arena_bytes(rw, rh);
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
    const vocr_model *m = (const vocr_model *)model_;
    std::vector<VocrLine> todoLines;
    for (unsigned i : todo) {
        VocrLine L = set.lines[i];
        L.x += rx;  // region -> whole image
        L.y += ry;
        todoLines.push_back(L);
    }
    unsigned n = (unsigned)todoLines.size();
    // workers: one per line up to workers_, each with its own context (made once per load)
    unsigned workers = runner_ ? std::min<unsigned>(workers_, n) : 1;
    while (ctxs_.size() < workers) {
        vocr_ctx *c2 = (vocr_ctx *)alignedAlloc(sizeof(vocr_ctx));
        unsigned char *a2 = (unsigned char *)alignedAlloc(arenaBytes_);
        vocr_opts o;
        memset(&o, 0, sizeof(o));
        o.mode = mode_;
        o.tile_frames = tileFrames_;
        o.head_frames = headFrames_;
        if (!c2 || !a2 || vocr_init(c2, m, &o, a2, arenaBytes_) != VOCR_OK) break;  // fewer workers then
        ctxs_.push_back(c2);
    }
    // top-K buffers per worker (a few KB): only when someone will use the alternatives
    while (fixer_ && topk_.size() < ctxs_.size()) {
        TopK t;
        t.cap = m->max_width / m->stride + 8;
        t.logit = (float *)alignedAlloc((size_t)t.cap * kTopK * sizeof(float));
        t.cls = (uint16_t *)alignedAlloc((size_t)t.cap * kTopK * sizeof(uint16_t));
        if (!t.logit || !t.cls) t = TopK();
        topk_.push_back(t);
    }
    if (workers > ctxs_.size()) workers = (unsigned)ctxs_.size();
    if (workers < 1) workers = 1;
    lastWorkers_ = workers;
    std::vector<char> text((size_t)std::max(n, 1u) * kLineCap, 0);
    std::vector<int> rc(std::max(n, 1u), VOCR_E_ARG);
    std::vector<RawAlt> rawAlts(fixer_ ? (size_t)std::max(n, 1u) * kMaxAlts : 1);
    std::vector<unsigned> nAlts(std::max(n, 1u), 0);
    LineJob job;
    job.model = m;
    job.ctxs = ctxs_.data();
    job.grey = grey;
    job.stride = w;
    job.lines = todoLines.data();
    job.count = n;
    job.text = text.data();
    job.rc = rc.data();
    job.tk = fixer_ && topk_.size() >= workers ? (const TopK *)topk_.data() : nullptr;
    job.alts = rawAlts.data();
    job.nAlts = nAlts.data();
    // the longest lines first: the last one handed out is then a short one, and the workers finish
    // close together (with 2 workers and lines of 10, 10 and 4 characters: 10 | 10 + 4, not 10 + 10 | 4)
    std::vector<unsigned> order(std::max(n, 1u), 0);
    for (unsigned k = 0; k < n; k++) order[k] = k;
    std::stable_sort(order.begin(), order.begin() + n,
                     [&](unsigned a, unsigned b) { return todoLines[a].w * todoLines[b].h > todoLines[b].w * todoLines[a].h; });
    job.order = order.data();
    if (workers > 1) {
        if (progress) progress(0, 1);
        runner_(workers, lineWorker, &job);
    } else {
        for (unsigned k = 0; k < n; k++) {  // one at a time, with progress
            if (progress) progress(k, n);
            job.rc[k] = runLine((vocr_ctx *)ctxs_[0], m, grey, w, todoLines[k], job.text + (size_t)k * kLineCap, kLineCap,
                                job.tk, job.alts + (size_t)k * kMaxAlts, &job.nAlts[k]);
        }
    }
    for (unsigned k = 0; k < n; k++) {
        const VocrLine &L = todoLines[k];
        const char *t = job.text + (size_t)k * kLineCap;
#ifndef __3DS__
        if (getenv("LO_DEBUG")) fprintf(stderr, "run r=%d [%s]\n", rc[k], t);
#endif
        if (rc[k] != VOCR_OK || !t[0]) continue;
        TextLine line;
        line.text = t;
        if (fixer_ && nAlts[k]) {  // let the dictionary pick between the recognizer's guesses
            std::vector<OcrAlt> alts;
            for (unsigned a = 0; a < nAlts[k]; a++) {
                const RawAlt &ra = rawAlts[(size_t)k * kMaxAlts + a];
                OcrAlt oa;
                oa.byte = ra.byte;
                oa.len = ra.len;
                for (unsigned q = 0; q < ra.n; q++) {
                    unsigned len;
                    const char *s = vocr_class_text(m, ra.cls[q], &len);
                    oa.options.push_back(std::string(s, len));
                }
                alts.push_back(oa);
            }
#ifndef __3DS__
            if (getenv("LO_DEBUG"))
                for (const OcrAlt &oa : alts) {
                    fprintf(stderr, "alt @%u %s:", oa.byte, line.text.substr(oa.byte, oa.len).c_str());
                    for (const std::string &o : oa.options) fprintf(stderr, " %s", o.c_str());
                    fprintf(stderr, "\n");
                }
#endif
            fixer_(line.text, alts, fixerUser_);
        }
        line.paragraph = (int)k;  // each line its own block; textParagraphs() joins a text box's lines
        line.centerX = (L.x + L.w / 2.0f) / w;
        line.centerY = (L.y + L.h / 2.0f) / h;
        line.width = (float)L.w / w;
        line.height = (float)L.h / h;
        lines.push_back(line);
    }
    if (lines.empty()) { error = "no text found on the screen"; return false; }
    return true;
}

uint8_t *greyFromRGB(const uint8_t *rgb, unsigned w, unsigned h) {
    uint8_t *grey = (uint8_t *)malloc((size_t)w * h);
    if (!grey) return nullptr;
    for (size_t i = 0; i < (size_t)w * h; i++)
        grey[i] = (uint8_t)((77u * rgb[i * 3] + 150u * rgb[i * 3 + 1] + 29u * rgb[i * 3 + 2]) >> 8);
    return grey;
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

static unsigned japaneseChars(const std::vector<std::string> &paragraphs) {
    unsigned n = 0;
    for (const std::string &p : paragraphs)
        for (size_t i = 0; i < p.size();) {
            uint32_t cp = utf8Next(p, i);
            if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF)) n++;
        }
    return n;
}

bool localReadScreen(LocalOcr &ocr, const uint8_t *grey, unsigned w, unsigned h, const std::vector<TextBox> &boxes,
                     std::vector<std::string> &paragraphs, std::string &error,
                     void (*progress)(unsigned, unsigned), int *usedBox) {
    paragraphs.clear();
    if (usedBox) *usedBox = -1;
    std::vector<TextLine> lines;
    for (size_t bi = 0; bi < boxes.size() && bi < 2; bi++) {  // the best two boxes, then the whole screen
        const TextBox &b = boxes[bi];
        unsigned x0 = b.x > 2 ? b.x - 2 : 0, y0 = b.y > 2 ? b.y - 2 : 0;
        unsigned x1 = b.x + b.w + 2 < w ? b.x + b.w + 2 : w, y1 = b.y + b.h + 2 < h ? b.y + b.h + 2 : h;
        std::string e;
        if (ocr.read(grey, w, h, lines, e, progress, x0, y0, x1 - x0, y1 - y0)) {
            paragraphs = textParagraphs(lines);
            cleanParagraphs(paragraphs);
            if (japaneseChars(paragraphs) >= 2) {
                if (usedBox) *usedBox = (int)bi;
                return true;
            }
        }
    }
    if (!ocr.read(grey, w, h, lines, error, progress)) return false;  // the whole screen
    paragraphs = textParagraphs(lines);
    cleanParagraphs(paragraphs);
    if (paragraphs.empty()) { error = "no text found on the screen"; return false; }
    return true;
}

}  // namespace kt
