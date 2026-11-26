#include "core/meta_dict.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/kana.hpp"

namespace kt {

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

// a < b as UTF-8 bytes (the order build_dict.py sorts by)
static int cmpKey(const char *a, size_t an, const char *b, size_t bn) {
    const int c = memcmp(a, b, std::min(an, bn));
    return c ? c : (an < bn ? -1 : an > bn ? 1 : 0);
}

bool MetaFile::open(Reader *r, std::string &error) {
    reader_ = nullptr;
    uint8_t h[64];
    if (!r || r->size() < 64 || !r->read(0, h, 64) || memcmp(h, "KTM1", 4)) {
        error = "not a pitch accent / frequency file";
        return false;
    }
    if (rd32(h + 4) != 1) {
        error = "made by a newer Kotoba3DS (update the plugin)";
        return false;
    }
    const uint32_t size = r->size();
    kind_ = rd32(h + 8) == PITCH ? PITCH : FREQ;
    records_ = rd32(h + 12);
    blockSize_ = rd32(h + 16);
    blocks_ = rd32(h + 20);
    dataOff_ = rd32(h + 24);
    keyOff_ = rd32(h + 28);
    const uint32_t titleOff = rd32(h + 32);
    groupStep_ = rd32(h + 36);
    const uint32_t groups = rd32(h + 40), groupOff = rd32(h + 44);
    largestGroup_ = rd32(h + 48);
    keyLen_ = titleOff - keyOff_;
    const bool sane = blockSize_ == 4096 && groupStep_ > 0 && groups == (blocks_ + groupStep_ - 1) / groupStep_ &&
                      (uint64_t)dataOff_ + (uint64_t)blocks_ * blockSize_ <= keyOff_ && keyOff_ <= titleOff &&
                      titleOff < groupOff && groupOff + groups * 4ull <= size && largestGroup_ <= 64 * 1024;
    if (!sane) {
        error = "the file is damaged (copy it again)";
        return false;
    }
    char t[128];
    const uint32_t tn = std::min<uint32_t>(sizeof(t) - 1, groupOff - titleOff);
    if (!r->read(titleOff, t, tn)) { error = "can't be read"; return false; }
    t[tn] = '\0';
    title_ = t;
    // the groups: offsets, then their first keys
    groupAt_.assign(groups, 0);
    if (groups && !r->read(groupOff, groupAt_.data(), groups * 4)) { error = "can't be read"; return false; }
    for (uint32_t &o : groupAt_) o = rd32((const uint8_t *)&o);
    const uint32_t keysAt = groupOff + groups * 4, keysLen = size - keysAt;
    if (keysLen > 256 * 1024) { error = "the file is damaged (copy it again)"; return false; }
    std::string raw(keysLen, '\0');
    if (keysLen && !r->read(keysAt, &raw[0], keysLen)) { error = "can't be read"; return false; }
    groupPool_.clear();
    groupPool_.reserve(keysLen);
    groupKeyAt_.assign(1, 0);
    groupKeyAt_.reserve(groups + 1);
    for (uint32_t g = 0, p = 0; g < groups; g++) {
        if (p + 2 > keysLen) { error = "the file is damaged (copy it again)"; return false; }
        const uint32_t n = rd16((const uint8_t *)raw.data() + p);
        if (p + 2 + n > keysLen) { error = "the file is damaged (copy it again)"; return false; }
        groupPool_.append(raw, p + 2, n);
        groupKeyAt_.push_back((uint32_t)groupPool_.size());
        p += 2 + n;
    }
    reader_ = r;
    return true;
}

bool MetaFile::find(const char *key, size_t kn, char *out, size_t cap, char *seg, size_t segCap, char *buf) {
    if (!reader_ || groupAt_.empty() || !cap) return false;
    // the group: the last whose first key <= key
    size_t lo = 0, hi = groupAt_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const char *k = groupPool_.data() + groupKeyAt_[mid];
        if (cmpKey(k, groupKeyAt_[mid + 1] - groupKeyAt_[mid], key, kn) <= 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return false;
    const size_t g = lo - 1;
    // its block keys: the last block whose first key <= key
    const uint32_t from = groupAt_[g], to = g + 1 < groupAt_.size() ? groupAt_[g + 1] : keyLen_;
    if (to <= from || to - from > segCap || to > keyLen_) return false;
    if (!reader_->read(keyOff_ + from, seg, to - from)) return false;
    uint32_t blk = (uint32_t)(g * groupStep_), b = blk;
    for (uint32_t p = 0; p + 2 <= to - from; b++) {
        const uint32_t n = rd16((const uint8_t *)seg + p);
        if (p + 2 + n > to - from) return false;
        if (cmpKey(seg + p + 2, n, key, kn) > 0) break;
        blk = b;
        p += 2 + n;
    }
    if (blk >= blocks_ || !reader_->read(dataOff_ + blk * blockSize_, buf, blockSize_)) return false;
    // its records: key NUL value NUL, in order, NULs after the last
    for (uint32_t p = 0; p < blockSize_ && buf[p];) {
        const char *k = buf + p;
        const char *kz = (const char *)memchr(k, 0, blockSize_ - p);
        if (!kz) return false;
        const char *v = kz + 1;
        const char *vz = (const char *)memchr(v, 0, blockSize_ - (v - buf));
        if (!vz) return false;
        const int c = cmpKey(k, kz - k, key, kn);
        if (c == 0) {
            const size_t n = std::min<size_t>(vz - v, cap - 1);
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        if (c > 0) return false;
        p = (uint32_t)(vz + 1 - buf);
    }
    return false;
}

MetaSet::~MetaSet() {
    free(seg_);
    free(block_);
}

bool MetaSet::add(Reader *r, std::string &error) {
    if (count_ >= kMaxFiles) {
        error = "more than 8 pitch accent / frequency files";
        return false;
    }
    MetaFile &f = files_[count_];
    if (!f.open(r, error)) return false;
    // the shared buffers: taken with the first file, the group buffer made larger only for a file that needs it
    if (!block_) block_ = (char *)malloc(4096);
    if (f.largestGroup() > segCap_) {
        char *s = (char *)realloc(seg_, f.largestGroup());
        if (!s) { error = "not enough memory"; return false; }
        seg_ = s;
        segCap_ = f.largestGroup();
    }
    if (!block_) { error = "not enough memory"; return false; }
    count_++;
    return true;
}

int MetaSet::count(MetaFile::Kind k) const {
    int n = 0;
    for (int i = 0; i < count_; i++) n += files_[i].kind() == k;
    return n;
}

bool MetaSet::find(int i, const std::string &expression, const std::string &reading, char *out, size_t cap) {
    if (i < 0 || i >= count_ || expression.empty()) return false;
    for (int pass = 0; pass < 2; pass++) {  // the pair, then the expression for any reading
        const std::string &rd = pass == 0 ? reading : std::string();
        if (pass == 0 && reading.empty()) continue;
        const size_t n = expression.size() + 1 + rd.size();
        if (n >= sizeof(key_)) return false;
        memcpy(key_, expression.data(), expression.size());
        key_[expression.size()] = '\t';
        memcpy(key_ + expression.size() + 1, rd.data(), rd.size());
        if (files_[i].find(key_, n, out, cap, seg_, segCap_, block_)) return true;
    }
    return false;
}

// small kana that join the mora before them
static bool joinsMora(uint32_t cp) {
    switch (cp) {
    case 0x3083: case 0x3085: case 0x3087: case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x308E:
    case 0x30E3: case 0x30E5: case 0x30E7: case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30EE:
        return true;
    default:
        return false;
    }
}

int moraCount(const std::string &reading) {
    int n = 0;
    for (size_t i = 0; i < reading.size();) {
        const uint32_t cp = utf8Next(reading, i);
        if (!joinsMora(cp) || n == 0) n++;
    }
    return n;
}

int pitchPieces(const std::string &reading, int position, PitchPiece *out, int cap) {
    int pieces = 0, mora = -1;
    size_t moraStart = 0;
    auto put = [&](size_t from, size_t to, int m) {  // mora m, bytes [from, to)
        const bool high = position == 1 ? m == 0 : m > 0 && (position == 0 || m < position);
        const bool drop = position >= 1 && m == position - 1;
        if (pieces > 0 && out[pieces - 1].high == high && !out[pieces - 1].drop) {
            out[pieces - 1].len = (uint16_t)(to - out[pieces - 1].off);
        } else {
            if (pieces >= cap) return;
            out[pieces].off = (uint16_t)from;
            out[pieces].len = (uint16_t)(to - from);
            out[pieces].high = high;
            out[pieces].drop = false;
            pieces++;
        }
        if (drop) out[pieces - 1].drop = true;
    };
    for (size_t i = 0; i < reading.size();) {
        const size_t at = i;
        const uint32_t cp = utf8Next(reading, i);
        if (!joinsMora(cp) || mora < 0) {
            if (mora >= 0) put(moraStart, at, mora);
            mora++;
            moraStart = at;
        }
    }
    if (mora >= 0) put(moraStart, reading.size(), mora);
    return pieces;
}

std::string groupDigits(const std::string &s) {
    size_t n = 0;  // the leading digits ("13390㋕": 5)
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') n++;
    if (n < 4 || n > 12) return s;
    std::string out;
    for (size_t i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return out + s.substr(n);
}

}  // namespace kt
