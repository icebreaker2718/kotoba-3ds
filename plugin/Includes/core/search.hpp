// search.hpp - turns what the player typed into a ranked result list.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"

namespace kt {

struct Result {
    uint32_t entryId = 0;
    std::string matchedKey;   // normalized key that matched (dictionary form)
    std::string reasons;      // "passive > negative > past", empty for exact hits
    bool prefixOnly = false;  // only matched as "words starting with…"
    std::string surface;      // text as it appeared (scanAt only), e.g. 食べられなかった
};

struct SearchOutput {
    std::string query;        // normalized query that was searched (hiragana/kanji)
    std::vector<Result> results;
};

// `raw` may mix romaji, kana and kanji. maxResults caps the list (the UI shows them all).
SearchOutput search(Dictionary &dict, const std::string &raw, uint32_t maxResults = 60);

// Yomitan-style scan: words that start at byte offset `start` of `text` (e.g. an OCR'd line),
// longest match first, conjugated forms included. Stops at punctuation.
SearchOutput scanAt(Dictionary &dict, const std::string &text, size_t start, uint32_t maxChars = 14,
                    uint32_t maxResults = 40);

// Splits text into words for the "tap a word" list without a PC: greedy longest dictionary
// match from each position, one character where nothing matches. The pieces join back to `text`.
std::vector<std::string> chunkText(Dictionary &dict, const std::string &text, uint32_t maxChars = 8);

// Picks the spelling the player actually saw: if the matched key is one of the entry's
// kanji forms, use that form instead of the default headword.
std::string expressionFor(const Entry &e, const std::string &matchedKey);

// One line for the results list: "食べる【たべる】 to eat; to live on…"
std::string summaryLine(const Entry &e, const Result &r, size_t maxBytes = 110);

// Full text for the details box.
std::string detailText(const Entry &e, const Result &r);

// Definition for the Anki card: senses joined with <br>.
std::string ankiDefinition(const Entry &e);

// One TSV line for mined.tsv (no trailing newline). Tabs/newlines in fields are stripped.
// Columns: time, titleId, expression, reading, definition, screenshot, query, reasons, entryId,
//          sentence (OCR'd line, empty for manual lookups)
std::string mineLogLine(const std::string &timestamp, const std::string &titleId,
                        const Entry &e, const Result &r, const std::string &screenshot,
                        const std::string &query, const std::string &sentence = "");

}  // namespace kt
