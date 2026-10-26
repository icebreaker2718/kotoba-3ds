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

bool isPatchableCall(const uint32_t *text, uint32_t words, uint32_t i, uint32_t svc) {
    if (!text || i + 1 >= words || i == 0) return false;
    if (text[i] != svcInstruction(svc)) return false;
    // the instruction after it runs whatever the flags say, so it can't depend on them
    return (text[i + 1] >> 28) == 0xE;
}

}  // namespace memshare
}  // namespace kt
