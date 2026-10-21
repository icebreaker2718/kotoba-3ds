// Wraps every entry of a real dict.bin (and tricky strings) and checks the invariants.
//   test_wrap dict.bin
#include <cstdio>
#include <cstdlib>
#include <string>
#include "core/dict.hpp"
#include "core/kana.hpp"
#include "core/wrap.hpp"
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
// roughly the 3DS system font: ASCII ~6-9 px, everything else 13 px
static float approx(const std::string &s) {
    float w = 0;
    for (size_t i = 0; i < s.size();) { uint32_t cp = utf8Next(s, i); w += cp < 0x80 ? (cp == ' ' ? 4.f : 7.f) : 13.f; }
    return w;
}
static int fails = 0;
static void check(const std::string &text, float maxW) {
    std::vector<std::string> lines = wrapText(text, maxW, approx);
    std::string joinedNoSpace, origNoSpace;
    for (auto &l : lines) {
        if (approx(l) > maxW && utf8Length(l) > 1) { if (fails++ < 5) printf("FAIL too wide (%g): %s\n", approx(l), l.c_str()); }
        for (char c : l) if (c != ' ' && c != '\n') joinedNoSpace += c;
        // valid UTF-8 boundaries
        size_t i = 0; while (i < l.size()) { if (utf8Next(l, i) == 0xFFFD) { if (fails++ < 5) printf("FAIL broken char in: %s\n", l.c_str()); break; } }
    }
    for (char c : text) if (c != ' ' && c != '\n') origNoSpace += c;
    if (joinedNoSpace != origNoSpace) { if (fails++ < 5) printf("FAIL text lost/changed: %s\n", text.c_str()); }
}
int main(int argc, char **argv) {
    // tricky cases: space exactly at the break, many spaces, long words, Japanese, blank lines
    for (float w = 20; w < 400; w += 7) {
        check("to live on (e.g. a salary); to live off; to subsist on", w);
        check("a  b   c    d     e      f", w);
        check("supercalifragilisticexpialidocious and more", w);
        check("ゆめせかいへいくときはこえをかけるんじゃぞ", w);
        check("食べる・喰べる【たべる】\n(passive > negative > past)\n\n1. (v1,vt) to eat", w);
        check(" leading and trailing ", w);
        check("x", w);
        check("", w);
    }
    int n = 0;
    if (argc > 1) {
        FileReader fr(argv[1]);
        Dictionary d; std::string err;
        if (!d.open(&fr, err)) { printf("dict: %s\n", err.c_str()); return 1; }
        for (uint32_t id = 0; id < d.entryCount(); id++) {
            Entry e;
            if (!d.entry(id, e)) continue;
            check(e.headline, 380.f);
            for (auto &s : e.senses) { check(s, 380.f); check(s, 284.f); }
            n++;
        }
    }
    printf("%d dictionary entries wrapped, %d failures\n", n, fails);
    printf(fails ? "FAILED\n" : "ALL PASSED\n");
    return fails ? 1 : 0;
}
