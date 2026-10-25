#include "core/search.hpp"

#include <algorithm>

#include "core/deinflect.hpp"
#include "core/kana.hpp"

namespace kt {

static bool endsWith(const std::string &s, const std::string &suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static bool hasResult(const std::vector<Result> &v, uint32_t id) {
    for (const Result &r : v) if (r.entryId == id) return true;
    return false;
}

SearchOutput search(Dictionary &dict, const std::string &raw, uint32_t maxResults) {
    SearchOutput so;
    // trim spaces
    std::string trimmed;
    for (char c : raw) if (c != ' ' && c != '\t') trimmed += c;
    so.query = normalizeKey(romajiToKana(trimmed, true));
    if (so.query.empty() || !dict.isOpen()) return so;

    // 1) exact + deinflected matches
    std::vector<Result> exactHits, deinflHits;
    std::vector<Candidate> cands = deinflect(so.query);
    for (const Candidate &c : cands) {
        std::vector<uint32_t> ids;
        dict.exact(c.text, ids);
        for (uint32_t id : ids) {
            if (c.type != 0 && (dict.posMask(id) & c.type) == 0) continue;
            Result r;
            r.entryId = id;
            r.matchedKey = c.text;
            r.reasons = describeReasons(c.reasons);
            std::vector<Result> &dst = c.type == 0 ? exactHits : deinflHits;
            if (!hasResult(exactHits, id) && !hasResult(deinflHits, id)) dst.push_back(r);
        }
        // する-verbs: 勉強した -> 勉強する -> 勉強 (noun that takes する)
        if ((c.type & POS_VSI) && endsWith(c.text, "する") && c.text.size() > 6) {
            std::string noun = c.text.substr(0, c.text.size() - 6);
            std::vector<uint32_t> nids;
            dict.exact(noun, nids);
            for (uint32_t id : nids) {
                if ((dict.posMask(id) & POS_VSN) == 0) continue;
                if (hasResult(exactHits, id) || hasResult(deinflHits, id)) continue;
                Result r;
                r.entryId = id;
                r.matchedKey = noun;
                r.reasons = std::string("suru verb") +
                            (c.reasons.empty() ? "" : " > " + describeReasons(c.reasons));
                deinflHits.push_back(r);
            }
        }
    }
    // entry ids are already in "most common first" order; keep deinflection order stable
    std::stable_sort(exactHits.begin(), exactHits.end(),
                     [](const Result &a, const Result &b) { return a.entryId < b.entryId; });
    std::stable_sort(deinflHits.begin(), deinflHits.end(),
                     [](const Result &a, const Result &b) { return a.entryId < b.entryId; });
    so.results = exactHits;
    so.results.insert(so.results.end(), deinflHits.begin(), deinflHits.end());

    // 2) words starting with the query
    if (so.results.size() < maxResults) {
        std::vector<std::pair<uint32_t, std::string>> pre;
        dict.prefix(so.query, 400, pre);
        std::stable_sort(pre.begin(), pre.end(),
                         [](const std::pair<uint32_t, std::string> &a, const std::pair<uint32_t, std::string> &b) {
                             if (a.second.size() != b.second.size()) return a.second.size() < b.second.size();
                             return a.first < b.first;
                         });
        for (auto &p : pre) {
            if (so.results.size() >= maxResults) break;
            if (hasResult(so.results, p.first)) continue;
            Result r;
            r.entryId = p.first;
            r.matchedKey = p.second;
            r.prefixOnly = true;
            so.results.push_back(r);
        }
    }
    if (so.results.size() > maxResults) so.results.resize(maxResults);
    return so;
}

static bool isStopChar(uint32_t cp) {
    // punctuation, brackets, spaces: a word never continues across these
    if (cp < 0x80) return !((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'));
    if (cp >= 0x3000 && cp <= 0x303F && cp != 0x3005) return true;   // 、。「」etc. (keep 々)
    if (cp == 0x30FB) return true;                                    // ・
    if (cp >= 0xFF01 && cp <= 0xFF0F) return true;                    // ！＂＃…／
    if (cp >= 0xFF1A && cp <= 0xFF20) return true;                    // ：；＜＝＞？＠
    if (cp == 0x2026 || cp == 0x2025 || cp == 0x2015 || cp == 0x2014) return true;  // … ‥ ― —
    return false;
}

SearchOutput scanAt(Dictionary &dict, const std::string &text, size_t start, uint32_t maxChars,
                    uint32_t maxResults) {
    SearchOutput so;
    if (!dict.isOpen() || start >= text.size()) return so;
    // collect up to maxChars code points (byte end offsets) before a stop character
    std::vector<size_t> ends;
    for (size_t i = start; i < text.size() && ends.size() < maxChars;) {
        size_t j = i;
        uint32_t cp = utf8Next(text, j);
        if (isStopChar(cp)) break;
        ends.push_back(j);
        i = j;
    }
    // How far the text here can be a dictionary key at all: dead is the shortest start of it that
    // no key begins with (empty: none). A candidate that begins with it can't be a key, so its
    // search (a few reads from the SD card) is skipped; the results are the same.
    std::string dead;
    {
        std::string prev;
        for (size_t n = 1; n <= ends.size(); n++) {
            std::string k = normalizeKey(text.substr(start, ends[n - 1] - start));
            if (k.compare(0, prev.size(), prev) != 0) break;  // (normalizing is per character: never)
            if (!dict.hasPrefix(k)) { dead = k; break; }
            prev = k;
        }
    }
    auto impossible = [&](const std::string &c) { return !dead.empty() && c.compare(0, dead.size(), dead) == 0; };
    for (size_t n = ends.size(); n > 0 && so.results.size() < maxResults; n--) {
        std::string surface = text.substr(start, ends[n - 1] - start);
        std::string key = normalizeKey(surface);
        std::vector<Result> exactHits, deinflHits;
        for (const Candidate &c : deinflect(key)) {
            std::vector<uint32_t> ids;
            if (!impossible(c.text)) dict.exact(c.text, ids);
            for (uint32_t id : ids) {
                if (c.type != 0 && (dict.posMask(id) & c.type) == 0) continue;
                if (hasResult(so.results, id) || hasResult(exactHits, id) || hasResult(deinflHits, id)) continue;
                Result r;
                r.entryId = id;
                r.matchedKey = c.text;
                r.reasons = describeReasons(c.reasons);
                r.surface = surface;
                (c.type == 0 ? exactHits : deinflHits).push_back(r);
            }
            if ((c.type & POS_VSI) && endsWith(c.text, "する") && c.text.size() > 6) {
                std::string noun = c.text.substr(0, c.text.size() - 6);
                std::vector<uint32_t> nids;
                if (!impossible(noun)) dict.exact(noun, nids);
                for (uint32_t id : nids) {
                    if ((dict.posMask(id) & POS_VSN) == 0) continue;
                    if (hasResult(so.results, id) || hasResult(exactHits, id) || hasResult(deinflHits, id)) continue;
                    Result r;
                    r.entryId = id;
                    r.matchedKey = noun;
                    r.reasons = std::string("suru verb") + (c.reasons.empty() ? "" : " > " + describeReasons(c.reasons));
                    r.surface = surface;
                    deinflHits.push_back(r);
                }
            }
        }
        if (so.query.empty() && (!exactHits.empty() || !deinflHits.empty())) so.query = surface;
        std::stable_sort(exactHits.begin(), exactHits.end(),
                         [](const Result &a, const Result &b) { return a.entryId < b.entryId; });
        std::stable_sort(deinflHits.begin(), deinflHits.end(),
                         [](const Result &a, const Result &b) { return a.entryId < b.entryId; });
        so.results.insert(so.results.end(), exactHits.begin(), exactHits.end());
        so.results.insert(so.results.end(), deinflHits.begin(), deinflHits.end());
    }
    if (so.results.size() > maxResults) so.results.resize(maxResults);
    if (so.query.empty()) so.query = text.substr(start, ends.empty() ? 0 : ends[0] - start);
    return so;
}

static size_t longestWordAt(Dictionary &dict, const std::string &text, size_t i, uint32_t maxChars) {
    SearchOutput so = scanAt(dict, text, i, maxChars, 1);
    size_t len = 0;
    for (const Result &r : so.results)
        if (!r.prefixOnly && r.surface.size() > len) len = r.surface.size();
    return i + len <= text.size() ? len : 0;
}

std::vector<std::string> chunkText(Dictionary &dict, const std::string &text, uint32_t maxChars) {
    static const char *const kParticles[] = {"が", "は", "を", "に", "へ", "で", "と", "も", "の", "や"};
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t next = i;
        utf8Next(text, next);
        size_t len = longestWordAt(dict, text, i, maxChars);  // longest dictionary word here
        // a particle glued to the next word (が+ある -> があ, と+ケーキ -> とケ): split it off when the
        // word after the particle reaches further
        if (len > 0) {
            std::string first = text.substr(i, next - i);
            bool particle = false;
            for (const char *p : kParticles) particle = particle || first == p;
            if (particle && next < text.size()) {
                size_t after = longestWordAt(dict, text, next, maxChars);
                if (after > 0 && next + after > i + len) len = next - i;
            }
        }
        size_t end = len > 0 ? i + len : next;
        out.push_back(text.substr(i, end - i));
        i = end;
    }
    return out;
}

std::string expressionFor(const Entry &e, const std::string &matchedKey) {
    for (const std::string &k : e.kanjiForms)
        if (normalizeKey(k) == matchedKey) return k;
    return e.expression;
}

std::string cardExpressionFor(const Entry &e, const std::string &matchedKey) {
    std::string seen = expressionFor(e, matchedKey);
    for (const std::string &k : e.kanjiForms)
        if (k == seen) return seen;      // looked up in kanji already: keep that spelling
    if (!e.kanjiForms.empty()) return e.kanjiForms[0];  // the word's main kanji spelling
    return seen;
}

static std::string truncateUtf8(const std::string &s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    return s.substr(0, cut) + "…";
}

static std::string stripSenseNumber(const std::string &s) {
    // "1. (v1,vt) to eat" -> "to eat"
    size_t p = s.find(". ");
    std::string t = p != std::string::npos && p < 4 ? s.substr(p + 2) : s;
    if (!t.empty() && t[0] == '(') {
        size_t q = t.find(") ");
        if (q != std::string::npos) t = t.substr(q + 2);
    }
    return t;
}

std::string summaryLine(const Entry &e, const Result &r, size_t maxBytes) {
    std::string expr = expressionFor(e, r.matchedKey);
    std::string s = expr;
    if (e.reading != expr) s += "【" + e.reading + "】";
    s += " ";
    for (size_t i = 0; i < e.senses.size() && i < 3; i++) {
        if (i) s += "; ";
        s += stripSenseNumber(e.senses[i]);
    }
    return truncateUtf8(s, maxBytes);
}

std::string detailText(const Entry &e, const Result &r) {
    std::string s = e.headline + "\n";
    if (!r.reasons.empty()) s += "(" + r.reasons + ")\n";
    for (const std::string &l : e.senses) s += l + "\n";
    return s;
}

std::string ankiDefinition(const Entry &e) {
    std::string s;
    for (const std::string &l : e.senses) {
        if (!s.empty()) s += "<br>";
        s += l;
    }
    return s;
}

static std::string clean(const std::string &s) {
    std::string o;
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

std::string mineLogLine(const std::string &timestamp, const std::string &titleId, const Entry &e,
                        const Result &r, const std::string &screenshot, const std::string &query,
                        const std::string &sentence) {
    std::string expr = cardExpressionFor(e, r.matchedKey);  // kanji even if seen in kana
    std::string cols[] = {timestamp, titleId, expr, e.reading, ankiDefinition(e), screenshot,
                          query, r.reasons, std::to_string(e.id), sentence};
    std::string line;
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        if (i) line += '\t';
        line += clean(cols[i]);
    }
    return line;
}

}  // namespace kt
