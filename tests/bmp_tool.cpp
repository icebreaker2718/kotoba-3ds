// bmp_tool in.bmp out.k3 - runs the plugin's screenshot converter natively (for test_bmp.py)
#include <cstdio>
#include <string>
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
int main(int, char **argv) {
    FileReader r(argv[1]);
    std::string out, err;
    if (!screenshotRGB565(r, out, err)) { printf("error: %s\n", err.c_str()); return 1; }
    FILE *f = fopen(argv[2], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
    printf("%zu bytes\n", out.size());
    return 0;
}
