#include "core/known_words.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace kt {

KnownWords::~KnownWords() { clear(); }

void KnownWords::clear() {
    free(pool_);
    free(offs_);
    pool_ = nullptr;
    offs_ = nullptr;
    count_ = 0;
    truncated_ = false;
    extra_.clear();
}

bool KnownWords::load(const char *text, size_t len, size_t maxBytes) {
    clear();
    if (len > maxBytes) len = maxBytes;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return false;
    memcpy(buf, text, len);
    return adopt(buf, len, len < maxBytes ? len : len + 1);
}

bool KnownWords::adopt(char *buf, size_t len, size_t fileSize) {
    clear();
    if (!buf) return false;
    pool_ = buf;
    if (fileSize > len) {  // cut short: whole lines only
        truncated_ = true;
        while (len > 0 && buf[len - 1] != '\n') len--;
    }
    buf[len] = '\0';
    size_t lines = 1;
    for (size_t i = 0; i < len; i++) lines += buf[i] == '\n';
    offs_ = (uint32_t *)malloc(lines * sizeof(uint32_t));
    if (!offs_) {
        clear();
        return false;
    }
    for (size_t s = 0; s < len;) {  // each line ends in '\0' where it is
        size_t e = s;
        while (e < len && buf[e] != '\n') e++;
        size_t a = s, b = e;
        while (a < b && (buf[a] == ' ' || buf[a] == '\t')) a++;
        while (b > a && (buf[b - 1] == '\r' || buf[b - 1] == ' ' || buf[b - 1] == '\t')) b--;
        buf[b] = '\0';
        if (b > a && buf[a] != '#') offs_[count_++] = (uint32_t)a;
        s = e + 1;
    }
    const char *pool = pool_;
    // the PC's list comes sorted already
    if (!std::is_sorted(offs_, offs_ + count_, [pool](uint32_t x, uint32_t y) { return strcmp(pool + x, pool + y) < 0; }))
        std::sort(offs_, offs_ + count_, [pool](uint32_t x, uint32_t y) { return strcmp(pool + x, pool + y) < 0; });
    count_ = std::unique(offs_, offs_ + count_, [pool](uint32_t x, uint32_t y) { return !strcmp(pool + x, pool + y); }) - offs_;
    return true;
}

void KnownWords::add(const std::string &word) {
    if (word.empty() || has(word)) return;
    extra_.insert(std::lower_bound(extra_.begin(), extra_.end(), word), word);
}

bool KnownWords::has(const std::string &word) const {
    if (word.empty()) return false;
    const char *w = word.c_str(), *pool = pool_;
    const uint32_t *it = std::lower_bound(offs_, offs_ + count_, w, [pool](uint32_t x, const char *v) { return strcmp(pool + x, v) < 0; });
    if (it != offs_ + count_ && !strcmp(pool + *it, w)) return true;
    return std::binary_search(extra_.begin(), extra_.end(), word);
}

}  // namespace kt
