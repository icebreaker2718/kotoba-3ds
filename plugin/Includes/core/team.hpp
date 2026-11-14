// team.hpp - threads that share out small jobs by spinning (the spotter's rows, the recognizer's layers).
//
// A runner (LocalOcr::Runner) starts the team's threads together; they stay for the whole piece of work. The
// thread that becomes member 0 (the leader) runs the work and, for each job (n independent items: a row's blocks,
// a layer's output blocks...), publishes it with forEach() and takes items itself; the other members take items as
// they come, spinning in between. An item is ~10-50 us of work, so a thread woken by an event would cost as much as
// it saves; spinning threads would be computing anyway.
//
// An item is claimed by a compare-and-swap of ticket = (job number << 16) | next item, so a thread still looking at
// one job can't take an item of the next. The leader waits for the job's `done` count before the next job, so a
// job's slot (job[number & 1]) is reused only once every item of the job before it has finished.
#pragma once
#include <atomic>
#include <cstdint>

namespace kt {

struct Team {
    typedef void (*Item)(void *ctx, unsigned i, unsigned worker);
    std::atomic<uint32_t> ticket{0};
    std::atomic<unsigned> done{0}, member{0};
    std::atomic<bool> quit{false};
    struct Job {
        std::atomic<unsigned> n{0};  // read before a claim (may be the slot's next job then: the claim fails)
        Item item = nullptr;
        void *ctx = nullptr;
    } job[2];
    uint32_t gen = 0;      // the leader's
    unsigned workers = 1;  // members that may take items (worker indices 0 .. workers - 1)

    // Takes and runs items of the current job until none is left.
    void claim(unsigned worker) {
        for (;;) {
            uint32_t v = ticket.load(std::memory_order_acquire);
            Job &j = job[(v >> 16) & 1u];
            const unsigned i = v & 0xFFFFu;
            if (i >= j.n.load(std::memory_order_relaxed)) return;
            if (!ticket.compare_exchange_weak(v, v + 1u, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
            j.item(j.ctx, i, worker);
            done.fetch_add(1u, std::memory_order_release);
        }
    }
    // Leader: runs item(ctx, i, worker) for every i < n (n < 65536) over the team; returns when all are done.
    void forEach(unsigned n, Item item, void *ctx) {
        if (!n) return;
        const uint32_t g = ++gen & 0xFFFFu;
        Job &j = job[g & 1u];
        j.n.store(n, std::memory_order_relaxed);
        j.item = item;
        j.ctx = ctx;
        done.store(0, std::memory_order_relaxed);
        ticket.store(g << 16, std::memory_order_release);
        claim(0);
        while (done.load(std::memory_order_acquire) < n) {
        }
    }
    // C-style forEach for vocr: team is a Team*.
    static void forEachC(void *team, unsigned n, void (*item)(void *, unsigned, unsigned), void *ctx) {
        ((Team *)team)->forEach(n, item, ctx);
    }
    // A member's entry: returns its index; the leader (0) then runs the work and calls finish(), the others call
    // help(index).
    unsigned join() { return member.fetch_add(1u); }
    void help(unsigned worker) {
        if (worker >= workers) return;
        while (!quit.load(std::memory_order_acquire)) claim(worker);
    }
    void finish() { quit.store(true, std::memory_order_release); }
};

}  // namespace kt
