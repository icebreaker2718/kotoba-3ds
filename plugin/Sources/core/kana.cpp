#include "core/kana.hpp"

#include <cstring>

namespace kt {

uint32_t utf8Next(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { i += 1; return c; }
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
    if (n < 0 || i + n >= s.size()) { i += 1; return 0xFFFD; }  // stray/truncated sequence
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { i += 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += n + 1;
    return cp;
}

void utf8Append(std::string &out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}

std::vector<uint32_t> utf8Decode(const std::string &s) {
    std::vector<uint32_t> v;
    for (size_t i = 0; i < s.size();) v.push_back(utf8Next(s, i));
    return v;
}

size_t utf8Length(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

void utf8PopBack(std::string &s) {
    while (!s.empty()) {
        unsigned char c = (unsigned char)s.back();
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

bool isKanaCp(uint32_t cp) { return (cp >= 0x3041 && cp <= 0x30FF); }
bool isKanjiCp(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || cp == 0x3005 /* 々 */;
}

std::string normalizeKey(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8Next(s, i);
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
        utf8Append(out, cp);
    }
    return out;
}

namespace {
struct RomajiPair { const char *r; const char *k; };
// Longest entries are matched first (we try lengths 4..1).
const RomajiPair kTable[] = {
    // 4
    {"xtsu", "っ"}, {"ltsu", "っ"},
    // 3
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"}, {"kye", "きぇ"},
    {"sha", "しゃ"}, {"shi", "し"}, {"shu", "しゅ"}, {"she", "しぇ"}, {"sho", "しょ"},
    {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
    {"cha", "ちゃ"}, {"chi", "ち"}, {"chu", "ちゅ"}, {"che", "ちぇ"}, {"cho", "ちょ"},
    {"cya", "ちゃ"}, {"cyu", "ちゅ"}, {"cyo", "ちょ"},
    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"tsu", "つ"}, {"tsa", "つぁ"}, {"tsi", "つぃ"}, {"tse", "つぇ"}, {"tso", "つぉ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"lya", "りゃ"}, {"lyu", "りゅ"}, {"lyo", "りょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"},
    {"zya", "じゃ"}, {"zyu", "じゅ"}, {"zyo", "じょ"},
    {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"}, {"lya", "ゃ"},
    {"xtu", "っ"}, {"ltu", "っ"}, {"xwa", "ゎ"}, {"lwa", "ゎ"},
    {"thi", "てぃ"}, {"dhi", "でぃ"}, {"twu", "とぅ"}, {"dwu", "どぅ"},
    // 2
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"ca", "か"}, {"cu", "く"}, {"co", "こ"},
    {"sa", "さ"}, {"si", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"ta", "た"}, {"ti", "ち"}, {"tu", "つ"}, {"te", "て"}, {"to", "と"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"hu", "ふ"}, {"fu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"fa", "ふぁ"}, {"fi", "ふぃ"}, {"fe", "ふぇ"}, {"fo", "ふぉ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"}, {"ye", "いぇ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"la", "ら"}, {"li", "り"}, {"lu", "る"}, {"le", "れ"}, {"lo", "ろ"},
    {"wa", "わ"}, {"wi", "うぃ"}, {"we", "うぇ"}, {"wo", "を"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"za", "ざ"}, {"zi", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"ja", "じゃ"}, {"ji", "じ"}, {"ju", "じゅ"}, {"je", "じぇ"}, {"jo", "じょ"},
    {"da", "だ"}, {"di", "ぢ"}, {"du", "づ"}, {"de", "で"}, {"do", "ど"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"va", "ゔぁ"}, {"vi", "ゔぃ"}, {"vu", "ゔ"}, {"ve", "ゔぇ"}, {"vo", "ゔぉ"},
    {"xa", "ぁ"}, {"xi", "ぃ"}, {"xu", "ぅ"}, {"xe", "ぇ"}, {"xo", "ぉ"},
    {"n'", "ん"},
    // 1
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"}, {"-", "ー"},
};

bool isVowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }
bool isAsciiLetter(char c) { return (c >= 'a' && c <= 'z'); }
}  // namespace

std::string romajiToKana(const std::string &in, bool finalN) {
    // lowercase ASCII, keep other bytes untouched
    std::string s = in;
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';

    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {  // non-ASCII: copy the whole code point
            size_t j = i;
            uint32_t cp = utf8Next(s, j);
            utf8Append(out, cp);
            i = j;
            continue;
        }
        // sokuon: doubled consonant (kk, tt, ...) or "tch"
        if (i + 1 < s.size() && isAsciiLetter(s[i]) && !isVowel(s[i]) && s[i] != 'n' &&
            (s[i + 1] == s[i] || (s[i] == 't' && s[i + 1] == 'c'))) {
            out += "っ";
            i += 1;
            continue;
        }
        // "nn": ん, and the second n starts the next syllable if a vowel/y follows
        // (konnichiha -> こんにちは, onna -> おんな, shinnen -> しんねん)
        if (s[i] == 'n' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += "ん";
            bool nextStartsSyllable = i + 2 < s.size() && (isVowel(s[i + 2]) || s[i + 2] == 'y');
            i += nextStartsSyllable ? 1 : 2;
            continue;
        }
        bool matched = false;
        for (int len = 4; len >= 1 && !matched; len--) {
            if (i + len > s.size()) continue;
            for (const RomajiPair &p : kTable) {
                if ((int)strlen(p.r) == len && s.compare(i, len, p.r) == 0) {
                    out += p.k;
                    i += len;
                    matched = true;
                    break;
                }
            }
        }
        if (matched) continue;
        if (s[i] == 'n') {
            // "n" before a consonant (not y) -> ん; at the end -> ん only if finalN
            if (i + 1 < s.size()) {
                char nx = s[i + 1];
                if (!isVowel(nx) && nx != 'y') { out += "ん"; i++; continue; }
            } else if (finalN) { out += "ん"; i++; continue; }
        }
        out += s[i];  // incomplete or unknown: keep as typed
        i++;
    }
    return out;
}

}  // namespace kt
