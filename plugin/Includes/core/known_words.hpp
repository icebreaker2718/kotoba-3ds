// known_words.hpp - the words the player has already (Anki cards, words saved on the 3DS), so the
// lookup screens can show them in green. A list of words, one per line, kept in one block of
// memory and searched by halves. Platform independent (unit-tested natively).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
    // One more word (a word saved just now). reserve(): room for that many without allocating.
    void add(const std::string &word);
    void reserve(size_t n) { extra_.reserve(n); }
    // Takes back a word add()ed (not one of the list's).
    void remove(const std::string &word);
    bool has(const std::string &word) const;
    size_t size() const { return count_ + extra_.size(); }
    bool truncated() const { return truncated_; }

private:
    char *pool_ = nullptr;      // the words, each ending in '\0'
    uint32_t *offs_ = nullptr;  // their offsets in pool_, sorted by the words' bytes
    size_t count_ = 0;
    bool truncated_ = false;
    std::vector<std::string> extra_;  // added one by one, sorted
};

}  // namespace kt
