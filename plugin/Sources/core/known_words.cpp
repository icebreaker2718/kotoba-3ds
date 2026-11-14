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
    file_ = nullptr;
    fileSize_ = 0;
    fileCount_ = 0;
    std::vector<uint32_t>().swap(blockStart_);
    std::vector<uint32_t>().swap(keyOff_);
    std::string().swap(keys_);
    std::vector<std::string>().swap(spill_);
    spillDropped_ = 0;
    std::vector<char>().swap(block_);
    blockIdx_ = -1;
}

namespace {
// A line's word: spaces and a '\r' trimmed; empty for a blank line or a '#' comment.
void lineWord(const char *s, size_t n, const char *&w, size_t &wn) {
    size_t a = 0, b = n;
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == '\r' || s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    w = s + a;
    wn = (b > a && s[a] != '#') ? b - a : 0;
}
// strcmp of (a, an) and the '\0'-ended b, by unsigned bytes
int cmpWord(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    int c = memcmp(a, b, an < bn ? an : bn);
    if (c) return c;
    return an < bn ? -1 : an > bn ? 1 : 0;
}
}  // namespace

bool KnownWords::openFile(Reader *r, size_t blockBytes) {
    clear();
    if (!r) return false;
    const uint32_t size = r->size();
    std::vector<char> chunk(4096);
    std::string carry, prev;  // a line cut by the chunk's end; the last word in order
    bool havePrev = false;
    uint32_t lineStart = 0, maxBlock = 0;
    auto line = [&](const char *s, size_t n, uint32_t at) {
        const char *w;
        size_t wn;
        lineWord(s, n, w, wn);
        if (!wn) return;
        if (havePrev) {
            const int c = cmpWord(w, wn, prev.c_str());
            if (c == 0) return;  // the same word again
            if (c < 0) {         // out of order: in memory
                if (spill_.size() < kMaxSpill) spill_.emplace_back(w, wn);
                else spillDropped_++;
                return;
            }
        }
        prev.assign(w, wn);
        havePrev = true;
        fileCount_++;
        if (blockStart_.empty() || at - blockStart_.back() >= blockBytes) {
            if (!blockStart_.empty()) maxBlock = std::max(maxBlock, at - blockStart_.back());
            blockStart_.push_back(at);
            keyOff_.push_back((uint32_t)keys_.size());
            keys_.append(w, wn);
            keys_.push_back('\0');
        }
    };
    for (uint32_t off = 0; off < size;) {
        const uint32_t n = std::min<uint32_t>((uint32_t)chunk.size(), size - off);
        if (!r->read(off, chunk.data(), n)) { clear(); return false; }
        size_t s = 0;
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != '\n') continue;
            if (!carry.empty()) {
                carry.append(chunk.data() + s, i - s);
                line(carry.data(), carry.size(), lineStart);
                carry.clear();
            } else {
                line(chunk.data() + s, i - s, lineStart);
            }
            s = i + 1;
            lineStart = off + (uint32_t)s;
        }
        carry.append(chunk.data() + s, n - s);
        off += n;
    }
    if (!carry.empty()) line(carry.data(), carry.size(), lineStart);
    if (!blockStart_.empty()) maxBlock = std::max(maxBlock, size - blockStart_.back());
    std::sort(spill_.begin(), spill_.end());
    spill_.erase(std::unique(spill_.begin(), spill_.end()), spill_.end());
    blockStart_.shrink_to_fit();
    keyOff_.shrink_to_fit();
    keys_.shrink_to_fit();
    spill_.shrink_to_fit();
    block_.resize(maxBlock);
    file_ = r;
    fileSize_ = size;
    return true;
}

bool KnownWords::hasInFile(const char *w) const {
    if (!file_ || blockStart_.empty()) return false;
    // the last block whose first word is <= w
    size_t lo = 0, hi = blockStart_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (strcmp(keys_.c_str() + keyOff_[mid], w) <= 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return false;
    const size_t b = lo - 1;
    const uint32_t start = blockStart_[b], end = b + 1 < blockStart_.size() ? blockStart_[b + 1] : fileSize_;
    if (end - start > block_.size()) return false;
    if (blockIdx_ != (int)b) {
        blockIdx_ = -1;
        if (!file_->read(start, block_.data(), end - start)) return false;
        blockIdx_ = (int)b;
    }
    const char *p = block_.data(), *e = p + (end - start);
    while (p < e) {
        const char *nl = (const char *)memchr(p, '\n', e - p);
        const char *le = nl ? nl : e;
        const char *lw;
        size_t ln;
        lineWord(p, le - p, lw, ln);
        if (ln && cmpWord(lw, ln, w) == 0) return true;
        p = le + 1;
    }
    return false;
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

void KnownWords::remove(const std::string &word) {
    auto it = std::lower_bound(extra_.begin(), extra_.end(), word);
    if (it != extra_.end() && *it == word) extra_.erase(it);
}

bool KnownWords::has(const std::string &word) const {
    if (word.empty()) return false;
    const char *w = word.c_str(), *pool = pool_;
    if (count_) {
        const uint32_t *it = std::lower_bound(offs_, offs_ + count_, w, [pool](uint32_t x, const char *v) { return strcmp(pool + x, v) < 0; });
        if (it != offs_ + count_ && !strcmp(pool + *it, w)) return true;
    }
    if (std::binary_search(extra_.begin(), extra_.end(), word)) return true;
    if (std::binary_search(spill_.begin(), spill_.end(), word)) return true;
    return hasInFile(w);
}

}  // namespace kt
