// bmp_tool in.bmp out.k3 - runs the plugin's screenshot converter natively (for test_bmp.py)
// bmp_tool --qoi in.rgb w h out.qoi - the plugin's QOI encoder on raw R, G, B bytes
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "core/bmp.hpp"
using namespace kt;
class FileReader : public Reader {
public:
    explicit FileReader(const char *p) { f_ = fopen(p, "rb"); if (f_) { fseek(f_, 0, SEEK_END); sz_ = ftell(f_); } }
    ~FileReader() { if (f_) fclose(f_); }
    bool read(uint32_t off, void *d, uint32_t n) override { return f_ && !fseek(f_, off, SEEK_SET) && fread(d, 1, n, f_) == n; }
    uint32_t size() override { return sz_; }
private:
    FILE *f_ = nullptr; uint32_t sz_ = 0;
};
static bool toFile(const uint8_t *d, size_t n, void *f) { return fwrite(d, 1, n, (FILE *)f) == n; }
int main(int, char **argv) {
    if (!strcmp(argv[1], "--qoi")) {
        unsigned w = atoi(argv[3]), h = atoi(argv[4]);
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        FILE *in = fopen(argv[2], "rb");
        if (!in || fread(rgb.data(), 1, rgb.size(), in) != rgb.size()) return 1;
        fclose(in);
        FILE *f = fopen(argv[5], "wb");
        static uint8_t buf[5000];
        QoiEncoder q(w, h, toFile, f, buf, sizeof(buf));
        for (size_t i = 0; i < (size_t)w * h; i++) q.push(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
        bool ok = q.finish();
        fclose(f);
        return ok ? 0 : 1;
    }
    FileReader r(argv[1]);
    std::string out, err;
    if (!screenshotRGB565(r, out, err)) { printf("error: %s\n", err.c_str()); return 1; }
    FILE *f = fopen(argv[2], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
    printf("%zu bytes\n", out.size());
    return 0;
}
