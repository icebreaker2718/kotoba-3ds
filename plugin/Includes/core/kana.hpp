// kana.hpp - UTF-8 helpers, romaji -> hiragana, key normalization.
// Platform independent (compiled both for the 3DS plugin and the native tests).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kt {

// Decode one UTF-8 code point starting at s[i]; advances i. Returns 0xFFFD on bad input.
uint32_t utf8Next(const std::string &s, size_t &i);
void utf8Append(std::string &out, uint32_t cp);
std::vector<uint32_t> utf8Decode(const std::string &s);
size_t utf8Length(const std::string &s);
// Remove the last code point (for "delete last character").
void utf8PopBack(std::string &s);

bool isKanaCp(uint32_t cp);
bool isKanjiCp(uint32_t cp);

// Katakana -> hiragana. Must match normalize_key() in tools/build_dict.py.
std::string normalizeKey(const std::string &s);

// Converts ASCII romaji runs to hiragana; leaves everything else (kanji, kana) alone.
// Trailing incomplete input ("k", "sh") is left as ASCII so it can be shown while typing.
// A trailing single "n" stays "n" unless finalN is true (then it becomes ん).
std::string romajiToKana(const std::string &in, bool finalN = true);

}  // namespace kt
