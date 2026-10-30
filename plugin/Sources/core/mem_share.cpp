#include "core/mem_share.hpp"

#include <cstring>

namespace kt {
namespace memshare {

bool countsPlugin(int64_t value, int64_t committed, uint32_t plugin) {
    if (plugin == 0 || committed <= 0 || value < (int64_t)plugin) return false;
    // with the share: close to `committed`; without it: about `plugin` below
    return value + (int64_t)(plugin / 2) > committed;
}

static bool takeOff(uint32_t &lo, uint32_t &hi, uint32_t plugin) {
    int64_t v = (int64_t)(((uint64_t)hi << 32) | lo);
    if (v < (int64_t)plugin) return false;  // never below zero
    v -= plugin;
    lo = (uint32_t)v;
    hi = (uint32_t)((uint64_t)v >> 32);
    return true;
}

void adjustRegisters(const Plan &plan, Counts &counts, uint32_t svc, uint32_t *regs) {
    if ((int32_t)regs[0] < 0) return;  // the call failed: nothing to change
    if (svc == kGetSystemInfo) {
        // inputs: r1 = type, r2 = param
        if (plan.appUsed && regs[5] == 0 && regs[6] == 1 && takeOff(regs[1], regs[2], plan.plugin))
            counts.systemInfo++;
    } else if (svc == kGetProcessInfo) {
        // inputs: r1 = process, r2 = type
        uint32_t type = regs[6];
        if (type < (uint32_t)kProcessInfoTypes && plan.processInfo[type] &&
            takeOff(regs[1], regs[2], plan.plugin))
            counts.processInfo++;
    }
}

void adjustLimits(const Plan &plan, Counts &counts, uint32_t svc, int32_t result, int64_t *values,
                  const uint32_t *names, int32_t count) {
    if (result < 0 || !plan.limits || !values || !names || count <= 0 || count > 16) return;
    if (svc != kLimitValues && svc != kCurrentValues) return;
    for (int32_t i = 0; i < count; i++) {
        uint32_t name;
        std::memcpy(&name, names + i, sizeof name);
        if (name != kResLimitCommit) continue;
        int64_t v;
        std::memcpy(&v, values + i, sizeof v);
        if (v < (int64_t)plan.plugin) continue;
        v -= plan.plugin;
        std::memcpy(values + i, &v, sizeof v);
        if (svc == kLimitValues) counts.limitValues++;
        else counts.currentValues++;
    }
}

bool isRelocatable(uint32_t ins) {
    if ((ins >> 28) != 0xE) return false;          // conditional (or an unconditional-space op)
    if ((ins >> 24) == 0xEF) return false;         // svc: would be a second, unpatched call
    if (((ins >> 25) & 7) == 5) return false;      // b / bl: pc-relative
    if ((ins & 0x0FFFFFD0) == 0x012FFF10)          // bx / blx rM (its middle bits are all ones)
        return (ins & 0xF) != 15;
    if (((ins >> 16) & 0xF) == 15) return false;   // pc as the base / first operand
    if ((ins & 0xF) == 15) return false;           // pc as a register operand (or an immediate
                                                   // ending in 0xF: refused to be safe)
    return true;
}

bool isPatchableCall(const uint32_t *text, uint32_t words, uint32_t i, uint32_t svc) {
    if (!text || i + 1 >= words || i == 0) return false;
    if (text[i] != svcInstruction(svc)) return false;
    return isRelocatable(text[i + 1]);
}

static const uint32_t kLdrPcLiteral = 0xE51FF004;  // ldr pc, [pc, #-4]: jump to the next word

void buildTrampoline(uint32_t *t, uint32_t tAddr, uint32_t standIn, uint32_t moved, uint32_t resume) {
    t[0] = 0xE24DD008;      // sub  sp, sp, #8
    t[1] = 0xE58DC000;      // str  r12, [sp]          the game's r12
    t[2] = 0xE59FC008;      // ldr  r12, [pc, #8]      = t[6]
    t[3] = 0xE58DC004;      // str  r12, [sp, #4]      where the stand-in goes back to
    t[4] = kLdrPcLiteral;   // ldr  pc, [pc, #-4]
    t[5] = standIn;
    t[6] = tAddr + 7 * 4;   // -> t[7]
    t[7] = moved;           // the instruction the patch moved
    t[8] = kLdrPcLiteral;
    t[9] = resume;
}

void buildSitePatch(uint32_t site[2], uint32_t tAddr) {
    site[0] = kLdrPcLiteral;
    site[1] = tAddr;
}

}  // namespace memshare
}  // namespace kt
