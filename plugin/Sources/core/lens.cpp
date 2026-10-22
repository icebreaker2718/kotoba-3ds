#include "core/lens.hpp"

#include <algorithm>
#include <cstring>

#include "core/kana.hpp"

namespace kt {

// ---- protobuf writing (just what the request needs) ----
static void putVarint(std::string &o, uint64_t v) {
    while (v >= 0x80) { o += (char)((v & 0x7F) | 0x80); v >>= 7; }
    o += (char)v;
}
static void putKey(std::string &o, int field, int wire) { putVarint(o, ((uint64_t)field << 3) | wire); }
static void putInt(std::string &o, int field, uint64_t v) { if (v) { putKey(o, field, 0); putVarint(o, v); } }
static void putBytes(std::string &o, int field, const std::string &b) {
    putKey(o, field, 2);
    putVarint(o, b.size());
    o += b;
}
static void putStr(std::string &o, int field, const std::string &s) { if (!s.empty()) putBytes(o, field, s); }

std::string lensRequest(const std::string &image, int width, int height, uint64_t random,
                        const std::string &language) {
    // xorshift for the analytics id
    uint64_t x = random ? random : 0x9E3779B97F4A7C15ull;
    std::string analytics;
    for (int i = 0; i < 16; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        analytics += (char)(x & 0xFF);
    }
    std::string requestId;                       // LensOverlayRequestId
    putInt(requestId, 1, random | 1);            //   uuid
    putBytes(requestId, 4, analytics);           //   analytics_id
    putBytes(requestId, 6, "");                  //   routing_info {}
    std::string locale;                          // LocaleContext
    putStr(locale, 1, language);
    putStr(locale, 2, "Asia/Tokyo");
    std::string filter, filters;                 // AppliedFilters { filter { AUTO_FILTER } }
    putInt(filter, 1, 7);
    putBytes(filters, 1, filter);
    std::string client;                          // LensOverlayClientContext
    putInt(client, 1, 3);                        //   platform = PLATFORM_WEB
    putInt(client, 2, 4);                        //   surface = SURFACE_CHROMIUM
    putBytes(client, 4, locale);
    putBytes(client, 17, filters);
    std::string context;                         // LensOverlayRequestContext
    putBytes(context, 3, requestId);
    putBytes(context, 4, client);
    std::string payload, meta, imageData;        // ImageData
    putBytes(payload, 1, image);
    putInt(meta, 1, (uint64_t)width);
    putInt(meta, 2, (uint64_t)height);
    putBytes(imageData, 1, payload);
    putBytes(imageData, 3, meta);
    std::string objects;                         // LensOverlayObjectsRequest
    putBytes(objects, 1, context);
    putBytes(objects, 3, imageData);
    std::string server;                          // LensOverlayServerRequest
    putBytes(server, 1, objects);
    return server;
}

// ---- protobuf reading ----
struct Field {
    int number = 0, wire = 0;
    uint64_t value = 0;        // varint / fixed
    const char *data = nullptr;
    size_t size = 0;           // length-delimited
};

class PbReader {
public:
    PbReader(const char *p, size_t n) : p_(p), end_(p + n) {}
    bool ok() const { return ok_; }
    bool next(Field &f) {
        if (p_ >= end_ || !ok_) return false;
        uint64_t key;
        if (!varint(key)) return fail();
        f.number = (int)(key >> 3);
        f.wire = (int)(key & 7);
        f.data = nullptr;
        f.size = 0;
        f.value = 0;
        switch (f.wire) {
        case 0: if (!varint(f.value)) return fail(); break;
        case 1: if (end_ - p_ < 8) return fail(); memcpy(&f.value, p_, 8); p_ += 8; break;
        case 5: { if (end_ - p_ < 4) return fail(); uint32_t v; memcpy(&v, p_, 4); f.value = v; p_ += 4; break; }
        case 2: {
            uint64_t len;
            if (!varint(len) || len > (uint64_t)(end_ - p_)) return fail();
            f.data = p_;
            f.size = (size_t)len;
            p_ += len;
            break;
        }
        default: return fail();
        }
        return true;
    }
private:
    bool varint(uint64_t &v) {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p_ >= end_) return false;
            uint8_t b = (uint8_t)*p_++;
            v |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool fail() { ok_ = false; return false; }
    const char *p_, *end_;
    bool ok_ = true;
};

static float asFloat(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

// first length-delimited field `n` inside (p, size)
static bool sub(const char *p, size_t size, int n, const char *&out, size_t &outSize) {
    PbReader r(p, size);
    Field f;
    while (r.next(f))
        if (f.number == n && f.wire == 2) { out = f.data; outSize = f.size; return true; }
    return false;
}

static void readBox(const char *p, size_t n, LensLine &line) {  // Geometry { 1: CenterRotatedBox }
    const char *b; size_t bn;
    if (!sub(p, n, 1, b, bn)) return;
    PbReader r(b, bn);
    Field f;
    while (r.next(f)) {
        if (f.wire != 5) continue;
        if (f.number == 1) line.centerX = asFloat(f.value);
        if (f.number == 2) line.centerY = asFloat(f.value);
        if (f.number == 3) line.width = asFloat(f.value);
        if (f.number == 4) line.height = asFloat(f.value);
    }
}

bool lensParse(const std::string &reply, std::vector<LensLine> &lines, std::string &error) {
    lines.clear();
    const char *objects, *text, *layout;
    size_t on, tn, ln;
    // LensOverlayServerResponse { 2: objects_response { 3: text { 1: text_layout } } }
    if (!sub(reply.data(), reply.size(), 2, objects, on)) {
        error = reply.empty() ? "Google Lens sent an empty reply" : "unexpected reply from Google Lens";
        return false;
    }
    if (!sub(objects, on, 3, text, tn) || !sub(text, tn, 1, layout, ln)) return true;  // no text found
    PbReader paragraphs(layout, ln);
    Field pf;
    int pi = 0;
    while (paragraphs.next(pf)) {
        if (pf.number != 1 || pf.wire != 2) continue;     // TextLayout.paragraphs
        PbReader lr(pf.data, pf.size);
        Field lf;
        while (lr.next(lf)) {
            if (lf.number != 2 || lf.wire != 2) continue;  // Paragraph.lines
            LensLine line;
            line.paragraph = pi;
            PbReader wr(lf.data, lf.size);
            Field wf;
            while (wr.next(wf)) {
                if (wf.number == 2 && wf.wire == 2) { readBox(wf.data, wf.size, line); continue; }  // Line.geometry
                if (wf.number != 1 || wf.wire != 2) continue;                                       // Line.words
                PbReader w(wf.data, wf.size);
                Field f;
                std::string word, sep;
                while (w.next(f)) {
                    if (f.wire != 2) continue;
                    if (f.number == 2) word.assign(f.data, f.size);       // plain_text
                    else if (f.number == 3) sep.assign(f.data, f.size);   // text_separator
                }
                line.text += word + sep;
            }
            while (!line.text.empty() && line.text.back() == ' ') line.text.pop_back();
            if (!line.text.empty()) lines.push_back(line);
        }
        pi++;
    }
    return true;
}

static bool kanaOnly(const std::string &s) {
    size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        uint32_t cp = utf8Next(s, i);
        if (cp == ' ') continue;
        if (!((cp >= 0x3040 && cp <= 0x30FF) || cp == 0x30FC)) return false;
        any = true;
    }
    return any;
}

static size_t countKanji(const std::string &s) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        uint32_t cp = utf8Next(s, i);
        if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || cp == 0x3005) n++;
    }
    return n;
}

