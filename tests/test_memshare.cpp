// Tests for core/mem_share: taking the plugin's share off the game's answers about its memory.
#include "core/mem_share.hpp"

#include <cstdio>
#include <cstdlib>

using namespace kt::memshare;

static int g_fail = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

static const uint32_t P = 5 * 1024 * 1024;  // the plugin's share
static const int64_t KB = 1024;

static void testCountsPlugin() {
    const int64_t committed = 9816 * KB;  // 逆転裁判123 at boot (log), share included
    CHECK(countsPlugin(committed, committed, P));               // the same figure
    CHECK(countsPlugin(committed - 300 * KB, committed, P));    // a little less (no stacks, say)
    CHECK(!countsPlugin(committed - P, committed, P));          // the figure without the share
    CHECK(!countsPlugin(committed - P - 600 * KB, committed, P));
    CHECK(!countsPlugin(37, committed, P));                     // a count, not memory
    CHECK(!countsPlugin(-1, committed, P));
    CHECK(!countsPlugin(committed, committed, 0));
}

static void setValue(uint32_t *r, int64_t v) {
    r[1] = (uint32_t)v;
    r[2] = (uint32_t)((uint64_t)v >> 32);
}
static int64_t value(const uint32_t *r) { return (int64_t)(((uint64_t)r[2] << 32) | r[1]); }

static void testRegisters() {
    Plan plan;
    plan.plugin = P;
    plan.processInfo[0] = true;
    plan.appUsed = true;
    Counts c;
    uint32_t r[8] = {};

    // GetProcessInfo type 0: taken off
    setValue(r, 30000 * KB);
    r[6] = 0;
    adjustRegisters(plan, c, kGetProcessInfo, r);
    CHECK(value(r) == 30000 * KB - P);
    CHECK(c.processInfo == 1);

    // type 2 (not in the plan): left alone
    setValue(r, 30000 * KB);
    r[6] = 2;
    adjustRegisters(plan, c, kGetProcessInfo, r);
    CHECK(value(r) == 30000 * KB);

    // types past the memory ones: left alone even if a plan said otherwise
    setValue(r, 30000 * KB);
    r[6] = 7;
    adjustRegisters(plan, c, kGetProcessInfo, r);
    CHECK(value(r) == 30000 * KB);

    // a failed call: left alone
    setValue(r, 30000 * KB);
    r[0] = 0xD8E007F7;
    r[6] = 0;
    adjustRegisters(plan, c, kGetProcessInfo, r);
    CHECK(value(r) == 30000 * KB);
    r[0] = 0;

    // never below zero
    setValue(r, 1000);
    adjustRegisters(plan, c, kGetProcessInfo, r);
    CHECK(value(r) == 1000);

    // GetSystemInfo(0, 1): taken off; (0, 2) and (0x10000, ...): left alone
    setValue(r, 60000 * KB);
    r[5] = 0;
    r[6] = 1;
    adjustRegisters(plan, c, kGetSystemInfo, r);
    CHECK(value(r) == 60000 * KB - P);
    CHECK(c.systemInfo == 1);
    setValue(r, 60000 * KB);
    r[6] = 2;
    adjustRegisters(plan, c, kGetSystemInfo, r);
    CHECK(value(r) == 60000 * KB);
    setValue(r, 60000 * KB);
    r[5] = 0x10000;
    r[6] = 1;
    adjustRegisters(plan, c, kGetSystemInfo, r);
    CHECK(value(r) == 60000 * KB);

    // a value above 4 GB keeps its high word right
    setValue(r, ((int64_t)1 << 32) + 100);
    r[5] = 0;
    r[6] = 1;
    adjustRegisters(plan, c, kGetSystemInfo, r);
    CHECK(value(r) == ((int64_t)1 << 32) + 100 - P);

    // an empty plan changes nothing
    Plan none;
    setValue(r, 60000 * KB);
    adjustRegisters(none, c, kGetSystemInfo, r);
    CHECK(value(r) == 60000 * KB);
}

