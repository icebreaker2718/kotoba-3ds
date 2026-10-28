#include "core/textlines.hpp"

#include <algorithm>

#include "core/kana.hpp"

namespace kt {

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

static bool hasKatakana(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = utf8Next(s, i);
        if (cp >= 0x30A1 && cp <= 0x30FA) return true;  // ァ..ヺ (not ー, which hiragana words use too)
    }
    return false;
}

static size_t countKanji(const std::string &s) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        uint32_t cp = utf8Next(s, i);
        if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || cp == 0x3005) n++;
    }
    return n;
}

std::vector<std::string> textParagraphs(const std::vector<TextLine> &lines, bool keepBreaks) {
    // typical text height: median over lines with kanji or long lines
    std::vector<float> hs;
    for (const TextLine &l : lines)
        if (l.height > 0 && !kanaOnly(l.text)) hs.push_back(l.height);
    if (hs.empty())
        for (const TextLine &l : lines) if (l.height > 0) hs.push_back(l.height);
    float typical = 0;
    if (!hs.empty()) {
        std::sort(hs.begin(), hs.end());
        typical = hs[hs.size() / 2];
    }
    std::vector<std::string> out;
    int current = -1;
    const TextLine *prev = nullptr;
    for (size_t li = 0; li < lines.size(); li++) {
        const TextLine &l = lines[li];
        bool furigana = typical > 0 && l.height > 0 && l.height < typical * 0.7f && kanaOnly(l.text) &&
                        utf8Length(l.text) <= 10;
        // Or: a short hiragana-only line (no punctuation) right above a line with enough kanji to
        // be its reading (its box can be as tall as a text line when it takes in the box's frame).
        // Not with katakana: a line of names over the next row (「クリスとジェシカが」) is text.
        if (!furigana && kanaOnly(l.text) && !hasKatakana(l.text) && li + 1 < lines.size()) {
            const TextLine &next = lines[li + 1];
            size_t kana = utf8Length(l.text), kanji = countKanji(next.text);
            float gap = next.centerY - l.centerY;
            furigana = kanji > 0 && kana <= 3 * kanji && kana <= 12 && gap > 0 &&
                       gap < 1.2f * (next.height > 0 ? next.height : l.height);
        }
        if (furigana) continue;
        if (l.paragraph != current || out.empty()) {
            // a new block that is really the next line of the same text box?
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
        if (!p.empty() && keepBreaks) p += '\n';
        else if (!p.empty() && (unsigned char)p.back() < 0x80 && (unsigned char)l.text[0] < 0x80) p += ' ';
        p += l.text;
    }
    out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
    return out;
}

static bool isFinalPunct(uint32_t cp) {
    return cp == 0xFF01 || cp == '!' || cp == 0xFF1F || cp == '?' || cp == 0x3002 || cp == 0x2026 ||
           cp == 0x300D || cp == 0x300F;  // ！ ! ？ ? 。 … 」 』
}

static unsigned japaneseChars(const std::string &s) {
    size_t i = 0;
    unsigned n = 0;
    while (i < s.size()) {
        uint32_t cp = utf8Next(s, i);
        if ((cp >= 0x3041 && cp <= 0x30FA) || (cp >= 0x3400 && cp <= 0x9FFF) || cp == 0x3005) n++;
    }
    return n;
}

void cleanParagraphs(std::vector<std::string> &paras) {
    for (std::string &p : paras) {
        // code points and their byte offsets
        std::vector<uint32_t> cps;
        std::vector<size_t> at;
        for (size_t i = 0; i < p.size();) {
            at.push_back(i);
            cps.push_back(utf8Next(p, i));
        }
        size_t lastPunct = cps.size();
        for (size_t k = cps.size(); k-- > 0;)
            if (isFinalPunct(cps[k])) { lastPunct = k; break; }
        if (lastPunct >= cps.size() || lastPunct + 1 >= cps.size()) continue;
        // the tail after it, ignoring 、。, : 1-2 characters with katakana or kanji = a speck
        unsigned n = 0;
        bool kataOrKanji = false, ascii = false;
        for (size_t k = lastPunct + 1; k < cps.size(); k++) {
            uint32_t c = cps[k];
            if (c == 0x3001 || c == 0x3002 || c == ',' || c == '.' || c == '\n') continue;
            n++;
            if ((c >= 0x30A1 && c <= 0x30FA) || (c >= 0x3400 && c <= 0x9FFF)) kataOrKanji = true;
            if (c < 0x80) ascii = true;
        }
        if (n >= 1 && n <= 2 && kataOrKanji && !ascii) p.erase(at[lastPunct + 1]);
    }
    // drop near-empty blocks when there is real text
    unsigned best = 0;
    for (const std::string &p : paras) best = std::max(best, japaneseChars(p));
    if (best >= 6) {
        std::vector<std::string> kept;
        for (const std::string &p : paras)
            if (japaneseChars(p) >= 3) kept.push_back(p);
        paras.swap(kept);
    }
}

}  // namespace kt
