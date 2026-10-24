// correct.hpp - fixes OCR misreads with the dictionary: where the recognizer was unsure of a
// character (its other guesses, from local_ocr), a guess that makes a dictionary word replaces a
// character that makes none (造跡 -> 遺跡: 遺 was its second guess, and 遺跡 is a word).
#pragma once
#include <string>
#include <vector>

#include "core/dict.hpp"
#include "core/local_ocr.hpp"

namespace kt {

// Length in characters of the longest dictionary word (conjugated forms included) that covers
// text[byte, byte + len); 0 if none, 1 if only the character itself is a word.
unsigned wordCover(Dictionary &dict, const std::string &text, size_t byte, size_t len);

// Applies the replacements to `text` (alts as LocalOcr reports them, byte offsets into text).
// Returns how many characters were changed.
unsigned fixOcrLine(Dictionary &dict, std::string &text, const std::vector<OcrAlt> &alts);

}  // namespace kt
