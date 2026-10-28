#include "core/local_ocr.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>

#include "core/kana.hpp"
#include "core/search.hpp"
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
    arenas_.clear();
    blobCap_ = arenaBytes_ = 0;
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

// Headroom in the model's memory, so another model file (VOCR5 and VOCR6 differ by a few KB) can
// take its place without a new allocation.
static const size_t kBlobSpare = 16 * 1024, kArenaSpare = 8 * 1024;

static bool readModelFile(Reader &file, unsigned char *blob, uint32_t n, std::string &error) {
    for (uint32_t off = 0; off < n;) {  // in pieces: SD reads of a few hundred KB at a time
        uint32_t piece = n - off > 256 * 1024 ? 256 * 1024 : n - off;
        if (!file.read(off, blob + off, piece)) { error = "could not read the OCR model"; return false; }
        off += piece;
    }
    return true;
}

bool LocalOcr::loadInner(Reader &file, std::string &error) {
    uint32_t n = file.size();
    if (n < 64 || n > 8 * 1024 * 1024) { error = "the OCR model file looks wrong"; return false; }
    blobCap_ = n + 32 + kBlobSpare;  // + the bytes VOCR's ARM kernel may read past the end
    unsigned char *blob = (unsigned char *)alignedAlloc(blobCap_);
    if (!blob) { error = "not enough memory for the OCR model"; return false; }
    if (!readModelFile(file, blob, n, error)) return false;
    vocr_model *m = (vocr_model *)alignedAlloc(sizeof(vocr_model));
    vocr_ctx *c = (vocr_ctx *)alignedAlloc(sizeof(vocr_ctx));
    if (!m || !c) { error = "not enough memory for the OCR model"; return false; }
    blob_ = blob;
    model_ = m;
    ctx_ = c;
    arena_ = nullptr;
    arenaBytes_ = 0;
    ctxs_.assign(1, c);
    arenas_.clear();
    if (!setUp(n, error)) {
        blob_ = nullptr;
        return false;
    }
    if (runner_) ensureWorkers(workers_);
    return true;
}

bool LocalOcr::reloadInPlace(Reader &file, std::string &error) {
    if (!blob_ || !model_) { error = "no model loaded"; return false; }
    const uint32_t n = file.size();
    if (n < 64 || (size_t)n + 32 > blobCap_) { error = "the other model doesn't fit"; return false; }
    // past this point the old model is gone: on a failure nothing is loaded
    if (!readModelFile(file, blob_, n, error) || !setUp(n, error)) {
        unload();
        return false;
    }
    return true;
}

// The model file is in blob_ (n bytes): parse and check it, then plan every context's working
// memory, in the arenas already there when they are big enough (a model swapped in place).
bool LocalOcr::setUp(uint32_t n, std::string &error) {
    vocr_model *m = (vocr_model *)model_;
    memset(blob_ + n, 0, 32);
    int r = vocr_load(m, blob_, n);
    // the CRC pass costs ~0.1 s on the 3DS: once per file per session is enough
    static uint32_t checkedSize[2] = {0, 0};  // the last two model files loaded
    if (r == VOCR_OK && checkedSize[0] != n && checkedSize[1] != n) {
        if (vocr_check_crc(blob_, n) != VOCR_OK) r = VOCR_E_FORMAT;
        else checkedSize[1] = checkedSize[0], checkedSize[0] = n;
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
    gate_ = speed_ && vocr_has_gate(m);
    twoLevel_ = speed_ && o.mode == VOCR_MODE_W8I && vocr_has_two_level(m);
    o.head_gate = gate_ ? VOCR_HEAD_GATE_ON : VOCR_HEAD_GATE_OFF;
    o.two_level = twoLevel_ ? 1 : 0;
    const size_t need = vocr_arena_bytes(m, &o);
    if (!need) { error = "could not start the OCR"; return false; }
    if (!arena_) {  // the first load: the main context's arena, with room for another model
        arenaBytes_ = need + kArenaSpare;
        arena_ = (unsigned char *)alignedAlloc(arenaBytes_);
        if (!arena_) { error = "not enough memory for the OCR"; return false; }
        arenas_.assign(1, arena_);
    }
    if (need > arenaBytes_) { error = "the other model doesn't fit"; return false; }
    for (unsigned k = 0; k < topk_.size(); k++)
        if (topk_[k].logit && topk_[k].cap < m->max_width / m->stride + 8) topk_[k] = TopK();  // too small now: none
    for (size_t k = 0; k < ctxs_.size(); k++)  // every worker's context again, in its own arena
        if (vocr_init((vocr_ctx *)ctxs_[k], m, &o, arenas_[k], arenaBytes_) != VOCR_OK) {
            if (k == 0) { error = "could not start the OCR"; return false; }
            ctxs_.resize(k);  // fewer workers then
            arenas_.resize(k);
            break;
        }
    mode_ = o.mode;
    char b[96];
    snprintf(b, sizeof(b), "%u characters, %u KB%s%s", m->n_classes, (unsigned)(n / 1024), gate_ ? ", head gate" : "",
             twoLevel_ ? ", two-level head" : "");
    info_ = b;
    return true;
}

// A context (and top-K buffers) for each of n workers, as far as memory allows. Made right after
// the model, so they sit next to it instead of in the gaps a read's pictures leave.
void LocalOcr::ensureWorkers(unsigned n) {
    const vocr_model *m = (const vocr_model *)model_;
    if (!m) return;
    while (ctxs_.size() < n) {
        vocr_ctx *c2 = (vocr_ctx *)alignedAlloc(sizeof(vocr_ctx));
        unsigned char *a2 = c2 ? (unsigned char *)alignedAlloc(arenaBytes_) : nullptr;
        vocr_opts o;
        memset(&o, 0, sizeof(o));
        o.mode = mode_;
        o.tile_frames = tileFrames_;
        o.head_frames = headFrames_;
        o.head_gate = gate_ ? VOCR_HEAD_GATE_ON : VOCR_HEAD_GATE_OFF;
        o.two_level = twoLevel_ ? 1 : 0;
        if (!c2 || !a2 || vocr_init(c2, m, &o, a2, arenaBytes_) != VOCR_OK) break;  // fewer workers then
        ctxs_.push_back(c2);
        arenas_.push_back(a2);
    }
    while (topk_.size() < ctxs_.size()) {  // a few KB each
        TopK t;
        t.cap = m->max_width / m->stride + 8;
        t.logit = (float *)alignedAlloc((size_t)t.cap * kTopK * sizeof(float));
        t.cls = (uint16_t *)alignedAlloc((size_t)t.cap * kTopK * sizeof(uint16_t));
        if (!t.logit || !t.cls) t = TopK();
        topk_.push_back(t);
    }
}

// Recognizes one line into out (cap bytes, NUL-terminated); a line too long for the model is read
// in pieces. With tk, also fills alts[0..*nAlts) with the characters the recognizer was unsure of.
// Runs on worker threads: no allocation, little stack.
static int runLine(vocr_ctx *c, const vocr_model *m, const uint8_t *grey, unsigned stride, const VocrLine &L,
                   char *out, size_t cap, const TopK *tk, RawAlt *alts, unsigned *nAlts, float *conf) {
    out[0] = 0;
    if (nAlts) *nAlts = 0;
    float shareSum = 0;
    unsigned shareN = 0;
    bool measured = true;
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
        if (!withAlts) measured = false;
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
                    shareSum += 1.0f / sum;  // the character's share of its frame's top guesses
                    shareN++;
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
            if (!same || at != used + len) { *nAlts = 0; measured = false; }  // couldn't line them up
        }
        used += len;
    }
    if (conf) *conf = measured && shareN ? shareSum / shareN : -1;
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
    float *conf = nullptr;
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
                           j->tk ? &j->tk[w] : nullptr, j->alts + (size_t)i * kMaxAlts, &j->nAlts[i], &j->conf[i]);
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
    std::vector<VocrLine> todoLines;
    for (unsigned i : todo) {
        VocrLine L = set.lines[i];
        L.x += rx;  // region -> whole image
        L.y += ry;
        todoLines.push_back(L);
    }
    return recognize(grey, w, h, todoLines.data(), (unsigned)todoLines.size(), lines, error, progress);
}

