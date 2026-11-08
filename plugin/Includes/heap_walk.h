// heap_walk.h - diagnostic (alpha, remove before test): how newlib's malloc (dlmalloc 2.6.4, mallocr.c) sees the
// plugin's heap at a failed request: every chunk from the start of the heap to malloc's top, its bins, and whether the
// heap's end (sbrk) is where malloc thinks it is. Reads malloc's own variables through weak symbols: with another
// malloc they are missing and ok is 0.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ok;                       // newlib's malloc variables found
    unsigned base, brk, heapEnd;  // sbrk's start, sbrk(0), fake_heap_end
    unsigned top, topSize;        // malloc's top chunk
    int walkOk;                   // the walk from base reached top exactly
    int fencepost;                // it stopped at a fencepost: malloc found the heap's end moved by someone else
    unsigned walkStop, walkStopSize;  // where it stopped and the size it read there (when not ok)
    unsigned chunks, usedBytes, freeBytes, freePieces;
    unsigned largestFree, largestFreeAt;
    unsigned fitting;             // free chunks big enough for the request (the walk)
    unsigned fittingInBins;       // of those (up to 8 checked), how many are on a bin list
    unsigned binChunks, binBytes, binLargest, lastRemainder, binBlocks;
    unsigned topPad, trimThreshold, maxSbrked;
} HeapWalk;

// Fills w (under malloc's lock); request: the size that was refused, for `fitting`.
void heap_walk(HeapWalk *w, unsigned request);

#ifdef __cplusplus
}
#endif
