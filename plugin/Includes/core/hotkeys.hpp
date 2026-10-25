// hotkeys.hpp - the buttons that start a read (one combination per reading mode, one button or
// several), their names, and telling them apart when one is part of another.
// Button bits are the 3DS's (HID / CTRPF Key): A = 1, B = 2, Select = 4, ... ZL = 1 << 14.
#pragma once
#include <cstdint>
#include <string>

namespace kt {

namespace keys {
enum : uint32_t {
    A = 1u << 0, B = 1u << 1, Select = 1u << 2, Start = 1u << 3, Right = 1u << 4, Left = 1u << 5,
    Up = 1u << 6, Down = 1u << 7, R = 1u << 8, L = 1u << 9, X = 1u << 10, Y = 1u << 11,
    ZL = 1u << 14, ZR = 1u << 15,
    // what a hotkey may use: the buttons and the D-pad (not the touchscreen or the sticks)
    Usable = A | B | Start | Right | Left | Up | Down | R | L | X | Y | ZL | ZR,
};
}

// "ZL + ZR", "Y", "L + R + Down"; "Off" for 0.
std::string keyComboName(uint32_t combo);
inline int keyCount(uint32_t combo) {
    int n = 0;
    for (; combo; combo &= combo - 1) n++;
    return n;
}

// Picks which of n hotkeys (0 = off) the buttons start, frame by frame. A hotkey that is part
// of another (Y, and Y + R) waits a moment: for the longer one, or for its buttons to be let go.
class HotkeyDispatcher {
public:
    static const int kMax = 4;
    static const uint32_t kWaitMs = 250;
    void set(int i, uint32_t combo) { if (i >= 0 && i < kMax) combo_[i] = combo; }
    uint32_t get(int i) const { return i >= 0 && i < kMax ? combo_[i] : 0; }
    // down: the buttons held now; pressed: those pressed since the last frame. Returns the hotkey
    // to run now, or -1.
    int update(uint32_t down, uint32_t pressed, uint64_t nowMs);
    void reset() { pending_ = -1; }

private:
    uint32_t combo_[kMax] = {0, 0, 0, 0};
    int pending_ = -1;
    uint64_t pendingAt_ = 0;
    bool partOfLonger(int i) const;
};

}  // namespace kt
