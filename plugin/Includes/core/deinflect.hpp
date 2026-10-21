// deinflect.hpp - rule-based conjugation reversal (食べられなかった -> 食べる),
// modelled on how Yomitan does it: each rule swaps a suffix and records which word
// class the result must belong to; the dictionary lookup then checks that class.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kt {

struct Candidate {
    std::string text;
    uint32_t type = 0;                 // required POS bits; 0 = the original input (anything)
    std::vector<const char *> reasons; // outermost first, e.g. {"past","negative","passive"}
};

// Returns the input itself first, then every plausible dictionary form.
std::vector<Candidate> deinflect(const std::string &word);

// "passive › negative › past" (innermost first, reads naturally from the base word)
std::string describeReasons(const std::vector<const char *> &reasons);

}  // namespace kt
