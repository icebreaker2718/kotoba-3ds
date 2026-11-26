// meta_dict.hpp - pitch accent and frequency files (.ktm, tools/build_dict.py --meta): values by (expression,
// reading), read from the SD card one block at a time. Each file keeps one key per 64 blocks in memory (a few KB);
// the files share one block buffer and one buffer for a group's keys, taken once. Platform independent.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"  // Reader

namespace kt {

class MetaFile {
public:
    enum Kind : uint8_t { PITCH = 1, FREQ = 2 };
    bool open(Reader *r, std::string &error);
    bool isOpen() const { return reader_ != nullptr; }
    Kind kind() const { return kind_; }
    const std::string &title() const { return title_; }
    uint32_t records() const { return records_; }
    uint32_t largestGroup() const { return largestGroup_; }
    size_t memoryBytes() const { return groupPool_.capacity() + groupAt_.capacity() * 4 + groupKeyAt_.capacity() * 4; }
    // The value for key (expression TAB reading), copied into out (NUL-terminated): false if there is none.
    // seg / block: the shared buffers (at least largestGroup() and 4096 bytes).
    bool find(const char *key, size_t keyLen, char *out, size_t cap, char *seg, size_t segCap, char *block);

private:
    Reader *reader_ = nullptr;
    Kind kind_ = FREQ;
    std::string title_;
    uint32_t records_ = 0, blockSize_ = 0, blocks_ = 0, dataOff_ = 0, keyOff_ = 0, keyLen_ = 0, groupStep_ = 0;
    uint32_t largestGroup_ = 0;
    std::vector<uint32_t> groupAt_;     // each group's first block key, as an offset into the block keys
    std::string groupPool_;             // the groups' first keys, one after another
    std::vector<uint32_t> groupKeyAt_;  // where each starts in groupPool_ (+ the end)
};

// The pitch accent and frequency files the lookup shows, in file-name order.
class MetaSet {
public:
    static constexpr int kMaxFiles = 8;
    ~MetaSet();
    // Opens one more file (r must stay open while the set is used). False (with error) if it isn't a .ktm.
    bool add(Reader *r, std::string &error);
    int count() const { return count_; }
    const MetaFile &file(int i) const { return files_[i]; }
    int count(MetaFile::Kind k) const;
    // The value in file i for the entry (expression, reading): the pair first, then the expression for any reading.
    bool find(int i, const std::string &expression, const std::string &reading, char *out, size_t cap);

private:
    MetaFile files_[kMaxFiles];
    int count_ = 0;
    char *seg_ = nullptr, *block_ = nullptr;
    size_t segCap_ = 0;
    char key_[512];
};

// Pitch accent: a reading split into morae, with which are high (Yomitan's rules for a downstep position:
// 0 = heiban, 1 = atamadaka, n = the drop after the n-th mora). Pieces of one height go together:
// {byte offset, byte length, high, drop after it}. Returns how many pieces (at most cap).
struct PitchPiece {
    uint16_t off = 0, len = 0;
    bool high = false, drop = false;
};
int pitchPieces(const std::string &reading, int position, PitchPiece *out, int cap);
int moraCount(const std::string &reading);

// "123456" -> "123,456", "13390㋕" -> "13,390㋕"; text that doesn't start with a number as it is.
std::string groupDigits(const std::string &s);

}  // namespace kt
