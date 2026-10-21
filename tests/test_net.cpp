// Native test of the plugin's network client against tools/ocr_server.py (see test_ocr.py).
//   test_net <port> <screenshot.bmp> <dict.bin>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/bmp.hpp"
#include "core/dict.hpp"
#include "core/net.hpp"
#include "core/search.hpp"

using namespace kt;

class FileReader : public Reader {
public:
    explicit FileReader(const char *path) { f_ = fopen(path, "rb"); if (f_) { fseek(f_, 0, SEEK_END); sz_ = ftell(f_); } }
    ~FileReader() { if (f_) fclose(f_); }
    bool read(uint32_t off, void *dst, uint32_t len) override {
        return f_ && fseek(f_, off, SEEK_SET) == 0 && fread(dst, 1, len, f_) == len;
    }
    uint32_t size() override { return sz_; }
private:
    FILE *f_ = nullptr;
    uint32_t sz_ = 0;
};

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    (void)argc;
    int port = atoi(argv[1]);
    std::string err;
    CHECK(netStart(err), "netStart");

    std::string host; int p2 = 0;
    bool found = netDiscover(port, 1500, host, p2, err);
    CHECK(found && p2 == port, "discovery found %s:%d", host.c_str(), p2);

    CHECK(netPing(host, port, 1500, err), "ping %s", err.c_str());

    FileReader shotFile(argv[2]);
    std::string compact;
    CHECK(topScreenRGB565(shotFile, compact, err), "compact screenshot: %zu bytes from a %u-byte BMP", compact.size(), shotFile.size());
    MemReader shot(compact);
    std::string body;
    clock_t t0 = clock();
    bool ok = netPost(host, port, "/ocr", {"X-Title-Id: 0004000000123400"}, shot, 2000, 10000, body, err);
    CHECK(ok, "upload %u bytes + OCR (%s) %.0f ms", shot.size(), err.c_str(), 1000.0 * (clock() - t0) / CLOCKS_PER_SEC);
    std::string sentence; std::vector<std::string> chunks;
    CHECK(parseOcrReply(body, sentence, chunks, err), "parse reply: 「%s」", sentence.c_str());
    std::string joined; for (auto &c : chunks) joined += c + "|";
    printf("     chunks: %s\n", joined.c_str());
    CHECK(sentence == "もう何も食べられなかった。", "sentence text");

    FileReader fr(argv[3]);
    Dictionary d;
    CHECK(d.open(&fr, err), "dict");
    size_t off = 0; bool foundTaberu = false;
    for (auto &c : chunks) {
        SearchOutput so = scanAt(d, sentence, off);
        Entry e;
        if (!so.results.empty() && d.entry(so.results[0].entryId, e)) {
            printf("     tap %-10s -> %s【%s】 (%s)\n", c.c_str(), expressionFor(e, so.results[0].matchedKey).c_str(),
                   e.reading.c_str(), so.results[0].reasons.c_str());
            if (expressionFor(e, so.results[0].matchedKey) == "食べる") foundTaberu = true;
        } else {
            printf("     tap %-10s -> (nothing)\n", c.c_str());
        }
        off += c.size();
    }
    CHECK(foundTaberu, "tapping the verb chunk finds 食べる");

    // failure paths must come back quickly with a message (so the plugin can fall back)
    t0 = clock();
    time_t w0 = time(nullptr);
    ok = netPing("127.0.0.1", port + 1, 1500, err);
    CHECK(!ok && time(nullptr) - w0 <= 2, "closed port fails fast: %s", err.c_str());
    w0 = time(nullptr);
    ok = netPing("10.255.255.1", port, 1200, err);
    CHECK(!ok && time(nullptr) - w0 <= 3, "unreachable PC times out: %s", err.c_str());
    ok = netDiscover(port + 2, 800, host, p2, err);
    CHECK(!ok, "no server -> discovery fails: %s", err.c_str());
    std::string hh; int pp;
    CHECK(parseHostPort("192.168.1.20:7331", hh, pp) && hh == "192.168.1.20" && pp == 7331, "parse host:port");
    CHECK(parseHostPort("192.168.1.20", hh, pp) && pp == 7331, "default port");
    CHECK(!parseHostPort("hello", hh, pp), "reject garbage address");
    CHECK(!parseOcrReply("KOTOBA1\n\n\n", sentence, chunks, err), "empty OCR -> error: %s", err.c_str());
    // in-game card creation, same sequence as SendCardToPc() in main.cpp
    if (argc > 4) {
        std::string line = argv[4];
        std::vector<std::string> hdr = {"X-Title-Id: 0004000000123400", "X-Shot-Name: shot - 007.bmp"};
        std::string reply;
        MemReader b1(line + "\n");
        bool ok1 = netPost(host, port, "/mine", hdr, b1, 1500, 6000, reply, err);
        CHECK(ok1 && reply.compare(0, 9, "NEED_SHOT") == 0, "unknown screenshot -> server asks for it (%s)", reply.c_str());
        MemReader shot2(compact);
        std::string r2;
        CHECK(netPost(host, port, "/shot", hdr, shot2, 1500, 10000, r2, err) && r2.compare(0, 2, "OK") == 0, "screenshot uploaded");
        MemReader b2(line + "\n");
        bool ok2 = netPost(host, port, "/mine", hdr, b2, 1500, 6000, reply, err);
        CHECK(ok2 && reply.compare(0, 2, "OK") == 0, "word accepted: %s", reply.c_str());

        // Read screen first, then save: the server must reuse the screenshot it OCR'd
        std::vector<std::string> hdr2 = {"X-Title-Id: 0004000000123400", "X-Shot-Name: shot - 008.bmp"};
        MemReader shot3(compact);
        std::string ocrReply;
        CHECK(netPost(host, port, "/ocr", hdr2, shot3, 1500, 10000, ocrReply, err), "OCR of shot - 008.bmp");
        std::string line2 = line;
        size_t p = line2.find("shot - 007.bmp");
        if (p != std::string::npos) line2.replace(p, 14, "shot - 008.bmp");
        p = line2.find("\t宝箱\t");
        if (p != std::string::npos) line2.replace(p, strlen("\t宝箱\t"), "\t宝箱x\t");  // a different word
        MemReader b3(line2 + "\n");
        bool ok3 = netPost(host, port, "/mine", hdr2, b3, 1500, 6000, reply, err);
        CHECK(ok3 && reply.compare(0, 2, "OK") == 0, "after Read screen, saving needs no upload (%s)", reply.c_str());
    }
    netStop();
    printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