std::vector<std::string> lensParagraphs(const std::vector<LensLine> &lines) {
    // typical text height: median over lines with kanji or long lines
    std::vector<float> hs;
    for (const LensLine &l : lines)
        if (l.height > 0 && !kanaOnly(l.text)) hs.push_back(l.height);
    if (hs.empty())
        for (const LensLine &l : lines) if (l.height > 0) hs.push_back(l.height);
    float typical = 0;
    if (!hs.empty()) {
        std::sort(hs.begin(), hs.end());
        typical = hs[hs.size() / 2];
    }
    std::vector<std::string> out;
    int current = -1;
    const LensLine *prev = nullptr;
    for (size_t li = 0; li < lines.size(); li++) {
        const LensLine &l = lines[li];
        bool furigana = typical > 0 && l.height > 0 && l.height < typical * 0.7f && kanaOnly(l.text) &&
                        utf8Length(l.text) <= 10;
        // Or: a short kana-only line (no punctuation) right above a line with enough kanji to be
        // its reading (its box can be as tall as a text line when it takes in the box's frame).
        if (!furigana && kanaOnly(l.text) && li + 1 < lines.size()) {
            const LensLine &next = lines[li + 1];
            size_t kana = utf8Length(l.text), kanji = countKanji(next.text);
            float gap = next.centerY - l.centerY;
            furigana = kanji > 0 && kana <= 3 * kanji && kana <= 12 && gap > 0 &&
                       gap < 1.2f * (next.height > 0 ? next.height : l.height);
        }
        if (furigana) continue;
        if (l.paragraph != current || out.empty()) {
            // a new Lens paragraph that is really the next line of the same text box?
            bool continues = false;
            if (prev && prev->height > 0 && l.height > 0) {
                float ratio = l.height / prev->height;
                float gap = l.centerY - prev->centerY;            // one line further down
                float leftPrev = prev->centerX - prev->width / 2, left = l.centerX - l.width / 2;
                continues = ratio > 0.7f && ratio < 1.43f && gap > 0.5f * prev->height &&
                            gap < 2.0f * prev->height && (left - leftPrev) < 0.06f && (leftPrev - left) < 0.06f;
            }
            if (!continues || out.empty()) out.push_back("");
            current = l.paragraph;
        }
        prev = &l;
        std::string &p = out.back();
        // English lines get a space between them, Japanese ones don't
        if (!p.empty() && (unsigned char)p.back() < 0x80 && (unsigned char)l.text[0] < 0x80) p += ' ';
        p += l.text;
    }
    out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
    return out;
}

}  // namespace kt
