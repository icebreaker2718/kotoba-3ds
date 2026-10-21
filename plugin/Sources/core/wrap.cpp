#include "core/wrap.hpp"

namespace kt {

static size_t nextChar(const std::string &s, size_t i) {
    size_t j = i + 1;
    while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
    return j;
}

static void wrapParagraph(const std::string &para, float maxWidth, TextWidthFn width,
                          std::vector<std::string> &out) {
    std::string line;
    for (size_t i = 0; i < para.size();) {
        size_t j = nextChar(para, i);
        std::string ch = para.substr(i, j - i);
        i = j;
        if (line.empty() && ch == " ") continue;  // no leading spaces on a wrapped line
        if (line.empty() || width(line + ch) <= maxWidth) {
            line += ch;
            continue;
        }
        // `ch` doesn't fit: break the line
        if (ch == " ") {  // break exactly at this space
            out.push_back(line);
            line.clear();
            continue;
        }
        size_t sp = line.rfind(' ');
        if (sp != std::string::npos && sp > 0 && sp >= line.size() / 3) {
            out.push_back(line.substr(0, sp));     // break at the last space
            line = line.substr(sp + 1) + ch;       // sp < line.size(), so sp + 1 <= size: safe
        } else {
            out.push_back(line);                   // no good space (e.g. Japanese): hard break
            line = ch;
        }
    }
    if (!line.empty()) out.push_back(line);
}

std::vector<std::string> wrapText(const std::string &text, float maxWidth, TextWidthFn width) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        size_t before = out.size();
        wrapParagraph(para, maxWidth, width, out);
        if (out.size() == before && nl != std::string::npos) out.push_back("");  // keep blank lines
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

}  // namespace kt
