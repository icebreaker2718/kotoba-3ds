// dict.hpp - reader for dict.bin (see tools/build_dict.py for the format).
// The file is never loaded whole: lookups binary-search the on-disk index through a
// small block cache, so RAM use stays around 64 KB.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kt {

// Random-access byte source. The 3DS build wraps CTRPF's File; tests wrap std::FILE.
class Reader {
public:
    virtual ~Reader() {}
    virtual bool read(uint32_t offset, void *dst, uint32_t len) = 0;
    virtual uint32_t size() = 0;
};

// Part-of-speech bits (must match build_dict.py)
enum : uint32_t {
    POS_V1 = 1, POS_V5 = 2, POS_VK = 4, POS_VSI = 8, POS_VSN = 16,
    POS_ADJ = 32, POS_VZ = 64, POS_ADJNA = 128,
};

struct Entry {
    uint32_t id = 0;
    std::string expression;   // headword to put on the card
    std::string reading;
    std::string pos;          // comma separated codes
    std::vector<std::string> kanjiForms, kanaForms;
    std::string headline;     // 食べる・喰べる【たべる】
    std::vector<std::string> senses;  // "1. (v1,vt) to eat"
    uint32_t freqRank = 0;    // rank in the frequency list dict.bin was built with (JPDB), 0 = none
};

struct Radical {
    std::string glyph;   // as written in RADKFILE (may be a stand-in kanji)
    std::string label;   // e.g. "water 氵" for stand-ins, else empty
    uint16_t strokes = 0;
    uint16_t count = 0;
    uint32_t listOff = 0;
};

class Dictionary {
public:
    bool open(Reader *r, std::string &error);
    bool isOpen() const { return reader_ != nullptr; }

    uint32_t entryCount() const { return entryCount_; }
    uint32_t keyCount() const { return keyCount_; }

    // All entries whose key equals `key` exactly (key already normalized).
    void exact(const std::string &key, std::vector<uint32_t> &entryIds);
    // Keys that start with `prefix` (normalized); returns (entryId, key) pairs, at most `limit`
    // keys scanned.
    void prefix(const std::string &prefix, uint32_t limit,
                std::vector<std::pair<uint32_t, std::string>> &out);

    uint32_t posMask(uint32_t entryId);
    bool entry(uint32_t entryId, Entry &out);

    const std::vector<Radical> &radicals();
    // Kanji containing every radical in `radicalIdx`, most common first.
    void kanjiWithRadicals(const std::vector<int> &radicalIdx, uint32_t limit,
                           std::vector<std::string> &out, uint32_t &total);

private:
    bool readAt(uint32_t off, void *dst, uint32_t len);
    uint32_t u32At(uint32_t off);
    std::string cstrAt(uint32_t off);
    std::string keyAt(uint32_t i, uint32_t *entryId);
    uint32_t lowerBound(const std::string &key);

    // The first levels of the binary search over the keys are the same for every lookup: their
    // keys are kept in memory once read (about 30 KB), so a lookup reads ~3 blocks instead of ~20.
    enum : uint32_t { kMemoLevels = 12 };
    std::unordered_map<std::string, std::vector<uint32_t>> exactMemo_;  // recent exact() answers
    void exactUncached(const std::string &key, std::vector<uint32_t> &ids);
    std::string memoPool_;            // the remembered keys, back to back
    std::vector<uint32_t> memoOff_;   // per tree node (heap order): offset into memoPool_ + 1, 0 = not read
    std::vector<uint16_t> memoLen_;
    int cmpProbe(uint32_t node, uint32_t index, const std::string &key);

    // block cache
    enum : uint32_t { kBlock = 2048, kBlocks = 48 };
    struct Block { uint32_t base = 0xFFFFFFFF; uint32_t stamp = 0; uint32_t len = 0; uint8_t data[kBlock]; };
    std::vector<Block> cache_;
    uint32_t clock_ = 0;

    Reader *reader_ = nullptr;
    uint32_t size_ = 0;
    uint32_t entryCount_ = 0, entryIdxOff_ = 0, keyCount_ = 0, keyIdxOff_ = 0;
    uint32_t radCount_ = 0, radOff_ = 0, kanjiCount_ = 0, kanjiOff_ = 0;
    std::vector<Radical> radicals_;
    bool radicalsLoaded_ = false;
};

}  // namespace kt
