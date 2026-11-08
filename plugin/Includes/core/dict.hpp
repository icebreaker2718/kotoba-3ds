// dict.hpp - reader for dict.bin (see tools/build_dict.py for the format).
// The file is never loaded whole: lookups binary-search the on-disk index through a
// small block cache, so RAM use stays around 64 KB.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kt {

// Memory for the dictionary's recent-answer caches: one fixed block taken once, handed out from the front and
// taken back all at once when a cache is emptied (reset). Their nodes, keys and answers (hundreds of small
// pieces that stayed between reads) no longer scatter through the plugin's heap. When the block is full, or
// couldn't be had, pieces come from malloc as before (and go back with free).
struct MemoArena {
    char *base = nullptr;
    size_t cap = 0, used = 0, peak = 0, overflow = 0;  // peak / overflow: for the tests and the log
    struct Spill {  // a piece from malloc (the block was full), listed so reset() can free what's left of them
        Spill *prev, *next;
    };
    Spill *spill = nullptr;
    void *take(size_t bytes) {
        bytes = (bytes + 7) & ~(size_t)7;
        if (base && used + bytes <= cap) {
            void *p = base + used;
            used += bytes;
            if (used > peak) peak = used;
            return p;
        }
        overflow++;
        Spill *s = (Spill *)malloc(sizeof(Spill) + bytes);
        if (!s) return nullptr;
        s->prev = nullptr;
        s->next = spill;
        if (spill) spill->prev = s;
        spill = s;
        return s + 1;
    }
    void give(void *p) {
        if (!p || (base && (char *)p >= base && (char *)p < base + cap)) return;  // the block's: back at reset()
        Spill *s = (Spill *)p - 1;
        if (s->prev) s->prev->next = s->next;
        else spill = s->next;
        if (s->next) s->next->prev = s->prev;
        free(s);
    }
    void reset() {  // only once nothing lives in the block any more
        while (spill) {
            Spill *n = spill->next;
            free(spill);
            spill = n;
        }
        used = 0;
    }
};

template <class T>
struct MemoAlloc {
    typedef T value_type;
    MemoArena *arena;
    explicit MemoAlloc(MemoArena *a) : arena(a) {}
    template <class U>
    MemoAlloc(const MemoAlloc<U> &o) : arena(o.arena) {}
    T *allocate(size_t n) {
        T *p = (T *)arena->take(n * sizeof(T));
        if (!p) abort();  // as the default allocator does with exceptions off
        return p;
    }
    void deallocate(T *p, size_t) { arena->give(p); }
    template <class U>
    bool operator==(const MemoAlloc<U> &o) const { return arena == o.arena; }
    template <class U>
    bool operator!=(const MemoAlloc<U> &o) const { return arena != o.arena; }
};

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
    Dictionary() = default;
    Dictionary(const Dictionary &) = delete;  // its caches point into its own blocks
    Dictionary &operator=(const Dictionary &) = delete;
    bool open(Reader *r, std::string &error);
    bool isOpen() const { return reader_ != nullptr; }

    uint32_t entryCount() const { return entryCount_; }
    uint32_t keyCount() const { return keyCount_; }

    // All entries whose key equals `key` exactly (key already normalized).
    void exact(const std::string &key, std::vector<uint32_t> &entryIds);
    // Whether some key starts with `prefix` (normalized). Remembered, like exact().
    bool hasPrefix(const std::string &prefix);
    // Keys that start with `prefix` (normalized); returns (entryId, key) pairs, at most `limit`
    // keys scanned.
    void prefix(const std::string &prefix, uint32_t limit,
                std::vector<std::pair<uint32_t, std::string>> &out);

    uint32_t posMask(uint32_t entryId);
    // How full the recent-answer caches are (for the read log: they hold on to memory between reads).
    void cacheCounts(size_t &exact, size_t &prefix, size_t &mask) const {
        exact = exactMemo_.size();
        prefix = prefixMemo_.size();
        mask = maskMemo_.size();
    }
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
    // The recent-answer caches below are emptied (their block reset) when this full. They stay for the session, so
    // they come out of the reader's room: at 512 / 512 / 4096 in malloc'd pieces they took ~100-170 KB in a long
    // session and a top-screen read ran out of memory (owner, Mario & Luigi, 2026-11-04, 36 reads); 128 / 128 / 1024
    // after that. Back at full size (2026-11-08) now that they live in fixed blocks (144 KB, taken at open()) and the
    // reader's other memory is kept for the session: ~11% fewer dictionary reads.
    enum : size_t { kExactMemoMax = 512, kPrefixMemoMax = 512, kMaskMemoMax = 4096 };
    // Kept in one fixed block each (MemoArena), keys as views of bytes copied into it: the same maps and the same
    // emptying as before (what is remembered changes which masks posMask() answers with), only the memory moved.
    typedef std::vector<uint32_t, MemoAlloc<uint32_t>> MemoIds;
    typedef std::unordered_map<std::string_view, MemoIds, std::hash<std::string_view>, std::equal_to<std::string_view>,
                               MemoAlloc<std::pair<const std::string_view, MemoIds>>> ExactMemo;
    typedef std::unordered_map<std::string_view, bool, std::hash<std::string_view>, std::equal_to<std::string_view>,
                               MemoAlloc<std::pair<const std::string_view, bool>>> PrefixMemo;
    typedef std::unordered_map<uint32_t, uint32_t, std::hash<uint32_t>, std::equal_to<uint32_t>,
                               MemoAlloc<std::pair<const uint32_t, uint32_t>>> MaskMemo;
    MemoArena exactArena_, prefixArena_, maskArena_;
    ExactMemo exactMemo_{0, std::hash<std::string_view>(), std::equal_to<std::string_view>(), MemoAlloc<int>(&exactArena_)};
    PrefixMemo prefixMemo_{0, std::hash<std::string_view>(), std::equal_to<std::string_view>(), MemoAlloc<int>(&prefixArena_)};
    MaskMemo maskMemo_{0, std::hash<uint32_t>(), std::equal_to<uint32_t>(), MemoAlloc<int>(&maskArena_)};
    void dropExact();
    void dropPrefix();
    void dropMask();
    std::string_view memoKey(MemoArena &a, const std::string &key);  // key's bytes copied into a
public:
    // How the caches' blocks are used (for tests / the log): high-water marks and pieces that didn't fit.
    // [0] exact, [1] prefix, [2] mask
    void memoArenaStats(size_t peakBytes[3], size_t capBytes[3], size_t overflows[3]) const {
        const MemoArena *a[3] = {&exactArena_, &prefixArena_, &maskArena_};
        for (int i = 0; i < 3; i++) {
            peakBytes[i] = a[i]->peak;
            capBytes[i] = a[i]->cap;
            overflows[i] = a[i]->overflow;
        }
    }
    ~Dictionary();
private:
    // version 2: every fenceStep_-th key in memory (fencePool_ from fenceAt_[i] to fenceAt_[i + 1])
    uint32_t version_ = 1, keyRec_ = 8, fenceStep_ = 0;
    std::string fencePool_;
    std::vector<uint32_t> fenceAt_;
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
