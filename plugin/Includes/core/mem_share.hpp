// mem_share.hpp - the game's view of its memory with the plugin's share left out.
//
// On a New 3DS, Luma takes the plugin's memory (5 MiB) out of the game's 64 MB budget. At boot
// the plugin raises the game's commit limit by that much; these functions then keep what the game
// is told consistent with it: whenever the game asks the system how much memory it uses (or what
// its limit is), the plugin's share is taken off the answer. The game then sees a normal 64 MB
// console, whichever way its start-up code sizes its memory.
//
// The 3DS side patches four system calls in the game's code; each patched call runs the real call
// and then passes its registers here. Platform independent, so it is tested on a PC.
#pragma once
#include <cstdint>

namespace kt {
namespace memshare {

// the system calls the game may use to ask about its memory
enum : uint32_t {
    kGetSystemInfo = 0x2A,      // (type 0, param 1): memory used in the application region
    kGetProcessInfo = 0x2B,     // types 0..3: memory used by the process
    kLimitValues = 0x39,        // GetResourceLimitLimitValues: the commit limit
    kCurrentValues = 0x3A,      // GetResourceLimitCurrentValues: memory committed
};
constexpr uint32_t kResLimitCommit = 1;  // ResourceLimitType: the commit (memory) limit
constexpr int kProcessInfoTypes = 4;     // GetProcessInfo types that may be memory amounts

// What to take off, decided once at boot (see countsPlugin).
struct Plan {
    uint32_t plugin = 0;                              // bytes: the plugin's share
    bool processInfo[kProcessInfoTypes] = {};         // GetProcessInfo types whose answer holds it
    bool appUsed = false;                             // GetSystemInfo(0, 1) holds it
    bool limits = false;                              // the commit limit and current values hold it
};

// How many answers were changed, per call (for the log).
struct Counts {
    uint32_t systemInfo = 0, processInfo = 0, limitValues = 0, currentValues = 0;
};

// Does an answer measured at boot include the plugin's share? `committed` is the game's committed
// memory, which does (checked at boot). An answer without the share is about `plugin` below it;
// one with it is within a few hundred KB of it. Counts (handles, threads) are far below both.
bool countsPlugin(int64_t value, int64_t committed, uint32_t plugin);

// After GetSystemInfo / GetProcessInfo: regs[0..2] are the call's outputs (result, value low,
// value high), regs[4..7] its inputs (r0..r3). Changes regs[1..2] when the plan says so.
void adjustRegisters(const Plan &plan, Counts &counts, uint32_t svc, uint32_t *regs);

// After GetResourceLimitLimitValues / CurrentValues: `values` and `names` are the call's arrays.
void adjustLimits(const Plan &plan, Counts &counts, uint32_t svc, int32_t result, int64_t *values,
                  const uint32_t *names, int32_t count);

// May the system call at text[i] be patched? It must be the call itself (svc #n), with the next
// instruction unconditional, so nothing after it reads flags the patch could change.
bool isPatchableCall(const uint32_t *text, uint32_t words, uint32_t i, uint32_t svc);

// The svc instruction for system call n.
constexpr uint32_t svcInstruction(uint32_t n) { return 0xEF000000u | n; }

}  // namespace memshare
}  // namespace kt
