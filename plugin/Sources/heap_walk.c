// heap_walk.c - see heap_walk.h. The layout is mallocr.c's (newlib, dlmalloc 2.6.4), 32-bit: a chunk is
// prev_size, size (bit 0: the previous chunk in use), then fd/bk when free; the bins are av_[2 + 2i] / av_[3 + 2i]
// seen as a chunk 8 bytes before them, bin 0's fd is top, bin 1 is the last remainder, av_[1] the bins bitmap.
#include "heap_walk.h"

#include <malloc.h>
#include <reent.h>
#include <string.h>
#include <unistd.h>

typedef struct Chunk {
    unsigned prev_size, size;
    struct Chunk *fd, *bk;
} Chunk;

extern void *__malloc_av_[] __attribute__((weak));
extern char *__malloc_sbrk_base __attribute__((weak));
extern unsigned long __malloc_top_pad __attribute__((weak));
extern unsigned long __malloc_trim_threshold __attribute__((weak));
extern unsigned long __malloc_max_sbrked_mem __attribute__((weak));
extern char *fake_heap_end __attribute__((weak));

#define SZ(p) ((p)->size & ~3u)
#define BIN(i) ((Chunk *)((char *)&__malloc_av_[2 * (i) + 2] - 8))

static int inBins(const Chunk *c) {
    for (int i = 1; i < 128; i++) {
        Chunk *b = BIN(i);
        unsigned guard = 0;
        for (Chunk *p = b->fd; p != b && guard < 100000; p = p->fd, guard++)
            if (p == c) return 1;
    }
    return 0;
}

void heap_walk(HeapWalk *w, unsigned request) {
    memset(w, 0, sizeof *w);
    if (!__malloc_av_ || !&__malloc_sbrk_base) return;
    __malloc_lock(_REENT);
    w->ok = 1;
    char *base = __malloc_sbrk_base;
    char *brk = (char *)sbrk(0);
    Chunk *top = (Chunk *)__malloc_av_[2];
    w->base = (unsigned)base;
    w->brk = (unsigned)brk;
    w->heapEnd = &fake_heap_end ? (unsigned)fake_heap_end : 0;
    w->top = (unsigned)top;
    w->topSize = SZ(top);
    w->topPad = &__malloc_top_pad ? (unsigned)__malloc_top_pad : 0;
    w->trimThreshold = &__malloc_trim_threshold ? (unsigned)__malloc_trim_threshold : 0;
    w->maxSbrked = &__malloc_max_sbrked_mem ? (unsigned)__malloc_max_sbrked_mem : 0;
    w->binBlocks = (unsigned)__malloc_av_[1];
    w->lastRemainder = BIN(1)->fd != BIN(1) ? SZ(BIN(1)->fd) : 0;

    // the bins
    for (int i = 1; i < 128; i++) {
        Chunk *b = BIN(i);
        unsigned guard = 0;
        for (Chunk *p = b->fd; p != b && guard < 100000; p = p->fd, guard++) {
            w->binChunks++;
            w->binBytes += SZ(p);
            if (SZ(p) > w->binLargest) w->binLargest = SZ(p);
        }
    }

    // every chunk from the heap's start to top
    Chunk *fit[8];
    unsigned nfit = 0;
    if (base && base != (char *)-1) {
        char *p = base;
        if ((unsigned)(p + 8) & 7u) p += 8 - ((unsigned)(p + 8) & 7u);  // the first chunk, aligned as malloc does
        while (p < (char *)top) {
            Chunk *c = (Chunk *)p;
            const unsigned s = SZ(c);
            if (s == 4) { w->fencepost = 1; break; }  // malloc's mark where a foreign sbrk cut its space
            if (s < 16 || (s & 7u) || p + s > (char *)top) break;
            Chunk *next = (Chunk *)(p + s);
            w->chunks++;
            if (next->size & 1u) {
                w->usedBytes += s;
            } else {
                w->freeBytes += s;
                w->freePieces++;
                if (s > w->largestFree) { w->largestFree = s; w->largestFreeAt = (unsigned)p; }
                if (s >= request + 8) {
                    w->fitting++;
                    if (nfit < 8) fit[nfit++] = c;
                }
            }
            p += s;
        }
        w->walkOk = p == (char *)top;
        if (!w->walkOk) {
            w->walkStop = (unsigned)p;
            w->walkStopSize = p < brk ? ((Chunk *)p)->size : 0;
        }
    }
    for (unsigned i = 0; i < nfit; i++) w->fittingInBins += inBins(fit[i]);
    __malloc_unlock(_REENT);
}
