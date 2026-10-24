#include "core/dict.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "core/kana.hpp"

namespace kt {

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool Dictionary::open(Reader *r, std::string &error) {
    reader_ = r;
    size_ = r->size();
    cache_.assign(kBlocks, Block());
    memoPool_.clear();
    memoPool_.reserve(64 * 1024);  // grows as the search visits the tree: reserved once, so it never moves
    memoOff_.clear();
    memoLen_.clear();
    uint8_t h[64];
    if (size_ < 64 || !r->read(0, h, 64)) { error = "dict.bin is missing or too small"; reader_ = nullptr; return false; }
    if (memcmp(h, "KTB1", 4) != 0) { error = "dict.bin has the wrong format (rebuild it)"; reader_ = nullptr; return false; }
    if (le32(h + 4) != 1) { error = "dict.bin version mismatch (rebuild it)"; reader_ = nullptr; return false; }
    entryCount_ = le32(h + 8);
    entryIdxOff_ = le32(h + 12);
    keyCount_ = le32(h + 16);
    keyIdxOff_ = le32(h + 20);
    radCount_ = le32(h + 24);
    radOff_ = le32(h + 28);
    kanjiCount_ = le32(h + 32);
    kanjiOff_ = le32(h + 36);
    if ((uint64_t)keyIdxOff_ + (uint64_t)keyCount_ * 8 > size_ ||
        (uint64_t)entryIdxOff_ + (uint64_t)(entryCount_ + 1) * 8 > size_) {
        error = "dict.bin is truncated (copy it again)";
        reader_ = nullptr;
        return false;
    }
    return true;
}

bool Dictionary::readAt(uint32_t off, void *dst, uint32_t len) {
    uint8_t *out = (uint8_t *)dst;
    while (len > 0) {
        uint32_t base = off - (off % kBlock);
        Block *hit = nullptr, *victim = &cache_[0];
        for (Block &b : cache_) {
            if (b.base == base) { hit = &b; break; }
            if (b.stamp < victim->stamp) victim = &b;
        }
        if (!hit) {
            uint32_t n = std::min<uint32_t>(kBlock, size_ > base ? size_ - base : 0);
            if (n == 0 || !reader_->read(base, victim->data, n)) return false;
            victim->base = base;
            victim->len = n;
            hit = victim;
        }
        hit->stamp = ++clock_;
        uint32_t inBlock = off - base;
        if (inBlock >= hit->len) return false;
        uint32_t take = std::min(len, hit->len - inBlock);
        memcpy(out, hit->data + inBlock, take);
        out += take;
        off += take;
        len -= take;
    }
    return true;
}

uint32_t Dictionary::u32At(uint32_t off) {
    uint8_t b[4] = {0, 0, 0, 0};
    readAt(off, b, 4);
    return le32(b);
}

std::string Dictionary::cstrAt(uint32_t off) {
    std::string s;
    char buf[64];
    while (off < size_) {
        uint32_t n = std::min<uint32_t>(sizeof(buf), size_ - off);
        if (!readAt(off, buf, n)) break;
        void *z = memchr(buf, 0, n);
        if (z) { s.append(buf, (char *)z - buf); break; }
        s.append(buf, n);
        off += n;
        if (s.size() > 4096) break;  // corrupt data guard
    }
    return s;
}

std::string Dictionary::keyAt(uint32_t i, uint32_t *entryId) {
    uint8_t rec[8];
    readAt(keyIdxOff_ + i * 8, rec, 8);
    if (entryId) *entryId = le32(rec + 4);
    return cstrAt(le32(rec));
}

static int cmpBytes(const std::string &a, const std::string &b) {
    // unsigned byte-wise comparison, same order as Python's bytes sort
    size_t n = std::min(a.size(), b.size());
    int c = memcmp(a.data(), b.data(), n);
    if (c) return c;
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

// Compares the key at `index` (node `node` of the search tree: 1 = root, children 2n, 2n + 1)
// with `key`. The first kMemoLevels levels are remembered after their first read.
int Dictionary::cmpProbe(uint32_t node, uint32_t index, const std::string &key) {
    if (node < memoOff_.size()) {
        if (!memoOff_[node]) {
            std::string k = keyAt(index, nullptr);
            memoOff_[node] = (uint32_t)memoPool_.size() + 1;
            memoLen_[node] = (uint16_t)std::min<size_t>(k.size(), 0xFFFF);
            memoPool_ += k;
        }
        const char *p = memoPool_.data() + memoOff_[node] - 1;
        size_t len = memoLen_[node], n = std::min(len, key.size());
        int c = memcmp(p, key.data(), n);
        if (c) return c;
        return len < key.size() ? -1 : (len > key.size() ? 1 : 0);
    }
    return cmpBytes(keyAt(index, nullptr), key);
}

uint32_t Dictionary::lowerBound(const std::string &key) {
    if (memoOff_.empty()) {
        memoOff_.assign(1u << kMemoLevels, 0);
        memoLen_.assign(1u << kMemoLevels, 0);
    }
    // mid depends only on (lo, hi), so every search walks the same tree: left = 2n, right = 2n + 1
    uint32_t lo = 0, hi = keyCount_, node = 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cmpProbe(node, mid, key) < 0) { lo = mid + 1; node = 2 * node + 1; }
        else { hi = mid; node = 2 * node; }
        if (node >= (1u << 30)) node = 1u << 30;  // deep levels are never remembered
    }
    return lo;
}

