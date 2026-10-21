#include "core/deinflect.hpp"

#include <cstring>

#include "core/dict.hpp"
#include "core/kana.hpp"

namespace kt {
namespace {

struct Rule {
    std::string from, to;
    uint32_t in;   // word class(es) the inflected form may have; 0 = only the original input
    uint32_t out;  // word class of the result
    const char *reason;
};

const uint32_t V1 = POS_V1, V5 = POS_V5, VK = POS_VK, VS = POS_VSI, ADJ = POS_ADJ, NA = POS_ADJNA;

// Never destroyed on purpose: freeing at game exit can crash the plugin (see main.cpp).
std::vector<Rule> *gRulesPtr = nullptr;
#define gRules (*gRulesPtr)

void add(const std::string &from, const std::string &to, uint32_t in, uint32_t out, const char *reason) {
    Rule r;
    r.from = from;
    r.to = to;
    r.in = in;
    r.out = out;
    r.reason = reason;
    gRules.push_back(r);
}

// "Verb-like" suffix set, written relative to a stem: used for ichidan (stem = "" + る),
// suru (し/さ/す…) and kuru, which share most auxiliaries.
struct Aux { const char *suffix; uint32_t in; const char *reason; };

void buildRules() {
    if (gRulesPtr) return;
    gRulesPtr = new std::vector<Rule>();

    // ---------------- godan ----------------
    struct G { const char *u, *a, *i, *e, *o, *te, *ta; };
    static const G godan[] = {
        {"く", "か", "き", "け", "こ", "いて", "いた"}, {"ぐ", "が", "ぎ", "げ", "ご", "いで", "いだ"},
        {"す", "さ", "し", "せ", "そ", "して", "した"}, {"つ", "た", "ち", "て", "と", "って", "った"},
        {"ぬ", "な", "に", "ね", "の", "んで", "んだ"}, {"ぶ", "ば", "び", "べ", "ぼ", "んで", "んだ"},
        {"む", "ま", "み", "め", "も", "んで", "んだ"}, {"る", "ら", "り", "れ", "ろ", "って", "った"},
        {"う", "わ", "い", "え", "お", "って", "った"},
        {"く", "か", "き", "け", "こ", "って", "った"},  // 行く: 行って / 行った
    };
    for (const G &g : godan) {
        std::string u = g.u, a = g.a, i = g.i, e = g.e, o = g.o, te = g.te, ta = g.ta;
        std::string teStem = te.substr(0, te.size() - 3);  // drop て/で (3 bytes)
        bool voiced = te.compare(te.size() - 3, 3, "で") == 0;
        bool isIku = (&g == &godan[9]);
        if (!isIku) {
            add(a + "ない", u, ADJ, V5, "negative");
            add(a + "ないで", u, 0, V5, "negative te-form");
            add(a + "ず", u, 0, V5, "negative (zu)");
            add(a + "ずに", u, 0, V5, "without doing");
            add(a + "ぬ", u, 0, V5, "negative (nu)");
            add(a + "ん", u, 0, V5, "negative (n)");
            add(i + "ます", u, 0, V5, "polite");
            add(i + "ません", u, 0, V5, "polite negative");
            add(i + "ました", u, 0, V5, "polite past");
            add(i + "ませんでした", u, 0, V5, "polite past negative");
            add(i + "ましょう", u, 0, V5, "polite volitional");
            add(i + "まして", u, 0, V5, "polite te-form");
            add(e + "ば", u, 0, V5, "conditional (ba)");
            add(o + "う", u, 0, V5, "volitional");
            add(e, u, 0, V5, "imperative");
            add(a + "れる", u, V1, V5, "passive");
            add(a + "せる", u, V1, V5, "causative");
            add(a + "す", u, V5, V5, "causative (short)");
            if (u != "す") add(a + "される", u, V1, V5, "causative passive");
            add(e + "る", u, V1, V5, "potential");
            add(i + "たい", u, ADJ, V5, "want to");
            add(i + "そう", u, 0, V5, "looks like");
            add(i + "すぎる", u, V1, V5, "too much");
            add(i + "なさい", u, 0, V5, "polite command");
            add(i + "ながら", u, 0, V5, "while");
            add(i + "に", u, 0, V5, "in order to");
            add(i, u, 0, V5, "stem");
        }
        add(te, u, 0, V5, "te-form");
        add(ta, u, 0, V5, "past");
        add(ta + "ら", u, 0, V5, "conditional (tara)");
        add(ta + "り", u, 0, V5, "tari");
        add(te + "いる", u, V1, V5, "te iru");
        add(te + "る", u, V1, V5, "te iru (contracted)");
        add(te + "しまう", u, V5, V5, "te shimau");
        add(teStem + (voiced ? "じゃう" : "ちゃう"), u, V5, V5, "te shimau (chau)");
        add(te + "おく", u, V5, V5, "te oku");
        add(teStem + (voiced ? "どく" : "とく"), u, V5, V5, "te oku (toku)");
    }

    // ---------------- ichidan / suru / kuru share the same auxiliaries ----------------
    static const Aux aux[] = {
        {"ない", ADJ, "negative"}, {"ないで", 0, "negative te-form"}, {"ず", 0, "negative (zu)"},
        {"ずに", 0, "without doing"}, {"ぬ", 0, "negative (nu)"}, {"ん", 0, "negative (n)"},
        {"ます", 0, "polite"}, {"ません", 0, "polite negative"}, {"ました", 0, "polite past"},
        {"ませんでした", 0, "polite past negative"}, {"ましょう", 0, "polite volitional"},
        {"まして", 0, "polite te-form"},
        {"て", 0, "te-form"}, {"た", 0, "past"}, {"たら", 0, "conditional (tara)"}, {"たり", 0, "tari"},
        {"たい", ADJ, "want to"}, {"そう", 0, "looks like"}, {"すぎる", V1, "too much"},
        {"なさい", 0, "polite command"}, {"ながら", 0, "while"}, {"に", 0, "in order to"},
        {"ている", V1, "te iru"}, {"てる", V1, "te iru (contracted)"},
        {"てしまう", V5, "te shimau"}, {"ちゃう", V5, "te shimau (chau)"},
        {"ておく", V5, "te oku"}, {"とく", V5, "te oku (toku)"},
    };

    // ichidan: stem + る
    for (const Aux &x : aux) add(x.suffix, "る", x.in, V1, x.reason);
    add("れば", "る", 0, V1, "conditional (ba)");
    add("よう", "る", 0, V1, "volitional");
    add("ろ", "る", 0, V1, "imperative");
    add("よ", "る", 0, V1, "imperative");
    add("られる", "る", V1, V1, "passive / potential");
    add("れる", "る", V1, V1, "potential (ra-nuki)");
    add("させる", "る", V1, V1, "causative");
    add("さす", "る", V5, V1, "causative (short)");
    add("", "る", 0, V1, "stem");

    // suru: し + aux, plus irregular forms
    for (const Aux &x : aux) add(std::string("し") + x.suffix, "する", x.in, VS, x.reason);
    add("せず", "する", 0, VS, "negative (zu)");
    add("せずに", "する", 0, VS, "without doing");
    add("せぬ", "する", 0, VS, "negative (nu)");
    add("すれば", "する", 0, VS, "conditional (ba)");
    add("しよう", "する", 0, VS, "volitional");
    add("しろ", "する", 0, VS, "imperative");
    add("せよ", "する", 0, VS, "imperative");
    add("される", "する", V1, VS, "passive");
    add("させる", "する", V1, VS, "causative");
    add("し", "する", 0, VS, "stem");

    // kuru: kana (こ/き/く) and kanji (来) spellings
    for (int pass = 0; pass < 2; pass++) {
        std::string base = pass ? "来る" : "くる";
        std::string ki = pass ? "来" : "き", ko = pass ? "来" : "こ", ku = pass ? "来" : "く";
        for (const Aux &x : aux) {
            std::string s = x.suffix;
            bool neg = (s == "ない" || s == "ないで" || s == "ず" || s == "ずに" || s == "ぬ" || s == "ん");
            add((neg ? ko : ki) + s, base, x.in, VK, x.reason);
        }
        add(ku + "れば", base, 0, VK, "conditional (ba)");
        add(ko + "よう", base, 0, VK, "volitional");
        add(ko + "い", base, 0, VK, "imperative");
        add(ko + "られる", base, V1, VK, "passive / potential");
        add(ko + "れる", base, V1, VK, "potential (ra-nuki)");
        add(ko + "させる", base, V1, VK, "causative");
    }

    // i-adjectives
    add("く", "い", 0, ADJ, "adverbial");
    add("くない", "い", ADJ, ADJ, "negative");
    add("かった", "い", 0, ADJ, "past");
    add("かったら", "い", 0, ADJ, "conditional (tara)");
    add("くて", "い", 0, ADJ, "te-form");
    add("ければ", "い", 0, ADJ, "conditional (ba)");
    add("きゃ", "い", 0, ADJ, "conditional (colloquial)");
    add("くちゃ", "い", 0, ADJ, "conditional (colloquial)");
    add("さ", "い", 0, ADJ, "noun (-sa)");
    add("そう", "い", 0, ADJ, "looks like");
    add("すぎる", "い", V1, ADJ, "too much");
    add("かろう", "い", 0, ADJ, "volitional");
    add("き", "い", 0, ADJ, "attributive (archaic)");
    add("くなる", "い", V5, ADJ, "become");

    // honorific godan verbs (-aru class) have an imperative in い: ください, なさい, いらっしゃい
    add("さい", "さる", 0, V5, "imperative (honorific)");
    add("ゃい", "ゃる", 0, V5, "imperative (honorific)");

    // na-adjectives (copula / particles)
    static const char *naSuffix[] = {"な", "に", "だ", "で", "です", "だった", "でした",
                                     "じゃない", "ではない", "だろう", "でしょう", "なら"};
    for (const char *s : naSuffix) add(s, "", 0, NA, "na-adjective");
}

bool endsWith(const std::string &s, const std::string &suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

}  // namespace

std::vector<Candidate> deinflect(const std::string &word) {
    buildRules();
    std::vector<Candidate> out;
    Candidate c0;
    c0.text = word;
    out.push_back(c0);
    for (size_t idx = 0; idx < out.size() && out.size() < 400; idx++) {
        Candidate cur = out[idx];  // copy: out may reallocate
        if (cur.reasons.size() >= 7) continue;
        for (const Rule &r : gRules) {
            if (cur.type != 0 && (cur.type & r.in) == 0) continue;
            if (cur.type == 0 && cur.reasons.size() > 0) continue;
            if (!endsWith(cur.text, r.from)) continue;
            if (r.from.empty() && r.to.empty()) continue;
            std::string t = cur.text.substr(0, cur.text.size() - r.from.size()) + r.to;
            // e.g. "ない" -> "る": no verb/adjective is one character long (but 変な -> 変 is fine)
            if (t.empty() || (utf8Length(t) < 2 && r.out != NA)) continue;
            bool dup = false;
            for (const Candidate &o : out)
                if (o.text == t && o.type == r.out) { dup = true; break; }
            if (dup) continue;
            Candidate n;
            n.text = t;
            n.type = r.out;
            n.reasons = cur.reasons;
            n.reasons.push_back(r.reason);
            out.push_back(n);
        }
    }
    return out;
}

std::string describeReasons(const std::vector<const char *> &reasons) {
    std::string s;
    for (size_t i = reasons.size(); i-- > 0;) {
        if (!s.empty()) s += " > ";
        s += reasons[i];
    }
    return s;
}

}  // namespace kt