bool LocalOcr::findLines(const uint8_t *grey, unsigned w, unsigned h, unsigned rx, unsigned ry, unsigned rw,
                         unsigned rh, std::vector<LineBox> &out) {
    out.clear();
    if (!grey || !rw || !rh || rx + rw > w || ry + rh > h) return false;
    VocrLinesImage im;
    im.pixels = grey + (size_t)ry * w + rx;
    im.width = rw;
    im.height = rh;
    im.stride = w;
    im.bpp = VOCR_LINES_GREY;
    size_t la = vocr_lines_arena_bytes(rw, rh);
    void *larena = malloc(la);
    if (!larena) return false;
    VocrLineSet set;
    int r = vocr_find_lines(&im, VOCR_LINES_GRAD, larena, la, &set);
    if (r == VOCR_LINES_OK) vocr_refine_lines(&im, VOCR_REFINE_DEFAULT, larena, la, &set, nullptr);
    free(larena);
    if (r != VOCR_LINES_OK) return false;
    for (unsigned i = 0; i < set.count; i++) {
        LineBox b;
        b.x = set.lines[i].x + rx;
        b.y = set.lines[i].y + ry;
        b.w = set.lines[i].w;
        b.h = set.lines[i].h;
        b.dark = set.lines[i].dark != 0;
        out.push_back(b);
    }
    return true;
}

bool LocalOcr::readLines(const uint8_t *grey, unsigned w, unsigned h, const std::vector<LineBox> &boxes,
                         std::vector<TextLine> &lines, std::string &error, void (*progress)(unsigned, unsigned)) {
    lines.clear();
    if (!blob_) { error = "no OCR model loaded"; return false; }
    std::vector<VocrLine> v;
    for (const LineBox &b : boxes) {
        if (!b.w || !b.h || b.x + b.w > w || b.y + b.h > h) continue;
        VocrLine L;
        memset(&L, 0, sizeof L);
        L.x = b.x;
        L.y = b.y;
        L.w = b.w;
        L.h = b.h;
        L.dark = b.dark ? 1 : 0;
        v.push_back(L);
    }
    return recognize(grey, w, h, v.data(), (unsigned)v.size(), lines, error, progress);
}