void Dictionary::exact(const std::string &key, std::vector<uint32_t> &ids) {
    if (!reader_ || key.empty()) return;
    for (uint32_t i = lowerBound(key); i < keyCount_; i++) {
        uint32_t eid;
        if (keyAt(i, &eid) != key) break;
        if (std::find(ids.begin(), ids.end(), eid) == ids.end()) ids.push_back(eid);
    }
}

void Dictionary::prefix(const std::string &pre, uint32_t limit,
                        std::vector<std::pair<uint32_t, std::string>> &out) {
    if (!reader_ || pre.empty()) return;
    uint32_t i = lowerBound(pre);
    for (uint32_t n = 0; i < keyCount_ && n < limit; i++, n++) {
        uint32_t eid;
        std::string k = keyAt(i, &eid);
        if (k.compare(0, pre.size(), pre) != 0) break;
        out.push_back(std::make_pair(eid, k));
    }
}

uint32_t Dictionary::posMask(uint32_t id) {
    if (!reader_ || id >= entryCount_) return 0;
    return u32At(entryIdxOff_ + id * 8 + 4);
}

static void splitInto(const std::string &s, const std::string &sep, std::vector<std::string> &out) {
    out.clear();
    if (s.empty()) return;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + sep.size();
    }
}

bool Dictionary::entry(uint32_t id, Entry &e) {
    if (!reader_ || id >= entryCount_) return false;
    uint8_t rec[12];
    if (!readAt(entryIdxOff_ + id * 8, rec, 12)) return false;
    uint32_t start = le32(rec), end = le32(rec + 8);
    if (end <= start || end - start > 64 * 1024) return false;
    std::string blob(end - start, '\0');
    if (!readAt(start, &blob[0], end - start)) return false;
    size_t z = blob.find('\0');
    if (z != std::string::npos) blob.resize(z);

    std::vector<std::string> lines, fields;
    splitInto(blob, "\n", lines);
    if (lines.size() < 2) return false;
    splitInto(lines[0], "\t", fields);
    fields.resize(5);
    e = Entry();
    e.id = id;
    e.expression = fields[0];
    e.reading = fields[1];
    e.pos = fields[2];
    splitInto(fields[3], "\xEF\xBD\x9C", e.kanjiForms);  // U+FF5C
    splitInto(fields[4], "\xEF\xBD\x9C", e.kanaForms);
    e.headline = lines[1];
    e.senses.assign(lines.begin() + 2, lines.end());
    return true;
}

const std::vector<Radical> &Dictionary::radicals() {
    if (!radicalsLoaded_ && reader_) {
        radicalsLoaded_ = true;
        radicals_.reserve(radCount_);
        for (uint32_t i = 0; i < radCount_; i++) {
            uint8_t rec[32];
            if (!readAt(radOff_ + i * 32, rec, 32)) break;
            Radical r;
            r.glyph.assign((const char *)rec, strnlen((const char *)rec, 8));
            r.strokes = le16(rec + 8);
            r.count = le16(rec + 10);
            r.listOff = le32(rec + 12);
            r.label.assign((const char *)rec + 16, strnlen((const char *)rec + 16, 16));
            radicals_.push_back(r);
        }
    }
    return radicals_;
}

void Dictionary::kanjiWithRadicals(const std::vector<int> &idx, uint32_t limit,
                                   std::vector<std::string> &out, uint32_t &total) {
    out.clear();
    total = 0;
    const std::vector<Radical> &rads = radicals();
    if (idx.empty()) return;
    std::vector<uint16_t> acc;
    bool first = true;
    for (int ri : idx) {
        if (ri < 0 || ri >= (int)rads.size()) continue;
        const Radical &r = rads[ri];
        std::vector<uint16_t> ids(r.count);
        if (r.count && !readAt(r.listOff, ids.data(), r.count * 2)) return;
        for (uint16_t &v : ids) v = le16((const uint8_t *)&v);
        if (first) { acc.swap(ids); first = false; }
        else {
            std::vector<uint16_t> tmp;
            std::set_intersection(acc.begin(), acc.end(), ids.begin(), ids.end(), std::back_inserter(tmp));
            acc.swap(tmp);
        }
    }
    total = acc.size();
    for (uint32_t i = 0; i < acc.size() && i < limit; i++) {
        uint32_t cp = u32At(kanjiOff_ + acc[i] * 4);
        std::string s;
        utf8Append(s, cp);
        out.push_back(s);
    }
}

}  // namespace kt
