// Minimal on-device OCR command line (no network code), used to run the 3DS's OCR path built for
// the ARM11 (ARMv6 SIMD kernel) under qemu-arm:   ocr_cli model.vocr shot.bmp [dict.bin]
// With dict.bin, misread kanji are corrected with the dictionary as on the console.
#include <cstdio>
#include <cstdlib>

#include "core/bmp.hpp"
#include "core/correct.hpp"
#include "core/local_ocr.hpp"
#include "core/textbox.hpp"

using namespace kt;
struct FileReader : Reader {
    FILE *f;
    uint32_t sz = 0;
    explicit FileReader(const char *p) { f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); } }
    ~FileReader() { if (f) fclose(f); }
    bool read(uint32_t o, void *d, uint32_t n) override { return f && !fseek(f, o, SEEK_SET) && fread(d, 1, n, f) == n; }
    uint32_t size() override { return sz; }
};
static void fix(std::string &t, const std::vector<OcrAlt> &alts, void *d) {
    std::string before = t;
    if (fixOcrLine(*(Dictionary *)d, t, alts)) printf("FIXED: %s -> %s\n", before.c_str(), t.c_str());
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FileReader mf(argv[1]), bmp(argv[2]);
    std::string e;
    unsigned w = 0, h = 0;
    uint8_t *rgb = topScreenRGB888(bmp, w, h, e);
    if (!rgb) { printf("BMP FAILED: %s\n", e.c_str()); return 3; }
    std::vector<TextBox> boxes = findTextBoxes(rgb, w, h);
    uint8_t *grey = greyFromRGB(rgb, w, h);
    free(rgb);
    LocalOcr ocr;
    FileReader df(argc > 3 ? argv[3] : "");
    Dictionary dict;
    if (argc > 3) {
        if (!dict.open(&df, e)) { printf("DICT FAILED: %s\n", e.c_str()); return 3; }
        ocr.setFixer(fix, &dict);
    }
    if (!ocr.load(mf, e)) { printf("MODEL FAILED: %s\n", e.c_str()); return 3; }
    std::vector<std::string> paras;
    bool ok = localReadScreen(ocr, grey, w, h, boxes, paras, e);
    free(grey);
    if (!ok) { printf("OCR FAILED: %s\n", e.c_str()); return 3; }
    for (auto &p : paras) printf("PARAGRAPH: %s\n", p.c_str());
    return 0;
}