static void testLimits() {
    Plan plan;
    plan.plugin = P;
    plan.limits = true;
    Counts c;
    // the raised limit and the game's committed memory, as the game sees them afterwards
    int64_t limits[3] = {70656 * KB, 32, 16};
    const uint32_t names[3] = {5 /* threads */, kResLimitCommit, 1};
    limits[1] = 70656 * KB;  // commit
    limits[2] = 70656 * KB;  // commit again
    limits[0] = 32;
    adjustLimits(plan, c, kLimitValues, 0, limits, names, 3);
    CHECK(limits[0] == 32);
    CHECK(limits[1] == 65536 * KB);  // the game's own 64 MB
    CHECK(limits[2] == 65536 * KB);
    CHECK(c.limitValues == 2);

    int64_t current[1] = {9816 * KB};
    const uint32_t commit[1] = {kResLimitCommit};
    adjustLimits(plan, c, kCurrentValues, 0, current, commit, 1);
    CHECK(current[0] == 9816 * KB - P);
    CHECK(c.currentValues == 1);

    // free memory as the game works it out is the same as the real free memory
    const int64_t realLimit = 70656 * KB, realUsed = 30000 * KB;
    int64_t l[1] = {realLimit}, u[1] = {realUsed};
    adjustLimits(plan, c, kLimitValues, 0, l, commit, 1);
    adjustLimits(plan, c, kCurrentValues, 0, u, commit, 1);
    CHECK(l[0] - u[0] == realLimit - realUsed);

    // failed call, no plan, silly counts, other calls: left alone
    int64_t v[1] = {70656 * KB};
    adjustLimits(plan, c, kLimitValues, -1, v, commit, 1);
    CHECK(v[0] == 70656 * KB);
    Plan none;
    none.plugin = P;
    adjustLimits(none, c, kLimitValues, 0, v, commit, 1);
    CHECK(v[0] == 70656 * KB);
    adjustLimits(plan, c, kLimitValues, 0, v, commit, 0);
    adjustLimits(plan, c, kLimitValues, 0, v, commit, 1000);
    adjustLimits(plan, c, kGetProcessInfo, 0, v, commit, 1);
    CHECK(v[0] == 70656 * KB);
    adjustLimits(plan, c, kLimitValues, 0, nullptr, commit, 1);
    adjustLimits(plan, c, kLimitValues, 0, v, nullptr, 1);
}

static void testPatchableCall() {
    const uint32_t svc2B = svcInstruction(kGetProcessInfo);
    // str r0, [sp, #-4]! / svc 0x2B / ldr r3, [sp], #4 / str r1, [r3]
    const uint32_t wrapper[] = {0xE52D0004, svc2B, 0xE49D3004, 0xE5831000, 0xE12FFF1E};
    CHECK(isPatchableCall(wrapper, 5, 1, kGetProcessInfo));
    CHECK(!isPatchableCall(wrapper, 5, 1, kGetSystemInfo));  // a different call
    CHECK(!isPatchableCall(wrapper, 5, 0, kGetProcessInfo));
    // the next instruction is conditional (bne), or uses pc: can't be moved
    const uint32_t cond[] = {0xE52D0004, svc2B, 0x1A000002};
    CHECK(!isPatchableCall(cond, 3, 1, kGetProcessInfo));
    const uint32_t pcrel[] = {0xE52D0004, svc2B, 0xE59F0004};
    CHECK(!isPatchableCall(pcrel, 3, 1, kGetProcessInfo));
    const uint32_t bxlr[] = {0xE52D0004, svc2B, 0xE12FFF1E};
    CHECK(isPatchableCall(bxlr, 3, 1, kGetProcessInfo));
    // the last word of the code, or the first: nothing to check around it
    const uint32_t end[] = {0xE52D0004, svc2B};
    CHECK(!isPatchableCall(end, 2, 1, kGetProcessInfo));
    const uint32_t first[] = {svc2B, 0xE12FFF1E};
    CHECK(!isPatchableCall(first, 2, 0, kGetProcessInfo));
    CHECK(svcInstruction(0x3A) == 0xEF00003A);
}

static void testRelocatable() {
    CHECK(isRelocatable(0xE49D3004));   // ldr r3, [sp], #4
    CHECK(isRelocatable(0xE12FFF1E));   // bx lr
    CHECK(isRelocatable(0xE3500000));   // cmp r0, #0
    CHECK(isRelocatable(0xE1B01FA0));   // lsrs r1, r0, #31
    CHECK(isRelocatable(0xE8BD8010));   // pop {r4, pc}
    CHECK(!isRelocatable(0xE12FFF1F));  // bx pc
    CHECK(!isRelocatable(0xE59F0004));  // ldr r0, [pc, #4]
    CHECK(!isRelocatable(0xE28F0010));  // add r0, pc, #16
    CHECK(!isRelocatable(0xEB000010));  // bl
    CHECK(!isRelocatable(0xEA000010));  // b
    CHECK(!isRelocatable(0x1A000002));  // bne
    CHECK(!isRelocatable(0xEF00002B));  // svc
    CHECK(!isRelocatable(0xE080000F));  // add r0, r0, pc
    CHECK(!isRelocatable(0xFA000000));  // blx (immediate)
}

static void testTrampoline() {
    uint32_t t[kTrampolineWords];
    const uint32_t at = 0x01000000, standIn = 0x07001234, moved = 0xE49D3004, resume = 0x00102348;
    buildTrampoline(t, at, standIn, moved, resume);
    CHECK(t[0] == 0xE24DD008);   // sub sp, sp, #8
    CHECK(t[2] == 0xE59FC008);   // ldr r12, [pc, #8]: pc reads as at+16, so it loads t[6]
    CHECK(t[4] == 0xE51FF004);
    CHECK(t[5] == standIn);
    CHECK(t[6] == at + 28);      // t[7]
    CHECK(t[7] == moved);
    CHECK(t[8] == 0xE51FF004);
    CHECK(t[9] == resume);
    uint32_t site[2];
    buildSitePatch(site, at);
    CHECK(site[0] == 0xE51FF004 && site[1] == at);
}

int main() {
    testRelocatable();
    testTrampoline();
    testCountsPlugin();
    testRegisters();
    testLimits();
    testPatchableCall();
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("test_memshare: all passed\n");
    return 0;
}
