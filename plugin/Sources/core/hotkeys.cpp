#include "core/hotkeys.hpp"

namespace kt {

std::string keyComboName(uint32_t combo) {
    static const struct { uint32_t bit; const char *name; } kNames[] = {
        {keys::ZL, "ZL"}, {keys::ZR, "ZR"}, {keys::L, "L"}, {keys::R, "R"}, {keys::A, "A"}, {keys::B, "B"},
        {keys::X, "X"}, {keys::Y, "Y"}, {keys::Up, "Up"}, {keys::Down, "Down"}, {keys::Left, "Left"},
        {keys::Right, "Right"}, {keys::Start, "Start"}, {keys::Select, "Select"},
    };
    std::string s;
    for (const auto &k : kNames)
        if (combo & k.bit) s += (s.empty() ? "" : " + ") + std::string(k.name);
    return s.empty() ? "Off" : s;
}

bool HotkeyDispatcher::partOfLonger(int i) const {
    for (int j = 0; j < kMax; j++)
        if (j != i && combo_[j] && (combo_[j] & combo_[i]) == combo_[i] && combo_[j] != combo_[i]) return true;
    return false;
}

int HotkeyDispatcher::update(uint32_t down, uint32_t pressed, uint64_t nowMs) {
    if (pending_ >= 0) {
        const uint32_t p = combo_[pending_];
        int best = -1;  // a longer hotkey that contains it, now held whole
        for (int j = 0; j < kMax; j++)
            if (combo_[j] && combo_[j] != p && (combo_[j] & p) == p && (down & combo_[j]) == combo_[j] &&
                (best < 0 || keyCount(combo_[j]) > keyCount(combo_[best])))
                best = j;
        if (best >= 0) {
            pending_ = -1;
            return best;
        }
        if ((down & p) != p || nowMs - pendingAt_ >= kWaitMs) {  // let go, or held long enough: it was meant
            int r = pending_;
            pending_ = -1;
            return r;
        }
        return -1;
    }
    int best = -1;
    for (int i = 0; i < kMax; i++) {
        const uint32_t c = combo_[i];
        if (!c || !(pressed & c) || (down & c) != c) continue;
        if (best < 0 || keyCount(c) > keyCount(combo_[best])) best = i;
    }
    if (best < 0) return -1;
    if (partOfLonger(best)) {
        pending_ = best;
        pendingAt_ = nowMs;
        return -1;
    }
    return best;
}

}  // namespace kt
