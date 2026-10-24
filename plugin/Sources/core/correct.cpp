#include "core/correct.hpp"

#include "core/kana.hpp"
#include "core/search.hpp"

namespace kt {

static bool isKanji(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || cp == 0x3005;
}

static bool isJapanese(uint32_t cp) {
    return isKanji(cp) || (cp >= 0x3041 && cp <= 0x30FA) || cp == 0x30FC;
}

static uint32_t firstCp(const std::string &s) {
    size_t i = 0;
    return s.empty() ? 0 : utf8Next(s, i);
}

unsigned wordCover(Dictionary &dict, const std::string &text, size_t byte, size_t len) {
    // starts: this character and up to 4 before it
    std::vector<size_t> starts;
    for (size_t i = 0; i < text.size() && i <= byte;) {
        starts.push_back(i);
        utf8Next(text, i);
    }
    if (starts.size() > 5) starts.erase(starts.begin(), starts.end() - 5);
    unsigned best = 0;
    for (size_t s : starts) {
        SearchOutput so = scanAt(dict, text, s, 10, 8);
        for (const Result &r : so.results) {
            if (r.prefixOnly || r.surface.empty()) continue;
            if (s + r.surface.size() < byte + len) continue;  // ends before the character
            unsigned chars = (unsigned)utf8Length(r.surface);
            if (chars > best) best = chars;
        }
    }
    return best;
}

unsigned fixOcrLine(Dictionary &dict, std::string &text, const std::vector<OcrAlt> &alts) {
    if (!dict.isOpen()) return 0;
    unsigned changed = 0;
    long shift = 0;  // byte offsets move when a replacement has another length
    for (const OcrAlt &a : alts) {
        long at = (long)a.byte + shift;
        if (at < 0 || (size_t)at + a.len > text.size()) continue;
        std::string orig = text.substr((size_t)at, a.len);
        // kanji the recognizer mixed up with another kanji (遺/造, 未/末...)
        if (!isKanji(firstCp(orig))) continue;
        unsigned origCover = wordCover(dict, text, (size_t)at, a.len);
        if (origCover >= 2) continue;  // already part of a word: trust it
        std::string pick;
        unsigned pickCover = 1;
        for (const std::string &o : a.options) {
            if (utf8Length(o) != 1 || !isJapanese(firstCp(o)) || !isKanji(firstCp(o))) continue;
            std::string t = text;
            t.replace((size_t)at, a.len, o);
            unsigned c = wordCover(dict, t, (size_t)at, o.size());
            if (c > pickCover) { pick = o; pickCover = c; }  // ties: the likelier guess (listed first)
        }
        if (pick.empty()) continue;
        text.replace((size_t)at, a.len, pick);
        shift += (long)pick.size() - (long)a.len;
        changed++;
    }
    return changed;
}

}  // namespace kt
