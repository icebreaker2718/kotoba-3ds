// Runs patched call sites on an ARM CPU (qemu-arm): a fake piece of game code whose svc is patched
// with buildSitePatch, a trampoline from buildTrampoline, and the stand-ins from
// plugin/Sources/mem_hooks.s. Checks the game gets back exactly what the system call would give it,
// less the plugin's share: every other register, lr, the flags and sp as they were, the moved
// instruction run once, and the code after the call reached. Run with tests/build_memhooks_arm.sh.
#include "core/mem_share.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

using namespace kt::memshare;

extern "C" {
void KotobaHookGetSystemInfo(void);
void KotobaHookGetProcessInfo(void);
void KotobaHookLimitValues(void);
void KotobaHookCurrentValues(void);
void RunStub(uint32_t *block);
void HarnessBack(void);
extern int64_t g_fakeValue;
extern uint32_t g_lastSvc;
}

static Plan g_plan;
static Counts g_counts;
static uint32_t g_seen[8];
static int g_adjustCalls = 0;

// as in main.cpp
extern "C" void KotobaMemHookAdjust(uint32_t svc, uint32_t *regs) {
    std::memcpy(g_seen, regs, sizeof g_seen);
    g_adjustCalls++;
    if (svc == kLimitValues || svc == kCurrentValues)
        adjustLimits(g_plan, g_counts, svc, (int32_t)regs[0], (int64_t *)(uintptr_t)regs[4],
                     (const uint32_t *)(uintptr_t)regs[6], (int32_t)regs[7]);
    else
        adjustRegisters(g_plan, g_counts, svc, regs);
}

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

static const uint32_t P = 5 * 1024 * 1024;

struct Run {
    uint32_t out[13], lr, cpsr, sp, spBefore;
};

// A fake piece of game code per stand-in, in executable memory: the patched call site at word 0,
// the code after it at word 2 (jumps back to the harness), the trampoline at word 16.
static uint32_t *g_code;
static const uint32_t kMoved = 0xE28BB001;  // add r11, r11, #1: the instruction the patch moves

static uint32_t makeSite(int k, void (*standIn)(void)) {
    uint32_t *c = g_code + k * 64;
    const uint32_t site = (uint32_t)(uintptr_t)c, tramp = (uint32_t)(uintptr_t)(c + 16);
    buildSitePatch(c, tramp);
    c[2] = 0xE51FF004;  // ldr pc, [pc, #-4]
    c[3] = (uint32_t)(uintptr_t)HarnessBack;
    buildTrampoline(c + 16, tramp, (uint32_t)(uintptr_t)standIn, kMoved, site + 8);
    __builtin___clear_cache((char *)c, (char *)(c + 64));
    return site;
}

static Run run(uint32_t site, const uint32_t in[4], bool misalign) {
    alignas(8) static uint32_t block[64];
    std::memset(block, 0, sizeof block);
    block[0] = site;
    std::memcpy(block + 1, in, 16);
    block[50] = misalign;
    g_adjustCalls = 0;
    RunStub(block);
    Run r;
    std::memcpy(r.out, block + 5, sizeof r.out);
    r.lr = block[18];
    r.cpsr = block[19];
    r.sp = block[20];
    r.spBefore = block[21];
    return r;
}

static void checkKept(const Run &r, uint32_t r3) {
    static const uint32_t markers[] = {0x44444444, 0x55555555, 0x66666666, 0x77777777,
                                       0x88888888, 0x99999999, 0xAAAAAAAA, 0xBBBBBBBC /* moved add */,
                                       0xCCCCCCCC};
    CHECK(r.out[3] == r3);
    for (int i = 0; i < 9; i++) CHECK(r.out[4 + i] == markers[i]);
    CHECK(r.lr == 0xEEEEEEEE);
    CHECK((r.cpsr >> 28) == 0x6);  // Z and C as they were
    CHECK(r.sp == r.spBefore);
    CHECK(g_adjustCalls == 1);
}

static int64_t value(const Run &r) { return (int64_t)(((uint64_t)r.out[2] << 32) | r.out[1]); }

int main() {
    g_code = (uint32_t *)mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_code == MAP_FAILED) {
        std::printf("mmap failed\n");
        return 1;
    }
    const uint32_t siteProcess = makeSite(0, KotobaHookGetProcessInfo);
    const uint32_t siteSystem = makeSite(1, KotobaHookGetSystemInfo);
    const uint32_t siteLimit = makeSite(2, KotobaHookLimitValues);
    const uint32_t siteCurrent = makeSite(3, KotobaHookCurrentValues);
    g_plan.plugin = P;
    g_plan.processInfo[0] = true;
    g_plan.appUsed = true;
    g_plan.limits = true;

    for (int misalign = 0; misalign < 2; misalign++) {
        // GetProcessInfo(process, type 0): r1 = process, r2 = type
        g_fakeValue = 30000 * 1024;
        const uint32_t pi[4] = {0x11111111, 0xFFFF8001, 0, 0x33333333};
        Run r = run(siteProcess, pi, misalign);
        CHECK(g_lastSvc == 0x2B);
        CHECK(r.out[0] == 0);
        CHECK(value(r) == 30000 * 1024 - P);
        CHECK(g_seen[4] == pi[0] && g_seen[5] == pi[1] && g_seen[6] == pi[2] && g_seen[7] == pi[3]);
        checkKept(r, pi[3]);

        // type 4 (a count): unchanged
        g_fakeValue = 37;
        const uint32_t pi4[4] = {0, 0xFFFF8001, 4, 0x33333333};
        r = run(siteProcess, pi4, misalign);
        CHECK(value(r) == 37);
        checkKept(r, pi4[3]);

        // GetSystemInfo(0, 1), with a value above 4 GB to check the high word
        g_fakeValue = ((int64_t)3 << 32) + 0x10000000;
        const uint32_t si[4] = {0x12345678, 0, 1, 0x3333AAAA};
        r = run(siteSystem, si, misalign);
        CHECK(g_lastSvc == 0x2A);
        CHECK(value(r) == ((int64_t)3 << 32) + 0x10000000 - P);
        checkKept(r, si[3]);

        // GetResourceLimitLimitValues(values, handle, names, 2): commit taken off, threads not
        alignas(8) int64_t values[2] = {-1, -1};
        uint32_t names[2] = {5, kResLimitCommit};
        g_fakeValue = 70656 * 1024;
        const uint32_t lv[4] = {(uint32_t)(uintptr_t)values, 0x1234, (uint32_t)(uintptr_t)names, 2};
        r = run(siteLimit, lv, misalign);
        CHECK(g_lastSvc == 0x39);
        CHECK(r.out[0] == 0);
        CHECK(values[0] == 7);
        CHECK(values[1] == 65536 * 1024);
        checkKept(r, lv[3]);

        // GetResourceLimitCurrentValues(values, handle, names, 1)
        alignas(8) int64_t cur[1] = {-1};
        uint32_t commit[1] = {kResLimitCommit};
        g_fakeValue = 9816 * 1024;
        const uint32_t cv[4] = {(uint32_t)(uintptr_t)cur, 0x1234, (uint32_t)(uintptr_t)commit, 1};
        r = run(siteCurrent, cv, misalign);
        CHECK(g_lastSvc == 0x3A);
        CHECK(cur[0] == 9816 * 1024 - (int64_t)P);
        checkKept(r, cv[3]);
    }
    CHECK(g_counts.processInfo == 2 && g_counts.systemInfo == 2);
    CHECK(g_counts.limitValues == 2 && g_counts.currentValues == 2);

    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("test_memhooks_arm: all passed\n");
    return 0;
}
