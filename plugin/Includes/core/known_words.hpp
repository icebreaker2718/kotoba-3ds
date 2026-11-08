// known_words.hpp - the words the player has already (Anki cards, words saved on the 3DS), so the
// lookup screens can show them in green. A list of words, one per line, either kept in one block of
// memory and searched by halves (load/adopt), or searched where it is, on the SD card (openFile): a
// small index in memory (the first word of each ~2 KB of the file) and one block read per check, so a
// collection of any size costs a few KB. Platform independent (unit-tested natively).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"  // Reader

namespace kt {

class KnownWords {
public:
    KnownWords() = default;
    ~KnownWords();
    KnownWords(const KnownWords &) = delete;
    KnownWords &operator=(const KnownWords &) = delete;

    void clear();
    // Replaces the list with `text`'s lines (blank lines and lines starting with '#' skipped, a
    // trailing '\r' and spaces trimmed). At most maxBytes of it is kept. False when memory ran out
    // (then the list is empty).
    bool load(const char *text, size_t len, size_t maxBytes = 1u << 20);
    // The same, for a file read into buf (malloc'd, len bytes of it read and room for one more):
    // the list keeps buf (no copy) and frees it. fileSize > len: the file was longer (then the
    // last, partial line is dropped).
    bool adopt(char *buf, size_t len, size_t fileSize);
    // Searches the list in r (not owned; kept open by the caller until clear()) instead of loading it. The
    // file is read once to index it: its words must come sorted by their UTF-8 bytes (as mine.py and
    // ocr_server.py write them); a word out of order (Make Anki cards appends its cards' words at the
    // end) is kept in memory instead, up to kMaxSpill of them (the rest are left out: spillDropped()).
    // False when it couldn't be read or memory ran out (then the list is empty).
    static const size_t kMaxSpill = 4096;
    bool openFile(Reader *r, size_t blockBytes = 2048);
    size_t spillDropped() const { return spillDropped_; }
    size_t fileWords() const { return fileCount_; }
    size_t fileBlocks() const { return blockStart_.size(); }
    // One more word (a word saved just now). reserve(): room for that many without allocating.
    void add(const std::string &word);
    void reserve(size_t n) { extra_.reserve(n); }
    // Takes back a word add()ed (not one of the list's).
    void remove(const std::string &word);
    bool has(const std::string &word) const;
    size_t size() const { return count_ + fileCount_ + spill_.size() + extra_.size(); }
    bool truncated() const { return truncated_; }

private:
    char *pool_ = nullptr;      // the words, each ending in '\0'
    uint32_t *offs_ = nullptr;  // their offsets in pool_, sorted by the words' bytes
    size_t count_ = 0;
    bool truncated_ = false;
    std::vector<std::string> extra_;  // added one by one, sorted

    // openFile()
    bool hasInFile(const char *w) const;
    Reader *file_ = nullptr;
    uint32_t fileSize_ = 0;
    size_t fileCount_ = 0;            // words in the file, in order
    std::vector<uint32_t> blockStart_;  // where each block starts in the file
    std::vector<uint32_t> keyOff_;      // its first word, in keys_
    std::string keys_;                  // those words, each ending in '\0'
    std::vector<std::string> spill_;    // words out of order in the file, sorted
    size_t spillDropped_ = 0;
    mutable std::vector<char> block_;   // the last block read
    mutable int blockIdx_ = -1;
};

}  // namespace kt
