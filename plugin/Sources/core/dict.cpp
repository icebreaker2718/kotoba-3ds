#include "core/dict.hpp"

#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "core/kana.hpp"

namespace kt {

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
// Each cache's block (MemoArena). Measured on 32-bit ARM (qemu) at 512 / 512 / 4096 over the saved screens' lookups
// (183 spotter reads, and 705 reads of every mode) with the tables reserved at full size: exact 29.8 KB, prefix
// 20.5 KB, mask 81.0 KB (full at kMaskMemoMax: its ceiling). With room for longer keys; what doesn't fit comes from
// malloc as before (64-bit builds' nodes are bigger: some does there).
static const size_t kExactArenaBytes = 36 * 1024, kPrefixArenaBytes = 24 * 1024, kMaskArenaBytes = 84 * 1024;

// Empties a cache: the map (its nodes in the block) goes, then the whole block is free again.
void Dictionary::dropExact() {
    { ExactMemo t(0, std::hash<std::string_view>(), std::equal_to<std::string_view>(), MemoAlloc<int>(&exactArena_)); exactMemo_.swap(t); }
    exactArena_.reset();
    exactMemo_.reserve(kExactMemoMax);  // its bucket table at full size once (a growing one leaves old tables in the block)
}
void Dictionary::dropPrefix() {
    { PrefixMemo t(0, std::hash<std::string_view>(), std::equal_to<std::string_view>(), MemoAlloc<int>(&prefixArena_)); prefixMemo_.swap(t); }
    prefixArena_.reset();
    prefixMemo_.reserve(kPrefixMemoMax);
}
void Dictionary::dropMask() {
    { MaskMemo t(0, std::hash<uint32_t>(), std::equal_to<uint32_t>(), MemoAlloc<int>(&maskArena_)); maskMemo_.swap(t); }
    maskArena_.reset();
    maskMemo_.reserve(kMaskMemoMax);
}
std::string_view Dictionary::memoKey(MemoArena &a, const std::string &key) {
    char *p = (char *)a.take(key.size() ? key.size() : 1);
    if (!p) abort();
    memcpy(p, key.data(), key.size());
    return std::string_view(p, key.size());
}
// The maps emptied (no table reserved again), then whatever came from malloc and the blocks themselves.
Dictionary::~Dictionary() {
    { ExactMemo t(0, std::hash<std::string_view>(), std::equal_to<std::string_view>(), MemoAlloc<int>(&exactArena_)); exactMemo_.swap(t); }
    { PrefixMemo t(0, std::hash<std::string_view>(), std::equal_to<std::string_view>(), MemoAlloc<int>(&prefixArena_)); prefixMemo_.swap(t); }
    { MaskMemo t(0, std::hash<uint32_t>(), std::equal_to<uint32_t>(), MemoAlloc<int>(&maskArena_)); maskMemo_.swap(t); }
    exactArena_.reset();
    prefixArena_.reset();
    maskArena_.reset();
    free(exactArena_.base);
    free(prefixArena_.base);
    free(maskArena_.base);
}

bool Dictionary::open(Reader *r, std::string &error) {
    reader_ = r;
    size_ = r->size();
    cache_.assign(kBlocks, Block());
    memoPool_.clear();
    memoOff_.clear();
    memoLen_.clear();
    // emptied, their memory kept: opening another dictionary file (Select on the lookup screen) reuses it
    fencePool_.clear();
    fenceAt_.clear();
    radicals_.clear();
    radicalsLoaded_ = false;
    if (!exactArena_.base) {  // the caches' blocks, once, while the heap is still in one piece
        exactArena_.base = (char *)malloc(kExactArenaBytes);
        exactArena_.cap = exactArena_.base ? kExactArenaBytes : 0;
        prefixArena_.base = (char *)malloc(kPrefixArenaBytes);
        prefixArena_.cap = prefixArena_.base ? kPrefixArenaBytes : 0;
        maskArena_.base = (char *)malloc(kMaskArenaBytes);
        maskArena_.cap = maskArena_.base ? kMaskArenaBytes : 0;
    }
    dropMask();
    dropPrefix();
    uint8_t h[64];
    if (size_ < 64 || !r->read(0, h, 64)) { error = "dict.bin is missing or too small"; reader_ = nullptr; return false; }
    if (memcmp(h, "KTB1", 4) != 0) { error = "dict.bin has the wrong format (rebuild it)"; reader_ = nullptr; return false; }
    version_ = le32(h + 4);
    if (version_ < 1 || version_ > 3) {
        error = version_ > 3 ? "this dictionary file is newer than the plugin (update Kotoba3DS)"
                             : "dict.bin version mismatch (rebuild it)";
        reader_ = nullptr;
        return false;
    }
    keyRec_ = version_ >= 2 ? 12 : 8;
    entryCount_ = le32(h + 8);
    entryIdxOff_ = le32(h + 12);
    keyCount_ = le32(h + 16);
    keyIdxOff_ = le32(h + 20);
    radCount_ = le32(h + 24);
    radOff_ = le32(h + 28);
    kanjiCount_ = le32(h + 32);
    kanjiOff_ = le32(h + 36);
    dropExact();
    if ((uint64_t)keyIdxOff_ + (uint64_t)keyCount_ * keyRec_ > size_ ||
        (uint64_t)entryIdxOff_ + (uint64_t)(entryCount_ + 1) * 8 > size_) {
        error = "dict.bin is truncated (copy it again)";
        reader_ = nullptr;
        return false;
    }
    if (version_ >= 2) {  // the fences, into memory in one read (~70 KB)
        fenceStep_ = le32(h + 40);
        const uint32_t count = le32(h + 44), off = le32(h + 48);
        const uint32_t bytes = off > keyIdxOff_ && off < size_ ? size_ - off : 0;  // the last section
        bool ok = fenceStep_ > 0 && count > 0 && count == (keyCount_ + fenceStep_ - 1) / fenceStep_ && bytes >= count * 2u;
        // read through the block cache, straight into the pool (no copy of the section in memory); first a pass for
        // the sizes, so the pool is taken once, with room for a somewhat bigger dictionary file opened later
        uint32_t text = 0;
        for (uint32_t i = 0, p = off; i < count && ok; i++) {
            uint8_t l[2];
            ok = p + 2 <= size_ && readAt(p, l, 2);
            const uint32_t len = le16(l);
            ok = ok && p + 2 + len <= size_;
            text += len;
            p += 2 + len;
        }
        if (ok) {
            if (fenceAt_.capacity() < count + 1) fenceAt_.reserve(count + 1 + count / 4);
            if (fencePool_.capacity() < text) fencePool_.reserve(text + text / 4);
            fencePool_.resize(text);
            uint32_t p = off, at = 0;
            for (uint32_t i = 0; i < count && ok; i++) {
                uint8_t l[2];
                ok = readAt(p, l, 2);
                const uint32_t len = le16(l);
                fenceAt_.push_back(at);
                ok = ok && (len == 0 || readAt(p + 2, &fencePool_[at], len));
                at += len;
                p += 2 + len;
            }
            fenceAt_.push_back(at);
        }
        if (!ok) {
            error = "dict.bin is damaged (copy it again)";
            reader_ = nullptr;
            return false;
        }
    } else {
        memoPool_.reserve(64 * 1024);  // grows as the search visits the tree: reserved once, so it never moves
    }
    tagPool_.clear();
    tags_.clear();
    title_.clear();
    if (version_ >= 3) {  // the tag table, whole (its rows run from the first row's offset to the offset table)
        const uint32_t tagOff = le32(h + 52), tagCount = le32(h + 56), titleOff = le32(h + 60);
        bool ok = tagCount < 65025 && (uint64_t)tagOff + tagCount * 4ull <= titleOff && titleOff < size_;
        const uint32_t first = ok && tagCount ? u32At(tagOff) : tagOff;
        ok = ok && first <= tagOff && tagOff - first < (1u << 20);
        if (ok) {
            if (tagPool_.capacity() < tagOff - first + 1) tagPool_.reserve(std::max<uint32_t>(16 * 1024, tagOff - first + 1));
            tagPool_.resize(tagOff - first + 1);
            ok = tagPool_.size() == 1 || readAt(first, &tagPool_[0], tagOff - first);
            tagPool_.back() = '\0';
        }
        if (ok) {
            if (tags_.capacity() < tagCount) tags_.reserve(std::max<uint32_t>(1024, tagCount));
            tags_.resize(tagCount);
            for (uint32_t i = 0; i < tagCount && ok; i++) {
                const uint32_t o = u32At(tagOff + i * 4);
                if (o < first || o + 1 >= tagOff) { ok = false; break; }
                const char *p = tagPool_.data() + (o - first), *end = tagPool_.data() + tagPool_.size() - 1;
                Tag &t = tags_[i];
                t.category = (uint8_t)*p++;
                const char **field[3] = {&t.code, &t.label, &t.description};
                for (const char **f : field) {
                    if (p >= end) { ok = false; break; }
                    *f = p;
                    p += strlen(p) + 1;
                }
            }
        }
        char name[128];
        uint32_t n = std::min<uint32_t>(sizeof(name) - 1, size_ - titleOff);
        if (ok && readAt(titleOff, name, n)) {
            name[n] = '\0';
            title_ = name;
        }
        if (!ok) {
            error = "the dictionary file is damaged (copy it again)";
            reader_ = nullptr;
            return false;
        }
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
    uint8_t rec[12];
    readAt(keyIdxOff_ + i * keyRec_, rec, keyRec_);
    if (entryId) {
        *entryId = le32(rec + 4);
        if (keyRec_ >= 12) {  // version 2: the entry's part-of-speech mask comes with it
            if (maskMemo_.size() >= kMaskMemoMax) dropMask();
            maskMemo_[*entryId] = le32(rec + 8);
        }
    }
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
    if (!fenceAt_.empty()) {  // version 2: between which fences (in memory), then the keys there
        const uint32_t count = (uint32_t)fenceAt_.size() - 1;
        uint32_t lo = 0, hi = count;
        while (lo < hi) {  // the first fence >= key
            const uint32_t mid = lo + (hi - lo) / 2;
            const char *p = fencePool_.data() + fenceAt_[mid];
            const size_t len = fenceAt_[mid + 1] - fenceAt_[mid], n = std::min(len, key.size());
            int c = memcmp(p, key.data(), n);
            if (!c) c = len < key.size() ? -1 : (len > key.size() ? 1 : 0);
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo == 0) return 0;  // key <= the first key
        // fence lo - 1 < key <= fence lo (or the end): the answer is in ((lo - 1) step, lo step]
        uint32_t a = (lo - 1) * fenceStep_ + 1, b = std::min(lo * fenceStep_, keyCount_);
        while (a < b) {
            const uint32_t mid = a + (b - a) / 2;
            if (cmpBytes(keyAt(mid, nullptr), key) < 0) a = mid + 1;
            else b = mid;
        }
        return a;
    }
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
    // the same keys come up again and again (each spot is looked up at several lengths, then
    // again when the cursor gets there): remember the last few hundred answers
    auto memo = exactMemo_.find(std::string_view(key));
    if (memo != exactMemo_.end()) {
        for (uint32_t eid : memo->second)
            if (std::find(ids.begin(), ids.end(), eid) == ids.end()) ids.push_back(eid);
        return;
    }
    std::vector<uint32_t> found;
    exactUncached(key, found);
    if (exactMemo_.size() >= kExactMemoMax) dropExact();
    exactMemo_.emplace(memoKey(exactArena_, key), MemoIds(found.begin(), found.end(), MemoAlloc<uint32_t>(&exactArena_)));
    for (uint32_t eid : found)
        if (std::find(ids.begin(), ids.end(), eid) == ids.end()) ids.push_back(eid);
}

void Dictionary::exactUncached(const std::string &key, std::vector<uint32_t> &ids) {
    for (uint32_t i = lowerBound(key); i < keyCount_; i++) {
        uint32_t eid;
        if (keyAt(i, &eid) != key) break;
        if (std::find(ids.begin(), ids.end(), eid) == ids.end()) ids.push_back(eid);
    }
}

bool Dictionary::hasPrefix(const std::string &pre) {
    if (!reader_) return false;
    if (pre.empty()) return true;
    auto memo = prefixMemo_.find(std::string_view(pre));
    if (memo != prefixMemo_.end()) return memo->second;
    uint32_t i = lowerBound(pre);
    bool yes = i < keyCount_ && keyAt(i, nullptr).compare(0, pre.size(), pre) == 0;
    if (prefixMemo_.size() >= kPrefixMemoMax) dropPrefix();
    prefixMemo_.emplace(memoKey(prefixArena_, pre), yes);
    return yes;
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
    auto m = maskMemo_.find(id);  // read with the key that found the entry (version 2)
    if (m != maskMemo_.end()) return m->second;
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

// The entry's bytes (its blob or record, without the NUL after it) into buf.
bool Dictionary::readRecord(uint32_t id, char *buf, uint32_t cap, uint32_t &len) {
    len = 0;
    if (!reader_ || id >= entryCount_) return false;
    uint8_t rec[12];
    if (!readAt(entryIdxOff_ + id * 8, rec, 12)) return false;
    uint32_t start = le32(rec), end = le32(rec + 8);
    if (end <= start || end - start > 64 * 1024 || end - start > cap) return false;
    if (!readAt(start, buf, end - start)) return false;
    len = end - start;
    while (len > 0 && buf[len - 1] == '\0') len--;
    return true;
}

bool Dictionary::entry(uint32_t id, Entry &e, bool withSenses) {
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
    if (version_ >= 3) {  // the machine line, then the display runs
        size_t nl = blob.find('\n');
        if (nl == std::string::npos) return false;
        lines.push_back(blob.substr(0, nl));
    } else {
        splitInto(blob, "\n", lines);
        if (lines.size() < 2) return false;
    }
    splitInto(lines[0], "\t", fields);
    fields.resize(6);  // the 6th (frequency rank) is missing in dict.bin files built before it
    e = Entry();
    e.id = id;
    e.expression = fields[0];
    e.reading = fields[1];
    e.pos = fields[2];
    splitInto(fields[3], "\xEF\xBD\x9C", e.kanjiForms);  // U+FF5C
    splitInto(fields[4], "\xEF\xBD\x9C", e.kanaForms);
    e.freqRank = (uint32_t)strtoul(fields[5].c_str(), nullptr, 10);
    if (version_ >= 3) {
        // no headline in the file: the forms, as dict.bin's headline shows them
        std::string k, r;
        for (size_t i = 0; i < e.kanjiForms.size() && i < 6; i++) k += (i ? "・" : "") + e.kanjiForms[i];
        for (size_t i = 0; i < e.kanaForms.size() && i < 6; i++) r += (i ? "・" : "") + e.kanaForms[i];
        e.headline = k.empty() ? r : k + "【" + r + "】";
        if (withSenses) {
            const size_t nl = lines[0].size() + 1;
            sensesFromRuns(*this, blob.data() + nl, blob.size() - nl, e.senses);
        }
    } else {
        e.headline = lines[1];
        if (withSenses) e.senses.assign(lines.begin() + 2, lines.end());
    }
    return true;
}

// Appends a run (text) to buf if it fits.
static bool put(char *buf, uint32_t cap, uint32_t &len, const char *s, size_t n) {
    if (len + n > cap) return false;
    memcpy(buf + len, s, n);
    len += (uint32_t)n;
    return true;
}
static bool putByte(char *buf, uint32_t cap, uint32_t &len, uint8_t b) { return put(buf, cap, len, (const char *)&b, 1); }

bool Dictionary::display(uint32_t id, char *buf, uint32_t cap, uint32_t &len) {
    uint32_t n = 0;
    len = 0;
    if (!readRecord(id, buf, cap, n)) return false;
    const char *nl = (const char *)memchr(buf, '\n', n);
    if (!nl) return false;
    if (version_ >= 3) {  // the runs after the machine line, moved to the front
        const uint32_t from = (uint32_t)(nl + 1 - buf);
        memmove(buf, buf + from, n - from);
        len = n - from;
        return true;
    }
    // dict.bin: its sense lines ("1. (v1,vt) to eat") as runs. They're moved to the back of buf first, and the runs
    // written from the front (never more bytes than the lines they come from, +6 a line: they don't overtake).
    const char *nl2 = (const char *)memchr(nl + 1, '\n', buf + n - (nl + 1));
    if (!nl2) return true;  // no senses
    const uint32_t from = (uint32_t)(nl2 + 1 - buf), lines = n - from;
    uint32_t extra = 0;
    for (uint32_t i = from; i < n; i++) extra += buf[i] == '\n' ? 8 : 0;
    if (lines + extra + 8 > cap) return false;
    char *src = buf + cap - lines;
    memmove(src, buf + from, lines);
    const char *p = src, *end = src + lines;
    while (p < end) {
        const char *eol = (const char *)memchr(p, '\n', end - p);
        if (!eol) eol = end;
        const char *t = p;
        while (t < eol && *t >= '0' && *t <= '9') t++;
        const bool numbered = t > p && t + 1 < eol && t[0] == '.' && t[1] == ' ';
        const uint8_t head[3] = {RUN_PARA, numbered ? P_SENSE : P_PLAIN, 1};
        put(buf, cap, len, (const char *)head, 3);
        const char *text = p;
        if (numbered) {
            text = t + 2;
            putByte(buf, cap, len, RUN_STYLE);
            putByte(buf, cap, len, S_DIM);
            put(buf, cap, len, p, text - p);
            putByte(buf, cap, len, RUN_HANG);
            if (text < eol && *text == '(') {
                const char *close = (const char *)memchr(text, ')', eol - text);
                if (close && close + 1 < eol && close[1] == ' ') {
                    put(buf, cap, len, text, close + 2 - text);
                    text = close + 2;
                }
            }
        }
        putByte(buf, cap, len, RUN_STYLE);
        putByte(buf, cap, len, numbered ? S_TEXT : S_DIM);
        for (const char *c = text; c < eol; c++) putByte(buf, cap, len, (uint8_t)*c < 0x20 ? ' ' : *c);
        p = eol + 1;
    }
    return true;
}

size_t runEnd(const char *runs, size_t i, size_t n) {
    switch ((uint8_t)runs[i]) {
    case RUN_PARA: case RUN_CHIP: return std::min(n, i + 3);
    case RUN_STYLE: return std::min(n, i + 2);
    case RUN_RUBY:
        while (i < n && (uint8_t)runs[i] != RUN_RUBY_END) i++;
        return std::min(n, i + 1);
    default:
        if ((uint8_t)runs[i] < 0x20) return i + 1;
        while (i < n && (uint8_t)runs[i] >= 0x20) i++;
        return i;
    }
}

void sensesFromRuns(const Dictionary &d, const char *runs, size_t n, std::vector<std::string> &senses) {
    // As build_dict.py's entry_blob writes them: "N. (" + part of speech when it changed + the other tags + ") " +
    // glosses, and notes after them as "; (note)". A group's tags apply to each of its senses.
    senses.clear();
    std::string groupPos, groupMisc, pos, misc, text, prevPos;
    bool havePrev = false;
    enum { kNone, kSense, kNote } part = kNone;
    bool inSense = false, beforeHang = false;
    uint8_t style = S_TEXT;
    auto add = [](std::string &list, const char *code) { list += (list.empty() ? "" : ",") + std::string(code); };
    auto finish = [&]() {
        if (inSense && !text.empty()) {
            std::string tags;
            std::string p = groupPos;
            if (!pos.empty()) p += (p.empty() ? "" : ",") + pos;
            if (!havePrev || p != prevPos) tags = p;
            prevPos = p;
            havePrev = true;
            std::string m = groupMisc;
            if (!misc.empty()) m += (m.empty() ? "" : ",") + misc;
            if (!m.empty()) tags += (tags.empty() ? "" : ",") + m;
            senses.push_back(std::to_string(senses.size() + 1) + ". " + (tags.empty() ? "" : "(" + tags + ") ") + text);
        }
        inSense = false;
        pos.clear();
        misc.clear();
        text.clear();
    };
    bool noteStarted = false;
    for (size_t i = 0; i < n;) {
        const uint8_t c = (uint8_t)runs[i];
        if (c == RUN_PARA && i + 2 < n) {
            const uint8_t kind = (uint8_t)runs[i + 1];
            if (kind == P_GROUP || kind == P_SENSE || kind == P_PLAIN) finish();
            if (kind == P_GROUP) {
                groupPos.clear();
                groupMisc.clear();
            }
            if (kind == P_SENSE) inSense = true;
            part = kind == P_SENSE ? kSense : kind == P_NOTE && inSense ? kNote : kNone;
            noteStarted = false;
            beforeHang = false;  // a sense's number, up to the hang, isn't part of its text
            for (size_t j = i + 3; part == kSense && j < n && (uint8_t)runs[j] != RUN_PARA; j = runEnd(runs, j, n))
                if ((uint8_t)runs[j] == RUN_HANG) { beforeHang = true; break; }
            style = S_TEXT;
            if (kind == P_GROUP) part = kNone;
            i += 3;
            if (kind == P_GROUP) {  // its chips
                for (; i < n && (uint8_t)runs[i] != RUN_PARA; i = runEnd(runs, i, n)) {
                    if ((uint8_t)runs[i] != RUN_CHIP || i + 2 >= n) continue;
                    const Tag *t = d.tag(((uint8_t)runs[i + 1] - 1u) * 255u + ((uint8_t)runs[i + 2] - 1u));
                    if (t) add(t->category == TAG_POS ? groupPos : groupMisc, t->code);
                }
            }
            continue;
        }
        const size_t next = runEnd(runs, i, n);
        if (c == RUN_STYLE && i + 1 < n) {
            style = (uint8_t)runs[i + 1];
        } else if (c == RUN_CHIP && i + 2 < n && part == kSense) {
            const Tag *t = d.tag(((uint8_t)runs[i + 1] - 1u) * 255u + ((uint8_t)runs[i + 2] - 1u));
            if (t) add(t->category == TAG_POS ? pos : misc, t->code);
        } else if (c == RUN_HANG) {
            beforeHang = false;
        } else if (c == RUN_RUBY || c >= 0x20) {
            const char *t = runs + i + (c == RUN_RUBY ? 1 : 0);
            size_t len = next - i - (c == RUN_RUBY ? 1 : 0);
            if (c == RUN_RUBY) len = (const char *)memchr(t, RUN_RUBY_MID, len) ? (size_t)((const char *)memchr(t, RUN_RUBY_MID, len) - t) : 0;
            if (part == kSense && !beforeHang) {
                text.append(t, len);
            } else if (part == kNote && (style & ~S_SMALL) != S_DIM) {  // a note's text, not its label
                if (!noteStarted) text += text.empty() ? "(" : "; (";
                else text.erase(text.size() - 1);  // reopen the bracket
                text.append(t, len);
                text += ")";
                noteStarted = true;
            }
        }
        i = next;
    }
    finish();
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
