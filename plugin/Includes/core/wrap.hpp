// wrap.hpp - word wrapping for on-screen text (platform independent, unit-tested).
#pragma once
#include <string>
#include <vector>

namespace kt {

// Width in pixels of a string as drawn (the 3DS build passes the system font's measurement).
typedef float (*TextWidthFn)(const std::string &);

// Splits text at '\n' and wherever a line would exceed maxWidth, preferring spaces.
// Never splits a UTF-8 character. Every output line fits unless a single character doesn't.
std::vector<std::string> wrapText(const std::string &text, float maxWidth, TextWidthFn width);

}  // namespace kt
