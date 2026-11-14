// Speed/regression harness: every reading mode the console has, over many screenshots, in one process.
//   bench_cli model.vocr spotter.vtsp dict.bin [modes] shot.bmp...
// modes (one argument, letters, default "fsbw"): f fast mode (flat boxes), s shaded boxes, b spotter,
// w the whole screen (the line finder, as a reading area does). Prints, per screen and mode, the boxes found
// and the text read; the output must stay byte-identical when the code is only made faster.
// KT_WORKERS=n reads lines on n threads (default 3, as on a New 3DS).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/bmp.hpp"
#include "core/correct.hpp"
#include "core/local_ocr.hpp"
#include "core/text_spot.hpp"
#include "core/textbox.hpp"

using namespace kt;
namespace kt { extern int g_spotJoinGapPct, g_spotJoinLeftPct; }
struct FileReader : Reader {
    FILE *f;
    uint32_t sz = 0;
    unsigned reads = 0;  // file reads (on the console: SD card reads), for KT_READS
    explicit FileReader(const char *p) { f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); } }
    ~FileReader() { if (f) fclose(f); }
    bool read(uint32_t o, void *d, uint32_t n) override { reads++; return f && !fseek(f, o, SEEK_SET) && fread(d, 1, n, f) == n; }
    uint32_t size() override { return sz; }
};
static Dictionary *g_dict;
static unsigned g_fixed;
static void fix(std::string &t, const std::vector<OcrAlt> &alts, void *) { g_fixed += fixOcrLine(*g_dict, t, alts); }
static void runThreads(unsigned n, void (*job)(void *), void *arg) {
    std::vector<std::thread> t;
    for (unsigned i = 0; i < n; i++) t.emplace_back(job, arg);
    for (auto &x : t) x.join();
}
static void print(const char *mode, bool ok, const std::vector<std::string> &paras, const std::string &e) {
    if (!ok) { printf("  %s FAILED: %s\n", mode, e.c_str()); return; }
    for (const std::string &p : paras) {
        std::string s = p;
        for (char &c : s) if (c == '\n') c = '|';
        printf("  %s: %s\n", mode, s.c_str());
    }
}
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: bench_cli model.vocr spotter.vtsp dict.bin [modes] shot.bmp...\n"); return 2; }
    std::string e;
    FileReader mf(argv[1]), sf(argv[2]), df(argv[3]);
    Dictionary dict;
    if (!dict.open(&df, e)) { printf("DICT FAILED: %s\n", e.c_str()); return 3; }
    g_dict = &dict;
    LocalOcr ocr;
    const char *wk = getenv("KT_WORKERS");
    const unsigned workers = wk ? (unsigned)atoi(wk) : 3;
    ocr.setParallel(workers, workers > 1 ? runThreads : nullptr);
    ocr.setFixer(fix, nullptr);
    if (getenv("KT_TILE")) ocr.setOptions((unsigned)atoi(getenv("KT_TILE")), 0);  // VOCR frames per tile (0: its default 4)
    if (!ocr.load(mf, e)) { printf("MODEL FAILED: %s\n", e.c_str()); return 3; }
    TextSpotter spot;
    if (!spot.load(sf, e)) { printf("SPOTTER FAILED: %s\n", e.c_str()); return 3; }
    spot.setParallel(workers, workers > 1 ? runThreads : nullptr);
    // as the plugin: the spotter works in the recognizer's workers' memory (KT_LEND=0: its own, as before);
    // KT_POISON=1 fills that memory with junk before every mode, to show nothing relies on what was left there
    void *scratch = nullptr;
    size_t scratchBytes = 0;
    const bool lend = !getenv("KT_LEND") || atoi(getenv("KT_LEND"));
    const bool poison = getenv("KT_POISON") && atoi(getenv("KT_POISON"));
    if (ocr.scratch(scratch, scratchBytes) && lend && !spot.lendMemory(scratch, scratchBytes, 400, 240))
        fprintf(stderr, "the spotter didn't take the recognizer's memory\n");
    fprintf(stderr, "recognizer's memory: %zu bytes\n", scratchBytes);
    if (getenv("KT_JOIN_GAP")) g_spotJoinGapPct = atoi(getenv("KT_JOIN_GAP"));    // block joining (spotter), for tuning
    if (getenv("KT_JOIN_LEFT")) g_spotJoinLeftPct = atoi(getenv("KT_JOIN_LEFT"));  // -1 turns it off
    int first = 4;
    std::string modes = "fsbw";
    if (argc > 4 && !strchr(argv[4], '.')) { modes = argv[4]; first = 5; }
    for (int a = first; a < argc; a++) {
        FileReader bmp(argv[a]);
        unsigned w = 0, h = 0;
        uint8_t *rgb = topScreenRGB888(bmp, w, h, e);
        const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        printf("%s\n", name);
        if (!rgb) { printf("  BMP FAILED: %s\n", e.c_str()); continue; }
        uint8_t *grey = greyFromRGB(rgb, w, h);
        for (char m : modes) {
            if (poison && scratch) memset(scratch, 0xA5 ^ (int)(m + a), scratchBytes);
            std::vector<std::string> paras;
            g_fixed = 0;
            if (m == 'f' || m == 's') {
                std::vector<TextBox> boxes = findTextBoxes(rgb, w, h, 3, m == 's');
                for (const TextBox &b : boxes)
                    printf("  %c box %u,%u %ux%u%s ink %u,%u %ux%u\n", m, b.x, b.y, b.w, b.h, b.shaded ? " shaded" : "", b.inkX,
                           b.inkY, b.inkW, b.inkH);
                bool ok = localReadScreen(ocr, grey, w, h, boxes, paras, e);
                print(m == 'f' ? "fast" : "shaded", ok, paras, e);
            } else if (m == 'b') {
                bool ok = readScreenSpot(ocr, spot, &dict, grey, w, h, paras, e);
                print("spot", ok, paras, e);
            } else if (m == 'w') {
                bool ok = localReadScreen(ocr, grey, w, h, std::vector<TextBox>(), paras, e);
                print("whole", ok, paras, e);
            }
            if (g_fixed) printf("  %c fixed %u\n", m, g_fixed);
            if (getenv("KT_READS")) fprintf(stderr, "%s %c dict reads %u\n", name, m, df.reads), df.reads = 0;
        }
        free(grey);
        free(rgb);
    }
    return 0;
}