bool LocalOcr::recognize(const uint8_t *grey, unsigned w, unsigned h, const void *linesIn, unsigned n,
                         std::vector<TextLine> &lines, std::string &error, void (*progress)(unsigned, unsigned)) {
    lines.clear();
    const vocr_model *m = (const vocr_model *)model_;
    const VocrLine *todoLines = (const VocrLine *)linesIn;
    // workers: one per line up to workers_, each with its own context (made at the load)
    unsigned workers = runner_ ? std::min<unsigned>(workers_, n) : 1;
    ensureWorkers(workers);
    if (workers > ctxs_.size()) workers = (unsigned)ctxs_.size();
    if (workers < 1) workers = 1;
    lastWorkers_ = workers;
    std::vector<char> text((size_t)std::max(n, 1u) * kLineCap, 0);
    std::vector<int> rc(std::max(n, 1u), VOCR_E_ARG);
    std::vector<RawAlt> rawAlts(fixer_ ? (size_t)std::max(n, 1u) * kMaxAlts : 1);
    std::vector<unsigned> nAlts(std::max(n, 1u), 0);
    std::vector<float> conf(std::max(n, 1u), -1.0f);
    LineJob job;
    job.model = m;
    job.ctxs = ctxs_.data();
    job.grey = grey;
    job.stride = w;
    job.lines = todoLines;
    job.count = n;
    job.text = text.data();
    job.rc = rc.data();
    bool tkOk = fixer_ && topk_.size() >= workers;
    for (unsigned k = 0; tkOk && k < workers; k++) tkOk = topk_[k].logit != nullptr;
    job.tk = tkOk ? (const TopK *)topk_.data() : nullptr;
    job.alts = rawAlts.data();
    job.nAlts = nAlts.data();
    job.conf = conf.data();
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
                                job.tk, job.alts + (size_t)k * kMaxAlts, &job.nAlts[k], &job.conf[k]);
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
        line.confidence = conf[k];
        line.dark = L.dark != 0;
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

static const unsigned kMinScreenChars = 4;  // a line read without a text box: this much Japanese at least

static unsigned japaneseChars(const std::vector<std::string> &paragraphs) {
    unsigned n = 0;
    for (const std::string &p : paragraphs)
        for (size_t i = 0; i < p.size();) {
            uint32_t cp = utf8Next(p, i);
            if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF)) n++;
        }
    return n;
}

namespace {
struct BoxRead {
    const TextBox *box;
    int index;
    std::vector<TextLine> lines;
};

// Reads each text box (a box mostly inside one read already is skipped), then gives each line
// read by two overlapping boxes to the box that read the most of it.
void readBoxLines(LocalOcr &ocr, const uint8_t *grey, unsigned w, unsigned h, const std::vector<TextBox> &boxes,
                  void (*progress)(unsigned, unsigned), std::vector<BoxRead> &reads) {
    typedef BoxRead Read;
    std::vector<TextLine> lines;
    reads.clear();
    for (size_t bi = 0; bi < boxes.size(); bi++) {
        const TextBox &b = boxes[bi];
        bool inside = false;  // mostly within a box read already (a box inside a box)
        for (const Read &g : reads) {
            const TextBox &o = *g.box;
            unsigned ix = std::min(b.x + b.w, o.x + o.w) > std::max(b.x, o.x) ? std::min(b.x + b.w, o.x + o.w) - std::max(b.x, o.x) : 0;
            unsigned iy = std::min(b.y + b.h, o.y + o.h) > std::max(b.y, o.y) ? std::min(b.y + b.h, o.y + o.h) - std::max(b.y, o.y) : 0;
            if ((uint64_t)ix * iy * 2 > (uint64_t)std::min(b.w * b.h, o.w * o.h)) inside = true;
        }
        if (inside) continue;
        unsigned x0 = b.x > 2 ? b.x - 2 : 0, y0 = b.y > 2 ? b.y - 2 : 0;
        unsigned x1 = b.x + b.w + 2 < w ? b.x + b.w + 2 : w, y1 = b.y + b.h + 2 < h ? b.y + b.h + 2 : h;
        if (b.shaded && b.inkW && b.inkH) {  // a rounded shaded box: just around the letters, not its frame
            const unsigned m = 6;
            x0 = std::max(x0, b.inkX > m ? b.inkX - m : 0);
            y0 = std::max(y0, b.inkY > m ? b.inkY - m : 0);
            x1 = std::min(x1, b.inkX + b.inkW + m);
            y1 = std::min(y1, b.inkY + b.inkH + m);
        }
        std::string e;
        if (!ocr.read(grey, w, h, lines, e, progress, x0, y0, x1 - x0, y1 - y0)) continue;
        reads.push_back({&b, (int)bi, lines});
    }
    // Boxes can overlap (a speech bubble and the light ground behind it can come out as two), so
    // a line can be read twice: once whole, once cut at the edge of the other box (「たえられる」
    // for 「きたえられる」). Each line goes to the box that read the most of it.
    auto overlap = [](const TextLine &a, const TextLine &b) {
        const float ix = std::min(a.centerX + a.width / 2, b.centerX + b.width / 2) -
                         std::max(a.centerX - a.width / 2, b.centerX - b.width / 2);
        const float iy = std::min(a.centerY + a.height / 2, b.centerY + b.height / 2) -
                         std::max(a.centerY - a.height / 2, b.centerY - b.height / 2);
        return ix > 0.5f * std::min(a.width, b.width) && iy > 0.5f * std::min(a.height, b.height);
    };
    // A line mostly inside another box that box read without finding it is a cut of something
    // that box left out on purpose: the furigana over its first row (「ほんき」), seen by a box
    // that clips the bubble's edge.
    auto insideBox = [w, h](const TextLine &l, const TextBox &b) {  // lines are in fractions of the screen
        const float bx0 = (float)b.x / w, bx1 = (float)(b.x + b.w) / w, by0 = (float)b.y / h, by1 = (float)(b.y + b.h) / h;
        const float ix = std::min(l.centerX + l.width / 2, bx1) - std::max(l.centerX - l.width / 2, bx0);
        const float iy = std::min(l.centerY + l.height / 2, by1) - std::max(l.centerY - l.height / 2, by0);
        return ix > 0.5f * l.width && iy > 0.5f * l.height;
    };
    std::vector<std::vector<TextLine>> read0;  // as read, before any line is given away
    for (const Read &rd : reads) read0.push_back(rd.lines);
    for (size_t r = 0; r < reads.size(); r++)
        for (size_t q = 0; q < reads.size(); q++) {
            if (q == r) continue;
            std::vector<TextLine> &mine = reads[r].lines;
            const std::vector<TextLine> &theirs = read0[q];
            mine.erase(std::remove_if(mine.begin(), mine.end(), [&](const TextLine &l) {
                           bool seen = false;
                           for (const TextLine &o : theirs) {
                               if (!overlap(l, o)) continue;
                               seen = true;  // theirs is wider (on a tie: the earlier box keeps it)
                               if (o.width > l.width || (o.width == l.width && q < r)) return true;
                           }
                           return !seen && !theirs.empty() && insideBox(l, *reads[q].box);
                       }), mine.end());
        }
}
}  // namespace

