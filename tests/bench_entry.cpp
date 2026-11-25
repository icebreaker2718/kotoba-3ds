// Lays out dictionary entries as the lookup screen does and reports the time and the memory taken while doing it.
// Built for the 3DS's CPU too (qemu-arm: build_arm_cli.sh's flags), where malloc is counted (-Wl,--wrap=malloc).
//   bench_entry dict-file [step]   (every step-th entry; default 1)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/dict.hpp"
#include "core/entry_layout.hpp"
#include "core/ui_text.hpp"

using namespace kt;

#ifdef COUNT_MALLOC
extern "C" void *__real_malloc(size_t);
static volatile unsigned long g_mallocs = 0;
extern "C" void *__wrap_malloc(size_t n) {
    g_mallocs++;
    return __real_malloc(n);
}
#else
static unsigned long g_mallocs = 0;
#endif

struct FileReader : Reader {
    FILE *f = nullptr;
    uint32_t sz = 0;
    explicit FileReader(const char *p) { f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); } }
    ~FileReader() { if (f) fclose(f); }
    bool read(uint32_t o, void *d, uint32_t n) override { return f && !fseek(f, o, SEEK_SET) && fread(d, 1, n, f) == n; }
    uint32_t size() override { return sz; }
};

struct BoxFont : GlyphSource {  // the 3DS font's cell, as test_ui's
    int cellWidth() const override { return 24; }
    int cellHeight() const override { return 30; }
    bool metrics(uint32_t cp, GlyphInfo &gi) override {
        bool wide = cp >= 0x2E80;
        gi.left = 1;
        gi.width = wide ? 22 : 10;
        gi.advance = wide ? 24 : 12;
        return true;
    }
    bool pixels(uint32_t, uint8_t *out) override {
        memset(out, 0, 24 * 30);
        return true;
    }
};

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const uint32_t step = argc > 2 ? (uint32_t)atoi(argv[2]) : 1;
    FileReader r(argv[1]);
    Dictionary d;
    std::string err;
    if (!d.open(&r, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    BoxFont font;
    TextRenderer tr(font);
    EntryLayout lay(tr);
    if (!lay.reserve()) return 1;
    unsigned long mallocs = 0, n = 0, cut = 0;
    double total = 0, worst = 0;
    uint32_t worstId = 0;
    for (uint32_t id = 0; id < d.entryCount(); id += step) {
        // one lookup's layout: the entry read (from the dictionary's block cache or the file) and laid out
        const unsigned long m0 = g_mallocs;
        auto t0 = std::chrono::steady_clock::now();
        lay.clear(12, 376);
        if (!lay.addEntry(d, id)) cut++;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        mallocs += g_mallocs - m0;
        total += us;
        if (us > worst) { worst = us; worstId = id; }
        n++;
    }
    printf("%lu entries laid out (v%u): mean %.1f us, slowest %.0f us (entry %u), %lu cut short, %lu mallocs while laying out\n",
           n, d.version(), total / n, worst, worstId, cut, mallocs);
    return 0;
}
