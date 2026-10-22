// Google Lens client, offline parts + HTTPS:
//   test_lens dict.bin                      protobuf request/reply, furigana, word chunking
//   test_lens --https host port roots.pem   one POST to a local TLS server (see test_https.py)
//   test_lens --live shot.bmp [dict.bin]    the real Google Lens (needs internet)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/bmp.hpp"
#include "core/dict.hpp"
#include "core/https.hpp"
#include "core/lens.hpp"
#include "core/net.hpp"
#include "core/search.hpp"

using namespace kt;
static int fails = 0;
static void check(bool ok, const std::string &what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}
static std::string slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
class FileReader : public Reader {
public:
    explicit FileReader(const char *p) { f_ = fopen(p, "rb"); if (f_) { fseek(f_, 0, SEEK_END); sz_ = ftell(f_); } }
    ~FileReader() { if (f_) fclose(f_); }
    bool read(uint32_t off, void *d, uint32_t n) override { return f_ && !fseek(f_, off, SEEK_SET) && fread(d, 1, n, f_) == n; }
    uint32_t size() override { return sz_; }
private:
    FILE *f_ = nullptr; uint32_t sz_ = 0;
};
static std::string dataDir(const char *argv0) {
    std::string a = argv0;
    size_t s = a.rfind('/');
    return (s == std::string::npos ? std::string(".") : a.substr(0, s)) + "/data/";
}

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "--https")) {
        std::string pem = slurp(argv[4]), body, err;
        int status = 0;
        std::string path = argc > 5 ? argv[5] : "/echo";
        bool ok = httpsPost(argv[2], path, {"Content-Type: application/x-protobuf"}, "hello lens", 8000,
                            body, status, err, atoi(argv[3]), pem.c_str());
        printf("ok=%d status=%d body=[%s] error=[%s]\n", ok, status, body.c_str(), err.c_str());
        return ok ? 0 : 2;
    }
    if (argc >= 3 && !strcmp(argv[1], "--live")) {
        FileReader bmp(argv[2]);
        std::string jpeg, err;
        int w = 0, h = 0;
        if (!topScreenJpeg(bmp, 2, 88, jpeg, w, h, err)) { printf("jpeg: %s\n", err.c_str()); return 1; }
        printf("jpeg %dx%d, %zu bytes\n", w, h, jpeg.size());
        std::vector<std::string> paras;
        std::string e;
        netStart(e);
        bool ok = lensOcr(jpeg, w, h, 0x1234567890ull, 20000, paras, e);
        if (!ok) { printf("LENS FAILED: %s\n", e.c_str()); return 3; }
        for (auto &p : paras) printf("PARAGRAPH: %s\n", p.c_str());
        if (argc > 3) {
            FileReader dr(argv[3]);
            Dictionary d;
            std::string de;
            if (d.open(&dr, de))
                for (auto &p : paras) {
                    std::string line;
                    for (auto &c : chunkText(d, p)) line += c + "|";
                    printf("CHUNKS: %s\n", line.c_str());
                }
        }
        return 0;
    }

    std::string dir = dataDir(argv[0]);
    // request: byte-identical to the one checked against Google's own .proto definitions
    std::string req = lensRequest(std::string("\xFF\xD8JPEGDATA", 10), 800, 480, 123456789);
    check(req == slurp(dir + "lens_request.bin"), "request matches the reference (decoded with Lens' protos)");

    // reply: built with Google's protos (furigana line, a 2-line paragraph, a menu, English)
    std::vector<LensLine> lines;
    std::string err;
    check(lensParse(slurp(dir + "lens_reply.bin"), lines, err), "reply parses " + err);
    check(lines.size() == 5, "5 lines");
    std::vector<std::string> paras = lensParagraphs(lines);
    check(paras.size() == 3, "3 paragraphs (furigana dropped)");
    if (paras.size() == 3) {
        check(paras[0] == "また夢世界へ行く時は声をかけるんじゃぞ！", "lines joined: " + paras[0]);
        check(paras[1] == "メニュー", "short katakana kept: " + paras[1]);
        check(paras[2] == "Press A", "word separators kept: " + paras[2]);
    }
    check(!lensParse("\x0a\x02\x08\x01", lines, err), "garbage reply rejected: " + err);
    check(!lensParse(std::string("\x12\x05\x0a\x03\x12\x01", 6), lines, err), "truncated reply rejected");
    std::string empty;
    check(!lensParse(empty, lines, err), "empty reply rejected");

    {   // Lens splitting a 2-line text box into 2 paragraphs; a menu elsewhere stays separate
        auto L = [](const char *t, int p, float cx, float cy, float w, float h) {
            LensLine l; l.text = t; l.paragraph = p; l.centerX = cx; l.centerY = cy; l.width = w; l.height = h; return l;
        };
        std::vector<LensLine> ls = {L("メニュー", 0, 0.10f, 0.05f, 0.15f, 0.05f),
                                    L("ゆめせかい", 1, 0.25f, 0.66f, 0.12f, 0.02f),
                                    L("また夢世界へ行く時は", 2, 0.36f, 0.72f, 0.60f, 0.07f),
                                    L("声をかけるんじゃぞ!", 3, 0.35f, 0.83f, 0.58f, 0.07f),
                                    L("はい", 4, 0.85f, 0.95f, 0.08f, 0.07f)};
        std::vector<std::string> ps = lensParagraphs(ls);
        check(ps.size() == 3 && ps[1] == "また夢世界へ行く時は声をかけるんじゃぞ!",
              "lines of one text box merged" + (ps.size() > 1 ? ": " + ps[1] : std::string()));
        check(ps.size() == 3 && ps[0] == "メニュー" && ps[2] == "はい", "menu and a far-off choice stay separate");
    }

    std::string chunked;
    check(dechunk("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", chunked) && chunked == "hello world", "chunked body");
    check(!dechunk("5\r\nhel", chunked), "cut-off chunked body rejected");

    if (argc > 1) {
        FileReader dr(argv[1]);
        Dictionary d;
        if (d.open(&dr, err)) {
            for (const char *t : {"また夢世界へ行く時は声をかけるんじゃぞ！", "この先に宝箱があるらしいぞ。",
                                  "もう何も食べられなかった。", "ABCあいう"}) {
                std::vector<std::string> c = chunkText(d, t);
                std::string joined, shown;
                for (auto &x : c) { joined += x; shown += x + "|"; }
                check(joined == t, std::string("chunks join back: ") + shown);
            }
            std::vector<std::string> c = chunkText(d, "この先に宝箱があるらしいぞ。");
            bool hasGa = false, hasTakarabako = false;
            for (auto &x : c) { hasGa |= x == "が"; hasTakarabako |= x == "宝箱"; }
            check(hasGa && hasTakarabako, "particle split off, compound kept");
        }
    }
    printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