bool localReadScreen(LocalOcr &ocr, const uint8_t *grey, unsigned w, unsigned h, const std::vector<TextBox> &boxes,
                     std::vector<std::string> &paragraphs, std::string &error,
                     void (*progress)(unsigned, unsigned), int *usedBox) {
    paragraphs.clear();
    if (usedBox) *usedBox = -1;
    std::vector<TextLine> lines;
    // every box with Japanese in it (a screen can have two speech bubbles), in reading order
    struct Got { const TextBox *box; std::vector<std::string> paras; int index; };
    std::vector<Got> got;
    typedef BoxRead Read;
    std::vector<Read> reads;
    readBoxLines(ocr, grey, w, h, boxes, progress, reads);
    for (const Read &rd : reads) {
        std::vector<std::string> ps = textParagraphs(rd.lines, true);
        cleanParagraphs(ps);
        if (japaneseChars(ps) >= 2) got.push_back({rd.box, ps, rd.index});
    }
    if (!got.empty()) {
        std::stable_sort(got.begin(), got.end(), [](const Got &a, const Got &b) {
            // top to bottom; side by side (overlapping rows): left to right
            bool sameRow = a.box->y < b.box->y + b.box->h && b.box->y < a.box->y + a.box->h;
            return sameRow ? a.box->x < b.box->x : a.box->y < b.box->y;
        });
        for (const Got &g : got) paragraphs.insert(paragraphs.end(), g.paras.begin(), g.paras.end());
        // a scrap from another box (「こー」) goes when there is real text, as within a box
        unsigned best = 0;
        for (const std::string &p : paragraphs) best = std::max(best, japaneseChars({p}));
        if (best >= 6)
            paragraphs.erase(std::remove_if(paragraphs.begin(), paragraphs.end(),
                                            [](const std::string &p) { return japaneseChars({p}) < 3; }),
                             paragraphs.end());
        if (usedBox) *usedBox = got[0].index;
        return true;
    }
    if (!ocr.read(grey, w, h, lines, error, progress)) return false;  // the whole screen
    // With no text box, the scenery can look like a few kana (「ンりツ」 in a face). A short line
    // stays only next to a real one (4+ characters) of about its size: a title over the text
    // (「ご注意」), a dialogue's short first or last row, a speaker's name.
    std::vector<char> real(lines.size());
    for (size_t i = 0; i < lines.size(); i++) real[i] = japaneseChars({lines[i].text}) >= kMinScreenChars;
    std::vector<TextLine> kept;
    for (size_t i = 0; i < lines.size(); i++) {
        bool keep = real[i];
        const TextLine &a = lines[i];
        for (size_t j = 0; j < lines.size() && !keep; j++) {
            if (!real[j] || a.height <= 0) continue;
            const TextLine &b = lines[j];
            const float ratio = a.height / b.height;
            const float gapY = a.centerY > b.centerY ? a.centerY - b.centerY : b.centerY - a.centerY;
            const float aL = a.centerX - a.width / 2, aR = a.centerX + a.width / 2;
            const float bL = b.centerX - b.width / 2, bR = b.centerX + b.width / 2;
            const bool overlapX = aL < bR + 0.05f && bL < aR + 0.05f;
            keep = ratio > 0.6f && ratio < 1.7f && gapY < 2.6f * std::max(a.height, b.height) && overlapX;
        }
        if (keep) kept.push_back(a);
    }
    paragraphs = textParagraphs(kept, true);
    cleanParagraphs(paragraphs);
    if (paragraphs.empty()) { error = "no text found on the screen"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Option 1: text boxes as a hint, every line scored
// ---------------------------------------------------------------------------
namespace {

unsigned japaneseIn(const std::string &t) {
    unsigned n = 0;
    for (size_t i = 0; i < t.size();) {
        uint32_t cp = utf8Next(t, i);
        n += (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF);
    }
    return n;
}

// Japanese characters of t that are part of a dictionary word of two characters or more (greedy
// longest match, as the lookup screen splits text).
unsigned dictCovered(Dictionary &dict, const std::string &t) {
    unsigned covered = 0;
    for (const std::string &c : chunkText(dict, t)) {
        unsigned n = 0, jp = 0;
        for (size_t i = 0; i < c.size();) {
            uint32_t cp = utf8Next(c, i);
            n++;
            jp += (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF);
        }
        if (n >= 2) covered += jp;
    }
    return covered;
}

// At least 30% of the line's Japanese is in dictionary words of two characters or more.
bool checkDict(Dictionary &dict, const std::string &t, unsigned jp, ScoredReadStats *st) {
    const uint64_t t0 = st && st->clock ? st->clock() : 0;
    const bool ok = dictCovered(dict, t) * 10 >= jp * 3;
    if (st) {
        st->dictChecks++;
        if (st->clock) st->dictMs += (uint32_t)(st->clock() - t0);
    }
    return ok;
}

// Does the line read like text? A line from a text box is trusted unless the recognizer was unsure
// of it and the dictionary finds no words in it (a sprite, an icon: 「凛」, 「ート」). A line found
// out in the scene must earn its place: the recognizer sure of it, or words in it.
bool keepLine(const TextLine &l, Dictionary *dict, bool fromBox, ScoredReadStats *st) {
    const unsigned jp = japaneseIn(l.text);
    const float c = l.confidence;
    if (fromBox) {
        if (c < 0 || c >= 0.8f) return true;
        if (jp == 0) return false;
        if (!dict) return c >= 0.6f;
        return checkDict(*dict, l.text, jp, st);
    }
    if (jp < 2) return false;  // a single character out in the scene is noise far more often than text
    if (c >= 0.93f) return true;
    if (c >= 0 && c < 0.75f) return false;
    if (!dict) return c >= 0.85f;
    return checkDict(*dict, l.text, jp, st);
}

bool boxesOverlap(const LocalOcr::LineBox &a, const LocalOcr::LineBox &b, float frac) {
    const int ix = std::min<int>(a.x + a.w, b.x + b.w) - std::max<int>(a.x, b.x);
    const int iy = std::min<int>(a.y + a.h, b.y + b.h) - std::max<int>(a.y, b.y);
    if (ix <= 0 || iy <= 0) return false;
    return (float)ix * iy > frac * std::min(a.w * a.h, b.w * b.h);
}

bool lineOverlaps(const TextLine &l, const LocalOcr::LineBox &b, unsigned w, unsigned h) {
    LocalOcr::LineBox a;
    a.x = (unsigned)std::max(0.0f, (l.centerX - l.width / 2) * w);
    a.y = (unsigned)std::max(0.0f, (l.centerY - l.height / 2) * h);
    a.w = (unsigned)(l.width * w) + 1;
    a.h = (unsigned)(l.height * h) + 1;
    return boxesOverlap(a, b, 0.5f);
}

}  // namespace

float textLikeness(const uint8_t *grey, unsigned w, unsigned h, const LocalOcr::LineBox &b) {
    if (!b.w || !b.h || b.x + b.w > w || b.y + b.h > h) return -100;
    // Otsu threshold over a 64-bin histogram (4 grey levels a bin)
    unsigned hist[64] = {0}, h256[256] = {0};
    for (unsigned y = 0; y < b.h; y++) {
        const uint8_t *row = grey + (size_t)(b.y + y) * w + b.x;
        for (unsigned x = 0; x < b.w; x++) { hist[row[x] >> 2]++; h256[row[x]]++; }
    }
    const float total = (float)b.w * b.h;
    float best = -1, sumAll = 0;
    unsigned thr = 128;
    for (unsigned i = 0; i < 64; i++) sumAll += i * (float)hist[i];
    float w0 = 0, sum0 = 0;
    for (unsigned t = 1; t < 64; t++) {
        w0 += hist[t - 1];
        sum0 += (t - 1) * (float)hist[t - 1];
        const float w1 = total - w0;
        if (w0 < 1 || w1 < 1) continue;
        const float m0 = sum0 / w0, m1 = (sumAll - sum0) / w1;
        const float sc = w0 * w1 * (m0 - m1) * (m0 - m1);
        if (sc > best) { best = sc; thr = t * 4; }
    }
    // ink on the text's side of the threshold; columns with ink and their runs; rows with ink
    unsigned ink = 0, runs = 0, inkRows = 0, edges = 0;
    std::vector<uint8_t> colInk(b.w, 0);
    for (unsigned y = 0; y < b.h; y++) {
        const uint8_t *row = grey + (size_t)(b.y + y) * w + b.x;
        unsigned rowInk = 0;
        for (unsigned x = 0; x < b.w; x++) {
            const bool on = b.dark ? row[x] < thr : row[x] > thr;
            rowInk += on;
            if (on) colInk[x] = 1;
            if (x && abs((int)row[x] - (int)row[x - 1]) > 40) edges++;
        }
        ink += rowInk;
        inkRows += rowInk * 50 > b.w;  // more than 2% of the row
    }
    for (unsigned x = 0; x < b.w; x++) runs += colInk[x] && (x == 0 || !colInk[x - 1]);
    // contrast: 95th minus 5th percentile
    unsigned acc = 0, p5 = 0, p95 = 255;
    bool got5 = false;
    for (unsigned v = 0; v < 256; v++) {
        acc += h256[v];
        if (!got5 && acc >= total * 0.05f) { p5 = v; got5 = true; }
        if (acc >= total * 0.95f) { p95 = v; break; }
    }
    const float inkf = ink / total, runsPerH = (float)runs * b.h / b.w, rowBand = (float)inkRows / b.h;
    const float edge = b.w > 1 ? (float)edges / ((b.w - 1) * (float)b.h) : 0, contrast = (float)(p95 - p5);
    const float aspect = (float)b.w / b.h;
    // weights fitted on 1,534 candidate lines from 170 labelled screens (logistic regression)
    return -1.121f + 1.3619f * runsPerH + 5.4859f * edge + 0.0065f * contrast - 1.075f * inkf - 2.7452f * rowBand +
           0.0088f * aspect;
}

bool readScreenScored(LocalOcr &ocr, Dictionary *dict, const uint8_t *grey, unsigned w, unsigned h,
                      const std::vector<TextBox> &boxes, std::vector<std::string> &paragraphs, std::string &error,
                      void (*progress)(unsigned, unsigned), ScoredReadStats *stats) {
    ScoredReadStats local;
    ScoredReadStats &st = stats ? *stats : local;
    uint64_t (*clock)() = st.clock;
    st = ScoredReadStats();
    st.clock = clock;
    auto now = [clock]() -> uint64_t { return clock ? clock() : 0; };
    const uint64_t tBox = now();
    paragraphs.clear();
    if (dict && !dict->isOpen()) dict = nullptr;
    // 1. the text boxes, each line scored; a line the recognizer was unsure of is "weak": it gives
    //    way to a line the bands read in the same place
    std::vector<BoxRead> reads;
    readBoxLines(ocr, grey, w, h, boxes, progress, reads);
    st.boxMs = (uint32_t)(now() - tBox);
    struct Block { float y, x; std::vector<std::string> paras; };
    std::vector<Block> blocks;
    auto strong = [](const TextLine &l) { return l.confidence < 0 || l.confidence >= 0.85f; };
    std::vector<std::vector<TextLine>> boxKept(reads.size());
    std::vector<TextLine> strongBoxLines;
    unsigned goodChars = 0;
    for (size_t r = 0; r < reads.size(); r++)
        for (const TextLine &l : reads[r].lines) {
            st.boxLines++;
            if (!keepLine(l, dict, true, &st)) { st.boxDropped++; continue; }
            boxKept[r].push_back(l);
            if (strong(l)) {
                goodChars += japaneseIn(l.text);
                strongBoxLines.push_back(l);
            }
        }
    std::vector<TextLine> bandKept;
    // 2. no text a reader would want from the boxes: search the screen in bands
    if (goodChars < 4) {
        st.usedBands = true;
        const uint64_t tFind = now();
        const unsigned bandH = std::min(60u, h), step = 30;
        std::vector<LocalOcr::LineBox> cands, found;
        std::vector<char> cut;  // the line touches the edge of its band: the band may have cut it
        for (unsigned y0 = 0;; y0 += step) {
            if (y0 + bandH > h) y0 = h - bandH;
            LocalOcr::findLines(grey, w, h, 0, y0, w, bandH, found);
            for (const LocalOcr::LineBox &b : found) {
                // a line, not a block of scenery: text-sized, wider than tall
                if (b.h < 9 || b.h > 48 || b.w < 16 || b.w * 2 < b.h * 3) continue;
                const bool bCut = (b.y <= y0 && y0 > 0) || (b.y + b.h >= y0 + bandH && y0 + bandH < h);
                bool dup = false;
                for (size_t k = 0; k < cands.size(); k++)
                    if (boxesOverlap(b, cands[k], 0.6f)) {
                        const unsigned lo = std::min(b.h, cands[k].h), hi = std::max(b.h, cands[k].h);
                        if (hi * 4 > lo * 5) continue;  // much taller or shorter: both are read, the surer kept
                        // the same line from two bands: the one its band didn't cut, else the tighter
                        if ((cut[k] && !bCut) || (cut[k] == bCut && b.h < cands[k].h)) {
                            cands[k] = b;
                            cut[k] = bCut;
                        }
                        dup = true;
                        break;
                    }
                if (!dup) {
                    cands.push_back(b);
                    cut.push_back(bCut);
                }
            }
            if (y0 + bandH >= h) break;
        }
        // furigana: a line much shorter than one right below it, over it (the reading of its kanji)
        std::vector<char> ruby(cands.size(), 0);
        for (size_t i = 0; i < cands.size(); i++)
            for (size_t j = 0; j < cands.size() && !ruby[i]; j++) {
                const LocalOcr::LineBox &r = cands[i], &t = cands[j];
                if (i == j || r.h * 16 > t.h * 10) continue;  // not under 5/8 of its height
                const int gap = (int)t.y - (int)(r.y + r.h);
                const bool above = gap >= -(int)r.h / 2 && gap <= (int)t.h / 2;
                const bool overX = r.x + r.w > t.x && t.x + t.w > r.x;
                ruby[i] = above && overX;
            }
        {
            size_t k = 0;
            for (size_t i = 0; i < cands.size(); i++)
                if (!ruby[i]) cands[k++] = cands[i];
            cands.resize(k);
        }
        st.bandCandidates = (unsigned)cands.size();
        // not what a box already read well
        cands.erase(std::remove_if(cands.begin(), cands.end(), [&](const LocalOcr::LineBox &b) {
                        for (const TextLine &l : strongBoxLines)
                            if (lineOverlaps(l, b, w, h)) return true;
                        return false;
                    }), cands.end());
        // the likeliest first, a few of them
        std::vector<std::pair<float, LocalOcr::LineBox>> ranked;
        for (const LocalOcr::LineBox &b : cands) {
            const float s = textLikeness(grey, w, h, b);
            if (s > -3.0f) ranked.push_back({s, b});
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const std::pair<float, LocalOcr::LineBox> &a, const std::pair<float, LocalOcr::LineBox> &b) {
                             return a.first > b.first;
                         });
        const size_t kMaxRead = 8;
        std::vector<LocalOcr::LineBox> toRead;
        for (size_t i = 0; i < ranked.size() && i < kMaxRead; i++) toRead.push_back(ranked[i].second);
        std::vector<TextLine> lines, kept;
        std::string e;
        const uint64_t tRead = now();
        st.bandFindMs = (uint32_t)(tRead - tFind);
        // a line found twice at different heights (a tight box and a taller one): the likelier box
        // first; the other only if that one didn't read clearly
        std::vector<LocalOcr::LineBox> first, second;
        for (size_t i = 0; i < toRead.size(); i++) {
            bool dup = false;
            for (size_t j = 0; j < i && !dup; j++) dup = boxesOverlap(toRead[i], toRead[j], 0.6f);
            (dup ? second : first).push_back(toRead[i]);
        }
        auto readKept = [&](const std::vector<LocalOcr::LineBox> &boxes) {
            if (boxes.empty() || !ocr.readLines(grey, w, h, boxes, lines, e, progress)) return;
            for (const TextLine &l : lines)
                if (keepLine(l, dict, false, &st)) kept.push_back(l);
        };
        readKept(first);
        second.erase(std::remove_if(second.begin(), second.end(), [&](const LocalOcr::LineBox &b) {
                         for (const TextLine &l : kept)
                             if (l.confidence >= 0.93f && lineOverlaps(l, b, w, h)) return true;
                         return false;
                     }), second.end());
        readKept(second);
        st.bandRead = (unsigned)(first.size() + second.size());
        st.bandReadMs = (uint32_t)(now() - tRead);
        // two readings of one line (a tight box and a taller one): the one the recognizer was surer of
        {
            auto box = [w, h](const TextLine &l) {
                LocalOcr::LineBox b;
                b.x = (unsigned)std::max(0.0f, (l.centerX - l.width / 2) * w);
                b.y = (unsigned)std::max(0.0f, (l.centerY - l.height / 2) * h);
                b.w = (unsigned)(l.width * w) + 1;
                b.h = (unsigned)(l.height * h) + 1;
                return b;
            };
            std::vector<char> drop(kept.size(), 0);
            for (size_t i = 0; i < kept.size(); i++)
                for (size_t j = 0; j < kept.size(); j++)
                    if (i != j && !drop[j] && boxesOverlap(box(kept[i]), box(kept[j]), 0.6f) &&
                        (kept[j].confidence > kept[i].confidence ||
                         (kept[j].confidence == kept[i].confidence && j < i)))
                        drop[i] = 1;
            size_t k = 0;
            for (size_t i = 0; i < kept.size(); i++)
                if (!drop[i]) kept[k++] = kept[i];
            kept.resize(k);
        }
        st.bandKept = (unsigned)kept.size();
        bandKept = kept;
        // top to bottom, then left to right, so lines of one block come together
        std::stable_sort(kept.begin(), kept.end(), [](const TextLine &a, const TextLine &b) {
            if (fabsf(a.centerY - b.centerY) > 0.5f * std::min(a.height, b.height)) return a.centerY < b.centerY;
            return a.centerX < b.centerX;
        });
        for (size_t i = 0; i < kept.size(); i++) kept[i].paragraph = (int)i;
        std::vector<std::string> ps = textParagraphs(kept, true);
        cleanParagraphs(ps);
        if (!ps.empty() && japaneseChars(ps) >= 2) {
            float y = kept.empty() ? 0 : kept[0].centerY * h, x = kept.empty() ? 0 : kept[0].centerX * w;
            blocks.push_back({y, x, ps});
        }
    }
    // the boxes' text, less weak lines the bands read better
    for (size_t r = 0; r < reads.size(); r++) {
        std::vector<TextLine> kept;
        for (const TextLine &l : boxKept[r]) {
            bool replaced = false;
            if (!strong(l))
                for (const TextLine &b : bandKept) {
                    LocalOcr::LineBox bb;
                    bb.x = (unsigned)std::max(0.0f, (b.centerX - b.width / 2) * w);
                    bb.y = (unsigned)std::max(0.0f, (b.centerY - b.height / 2) * h);
                    bb.w = (unsigned)(b.width * w) + 1;
                    bb.h = (unsigned)(b.height * h) + 1;
                    replaced = replaced || lineOverlaps(l, bb, w, h);
                }
            if (!replaced) kept.push_back(l);
        }
        std::vector<std::string> ps = textParagraphs(kept, true);
        cleanParagraphs(ps);
        if (japaneseChars(ps) >= 2) blocks.push_back({(float)reads[r].box->y, (float)reads[r].box->x, ps});
    }
    std::stable_sort(blocks.begin(), blocks.end(), [](const Block &a, const Block &b) { return a.y < b.y; });
    for (const Block &b : blocks) paragraphs.insert(paragraphs.end(), b.paras.begin(), b.paras.end());
    // a scrap (「こー」) goes when there is real text
    unsigned best = 0;
    for (const std::string &p : paragraphs) best = std::max(best, japaneseChars({p}));
    if (best >= 6)
        paragraphs.erase(std::remove_if(paragraphs.begin(), paragraphs.end(),
                                        [](const std::string &p) { return japaneseChars({p}) < 3; }),
                         paragraphs.end());
    if (paragraphs.empty()) { error = "no text found on the screen"; return false; }
    return true;
}


// ---------------------------------------------------------------------------
// The text spotter's reader
// ---------------------------------------------------------------------------
static unsigned kanaIn(const std::string &t) {
    unsigned n = 0;
    for (size_t i = 0; i < t.size();) {
        const uint32_t cp = utf8Next(t, i);
        n += cp >= 0x3041 && cp <= 0x30FF;
    }
    return n;
}

// How far each spotter box is widened on both sides, in % of its height: the spotter stops short of a
// faint glyph at a line's end (ー, …, ！), which the recognizer then loses (35%: best on the labelled
// screens). A variable for the tests.
unsigned g_spotGrowPct = 35;

// Kana and the marks around them only (a line of furigana), at least one kana.
static bool kanaOnly(const std::string &t) {
    unsigned kana = 0;
    for (size_t i = 0; i < t.size();) {
        const uint32_t cp = utf8Next(t, i);
        if ((cp >= 0x3041 && cp <= 0x30FF) || cp == 0x30FC) kana++;
        else if (cp >= 0x3400 && cp <= 0x9FFF) return false;  // a kanji: text, not its reading
        else if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return false;
    }
    return kana > 0;
}

bool readScreenSpot(LocalOcr &ocr, const TextSpotter &spot, Dictionary *dict, const uint8_t *grey, unsigned w,
                    unsigned h, std::vector<std::string> &paragraphs, std::string &error,
                    void (*progress)(unsigned i, unsigned n), SpotReadStats *stats) {
    SpotReadStats local;
    SpotReadStats &st = stats ? *stats : local;
    uint64_t (*clock)() = st.clock;
    st = SpotReadStats();
    st.clock = clock;
    auto now = [clock]() -> uint64_t { return clock ? clock() : 0; };
    paragraphs.clear();
    if (dict && !dict->isOpen()) dict = nullptr;
    const uint64_t t0 = now();
    std::vector<SpotBox> boxes;
    if (!spot.find(grey, w, h, boxes, error)) return false;
    st.spotMs = (uint32_t)(now() - t0);
    st.boxes = (unsigned)boxes.size();
    // furigana: the spotter finds a line of readings as a line of its own. A line shorter than a
    // line right below it, pressed against it and within its width may be one; it goes if it reads
    // as kana only and the line below has kanji (below).
    std::vector<int> ruby(boxes.size(), -1);  // the line it may be the reading of
    for (size_t i = 0; i < boxes.size(); i++)
        for (size_t j = 0; j < boxes.size() && ruby[i] < 0; j++) {
            const SpotBox &r = boxes[i], &t = boxes[j];
            if (i == j || r.vertical || t.vertical || r.h >= t.h) continue;
            // above it and pressed against it: the boxes, padded, overlap by a quarter of the reading's
            // (lines of text stand apart)
            const int overlap = (int)(r.y + r.h) - (int)t.y;
            const bool above = r.y < t.y && overlap * 4 >= (int)r.h && r.y * 2 + r.h < t.y * 2 + t.h;
            const int inX = (int)std::min(r.x + r.w, t.x + t.w) - (int)std::max(r.x, t.x);
            if (above && inX * 2 >= (int)r.w) ruby[i] = (int)j;
        }
    std::vector<LocalOcr::LineBox> toRead;
    std::vector<const SpotBox *> of;
    for (size_t i = 0; i < boxes.size(); i++) {
        const SpotBox &b = boxes[i];
        if (b.vertical) { st.vertical++; continue; }  // no recognizer here reads a column
        LocalOcr::LineBox lb;
        const unsigned grow = g_spotGrowPct * b.h / 100u;  // the spotter stops short of a faint last glyph (ー, …)
        lb.x = b.x > grow ? b.x - grow : 0;
        lb.y = b.y;
        lb.w = std::min(w, b.x + b.w + grow) - lb.x;
        lb.h = b.h;
        lb.dark = b.dark;
        toRead.push_back(lb);
        of.push_back(&b);
    }
    std::vector<TextLine> lines;
    const uint64_t t1 = now();
    if (!toRead.empty() && !ocr.readLines(grey, w, h, toRead, lines, error, progress)) return false;
    st.readMs = (uint32_t)(now() - t1);
    st.read = (unsigned)toRead.size();
    // each line back to its box (readLines leaves out the lines that read as nothing)
    std::vector<const TextLine *> lineOf(boxes.size(), nullptr);
    for (const TextLine &l : lines)
        for (size_t i = 0; i < toRead.size(); i++) {
            const float cx = (toRead[i].x + toRead[i].w * 0.5f) / w, cy = (toRead[i].y + toRead[i].h * 0.5f) / h;
            if (fabsf(cx - l.centerX) * w < 1.0f && fabsf(cy - l.centerY) * h < 1.0f) {
                lineOf[of[i] - boxes.data()] = &l;
                break;
            }
        }
    struct Kept { unsigned block; TextLine line; };
    std::vector<Kept> kept;
    const uint32_t dict0 = st.dictMs;
    ScoredReadStats sst;
    sst.clock = clock;
    for (size_t i = 0; i < boxes.size(); i++) {
        const TextLine *l = lineOf[i];
        if (!l) continue;
        const TextLine *below = ruby[i] >= 0 ? lineOf[ruby[i]] : nullptr;
        if (below && kanaOnly(l->text) && japaneseIn(below->text) > kanaIn(below->text)) { st.ruby++; continue; }
        // as a line found out in the scene: the recognizer sure of it, or words in it (judged as a text
        // box's line, the spotter's dialogue kept sprites and decorations read as text: 「ーロエロー」)
        if (!keepLine(*l, dict, false, &sst)) continue;
        kept.push_back({boxes[i].block, *l});
    }
    st.dictChecks = sst.dictChecks;
    st.dictMs = dict0 + sst.dictMs;
    st.kept = (unsigned)kept.size();
    // one paragraph per block, its lines in the spotter's order, one per row
    for (size_t i = 0; i < kept.size();) {
        std::string p;
        size_t j = i;
        for (; j < kept.size() && kept[j].block == kept[i].block; j++) {
            if (!p.empty()) p += '\n';
            p += kept[j].line.text;
        }
        paragraphs.push_back(p);
        i = j;
    }
    cleanParagraphs(paragraphs);
    // a scrap (「こー」) goes when there is real text
    unsigned best = 0;
    for (const std::string &p : paragraphs) best = std::max(best, japaneseChars({p}));
    if (best >= 6)
        paragraphs.erase(std::remove_if(paragraphs.begin(), paragraphs.end(),
                                        [](const std::string &p) { return japaneseChars({p}) < 3; }),
                         paragraphs.end());
    if (paragraphs.empty()) { error = "no text found on the screen"; return false; }
    return true;
}

}  // namespace kt
