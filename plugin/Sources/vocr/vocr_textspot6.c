/* vocr_textspot6.c - the full-frame text spotter for VTSP version 1, 2, 3 and 4 blobs (vocr_textspot6.h, issue #128,
 * docs/textspot6.md); version 4 is the tagged spotter.
 *
 * synth/textspot6_twin.py is the specification; this file computes exactly what it computes, for every frame and
 * every blob its Model accepts (tests/textspot6_parity.py checks the maps, the boxes with their classes, blocks,
 * order, pick, truncated flag and presence). It is runtime/vocr_textspot3.c (textspot3's runtime, frozen; copied, not
 * linked) with version 4 added: for a version-1, 2 or 3 blob every step below is vocr_textspot3.c's, so the results
 * and the arena sizes are its own, bit for bit (the parity also runs runtime/vocr_textspot3_test on the same frames),
 * with every class 0 (VERSION 4 below: with no tag map, every box is class 0, the class conditions hold for every
 * pair and the keys gain a constant). vocr_textspot3.c is itself vocr_textspot2.c with version 3 added. Versions 1
 * and 2, in short (synth/textspot2_twin.py):
 *
 * Input. grey = (77 R + 150 G + 29 B) >> 8. A frame W x H wider than the blob's half_above (> 0) is halved:
 * f = 2 and the network reads x[i][j] = (g[2i][2j] + g[2i][2j'] + g[2i'][2j] + g[2i'][2j'] + 2) >> 2 with
 * 2i' = min(2i + 1, H - 1), 2j' = min(2j + 1, W - 1) (ceil(H/2) x ceil(W/2)); else f = 1 and it reads g. The
 * halving keeps one row of ceil(W/2) pair sums: an even frame row writes g[y][2j] + g[y][2j'], the odd row
 * after it adds its own and the input row (sum + 2) >> 2 goes into the network; the last row of an odd height
 * adds its pairs twice (the twin repeats it).
 *
 * Network (vocr_textdet.c, textdet_twin.run_net). x0 = x - 128 (int8). Each layer (dense or depthwise, 1x1 or
 * 3x3 with a stride of 1 or 2 and a dilation d, zero padding of d) sums int8 weights times int8 codes into an
 * int32 accumulator, adds the int32 bias and requantises:
 *     out = clamp(floor(((bias + sum) * mult + 2^(shift - 1)) / 2^shift), relu ? 0 : -128, 127).
 * Version 1: the last layer gives four int8 maps at the output stride s: K (kernel), O (vertical), A1, A2 (near
 * and far edge distances across the line, in 1/2 network-input px). Version 2 (d2s): the last layer gives 16
 * channels at stride 8; channel 4 (2 dy + dx) + m of head cell (i, j) is map m of cell (2 i + dy, 2 j + dx) of
 * the maps at stride 4, which the decode reads as version 1's, cropped to ceil(h / 4) x ceil(w / 4) of the
 * network's h x w input (textspot2_twin.head_to_maps): each head row gives the decode two map rows (one at the
 * bottom edge), rearranged into a row buffer of four planes.
 *
 * Streaming. Every 3x3 layer keeps a ring of the (k - 1) d + 1 = 2 d + 1 input rows its kernel spans, each row
 * one plane per channel with d zero columns either side and BLK * stride zero columns of slack; an output row is
 * computed, BLK = 8 pixels at a time, once the last input row it reads has arrived (or lies below the input), and
 * the layers are drained deepest first, so a ring slot is overwritten only when no later output row reads it.
 * 1x1 layers keep no rows: they run on the block of 8 pixels the layer before them has just produced, into the
 * next 3x3 layer's ring or the head's row. The arena holds the rings, three blocks, the head's row, the d2s map
 * row (version 2), the halving row and the decode below, for any height.
 *
 * Decode (textspot_twin.candidates), on the maps at stride s (the network's for version 1, 4 for version 2). In
 * each map row the kernel cells (K > 0) form runs; a run joins the components of the runs of the row above it
 * overlaps (4-connectivity), by union-find over component slots, and adds its cells' sums (n, K, O, A1, A2, rows,
 * columns). After a row, a component no run of the row continues is complete: n >= min cells;
 * conf = floor(sum K / n) >= conf_min; vertical = sum O > 0; across the line
 * near = floor(f (2 s sum i + n (s - 1) - sum A1) / 2n), far = ceil(f (2 s sum i + n (s - 1) + sum A2) / 2n);
 * along it [f s first, f s (last + 1)); dropped when far - near < 4 f; padded by max(2, ((far - near) 18 + 50)
 * / 100), clipped to the frame, dropped when empty. Live components are bounded by the runs of two rows, at most
 * the map's width (a slot per distinct component of the row above plus per run of this row touching none, and
 * their first columns are distinct). The 64 best candidates by the cap key (-conf, -n, vertical, y, x, h, w) are
 * kept in a sorted list (truncated when one more arrives).
 *
 * Blocks (textspot_twin.group_and_order) on the kept boxes sorted by (x, y, w, h, vertical, conf): union-find
 * over every pair that joins (one orientation, overlap along, gap across <= the larger size across, the larger
 * size across <= 3/2 of the smaller), blocks in the order of their first box; each block's boxes in rows (or
 * column groups) by stable sorts with the twin's keys; blocks in rows top to bottom by stable sorts; the pick.
 * Then each box's dark flag, boxes in output order (vocr_line_dark of its box on the frame).
 *
 * Integer ranges (with VOCR_TEXTSPOT_MAX_SIDE = 2048; the network's input side is at most 2048 at f = 1 and
 * 1024 at f = 2; a version-2 decode map is a stride-4 map, so every bound below holds for it as for s = 4):
 *  - an accumulator sums at most 9 * 64 = 576 products of two int8 codes, |w x| <= 2^14, so |sum| <= 576 * 2^14
 *    < 2^24 fits int32. The bias enters through bq = bias * mult + 2^(shift - 1) (int64, per channel):
 *    |bias * mult| <= 2^31 * (2^31 - 1) < 2^62, 2^(shift - 1) <= 2^61 and |sum * mult| < 2^24 * 2^31 = 2^55, so
 *    bq + sum * mult is the exact value and below 2^63. The NEON code, for a channel whose |bias| <= 2^31 - 1 -
 *    2^24 (so bias + sum fits int32), forms (bias + sum) * mult < 2^62 instead and lets VRSHL add 2^(shift - 1).
 *  - a halving pair sum is at most 2 * 255 and the row sum 4 * 255 + 2, in uint16.
 *  - the map has at most ceil(2048 / 4)^2 = 2^18 cells at f = 1 (2^16 at f = 2), so a component has n <= 2^18
 *    cells, sum K <= 127 n < 2^25, and |sum O|, |sum A1|, |sum A2| <= 128 n. Across the line a cell's
 *    c2 = 2 s i + s - 1 <= 4091 at f = 1 (map side <= 512 at s = 4, 256 at s = 8) and <= 2043 at f = 2, so
 *    |2 s sum i + n (s - 1) -+ sum A| <= n (4091 + 128) <= 2^18 * 4219 < 2^31 at f = 1 and f n (2043 + 128)
 *    <= 2 * 2^16 * 2171 < 2^29 at f = 2; 2 n <= 2^19. Then near >= -124 and far <= 2170, so across <= 2294, pad
 *    <= 413 and an edge lies in [-537, 2583] before the clip; after it every coordinate fits uint16.
 *  - a block's score, the sum of its boxes' w h doubled, is at most 2 * 64 * 2048^2 = 2^29 (uint32).
 *
 * Memory: no allocation; one arena (vocr_textspot6_frame_arena_bytes for the frame, vocr_textspot6_arena_bytes
 * for every frame up to a size) holds the per-layer state, the requantisation constants, the rings, the head's
 * row, the d2s map row, the halving row, the decode's component slots and run lists, the candidates and (over the
 * network's part, dead by then) the blocks' scratch. libc: memset and memcpy only (and libgcc's division helpers
 * on 32-bit ARM and MIPS, a few per row and per component).
 *
 * VERSION 3 (synth/textspot3_twin.py's docstring: a stride-8 stream, then ops on whole maps, then version 2's head).
 *
 * Input. As above, but f = 2 when the frame is wider than half_above (> 0) or taller than half_above_h (> 0); with
 * the exporter's 480 and 272 every frame up to 960 x 544 reaches the network at most 480 x 272.
 *
 * Stream. The stream layers are a version-1 network without its head, run row by row exactly as above (rings, the
 * 1x1 chains, BLK pixels at a time, the same kernels); its last layer's rows, which version 1 decodes, go into the
 * whole stride-8 map F8 (map 0), cell by cell (to_f8).
 *
 * Maps. Every map is stored cell-major (cells row by row, a cell's channels contiguous) in a buffer ("slot") of its
 * stride: rows x cols cells of cap bytes, cap the most channels of the maps the slot holds in turn (a multiple of
 * 8). The plan is the graph's alone (ms_graph, at load): an op writes its output over a source it reads for the
 * last time when it can do so in place (a 1x1 op over a source it reads at u = 0, a stride-1 depthwise op over its
 * source), else into a new slot. Slots live on a stack in the arena: a new slot goes on top; a slot whose map has
 * been read for the last time is marked dead, and dead slots are popped from the top (an FPN's top-down neck frees
 * its stages last in, first out). An op's scratch (its requantisation constants, a block's sums, its cells' source
 * offsets and gathered codes or its in-place block, the depthwise weights by tap, a row's sums and the lag ring) lies
 * on top of the stack while it runs, over the stream's dead rings. Every term of the plan is a sum or a maximum of
 * buffer sizes that grow with the network input's width and height, so every arena size is non-decreasing in both
 * (vocr_textspot6_arena_bytes relies on it).
 *  - A 1x1 op (out[c] = requant(bias[c] + sum_q w[c][q] x[q]), x the concatenation of its sources' channels, source
 *    cell (i >> u, j >> u) for output cell (i, j)) runs on blocks of cells in raster order (pw_block: a block's
 *    inputs about 12 KB, so they stay in the L1 cache while the weights of each 4 output channels pass once). An op
 *    of one source read at u = 0 reads a block's codes where they lie (its cells are contiguous in the source's
 *    slot); any other op first gathers the block's concatenated codes (pw_gather: cin bytes per cell, at most about
 *    8 KB). In place, a gathered block's outputs go straight over their own source cells (the block's codes are all
 *    gathered by then, and no other block reads them); an op that reads its codes in place writes a block's outputs
 *    to a scratch block first and over the block's cells once every channel is done (an output cell reads its own
 *    source cell only).
 *  - A 3x3 depthwise op runs row by row; in place, output row r waits in a ring of d + 1 rows and is written back
 *    once row r + d is done (no later output row reads source row r).
 *  - The head (the last op) runs row by row, each row of 16 channels going straight to the decode, two stride-4
 *    map rows per head row (textspot2_twin.head_to_maps), as version 2's head rows do; then the decode, blocks,
 *    order, pick and dark flags are version 2's.
 *
 * Kernels (NEON on the Vita; the scalar code gives the same bits). A 1x1 op's microkernel (pw_sums) computes 4
 * output channels for 2 cells: per 16 input channels 6 loads (each weight row and each cell once, every pointer
 * post-incremented), VMULL.S8 + VMLAL.S8 put two products of int8 codes in each int16 lane and VPADAL.S16 folds
 * them into int32 lanes (the first 16 channels by VPADDL, so the sums need no clearing): 32 instructions per 128
 * multiply-adds; each weight row is read once per block of cells; the sums of a block's pairs of cells go to a
 * scratch row and are requantised in a pass of their own (pw_store), so no NEON register spills to the stack (an
 * extension-register load or store between NEON instructions costs the A9 a pipeline switch). A depthwise op
 * (dw_row): 8 channels per pass, two passes per row: the sums of the cells whose taps lie inside the map with the 9
 * taps' weights in registers (two taps' products per int16 lane, VADDW.S16 into int32; a source row outside the map
 * read as the centre row with its weights 0), the border cells tap by tap (dw_border), then the requantisation of
 * the row's sums. The requantisation (rq4v), per 4 channels: when every channel's bias fits the int32 form and its
 * shift is >= 32, VQDMULH.S32 then VRSHL.S32 by -(shift - 31); when every bias fits, VMULL.S32 and VRSHL.S64 by
 * -shift; else the int64 constant bias * mult + 2^(shift - 1), VMLAL.S32 and VSHL.S64 (a floor).
 *
 * Integer ranges, version 3 (every weight is in -127..127: the loader refuses -128; every code is in -128..127):
 *  - one product |w x| <= 127 * 128 = 16,256, two of them <= 32,512 < 2^15: the kernels' int16 lanes hold them
 *    exactly. A 1x1 op sums cin <= 1,024 products: |sum| <= 1,024 * 16,256 = 16,646,144 < 2^24; a depthwise op 9:
 *    <= 146,304. A stream layer: version 1's bound above.
 *  - the requantisation: when |bias| <= 2^31 - 1 - 2^24 (Chan.fast), v = bias + sum fits int32. VQDMULH.S32(v, m)
 *    = floor(v m / 2^31) (|2 v m| < 2^63, m > 0: it never saturates), and with k = shift - 31 >= 1 VRSHL by -k
 *    gives floor((floor(v m / 2^31) + 2^(k - 1)) / 2^k) = floor((v m + 2^(shift - 1)) / 2^shift), the twin's
 *    value (floor((N + e) / 2^k) = floor(N / 2^k) for an integer N and 0 <= e < 1). The int64 forms stay below
 *    2^63: |v m| < 2^62, |bias m| < 2^62, |sum m| < 2^55, 2^(shift - 1) <= 2^61. The value is then saturated to
 *    int32, int16 and int8 (one clamp to [-128, 127]) and, with ReLU, raised to 0, as requant() does.
 *  - the plan is computed in 64 bits; a frame whose version-3 arena would pass MS_LIMIT (2^31 - 256 bytes) gets 0
 *    from the arena functions and VOCR_LINES_E_ARENA from the calls, so every offset into the arena fits uint32.
 *  - the decode reads stride-4 maps of a network input of at most 2048 px per side: version 2's ranges hold.
 *
 * Memory, version 3: one arena (vocr_textspot6_frame_arena_bytes for the frame, vocr_textspot6_arena_bytes for
 * every frame up to a size): the 64 candidates, the slots' offsets, then the stack (F8's slot, the stream's state
 * over it while the frame is read, the other slots and each op's scratch, the head's decode state; the blocks'
 * scratch over its base at the end).
 *
 * VERSION 4 (synth/textspot6_twin.py's docstring: the tagged spotter). The VTSP header is version 3's with the
 * reserved byte 19 the tag count T (1..4); the network is a VTDT trunk (version 2's checks, outputs 16 + 4 T) or a
 * VTMS section (version 3's checks, outputs 16 + 4 T, a multiple of 8, in its header and its head op). Every step
 * is the same network's as above, with these changes:
 *
 * Input. Both forms halve a frame wider than half_above (> 0) or taller than half_above_h (> 0), version 3's rule.
 * A trunk's arena (plan) therefore depends on the height through f, and vocr_textspot6_arena_bytes takes version
 * 3's form for both (the largest frame read at full size, and the frame itself when it is halved).
 *
 * Head. 16 + 4 T channels: channel 4 (2 dy + dx) + m is map m of the sub-cell (dy, dx) as in version 2, channel
 * 16 + T (2 dy + dx) + t tag map t of it. A head row gives the decode two rows of 4 + T planes (sub_row from a
 * trunk's planar head row, sub_row3 from a multi-scale head's cell-major row of 16 + 4 T codes per cell).
 *
 * Decode. Per component the sums of the T tag maps over its cells (tsum: T int32 per component slot, beside the
 * slots; a run adds its cells' codes once its root is known, unite adds a joined slot's sums). For a kept candidate
 * of n cells (finish): class 0's score 0, class t + 1's the sum of tag map t; the class (tag) of the largest score,
 * the lowest on a tie; tconf = min(127, floor((best - second) / n)), best and second the two largest scores. The cap
 * keeps the 64 smallest by (-conf, -n, vertical, y, x, h, w, tag, tconf): the twin sorts (key, box) pairs, so after
 * its key (which ends in the class) the box's tconf breaks a tie; two candidates equal in all of it are the same
 * box in every output field.
 *
 * Blocks. Sorted by (x, y, w, h, vertical, conf, tag, tconf) (the twin's (x, y, w, h, vertical, conf, dark, tag,
 * tconf): dark follows from the box); two boxes join only when they also have one class; a block's class is its
 * boxes'; blocks sorted by (y, x, h, w, tag), rows of blocks by (x, y, h, w, tag), or (-(x + w), y, h, w, tag) when
 * all vertical; the boxes inside a block keep version 1's keys. The pick is among the class-0 blocks only (-1 when
 * there is none), with version 1's score and ties; presence is the largest confidence of a class-0 box (0 without
 * one). Versions 1-3 have T = 0: every class is 0, so every key gains a constant, every pair the condition, every
 * block the pick's filter, and presence is the largest confidence; and the tag sums take no arena (0 bytes).
 *
 * Integer ranges, version 4: a tag sum is at most 128 n <= 2^25 in magnitude (n <= 2^18 cells), so a score
 * difference is below 2^26 (int32); every other range is version 2's (a trunk; its head's 1x1 layer has cin <= 64)
 * or version 3's (a multi-scale network; its head op cin <= 1,024).
 *
 * The arena: version 4's decode adds the T tag planes to the decode's map row ((4 + T) wm bytes) and T int32 per
 * component slot; a multi-scale head's cell-major row holds 16 + 4 T codes per cell. With T = 0 (versions 1-3) both
 * additions are 0 bytes and the Cand and Comp records keep their sizes, so every arena is vocr_textspot3.c's.
 *
 * VOCR_TEXTSPOT6_NO_MS leaves out every VTMS path (parse_ms, ms_graph, the version-3 arena plan, the map-op kernels,
 * the head op and to_f8): the loader refuses version 3 and a version-4 VTMS network; nothing else changes.
 *
 * VOCR_TEXTSPOT6_SCALE1 / _SCALE2 (any version; textspot6_twin.spot(model, img, scale=f)): f forced to 1 or 2 instead
 * of the blob's rule, for a region read at the scale of the frame it was cut from. Only f changes: the input (the
 * 2x2 mean or the grey itself), the network, the decode with that f, the arena plan of that f
 * (vocr_textspot6_frame_arena_bytes_flags); everything else is the same call's.
 *
 * Changes from vocr_textspot3.c beyond version 4 (the same bits): pw_sums and pw_store index their cells instead of
 * stepping a pointer past the end of a block after an odd last cell (textspot3's audit, N4); CELL8 asks for the
 * alignment only under __GNUC__ (N5).
 */
#include "vocr_textspot6.h"

#include <stdint.h>
#include <string.h>

/* NEON kernels on the Vita (-mfpu=neon), the same arithmetic as the scalar ones; -DVOCR_NO_NEON forces the
 * scalar code (as for vocr.c and vocr_textdet.c). */
#if defined(__ARM_NEON) && !defined(VOCR_NO_NEON)
#include <arm_neon.h>
#define TS_NEON 1
#else
#define TS_NEON 0
#endif

/* Kotoba3DS addition: ARMv6 SIMD on the 3DS's ARM11 (no NEON): SXTB16 and SMLAD for the 1x1 layers. With
 * VOCR_SIMD32_EMULATE the same path is built with C stand-ins for the instructions, to test it on a PC. */
#if !TS_NEON && !defined(VOCR_NO_SIMD32) && (defined(__ARM_FEATURE_SIMD32) || defined(VOCR_SIMD32_EMULATE))
#define TS_SIMD32 1
#if defined(__ARM_FEATURE_SIMD32) && !defined(VOCR_SIMD32_EMULATE)
#include <arm_acle.h>
#define ts_sxtb16(x) ((uint32_t)__sxtb16(x))
#define ts_smlad(a, b, acc) __smlad((a), (b), (acc))
#else
static uint32_t ts_sxtb16(uint32_t x)
{
    return (uint32_t)(uint16_t)(int16_t)(int8_t)(x & 0xFFu) |
           ((uint32_t)(uint16_t)(int16_t)(int8_t)((x >> 16) & 0xFFu) << 16);
}
static int32_t ts_smlad(uint32_t a, uint32_t b, int32_t acc)
{
    const int32_t p0 = (int32_t)(int16_t)(a & 0xFFFFu) * (int32_t)(int16_t)(b & 0xFFFFu);
    const int32_t p1 = (int32_t)(int16_t)(a >> 16) * (int32_t)(int16_t)(b >> 16);
    return (int32_t)((uint32_t)acc + (uint32_t)p0 + (uint32_t)p1);
}
#endif
#define ts_ror8(x) (((uint32_t)(x) >> 8) | ((uint32_t)(x) << 24))
#if defined(__ARM_FEATURE_SAT) && !defined(VOCR_SIMD32_EMULATE)
#define ts_ssat8(x) __ssat((x), 8)
#else
static int32_t ts_ssat8(int32_t x) { return x < -128 ? -128 : x > 127 ? 127 : x; }
#endif
#else
#define TS_SIMD32 0
#endif

/* The version-3 and version-4 stages stay functions of their own (the A9 table's cost split reads them by name). */
#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

#define VTSP_HEADER 32u
#define HEADER 64u          /* the VTDT header */
#define RECORD 32u
#define LOADED 0x36505354u  /* "TSP6" */
#define CANDS VOCR_TEXTSPOT_MAX_BOXES
#define PAD_PCT 18
#define PAD_MIN 2
#define MIN_ACROSS 4
#define MAXCH 64u           /* channels of a layer */
#define NONE 0xFFFFu        /* no component slot / a free slot */
#define BLK 8u              /* output pixels computed together (a block) */
#define MAPS 4u             /* K, O, A1, A2 */
#define D2S_OUT 16u         /* a version-2 head: 4 sub-cells x (K, O, A1, A2) */
#define D2S_STRIDE 8u       /* a version-2 trunk's output stride */
#define TCONF_MAX 127       /* a version-4 box's tconf */

/* ---------------------------------------------------------------- the blob */

static uint32_t rd16(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t rds32(const unsigned char *p)
{
    uint32_t v = rd32(p);
    return v < 0x80000000u ? (int32_t)v : -(int32_t)(0xFFFFFFFFu - v) - 1;
}

static uint32_t crc32_ieee(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        int b;
        c ^= *p++;
        for (b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* [off, off + size) inside the data part of the blob, off 4-byte aligned (twin: textdet_twin.Model). */
static int section_ok(uint32_t off, uint32_t size, uint32_t data, uint32_t total)
{
    return !(off % 4u) && off >= data && off <= total && size <= total - off;
}

/* Every check of textdet_twin.Model on the VTDT blob b (bytes long), for a head of `outputs` channels: 4
 * (version 1: textdet_twin.Model exactly), 16 at stride 8 (version 2: textspot2_twin.Net) or 16 + 4 T at stride 8
 * (version 4: textspot6_twin.TrunkNet); fills m's network fields. */
static int parse_net(vocr_textspot6 *m, const unsigned char *b, size_t bytes, uint32_t outputs)
{
    uint32_t total, n, data, cin = 1, stride = 1, len, i;
    if (bytes < HEADER || b[0] != 'V' || b[1] != 'T' || b[2] != 'D' || b[3] != 'T') return VOCR_TEXTSPOT_E_BLOB;
    total = rd32(b + 8);
    if (rd32(b + 4) != 1u || (size_t)total != bytes) return VOCR_TEXTSPOT_E_BLOB;
    if (crc32_ieee(b + 16, bytes - 16u) != rd32(b + 12)) return VOCR_TEXTSPOT_E_BLOB;
    n = rd16(b + 16);
    if ((b[18] != 4u && b[18] != 8u) || b[19] != outputs || b[20] != 2u || b[21] < 1u || b[21] > 4u)
        return VOCR_TEXTSPOT_E_BLOB;
    if (outputs != MAPS && b[18] != D2S_STRIDE) return VOCR_TEXTSPOT_E_BLOB;   /* a d2s head: a stride-8 trunk */
    if (n < 2u || n > VOCR_TEXTSPOT_MAX_LAYERS || HEADER + n * RECORD > total) return VOCR_TEXTSPOT_E_BLOB;
    for (len = 32; len && !b[32 + len - 1u]; len--) {}   /* the name: ASCII once trailing NULs go */
    for (i = 0; i < len; i++)
        if (b[32 + i] >= 0x80u) return VOCR_TEXTSPOT_E_BLOB;
    data = HEADER + n * RECORD;
    for (i = 0; i < n; i++) {
        const unsigned char *r = b + HEADER + i * RECORD;
        vocr_textspot_layer *L = &m->layers[i];
        uint32_t kind = r[0], k = r[1], st = r[2], dil = r[3], relu = r[4];
        uint32_t lc = rd16(r + 8), cout = rd16(r + 10), nw, c;
        uint32_t wo = rd32(r + 12), bo = rd32(r + 16), mo = rd32(r + 20), so = rd32(r + 24);
        if (kind > 1u || (k != 1u && k != 3u) || (st != 1u && st != 2u) || dil < 1u || dil > 8u || relu > 1u ||
            lc != cin || cout < 1u || cout > MAXCH)
            return VOCR_TEXTSPOT_E_BLOB;
        if (kind == 1u && (cout != lc || k != 3u)) return VOCR_TEXTSPOT_E_BLOB;
        if (st == 2u && (k != 3u || dil != 1u)) return VOCR_TEXTSPOT_E_BLOB;
        nw = cout * (kind == 1u ? 1u : lc) * k * k;
        if (!section_ok(wo, nw, data, total) || !section_ok(bo, 4u * cout, data, total) ||
            !section_ok(mo, 4u * cout, data, total) || !section_ok(so, cout, data, total))
            return VOCR_TEXTSPOT_E_BLOB;
        for (c = 0; c < cout; c++)
            if (rds32(b + mo + 4u * c) <= 0 || b[so + c] < 1u || b[so + c] > 62u) return VOCR_TEXTSPOT_E_BLOB;
        L->w = b + wo;
        L->b = b + bo;
        L->m = b + mo;
        L->sh = b + so;
        L->cin = (unsigned short)lc;
        L->cout = (unsigned short)cout;
        L->kind = (unsigned char)kind;
        L->k = (unsigned char)k;
        L->stride = (unsigned char)st;
        L->dil = (unsigned char)dil;
        L->relu = (unsigned char)relu;
        m->channels += cout;
        stride *= st;
        cin = cout;
    }
    if (stride != b[18] || cin != outputs || m->layers[n - 1u].relu) return VOCR_TEXTSPOT_E_BLOB;
    m->n_layers = n;
    m->outputs = outputs;
    m->net_stride = b[18];
    m->stride = outputs != MAPS ? D2S_STRIDE / 2u : b[18];   /* the stride of the maps the decode reads */
    m->min_cells = b[21];
    m->net_bytes = (unsigned long)total;
    memcpy(m->ckpt, b + 24, 8);
    memcpy(m->name, b + 32, 32);
    m->name[32] = '\0';
    return VOCR_LINES_OK;
}

/* ---------------------------------------------------------------- versions 3 and 4: the VTMS network */

#define VTMS_HEADER 64u      /* the VTMS section's header */
#define NONE8 0xFFu          /* no slot: the head's output */
#define MS_MAXC 1024u        /* a VTMS op's channels, a 1x1 op's cin */
#define MS_MAX_STRIDE 256u   /* the deepest map's stride */
#define OP_PW 0u
#define OP_DW 1u
#define MS_LIMIT 0x7FFFFF00u /* the largest VTMS arena (bytes before the 7 alignment bytes) */

#ifndef VOCR_TEXTSPOT6_NO_MS
/* A VTMS network: version 3, or version 4's multi-scale form (a VOCR_TEXTSPOT6_NO_MS build refuses both at load). */
static int is_ms(const vocr_textspot6 *m)
{
    return m->version == VOCR_TEXTSPOT6_VERSION_MS || m->form == VOCR_TEXTSPOT6_FORM_MS;
}

/* No weight is -128 (the VTMS kernels add two products of int8 codes in an int16 lane). */
static int no_m128(const unsigned char *w, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (w[i] == 0x80u) return 0;
    return 1;
}

/* Multipliers > 0 and shifts 1..62. */
static int requant_ok(const unsigned char *b, uint32_t mo, uint32_t so, uint32_t cout)
{
    uint32_t c;
    for (c = 0; c < cout; c++)
        if (rds32(b + mo + 4u * c) <= 0 || b[so + c] < 1u || b[so + c] > 62u) return 0;
    return 1;
}

/* Every check of textspot3_twin.Net on the VTMS section b (bytes long), for a head of `outputs` channels: 16
 * (version 3: textspot3_twin.Net exactly) or 16 + 4 T, a multiple of 8 (version 4: textspot6_twin._ms_net, which is
 * textspot3_twin.Net's checks with the header's outputs and the head op's cout both 16 + 4 T, the head's sections and
 * requantisation over all its channels); fills m's network fields. */
static int parse_ms(vocr_textspot6 *m, const unsigned char *b, size_t bytes, uint32_t outputs)
{
    uint32_t total, ns, no, data, cin = 1, stride = 1, len, i, k, q;
    if (outputs % 8u) return VOCR_TEXTSPOT_E_BLOB;
    if (bytes < VTMS_HEADER || b[0] != 'V' || b[1] != 'T' || b[2] != 'M' || b[3] != 'S') return VOCR_TEXTSPOT_E_BLOB;
    total = rd32(b + 8);
    if (rd32(b + 4) != 1u || (size_t)total != bytes) return VOCR_TEXTSPOT_E_BLOB;
    if (crc32_ieee(b + 16, bytes - 16u) != rd32(b + 12)) return VOCR_TEXTSPOT_E_BLOB;
    ns = rd16(b + 16);
    no = rd16(b + 22);
    if (b[18] != D2S_STRIDE || b[19] != outputs || b[20] != 2u || b[21] < 1u || b[21] > 4u)
        return VOCR_TEXTSPOT_E_BLOB;
    if (ns < 1u || ns > VOCR_TEXTSPOT_MAX_LAYERS || no < 1u || no > VOCR_TEXTSPOT6_MAX_OPS ||
        VTMS_HEADER + (ns + no) * RECORD > total)
        return VOCR_TEXTSPOT_E_BLOB;
    for (len = 32; len && !b[32 + len - 1u]; len--) {}   /* the name: ASCII once trailing NULs go */
    for (i = 0; i < len; i++)
        if (b[32 + i] >= 0x80u) return VOCR_TEXTSPOT_E_BLOB;
    data = VTMS_HEADER + (ns + no) * RECORD;
    for (i = 0; i < ns; i++) {                            /* the stream: textdet_twin's layer records */
        const unsigned char *r = b + VTMS_HEADER + i * RECORD;
        vocr_textspot_layer *L = &m->layers[i];
        uint32_t kind = r[0], kk = r[1], st = r[2], dil = r[3], relu = r[4];
        uint32_t lc = rd16(r + 8), cout = rd16(r + 10), nw;
        uint32_t wo = rd32(r + 12), bo = rd32(r + 16), mo = rd32(r + 20), so = rd32(r + 24);
        if (kind > 1u || (kk != 1u && kk != 3u) || (st != 1u && st != 2u) || dil < 1u || dil > 8u || relu > 1u ||
            lc != cin || cout < 1u || cout > MAXCH)
            return VOCR_TEXTSPOT_E_BLOB;
        if (kind == 1u && (cout != lc || kk != 3u)) return VOCR_TEXTSPOT_E_BLOB;
        if (st == 2u && (kk != 3u || dil != 1u)) return VOCR_TEXTSPOT_E_BLOB;
        nw = cout * (kind == 1u ? 1u : lc) * kk * kk;
        if (!section_ok(wo, nw, data, total) || !section_ok(bo, 4u * cout, data, total) ||
            !section_ok(mo, 4u * cout, data, total) || !section_ok(so, cout, data, total))
            return VOCR_TEXTSPOT_E_BLOB;
        if (!no_m128(b + wo, nw) || !requant_ok(b, mo, so, cout)) return VOCR_TEXTSPOT_E_BLOB;
        L->w = b + wo;
        L->b = b + bo;
        L->m = b + mo;
        L->sh = b + so;
        L->cin = (unsigned short)lc;
        L->cout = (unsigned short)cout;
        L->kind = (unsigned char)kind;
        L->k = (unsigned char)kk;
        L->stride = (unsigned char)st;
        L->dil = (unsigned char)dil;
        L->relu = (unsigned char)relu;
        m->channels += cout;
        stride *= st;
        cin = cout;
    }
    if (stride != D2S_STRIDE || cin % 8u) return VOCR_TEXTSPOT_E_BLOB;
    m->map_stride[0] = (unsigned short)D2S_STRIDE;
    m->map_chans[0] = (unsigned short)cin;
    for (k = 0; k < no; k++) {                            /* the map ops */
        const unsigned char *r = b + VTMS_HEADER + (ns + k) * RECORD;
        vocr_textspot6_op *op = &m->ops[k];
        uint32_t kind = r[0], st = r[1], dil = r[2], relu = r[3], nsrc = r[4], cout = rd16(r + 6);
        uint32_t wo = rd32(r + 16), bo = rd32(r + 20), mo = rd32(r + 24), so = rd32(r + 28);
        uint32_t per, kk, s_out = 0, ci = 0, nw;
        if (kind > 1u || (st != 1u && st != 2u) || dil < 1u || dil > 8u || relu > 1u || r[5] || rd16(r + 14))
            return VOCR_TEXTSPOT_E_BLOB;
        if (cout < 8u || cout > MS_MAXC || cout % 8u) return VOCR_TEXTSPOT_E_BLOB;
        if (nsrc < 1u || nsrc > (kind == OP_DW ? 1u : 3u)) return VOCR_TEXTSPOT_E_BLOB;
        for (q = nsrc; q < 3u; q++)
            if (r[8 + q] || r[11 + q]) return VOCR_TEXTSPOT_E_BLOB;   /* an unused entry is 0 */
        for (q = 0; q < nsrc; q++)
            if (r[8 + q] > k || r[11 + q] > 3u) return VOCR_TEXTSPOT_E_BLOB;   /* an earlier map; u <= 3 */
        if (kind == OP_DW) {
            uint32_t s0 = r[8];
            if (r[11] || cout != m->map_chans[s0] || (st == 2u && dil != 1u)) return VOCR_TEXTSPOT_E_BLOB;
            kk = 3u;
            per = 1u;
            ci = cout;
            s_out = (uint32_t)m->map_stride[s0] * st;
        } else {
            if (st != 1u || dil != 1u) return VOCR_TEXTSPOT_E_BLOB;
            for (q = 0; q < nsrc; q++) {                  /* the sources meet at one stride >= 8 */
                uint32_t sm = m->map_stride[r[8 + q]], u = r[11 + q];
                uint32_t at = (sm >> u) << u == sm ? sm >> u : 0u;
                if (!at || (q && at != s_out)) return VOCR_TEXTSPOT_E_BLOB;
                s_out = at;
                ci += m->map_chans[r[8 + q]];
            }
            if (s_out < D2S_STRIDE || ci > MS_MAXC) return VOCR_TEXTSPOT_E_BLOB;
            kk = 1u;
            per = ci;
        }
        if (s_out > MS_MAX_STRIDE) return VOCR_TEXTSPOT_E_BLOB;
        nw = cout * per * kk * kk;
        if (!section_ok(wo, nw, data, total) || !section_ok(bo, 4u * cout, data, total) ||
            !section_ok(mo, 4u * cout, data, total) || !section_ok(so, cout, data, total))
            return VOCR_TEXTSPOT_E_BLOB;
        if (!no_m128(b + wo, nw) || !requant_ok(b, mo, so, cout)) return VOCR_TEXTSPOT_E_BLOB;
        op->w = b + wo;
        op->b = b + bo;
        op->m = b + mo;
        op->sh = b + so;
        op->cin = (unsigned short)ci;
        op->cout = (unsigned short)cout;
        op->out_stride = (unsigned short)s_out;
        op->kind = (unsigned char)kind;
        op->stride = (unsigned char)st;
        op->dil = (unsigned char)dil;
        op->relu = (unsigned char)relu;
        op->nsrc = (unsigned char)nsrc;
        for (q = 0; q < 3u; q++) {
            op->src[q] = r[8 + q];
            op->up[q] = r[11 + q];
        }
        m->map_stride[k + 1u] = (unsigned short)s_out;
        m->map_chans[k + 1u] = (unsigned short)cout;
    }
    {
        const vocr_textspot6_op *h = &m->ops[no - 1u];
        if (h->kind != OP_PW || h->cout != outputs || h->relu || h->out_stride != D2S_STRIDE)
            return VOCR_TEXTSPOT_E_BLOB;
    }
    m->n_layers = ns;
    m->n_ops = no;
    m->outputs = outputs;
    m->net_stride = D2S_STRIDE;
    m->stride = D2S_STRIDE / 2u;
    m->min_cells = b[21];
    m->net_bytes = (unsigned long)total;
    memcpy(m->ckpt, b + 24, 8);
    memcpy(m->name, b + 32, 32);
    m->name[32] = '\0';
    return VOCR_LINES_OK;
}

/* The buffer plan of a VTMS network (the module comment's "Maps"): each op's output slot, in place or new,
 * and each slot's channels per cell and stride; the head's output has none. */
static void ms_graph(vocr_textspot6 *m)
{
    unsigned k, q, n = m->n_ops;
    for (k = 0; k <= n; k++) m->last_use[k] = -1;
    for (k = 0; k < n; k++)
        for (q = 0; q < m->ops[k].nsrc; q++) m->last_use[m->ops[k].src[q]] = (signed char)k;
    m->n_slots = 1;
    m->map_slot[0] = 0;
    m->slot_cap[0] = m->map_chans[0];
    m->slot_stride[0] = (unsigned short)D2S_STRIDE;
    for (k = 0; k < n; k++) {
        vocr_textspot6_op *op = &m->ops[k];
        unsigned target = NONE8, slot;
        op->inplace = 0;
        if (k + 1u == n) {
            op->slot = (unsigned char)NONE8;
            m->map_slot[k + 1u] = (unsigned char)NONE8;
            continue;
        }
        if (op->kind == OP_DW) {
            if (op->stride == 1u && m->last_use[op->src[0]] == (signed char)k) target = op->src[0];
        } else {
            for (q = 0; q < op->nsrc && target == NONE8; q++)
                if (!op->up[q] && m->last_use[op->src[q]] == (signed char)k) target = op->src[q];
        }
        if (target != NONE8) {                            /* over its dying source */
            slot = m->map_slot[target];
            op->inplace = 1;
            if (op->cout > m->slot_cap[slot]) m->slot_cap[slot] = op->cout;
        } else {
            slot = m->n_slots++;
            m->slot_cap[slot] = op->cout;
            m->slot_stride[slot] = op->out_stride;
        }
        op->slot = (unsigned char)slot;
        m->map_slot[k + 1u] = (unsigned char)slot;
    }
}
#endif /* !VOCR_TEXTSPOT6_NO_MS */

/* Every check of textspot6_twin.Model: for version 1 or 2 textspot2_twin.Model's (the VTSP header, then the inner
 * VTDT blob's: outputs 4 for version 1, 16 for version 2); for version 3 textspot3_twin.Model's (its header, then the
 * VTMS section's); for version 4 its header (tags 1..4 in byte 19), then by the network's magic a VTDT trunk or a
 * VTMS section of 16 + 4 tags outputs. With VOCR_TEXTSPOT6_NO_MS every VTMS network is refused. */
static int parse(vocr_textspot6 *m, const unsigned char *b, size_t bytes)
{
    uint32_t total, version;
    if (bytes < VTSP_HEADER || b[0] != 'V' || b[1] != 'T' || b[2] != 'S' || b[3] != 'P') return VOCR_TEXTSPOT_E_BLOB;
    total = rd32(b + 8);
    version = rd32(b + 4);
    if ((version != 1u && version != VOCR_TEXTSPOT6_VERSION_D2S && version != VOCR_TEXTSPOT6_VERSION_MS &&
         version != VOCR_TEXTSPOT6_VERSION_TAG) || (size_t)total != bytes)
        return VOCR_TEXTSPOT_E_BLOB;
    if (crc32_ieee(b + 16, bytes - 16u) != rd32(b + 12)) return VOCR_TEXTSPOT_E_BLOB;
    if (version == VOCR_TEXTSPOT6_VERSION_MS || version == VOCR_TEXTSPOT6_VERSION_TAG) {
        /* u16 half_above, u8 conf_min, u8 0 (version 3) or the tags 1..4 (version 4), u32 offset 32, u32 size
         * total - 32, u16 half_above_h, u16 0 */
        const unsigned char *net = b + VTSP_HEADER;
        uint32_t tags = version == VOCR_TEXTSPOT6_VERSION_TAG ? b[19] : 0u;
        if (b[18] < 1u || b[18] > 127u || rd32(b + 20) != VTSP_HEADER || rd32(b + 24) != total - VTSP_HEADER ||
            rd16(b + 30))
            return VOCR_TEXTSPOT_E_BLOB;
        if (version == VOCR_TEXTSPOT6_VERSION_MS ? b[19] != 0u : (tags < 1u || tags > VOCR_TEXTSPOT6_MAX_TAGS))
            return VOCR_TEXTSPOT_E_BLOB;
        m->half_above = rd16(b + 16);
        m->half_above_h = rd16(b + 28);
        m->conf_min = b[18];
        m->blob_bytes = (unsigned long)total;
        m->version = version;
        m->tags = tags;
        if (version == VOCR_TEXTSPOT6_VERSION_TAG && total - VTSP_HEADER >= 4u && net[0] == 'V' && net[1] == 'T' &&
            net[2] == 'D' && net[3] == 'T') {
            m->form = VOCR_TEXTSPOT6_FORM_TRUNK;
            return parse_net(m, net, (size_t)(total - VTSP_HEADER), D2S_OUT + 4u * tags);
        }
#ifdef VOCR_TEXTSPOT6_NO_MS
        return VOCR_TEXTSPOT_E_BLOB;      /* a VTMS network (or neither magic): not in this build */
#else
        if (version == VOCR_TEXTSPOT6_VERSION_TAG) m->form = VOCR_TEXTSPOT6_FORM_MS;
        return parse_ms(m, net, (size_t)(total - VTSP_HEADER), D2S_OUT + 4u * tags);
#endif
    }
    /* u16 half_above, u8 conf_min, u8 0, u32 offset 32, u32 size total - 32, u32 0 */
    if (b[18] < 1u || b[18] > 127u || b[19] || rd32(b + 20) != VTSP_HEADER || rd32(b + 24) != total - VTSP_HEADER ||
        rd32(b + 28))
        return VOCR_TEXTSPOT_E_BLOB;
    m->half_above = rd16(b + 16);
    m->conf_min = b[18];
    m->blob_bytes = (unsigned long)total;
    m->version = version;
    return parse_net(m, b + VTSP_HEADER, (size_t)(total - VTSP_HEADER), version == 1u ? MAPS : D2S_OUT);
}

int vocr_textspot6_load(vocr_textspot6 *m, const void *blob, size_t bytes)
{
    int rc;
    if (!m) return VOCR_LINES_E_ARG;
    memset(m, 0, sizeof(*m));
    if (!blob) return VOCR_LINES_E_ARG;
    rc = parse(m, (const unsigned char *)blob, bytes);
#ifndef VOCR_TEXTSPOT6_NO_MS
    if (!rc && is_ms(m)) ms_graph(m);
#endif
    if (rc) {
        memset(m, 0, sizeof(*m));
        return rc;
    }
    m->loaded = LOADED;
    return VOCR_LINES_OK;
}

#if TS_SIMD32
static int k1_packable(const vocr_textspot_layer *L);
#endif

int vocr_textspot6_pack_simd32(vocr_textspot6 *m)
{
#if TS_SIMD32
    unsigned l, nb, q, j, c;
    int8_t t[8u * MAXCH];
    if (!m || m->loaded != LOADED || is_ms(m)) return 0;
    if (m->packed) return 1;
    for (l = 0; l < m->n_layers; l++) {
        const vocr_textspot_layer *L = &m->layers[l];
        const unsigned cin = L->cin;
        if (!k1_packable(L)) continue;
        for (nb = 0; nb < L->cout / 8u; nb++) {
            int8_t *w = (int8_t *)(uintptr_t)(const void *)(L->w + (size_t)nb * 8u * cin);   /* [8][cin] */
            for (c = 0; c < 8u; c++)
                for (q = 0; q < cin; q++) t[c * cin + q] = w[c * cin + q];
            for (q = 0; q < cin; q += 2u)       /* -> per pair of q: [w(q,j) w(q,j+1) w(q+1,j) w(q+1,j+1)], j even */
                for (j = 0; j < 8u; j += 2u) {
                    int8_t *d = w + (size_t)q * 8u + 2u * j;
                    d[0] = t[j * cin + q];
                    d[1] = t[(j + 1u) * cin + q];
                    d[2] = t[j * cin + q + 1u];
                    d[3] = t[(j + 1u) * cin + q + 1u];
                }
        }
    }
    m->packed = 1;
    return 1;
#else
    (void)m;
    return 0;
#endif
}

/* ---------------------------------------------------------------- arena */

typedef struct {
    int32_t b;    /* bias */
    int32_t m;    /* multiplier */
    int32_t sh;   /* shift */
    int32_t fast; /* |b| <= 2^31 - 1 - 2^24: bias + sum fits int32 (NEON's 32-bit form) */
} Chan;

typedef struct {
    uint32_t ring;       /* arena offset of a 3x3 layer's input ring */
    uint32_t plane;      /* bytes per channel plane of a ring row: w_in + 2 dil + BLK stride */
    uint16_t w_in, h_in, w_out, h_out;
    uint16_t received;   /* input rows in the ring so far (3x3 layers) */
    uint16_t next_out;   /* next output row */
    uint16_t chan;       /* the layer's first channel in the Chan table */
    uint16_t next;       /* the next 3x3 layer, or n_layers: the head's map row */
} Run;

typedef struct {
    uint16_t a, b, comp; /* map columns [a, b) of a kernel run and its component slot */
} Seg;

typedef struct {
    int32_t n, sk, so, sa1, sa2, sr, sc;  /* cells; sums of K, O, A1, A2, map rows, map columns */
    uint16_t r0, r1, c0, c1;              /* first and last row, first and last column */
    uint16_t parent;                      /* union-find parent; NONE when the slot is free */
    uint16_t stamp;                       /* map row + 1 of the last row whose runs joined it */
} Comp;

/* A kept box (the cap's candidates) or, in the blocks' scratch, a block: 16 bytes, as vocr_textspot3.c's record (the
 * candidates' section and the blocks' scratch keep their sizes, so versions 1-3 keep their arenas). */
typedef struct {
    uint16_t x, y, w, h;      /* the box, clipped to the frame (a block: its boxes' union) */
    uint8_t v, conf;          /* vertical flag, confidence (boxes) */
    uint8_t tag, tconf;       /* the class (a block: its boxes'), and a box's margin of it (version 4; else 0) */
    union {
        uint32_t n;           /* boxes: the component's cells (in the cap key) */
        struct {
            uint8_t first, count;   /* blocks: their boxes' indices are member[first .. first + count) */
        } b;
    } u;
} Cand;

typedef char cand_is_16_bytes[sizeof(Cand) == 16u ? 1 : -1];

/* The blocks' scratch, over the network's part of the arena once the frame has been read. */
typedef struct {
    uint8_t parent[CANDS];    /* union-find over the boxes in sorted order (by position) */
    uint8_t sorted[CANDS];    /* the boxes (indices into the candidates) by (x, y, w, h, vertical, conf, tag, tconf) */
    uint8_t member[CANDS];    /* the boxes block by block, each block in reading order */
    uint8_t order[CANDS];     /* the blocks in reading order */
    Cand blk[CANDS];          /* the blocks in the order of their first box */
} Group;

typedef struct {
    size_t run, chan, rings, tmp, head, drow, comp, tsum, freel, prev, cur, psum, cand, total;
    unsigned f, win, wn, wm, nc, nr;
} Plan;

static size_t up8(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}

static int loaded(const vocr_textspot6 *m)
{
    return m && m->loaded == LOADED && m->n_layers >= 2u && m->n_layers <= VOCR_TEXTSPOT_MAX_LAYERS;
}

/* 2 when a frame this wide is halved before the network (textspot_twin.scale_of: versions 1 and 2), else 1. */
static unsigned scale_of(const vocr_textspot6 *m, unsigned width)
{
    return m->half_above && width > m->half_above ? 2u : 1u;
}

/* 2 when a version-3 or version-4 frame of this size is halved (textspot3_twin.scale_of, textspot6_twin.scale_of),
 * else 1. */
static unsigned scale3(const vocr_textspot6 *m, unsigned w, unsigned h)
{
    return (m->half_above && w > m->half_above) || (m->half_above_h && h > m->half_above_h) ? 2u : 1u;
}

/* The scale of a frame of this size for any version: by its width alone for versions 1 and 2. */
static unsigned scale_wh(const vocr_textspot6 *m, unsigned w, unsigned h)
{
    return m->version >= VOCR_TEXTSPOT6_VERSION_MS ? scale3(m, w, h) : scale_of(m, w);
}

/* Bytes per channel plane of a 3x3 layer's ring row: the d zero columns either side of the w input pixels, then
 * BLK * stride zero columns that a last, partial block of BLK output pixels reads past them (BLK - 1 strides for
 * the scalar code, one byte more for NEON's 16-byte de-interleaving load). */
static size_t plane_bytes(unsigned w, const vocr_textspot_layer *L)
{
    return (size_t)w + 2u * L->dil + BLK * L->stride;
}

/* The arena layout of a VTDT network (versions 1, 2 and version 4's trunk) for a frame of the given size read at the
 * scale `force` (1 or 2; 0: the blob's rule), sections 8-byte aligned from an aligned base: the height matters only
 * through the scale (version 4). The blocks' scratch (Group) lies over the sections before the candidates, so those
 * span at least its size. With no tag map (versions 1 and 2) and the rule's scale it is vocr_textspot3.c's plan
 * exactly. */
static void plan(const vocr_textspot6 *m, unsigned width, unsigned height, unsigned force, Plan *p)
{
    unsigned i, f = force ? force : scale_wh(m, width, height), w = f == 2u ? (width + 1u) / 2u : width, maxch = 1;
    size_t rings = 0;
    p->f = f;
    p->win = w;
    for (i = 0; i < m->n_layers; i++) {
        const vocr_textspot_layer *L = &m->layers[i];
        if (L->k == 3u) rings += (size_t)(2u * L->dil + 1u) * L->cin * plane_bytes(w, L);
        if (L->cout > maxch) maxch = L->cout;
        w = (w + L->stride - 1u) / L->stride;
    }
    p->wn = w;                                                   /* the head's row: cells */
    p->wm = m->outputs != MAPS ? (p->win + 3u) / 4u : w;        /* the decode's map row: cells */
    p->nc = p->wm;
    p->nr = (p->wm + 1u) / 2u;
    p->run = 0;
    p->chan = p->run + up8((size_t)m->n_layers * sizeof(Run));
    p->rings = p->chan + up8((size_t)m->channels * sizeof(Chan));
    p->tmp = p->rings + up8(rings);
    p->head = p->tmp + up8((size_t)3u * BLK * maxch);   /* three blocks: a 3x3 output, two 1x1 ones */
    p->drow = p->head + up8((size_t)m->outputs * p->wn);                           /* outputs planes */
    /* a d2s head's map row: K, O, A1, A2 and the tag maps (versions 2 and 4) */
    p->comp = p->drow + up8(m->outputs != MAPS ? (size_t)(MAPS + m->tags) * p->wm : 0u);
    p->tsum = p->comp + up8((size_t)p->nc * sizeof(Comp));
    p->freel = p->tsum + up8((size_t)p->nc * m->tags * sizeof(int32_t));        /* version 4's tag sums */
    p->prev = p->freel + up8((size_t)p->nc * sizeof(uint16_t));
    p->cur = p->prev + up8((size_t)p->nr * sizeof(Seg));
    p->psum = p->cur + up8((size_t)p->nr * sizeof(Seg));
    p->cand = p->psum + up8(f == 2u ? (size_t)p->win * sizeof(uint16_t) : 0u);
    if (p->cand < up8(sizeof(Group))) p->cand = up8(sizeof(Group));
    p->total = p->cand + up8((size_t)CANDS * sizeof(Cand));
}

static int side_ok(unsigned w, unsigned h)
{
    return w && h && w <= VOCR_TEXTSPOT_MAX_SIDE && h <= VOCR_TEXTSPOT_MAX_SIDE;
}

/* ---------------------------------------------------------------- versions 3 and 4: a VTMS network's arena */

#ifndef VOCR_TEXTSPOT6_NO_MS
typedef struct {
    uint64_t run, chan, rings, tmp, head, psum, end;   /* the stream's state, from its base */
} StreamLay;

typedef struct {
    uint64_t mode, sums, xp, tmp;   /* a 1x1 op's scratch after its constants: modes, sums, offsets, block */
    uint64_t wt, acc, ring;         /* a depthwise op's: taps, row sums, ring */
    uint64_t end;
    uint32_t pb;                    /* a 1x1 op's cells per block */
} OpLay;

typedef struct {
    uint64_t mode, sums, xp, gat, hwc, drow, comp, tsum, freel, prev, cur, end;   /* the head's scratch, the decode's */
    uint32_t pb, wn, wm, nc, nr;
} HeadLay;

typedef struct {
    uint64_t top;                                    /* the stack's top, from its base */
    uint32_t at[VOCR_TEXTSPOT6_MAX_MAPS];            /* a pushed slot's offset */
    unsigned char stk[VOCR_TEXTSPOT6_MAX_MAPS];      /* the pushed slots, bottom first */
    unsigned char dead[VOCR_TEXTSPOT6_MAX_MAPS];
    unsigned n, over;
} Stack;

typedef struct {
    uint64_t total;          /* from the aligned base: the candidates, the slots' offsets, the stack */
    uint64_t slots, stack;   /* offsets of the slots' offsets and of the stack */
    unsigned f, win, hin, over;
} Plan3;

static uint64_t up8x(uint64_t n)
{
    return (n + 7u) & ~(uint64_t)7u;
}

static uint32_t cdiv(uint32_t a, uint32_t b)
{
    return (a + b - 1u) / b;
}

static uint64_t cells_of(unsigned stride, unsigned win, unsigned hin)
{
    return (uint64_t)cdiv(hin, stride) * cdiv(win, stride);
}

static uint64_t slot_bytes(const vocr_textspot6 *m, unsigned s, unsigned win, unsigned hin)
{
    return up8x(cells_of(m->slot_stride[s], win, hin) * m->slot_cap[s]);
}

/* The stream's state for a network input win wide (version 1's rings, blocks, head row and halving row; the head
 * row holds the stream's last layer's c8 planes). */
static void stream_lay(const vocr_textspot6 *m, unsigned win, unsigned f, StreamLay *L)
{
    unsigned i, w = win, maxch = 1;
    uint64_t rings = 0;
    for (i = 0; i < m->n_layers; i++) {
        const vocr_textspot_layer *Ly = &m->layers[i];
        if (Ly->k == 3u) rings += (uint64_t)(2u * Ly->dil + 1u) * Ly->cin * plane_bytes(w, Ly);
        if (Ly->cout > maxch) maxch = Ly->cout;
        w = (w + Ly->stride - 1u) / Ly->stride;
    }
    L->run = 0;
    L->chan = up8x((uint64_t)m->n_layers * sizeof(Run));
    L->rings = L->chan + up8x((uint64_t)m->channels * sizeof(Chan));
    L->tmp = L->rings + up8x(rings);
    L->head = L->tmp + up8x((uint64_t)3u * BLK * maxch);
    L->psum = L->head + up8x((uint64_t)m->map_chans[0] * w);
    L->end = L->psum + up8x(f == 2u ? (uint64_t)win * sizeof(uint16_t) : 0u);
}

/* A 1x1 op reads a block's codes where they lie (one source at u = 0: the block's cells are contiguous in its slot)
 * or gathers them first into a block of cin bytes per cell (several sources, or an upsampled one). */
static int pw_gathers(const vocr_textspot6_op *op)
{
    return op->nsrc > 1u || op->up[0] != 0u;
}

/* A 1x1 op's cells per block: its inputs about 12 KB (8 KB when gathered; they stay in the L1 cache while the weights
 * of every 4 output channels pass), at most 4 KB of outputs when it writes in place through a block, even, 2..128, at
 * most the cells. */
static uint32_t pw_block(const vocr_textspot6_op *op, uint64_t cells)
{
    int g = pw_gathers(op);
    uint32_t pb = (g ? 8192u : 12288u) / op->cin;
    if (op->inplace && !g && pb > 4096u / op->cout) pb = 4096u / op->cout;
    pb &= ~1u;
    if (pb < 2u) pb = 2u;
    if (pb > 128u) pb = 128u;
    if (pb > cells) pb = (uint32_t)cells;
    return pb;
}

/* Op k's scratch: 3 cout int32 constants, a mode per 4 channels, then for a 1x1 op the sums of a block's pairs of
 * cells for 4 channels, and when it gathers its cells' source offsets and the gathered block (cin bytes per cell),
 * else when it writes in place its block of outputs; for a depthwise op its weights by tap, a row's sums for 8
 * channels, and when it writes in place its ring of d + 1 rows. */
static void op_lay(const vocr_textspot6 *m, unsigned k, unsigned win, unsigned hin, OpLay *L)
{
    const vocr_textspot6_op *op = &m->ops[k];
    L->mode = up8x((uint64_t)12u * op->cout);
    L->pb = 0;
    if (op->kind == OP_PW) {
        int g = pw_gathers(op);
        L->pb = pw_block(op, cells_of(op->out_stride, win, hin));
        L->sums = L->mode + up8x(op->cout / 4u);
        L->xp = L->sums + up8x((uint64_t)(L->pb + 1u) * 4u * sizeof(int32_t));
        L->tmp = L->xp + (g ? up8x((uint64_t)L->pb * op->nsrc * sizeof(uint32_t)) : 0u);
        L->end = L->tmp + (g ? up8x((uint64_t)L->pb * op->cin) : op->inplace ? up8x((uint64_t)L->pb * op->cout) : 0u);
        L->wt = L->acc = L->ring = L->end;
    } else {
        L->wt = L->mode + up8x(op->cout / 4u);
        L->acc = L->wt + up8x((uint64_t)9u * op->cout);
        L->ring = L->acc + up8x((uint64_t)cdiv(win, op->out_stride) * 8u * sizeof(int32_t));
        L->end = L->ring + (op->inplace ? up8x((uint64_t)(op->dil + 1u) * cdiv(win, op->out_stride) *
                                               m->slot_cap[op->slot]) : 0u);
        L->sums = L->xp = L->tmp = L->end;
    }
}

/* The head's scratch (constants, modes, sums, and when it gathers its cells' offsets and gathered block; its HWC row
 * of 16 + 4 T channels) and the decode's state (its map row of 4 + T planes, the component slots and their T tag
 * sums each, the run lists). */
static void head_lay(const vocr_textspot6 *m, unsigned win, HeadLay *L)
{
    const vocr_textspot6_op *op = &m->ops[m->n_ops - 1u];
    int g = pw_gathers(op);
    L->wn = cdiv(win, D2S_STRIDE);
    L->wm = cdiv(win, D2S_STRIDE / 2u);
    L->nc = L->wm;
    L->nr = (L->wm + 1u) / 2u;
    L->pb = pw_block(op, L->wn);
    L->mode = up8x((uint64_t)12u * op->cout);
    L->sums = L->mode + up8x(op->cout / 4u);
    L->xp = L->sums + up8x((uint64_t)(L->pb + 1u) * 4u * sizeof(int32_t));
    L->gat = L->xp + (g ? up8x((uint64_t)L->pb * op->nsrc * sizeof(uint32_t)) : 0u);
    L->hwc = L->gat + (g ? up8x((uint64_t)L->pb * op->cin) : 0u);
    L->drow = L->hwc + up8x((uint64_t)L->wn * m->outputs);
    L->comp = L->drow + up8x((uint64_t)(MAPS + m->tags) * L->wm);
    L->tsum = L->comp + up8x((uint64_t)L->nc * sizeof(Comp));
    L->freel = L->tsum + up8x((uint64_t)L->nc * m->tags * sizeof(int32_t));
    L->prev = L->freel + up8x((uint64_t)L->nc * sizeof(uint16_t));
    L->cur = L->prev + up8x((uint64_t)L->nr * sizeof(Seg));
    L->end = L->cur + up8x((uint64_t)L->nr * sizeof(Seg));
}

static void stk_init(Stack *s)
{
    memset(s, 0, sizeof(*s));
}

/* A new slot on top (its offset into off[slot] too when off is given). */
static void stk_push(Stack *s, unsigned slot, uint64_t bytes, uint32_t *off)
{
    if (s->top > MS_LIMIT) s->over = 1;
    s->at[slot] = s->over ? 0u : (uint32_t)s->top;
    if (off) off[slot] = s->at[slot];
    s->top += bytes;
    s->stk[s->n++] = (unsigned char)slot;
    s->dead[slot] = 0;
}

/* The slot is dead; dead slots leave the top. */
static void stk_free(Stack *s, unsigned slot)
{
    s->dead[slot] = 1;
    while (s->n && s->dead[s->stk[s->n - 1u]]) s->top = s->at[s->stk[--s->n]];
}

/* Before op k: its new output slot on top; returns the offset of its scratch (the top). */
static uint64_t op_begin(Stack *s, const vocr_textspot6 *m, unsigned k, unsigned win, unsigned hin, uint32_t *off)
{
    const vocr_textspot6_op *op = &m->ops[k];
    if (op->slot != NONE8 && !op->inplace) stk_push(s, op->slot, slot_bytes(m, op->slot, win, hin), off);
    return s->top;
}

/* After op k: the slots of the maps it read for the last time (but the one its output took over), and its own when
 * no op reads its output. */
static void op_end(Stack *s, const vocr_textspot6 *m, unsigned k)
{
    const vocr_textspot6_op *op = &m->ops[k];
    unsigned q, r;
    for (q = 0; q < op->nsrc; q++) {
        unsigned src = op->src[q], dup = 0;
        for (r = 0; r < q; r++) dup |= op->src[r] == src;
        if (dup || m->last_use[src] != (signed char)k) continue;
        if (op->inplace && m->map_slot[src] == op->slot) continue;
        stk_free(s, m->map_slot[src]);
    }
    if (op->slot != NONE8 && m->last_use[k + 1u] < 0) stk_free(s, op->slot);
}

/* The stack's peak for an h x w network input: F8's slot under the stream's state, then each op's slots under its
 * scratch, then the head's scratch, and at least the blocks' scratch. */
static uint64_t stack_peak(const vocr_textspot6 *m, unsigned win, unsigned hin, unsigned f, unsigned *over)
{
    Stack s;
    StreamLay sl;
    OpLay ol;
    HeadLay hl;
    uint64_t peak, sc;
    unsigned k;
    stk_init(&s);
    stk_push(&s, 0, slot_bytes(m, 0, win, hin), NULL);
    stream_lay(m, win, f, &sl);
    peak = s.top + sl.end;
    for (k = 0; k + 1u < m->n_ops; k++) {
        sc = op_begin(&s, m, k, win, hin, NULL);
        op_lay(m, k, win, hin, &ol);
        if (sc + ol.end > peak) peak = sc + ol.end;
        op_end(&s, m, k);
    }
    head_lay(m, win, &hl);
    if (s.top + hl.end > peak) peak = s.top + hl.end;
    if (peak < up8x(sizeof(Group))) peak = up8x(sizeof(Group));
    *over = s.over;
    return peak;
}

/* A VTMS network's arena for a fw x fh frame read at the scale `force` (1 or 2; 0: the blob's rule): the candidates
 * at 0, the slots' offsets, the stack. */
static void plan3(const vocr_textspot6 *m, unsigned fw, unsigned fh, unsigned force, Plan3 *p)
{
    p->f = force ? force : scale3(m, fw, fh);
    p->win = p->f == 2u ? (fw + 1u) / 2u : fw;
    p->hin = p->f == 2u ? (fh + 1u) / 2u : fh;
    p->slots = up8x((uint64_t)CANDS * sizeof(Cand));
    p->stack = p->slots + up8x((uint64_t)m->n_slots * sizeof(uint32_t));
    p->total = p->stack + stack_peak(m, p->win, p->hin, p->f, &p->over);
    if (p->total > MS_LIMIT) p->over = 1;
}

static size_t frame3(const vocr_textspot6 *m, unsigned width, unsigned height, unsigned force)
{
    Plan3 p;
    plan3(m, width, height, force, &p);
    return p.over ? 0u : (size_t)p.total + 7u;
}
#endif /* !VOCR_TEXTSPOT6_NO_MS */

/* A frame's need at the scale `force` (1 or 2; 0: the blob's rule): a VTMS network's plan3, else plan (0 past the
 * VTMS limit). */
static size_t frame_need(const vocr_textspot6 *m, unsigned width, unsigned height, unsigned force)
{
    Plan p;
#ifndef VOCR_TEXTSPOT6_NO_MS
    if (is_ms(m)) return frame3(m, width, height, force);
#endif
    plan(m, width, height, force, &p);
    return p.total + 7u;
}

/* The scale the flags force: 1 or 2, 0 for none (the blob's rule), 3 for both (an argument error). */
static unsigned forced(unsigned flags)
{
    return (flags & VOCR_TEXTSPOT6_SCALE1 ? 1u : 0u) | (flags & VOCR_TEXTSPOT6_SCALE2 ? 2u : 0u);
}

size_t vocr_textspot6_frame_arena_bytes(const vocr_textspot6 *m, unsigned width, unsigned height)
{
    if (!loaded(m) || !side_ok(width, height)) return 0;
    return frame_need(m, width, height, 0);
}

size_t vocr_textspot6_frame_arena_bytes_flags(const vocr_textspot6 *m, unsigned width, unsigned height,
                                              unsigned flags)
{
    unsigned force = forced(flags);
    if (!loaded(m) || !side_ok(width, height) || (flags & ~VOCR_TEXTSPOT6_FLAGS) || force == 3u) return 0;
    return frame_need(m, width, height, force);
}

size_t vocr_textspot6_arena_bytes_flags(const vocr_textspot6 *m, unsigned max_width, unsigned max_height,
                                        unsigned flags)
{
    unsigned force = forced(flags);
    if (!loaded(m) || !side_ok(max_width, max_height) || (flags & ~VOCR_TEXTSPOT6_FLAGS) || force == 3u) return 0;
    if (!force) return vocr_textspot6_arena_bytes(m, max_width, max_height);
    return frame_need(m, max_width, max_height, force);   /* at a fixed scale the plans grow with both sides */
}

size_t vocr_textspot6_arena_bytes(const vocr_textspot6 *m, unsigned max_width, unsigned max_height)
{
    size_t a, b;
    if (loaded(m) && m->version >= VOCR_TEXTSPOT6_VERSION_MS) {
        /* versions 3 and 4, the most of every frame up to the size (both plans grow with the network input's sides,
         * and a trunk's with its width for either scale): the largest frame read at full size, and the frame itself
         * when it is halved */
        unsigned w1 = m->half_above && max_width > m->half_above ? m->half_above : max_width;
        unsigned h1 = m->half_above_h && max_height > m->half_above_h ? m->half_above_h : max_height;
        if (!side_ok(max_width, max_height)) return 0;
        a = frame_need(m, w1, h1, 0);
        if (a && scale3(m, max_width, max_height) == 2u) {
            b = frame_need(m, max_width, max_height, 0);
            a = !b ? 0u : b > a ? b : a;
        }
        return a;
    }
    a = vocr_textspot6_frame_arena_bytes(m, max_width, max_height);
    if (a && scale_of(m, max_width) == 2u) {   /* a frame of half_above px is read at full size */
        b = vocr_textspot6_frame_arena_bytes(m, m->half_above, max_height);
        if (b > a) a = b;
    }
    return a;
}

/* ---------------------------------------------------------------- network */

typedef struct {
    const VocrLinesImage *im;   /* a frame in memory, or */
    const VocrLinesRows *rows;  /* a row source */
} Src;

typedef struct {
    const vocr_textspot6 *m;
    unsigned char *base;        /* the aligned arena */
    Run *run;
    Chan *chan;
    int8_t *head;               /* the head's row: its outputs planes of wn cells (version 1: K, O, A1, A2) */
    int8_t *drow;               /* a d2s head: the decode's map row, the K, O, A1, A2 (and T tag) planes of wm cells */
    int8_t *tmp;                /* three blocks of BLK x maxch codes: a 3x3 output, two 1x1 ones */
    uint16_t *psum;             /* the halving's pair sums (f = 2) */
    unsigned maxch;             /* the most output channels of a layer */
    unsigned rl[VOCR_TEXTSPOT_MAX_LAYERS], nrl;   /* the 3x3 layers, in order */
    unsigned f, fw, fh;         /* the scale and the frame's size */
    unsigned width, height;     /* the network's input size */
    unsigned wn, hn;            /* the head's output size */
    unsigned wm, hm;            /* the size of the maps the decode reads (version 1: the head's) */
    vocr_textspot_map_row hook;
    void *user;
    /* decode */
    Comp *comp;
    int32_t *tsum;              /* version 4: T tag sums per component slot (nothing for versions 1-3) */
    uint16_t *freel;
    unsigned nfree, nc, tags;
    Seg *prev, *cur;
    unsigned np;
    Cand *cand;
    unsigned ncand, ctrunc;
    unsigned char *grp;         /* the blocks' scratch (Group): the arena's base, or (a VTMS network) the stack's */
    /* a VTMS network */
    unsigned char *stack;       /* the slots' stack */
    uint32_t *slot_off;         /* each slot's offset from it */
    int8_t *f8;                 /* map 0's slot */
    unsigned f8cap;             /* its bytes per cell */
} Net;

static const unsigned char *src_row(const Src *s, unsigned y)
{
    if (s->im) return s->im->pixels + (size_t)y * s->im->stride;
    return s->rows->row(s->rows->user, y);
}

/* bias * m + 2^(sh - 1): the requantisation's constant term (int64, see the ranges above) */
static int64_t bq_of(const Chan *c)
{
    return (int64_t)c->b * c->m + ((int64_t)1 << (c->sh - 1));
}

#if !TS_NEON
/* floor(v / 2^s) without shifting a negative value */
static int64_t shr64(int64_t v, unsigned s)
{
    return v < 0 ? ~(~v >> s) : v >> s;
}

static int32_t shr32(int32_t v, unsigned s)
{
    return v < 0 ? ~(~v >> s) : v >> s;
}

static int8_t requant(int32_t acc, const Chan *c, int32_t lo)
{
    int64_t p = bq_of(c) + (int64_t)acc * c->m;
    int32_t q;
    if (c->sh >= 32) {
        q = shr32((int32_t)shr64(p, 32u), (unsigned)(c->sh - 32));   /* floor(floor(p / 2^32) / 2^(sh - 32)) */
    } else {
        int64_t r = shr64(p, (unsigned)c->sh);
        q = r < lo ? lo : r > 127 ? 127 : (int32_t)r;
    }
    return (int8_t)(q < lo ? lo : q > 127 ? 127 : q);
}
#endif

#if TS_NEON
/* ---- NEON (the Vita): a block's 8 int32 sums in two registers; the arithmetic of requant() */

/* (bias + sum) * m + 2^(sh - 1), floor-divided by 2^sh, per lane, saturated to int8, then at least lo; n lanes
 * stored at out. Where bias + sum fits int32 (fast: every channel of a trained blob) that is VRSHL by -sh of the
 * exact int64 product: (x + 2^(sh-1)) >> sh, round half up, the twin's rule. Otherwise the int64 constant
 * bias * m + 2^(sh - 1) is added by VMLAL and VSHL shifts (floor). VSHL / VRSHL read the shift from each 64-bit
 * lane's lowest byte, so a dup of the int32 -sh serves. */
#define RQ_STORE(LO, HI, C, LOW, OUT, N) do { \
        const Chan *c_ = (C); \
        int64x2_t s_ = vreinterpretq_s64_s32(vdupq_n_s32(-c_->sh)), p0_, p1_, p2_, p3_; \
        int32x2_t m_ = vdup_n_s32(c_->m); \
        if (c_->fast) { \
            int32x4_t b_ = vdupq_n_s32(c_->b), l_ = vaddq_s32((LO), b_), h_ = vaddq_s32((HI), b_); \
            p0_ = vrshlq_s64(vmull_s32(vget_low_s32(l_), m_), s_); \
            p1_ = vrshlq_s64(vmull_s32(vget_high_s32(l_), m_), s_); \
            p2_ = vrshlq_s64(vmull_s32(vget_low_s32(h_), m_), s_); \
            p3_ = vrshlq_s64(vmull_s32(vget_high_s32(h_), m_), s_); \
        } else { \
            int64x2_t b_ = vdupq_n_s64(bq_of(c_)); \
            p0_ = vshlq_s64(vmlal_s32(b_, vget_low_s32(LO), m_), s_); \
            p1_ = vshlq_s64(vmlal_s32(b_, vget_high_s32(LO), m_), s_); \
            p2_ = vshlq_s64(vmlal_s32(b_, vget_low_s32(HI), m_), s_); \
            p3_ = vshlq_s64(vmlal_s32(b_, vget_high_s32(HI), m_), s_); \
        } \
        { \
            int16x4_t h0_ = vqmovn_s32(vcombine_s32(vqmovn_s64(p0_), vqmovn_s64(p1_))); \
            int16x4_t h1_ = vqmovn_s32(vcombine_s32(vqmovn_s64(p2_), vqmovn_s64(p3_))); \
            int8x8_t v_ = vqmovn_s16(vcombine_s16(h0_, h1_)); \
            if (!(LOW)) v_ = vmax_s8(v_, vmov_n_s8(0));   /* ReLU; -128 is the saturation's own floor */ \
            if ((N) == BLK) { \
                vst1_s8((OUT), v_); \
            } else { \
                int8_t t_[BLK]; \
                unsigned i_; \
                vst1_s8(t_, v_); \
                for (i_ = 0; i_ < (N); i_++) (OUT)[i_] = t_[i_]; \
            } \
        } \
    } while (0)

/* 8 pixels of one input channel at stride S, widened (vld2 reads 16 bytes: the plane's slack covers it) */
#define LOAD8(x, S) vmovl_s8((S) == 1u ? vld1_s8(x) : vld2_s8(x).val[0])
/* LO/HI += X * lane L of the int16x4 weights W, or * the scalar K */
#define MACL(LO, HI, X, W, L) do { \
        int16x8_t x_ = (X); \
        (LO) = vmlal_lane_s16((LO), vget_low_s16(x_), (W), L); \
        (HI) = vmlal_lane_s16((HI), vget_high_s16(x_), (W), L); \
    } while (0)
#define MACN(LO, HI, X, K) do { \
        int16x8_t x_ = (X); \
        (LO) = vmlal_n_s16((LO), vget_low_s16(x_), (int16_t)(K)); \
        (HI) = vmlal_n_s16((HI), vget_high_s16(x_), (int16_t)(K)); \
    } while (0)

/* A block of BLK output pixels from pixel j0 of a 3x3 layer with input stride S, output channel by output channel
 * into out[c * ostride + p] (p < n). rows[ky] is the padded input row of kernel row ky (the centre row where
 * valid[ky] is 0, its weights then masked to 0), plane bytes from one input channel to the next. Depthwise:
 * output channel c reads input channel c; otherwise every input channel (cin of them). */
#define K3_BLOCK(name, S) \
static void name(const int8_t *w, const Chan *ch, int32_t lo, unsigned cin, unsigned cout, int depthwise, \
                 const int8_t *const *rows, const int *valid, size_t plane, unsigned d, unsigned j0, int8_t *out, \
                 size_t ostride, unsigned n) \
{ \
    static const int16_t top_[8] = {0, 0, 0, -1, -1, -1, -1, -1}, mid_[8] = {-1, -1, -1, -1, -1, -1, 0, 0}; \
    int16x8_t keep = vdupq_n_s16(-1); \
    int last = valid[2]; \
    unsigned c, q, nq = depthwise ? 1u : cin; \
    const int8_t *r0 = rows[0] + (size_t)j0 * (S), *r1 = rows[1] + (size_t)j0 * (S); \
    const int8_t *r2 = rows[2] + (size_t)j0 * (S), *wq = w; \
    if (!valid[0]) keep = vandq_s16(keep, vld1q_s16(top_)); \
    if (!valid[2]) keep = vandq_s16(keep, vld1q_s16(mid_)); \
    for (c = 0; c < cout; c++) { \
        int32x4_t lo4 = vdupq_n_s32(0), hi4 = lo4; \
        const int8_t *x0 = r0, *x1 = r1, *x2 = r2; \
        if (depthwise) { \
            x0 += c * plane; \
            x1 += c * plane; \
            x2 += c * plane; \
        } \
        for (q = 0; q < nq; q++, wq += 9, x0 += plane, x1 += plane, x2 += plane) { \
            int16x8_t wv = vandq_s16(vmovl_s8(vld1_s8(wq)), keep); \
            int16x4_t wa = vget_low_s16(wv), wb = vget_high_s16(wv); \
            const int8_t *x = x0; \
            MACL(lo4, hi4, LOAD8(x, S), wa, 0); \
            x += d; \
            MACL(lo4, hi4, LOAD8(x, S), wa, 1); \
            x += d; \
            MACL(lo4, hi4, LOAD8(x, S), wa, 2); \
            x = x1; \
            MACL(lo4, hi4, LOAD8(x, S), wa, 3); \
            x += d; \
            MACL(lo4, hi4, LOAD8(x, S), wb, 0); \
            x += d; \
            MACL(lo4, hi4, LOAD8(x, S), wb, 1); \
            x = x2; \
            MACL(lo4, hi4, LOAD8(x, S), wb, 2); \
            x += d; \
            MACL(lo4, hi4, LOAD8(x, S), wb, 3); \
            if (last) MACN(lo4, hi4, LOAD8(x + d, S), wq[8]); \
        } \
        RQ_STORE(lo4, hi4, &ch[c], lo, out + c * ostride, n); \
    } \
}
K3_BLOCK(k3_block_s1, 1u)
K3_BLOCK(k3_block_s2, 2u)

/* A block of a 1x1 layer: in[q * BLK + p] -> out[c * ostride + p] (p < n). Two output channels per pass (four
 * sums, two weight vectors and the inputs stay in the 16 Q registers), weights 8 input channels at a time (never
 * read past a weight row), then one at a time. */
#define K1_STEP(U, H, L) do { \
        int16x8_t x_ = vmovl_s8(vld1_s8(in + (q + (U)) * BLK)); \
        int16x4_t xl_ = vget_low_s16(x_), xh_ = vget_high_s16(x_); \
        a0 = vmlal_lane_s16(a0, xl_, H(k0), L); b0 = vmlal_lane_s16(b0, xh_, H(k0), L); \
        a1 = vmlal_lane_s16(a1, xl_, H(k1), L); b1 = vmlal_lane_s16(b1, xh_, H(k1), L); \
    } while (0)

static void k1_block(const int8_t *w, const Chan *ch, int32_t lo, unsigned cin, unsigned cout, const int8_t *in,
                     int8_t *out, size_t ostride, unsigned n)
{
    unsigned c = 0, q;
    for (; c + 2u <= cout; c += 2u) {
        int32x4_t a0 = vdupq_n_s32(0), b0 = a0, a1 = a0, b1 = a0;
        const int8_t *w0 = w + (size_t)c * cin, *w1 = w0 + cin;
        for (q = 0; q + 8u <= cin; q += 8u) {
            int16x8_t k0 = vmovl_s8(vld1_s8(w0 + q)), k1 = vmovl_s8(vld1_s8(w1 + q));
            K1_STEP(0, vget_low_s16, 0);
            K1_STEP(1, vget_low_s16, 1);
            K1_STEP(2, vget_low_s16, 2);
            K1_STEP(3, vget_low_s16, 3);
            K1_STEP(4, vget_high_s16, 0);
            K1_STEP(5, vget_high_s16, 1);
            K1_STEP(6, vget_high_s16, 2);
            K1_STEP(7, vget_high_s16, 3);
        }
        for (; q < cin; q++) {
            int16x8_t x = vmovl_s8(vld1_s8(in + q * BLK));
            MACN(a0, b0, x, w0[q]);
            MACN(a1, b1, x, w1[q]);
        }
        RQ_STORE(a0, b0, &ch[c], lo, out + c * ostride, n);
        RQ_STORE(a1, b1, &ch[c + 1u], lo, out + (c + 1u) * ostride, n);
    }
    for (; c < cout; c++) {
        int32x4_t lo4 = vdupq_n_s32(0), hi4 = lo4;
        const int8_t *wc = w + (size_t)c * cin;
        for (q = 0; q < cin; q++) MACN(lo4, hi4, vmovl_s8(vld1_s8(in + q * BLK)), wc[q]);
        RQ_STORE(lo4, hi4, &ch[c], lo, out + c * ostride, n);
    }
}
#else
/* ---- scalar (the PSP, hosts): eight named accumulators, so they stay in registers */

typedef struct {
    int32_t v[BLK];
} Acc;

static Acc acc_zero(void)
{
    Acc a = {{0, 0, 0, 0, 0, 0, 0, 0}};
    return a;
}

#define MAC(a, k, x, S) do { \
        const int8_t *x_ = (x); \
        int32_t k_ = (k); \
        (a).v[0] += k_ * x_[0]; (a).v[1] += k_ * x_[1 * (S)]; (a).v[2] += k_ * x_[2 * (S)]; \
        (a).v[3] += k_ * x_[3 * (S)]; (a).v[4] += k_ * x_[4 * (S)]; (a).v[5] += k_ * x_[5 * (S)]; \
        (a).v[6] += k_ * x_[6 * (S)]; (a).v[7] += k_ * x_[7 * (S)]; \
    } while (0)

static void store_acc(Acc a, const Chan *c, int32_t lo, int8_t *out, unsigned n)
{
    unsigned p;
    if (c->sh >= 32) {                        /* the usual case: the high word, then a 32-bit shift */
        int64_t bq = bq_of(c);
        int32_t m = c->m;
        unsigned s = (unsigned)(c->sh - 32);
#if TS_SIMD32 && defined(__GNUC__)
        /* Kotoba3DS: GCC's >> of a negative value is the arithmetic shift, floor division as shr64 / shr32
         * compute it: SMLAL, a shift and SSAT on the ARM11 */
        for (p = 0; p < n; p++) {
            int32_t q = ts_ssat8((int32_t)((bq + (int64_t)a.v[p] * m) >> 32) >> s);
            out[p] = (int8_t)(q < lo ? lo : q);
        }
#else
        for (p = 0; p < n; p++) {
            int32_t q = shr32((int32_t)shr64(bq + (int64_t)a.v[p] * m, 32u), s);
            out[p] = (int8_t)(q < lo ? lo : q > 127 ? 127 : q);
        }
#endif
    } else {
        for (p = 0; p < n; p++) out[p] = requant(a.v[p], c, lo);
    }
}

/* A block of BLK output pixels from pixel j0 of a 3x3 layer with input stride S, output channel by output channel
 * into out[c * ostride + p] (p < n). rows[ky] is the padded input row of kernel row ky (valid[ky] when it lies
 * inside the input), plane bytes from one input channel to the next. Depthwise: output channel c reads input
 * channel c; otherwise every input channel (cin of them). */
#define K3_BLOCK(name, S) \
static void name(const int8_t *w, const Chan *ch, int32_t lo, unsigned cin, unsigned cout, int depthwise, \
                 const int8_t *const *rows, const int *valid, size_t plane, unsigned d, unsigned j0, int8_t *out, \
                 size_t ostride, unsigned n) \
{ \
    unsigned c, q, ky; \
    for (c = 0; c < cout; c++) { \
        Acc a = acc_zero(); \
        unsigned q0 = depthwise ? c : 0, q1 = depthwise ? c + 1u : cin; \
        for (q = q0; q < q1; q++) { \
            const int8_t *wq = w + 9u * (depthwise ? c : c * cin + q); \
            for (ky = 0; ky < 3u; ky++) { \
                const int8_t *x = rows[ky] + q * plane + (size_t)j0 * (S), *wk = wq + 3u * ky; \
                if (!valid[ky]) continue; \
                MAC(a, wk[0], x, S); \
                MAC(a, wk[1], x + d, S); \
                MAC(a, wk[2], x + 2u * d, S); \
            } \
        } \
        store_acc(a, &ch[c], lo, out + c * ostride, n); \
    } \
}
K3_BLOCK(k3_block_s1, 1u)
K3_BLOCK(k3_block_s2, 2u)

/* A block of a 1x1 layer: in[q * BLK + p] -> out[c * ostride + p] (p < n). */
static void k1_block_c(const int8_t *w, const Chan *ch, int32_t lo, unsigned cin, unsigned cout, const int8_t *in,
                       int8_t *out, size_t ostride, unsigned n)
{
    unsigned c, q;
    for (c = 0; c < cout; c++) {
        Acc a = acc_zero();
        const int8_t *wc = w + (size_t)c * cin;
        for (q = 0; q < cin; q++) MAC(a, wc[q], in + q * BLK, 1u);
        store_acc(a, &ch[c], lo, out + c * ostride, n);
    }
}

#if TS_SIMD32
/* Kotoba3DS addition: the 1x1 layers on the ARM11's ARMv6 SIMD (no NEON there), through the recognizer's kernel
 * vocr__k2x8 (vocr.c): SMLAD multiplies two 16-bit pairs at once, a weight word holds two output channels' weights
 * for two input channels (vocr_textspot6_pack_simd32's layout: per block of 8 output channels and pair of input
 * channels q, q + 1, the 16 bytes [w(q, j) w(q, j + 1) w(q + 1, j) w(q + 1, j + 1)] for j = 0, 2, 4, 6), and each
 * is sign-extended once for two pixels. Integer sums are exact: the same bits as k1_block_c. */
#if defined(__ARM_FEATURE_SIMD32) && defined(__arm__) && !defined(__thumb__) && !defined(VOCR_SIMD32_EMULATE)
void vocr__k2x8(const void *w, const void *x, unsigned npairs, int32_t *out);   /* vocr.c's ARM assembly */
#define ts_k2x8 vocr__k2x8
#else
static void ts_k2x8(const void *w, const void *x, unsigned npairs, int32_t *out)   /* vocr.c's C stand-in */
{
    const uint32_t *wp = (const uint32_t *)w, *xp = (const uint32_t *)x;
    unsigned i, h;
    for (h = 0; h < 16; h++) out[h] = 0;
    for (i = 0; i < npairs; i++, wp += 4, xp += 2)
        for (h = 0; h < 4; h++) {
            const uint32_t lo = ts_sxtb16(wp[h]), hi = ts_sxtb16(ts_ror8(wp[h]));
            out[2 * h] = ts_smlad(lo, xp[0], out[2 * h]);
            out[2 * h + 1] = ts_smlad(hi, xp[0], out[2 * h + 1]);
            out[8 + 2 * h] = ts_smlad(lo, xp[1], out[8 + 2 * h]);
            out[8 + 2 * h + 1] = ts_smlad(hi, xp[1], out[8 + 2 * h + 1]);
        }
}
#endif

/* a 1x1 layer the packed kernel takes */
static int k1_packable(const vocr_textspot_layer *L)
{
    return L->kind == 0u && L->k == 1u && L->cin >= 2u && !(L->cin & 1u) && L->cin <= MAXCH && !(L->cout & 7u);
}

static void k1_block(const int8_t *w, const Chan *ch, int32_t lo, unsigned cin, unsigned cout, const int8_t *in,
                     int8_t *out, size_t ostride, unsigned n, int packed)
{
    /* the inputs as vocr__k2x8 reads them: for pixels 2i and 2i + 1, per pair of input channels one word
     * (x_2i(q), x_2i(q + 1)) then one word (x_2i+1(q), x_2i+1(q + 1)); + 2 words of slack (the kernel reads 8
     * bytes past x) */
    uint32_t xq[BLK / 2u][MAXCH + 2u];
    int32_t sums[16];
    const unsigned np = cin / 2u;
    unsigned i, q, nb, j, p;
    if (!packed) {
        k1_block_c(w, ch, lo, cin, cout, in, out, ostride, n);
        return;
    }
    for (i = 0; i < BLK / 2u; i++) {
        uint32_t *d = xq[i];
        for (q = 0; q < cin; q += 2u, d += 2) {
            const int8_t *r0 = in + (size_t)q * BLK + 2u * i, *r1 = r0 + BLK;
            d[0] = (uint32_t)(uint16_t)(int16_t)r0[0] | (uint32_t)(int32_t)r1[0] << 16;
            d[1] = (uint32_t)(uint16_t)(int16_t)r0[1] | (uint32_t)(int32_t)r1[1] << 16;
        }
        d[0] = d[1] = 0;
    }
    for (nb = 0; nb < cout / 8u; nb++) {
        const int8_t *wb = w + (size_t)nb * 8u * cin;
        const Chan *cb = ch + nb * 8u;
        int64_t bq[8];
        int fast = n == BLK;   /* every channel's shift 32 or more (store_acc's usual case), a whole block */
        for (j = 0; j < 8u; j++) {
            bq[j] = bq_of(&cb[j]);
            fast = fast && cb[j].sh >= 32;
        }
        for (i = 0; i < BLK / 2u && 2u * i < n; i++) {
            ts_k2x8(wb, xq[i], np, sums);
            if (fast) {
                int8_t *oc = out + 2u * i;
                for (j = 0; j < 8u; j++, oc += ostride) {
                    const int32_t m = cb[j].m;
                    const unsigned sh = (unsigned)(cb[j].sh - 32);
                    const int32_t v0 = ts_ssat8((int32_t)((bq[j] + (int64_t)sums[j] * m) >> 32) >> sh);
                    const int32_t v1 = ts_ssat8((int32_t)((bq[j] + (int64_t)sums[8u + j] * m) >> 32) >> sh);
                    oc[(size_t)nb * 8u * ostride] = (int8_t)(v0 < lo ? lo : v0);
                    oc[(size_t)nb * 8u * ostride + 1u] = (int8_t)(v1 < lo ? lo : v1);
                }
                continue;
            }
            for (j = 0; j < 8u; j++) {
                const Chan *cc = &cb[j];
                int8_t *oc = out + (size_t)(nb * 8u + j) * ostride + 2u * i;
                for (p = 0; p < 2u && 2u * i + p < n; p++) {
                    const int32_t a = sums[8u * p + j];
                    int32_t v;
                    if (cc->sh >= 32) {
                        v = ts_ssat8((int32_t)((bq[j] + (int64_t)a * cc->m) >> 32) >> (cc->sh - 32));
                    } else {
                        v = requant(a, cc, lo);
                    }
                    oc[p] = (int8_t)(v < lo ? lo : v);
                }
            }
        }
    }
}
#else
#define k1_block(w, ch, lo, cin, cout, in, out, ostride, n, packed) k1_block_c(w, ch, lo, cin, cout, in, out, ostride, n)
#endif
#endif

static const int8_t *weights(const vocr_textspot_layer *L)
{
    return (const int8_t *)(const void *)L->w;
}

/* The 1x1 layers [from, to) on a block in[q * BLK + p]; the last writes n pixels to dst (plane dstride bytes
 * apart); a and b are BLK * maxch scratch blocks. */
static void chain(const Net *t, unsigned from, unsigned to, const int8_t *in, int8_t *a, int8_t *b, int8_t *dst,
                  size_t dstride, unsigned n)
{
    unsigned l;
    for (l = from; l < to; l++) {
        const vocr_textspot_layer *L = &t->m->layers[l];
        int last = l + 1u == to;
        int8_t *o = last ? dst : a;
#if TS_SIMD32
        k1_block(weights(L), t->chan + t->run[l].chan, L->relu ? 0 : -128, L->cin, L->cout, in, o,
                 last ? dstride : BLK, last ? n : BLK, t->m->packed && k1_packable(L));
#else
        k1_block(weights(L), t->chan + t->run[l].chan, L->relu ? 0 : -128, L->cin, L->cout, in, o,
                 last ? dstride : BLK, last ? n : BLK, 0);
#endif
        in = o;
        a = b;
        b = o;
    }
}

static unsigned min_u(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

/* ---------------------------------------------------------------- decode */

static unsigned root_of(Comp *c, unsigned x)
{
    while (c[x].parent != x) {
        c[x].parent = c[c[x].parent].parent;
        x = c[x].parent;
    }
    return x;
}

static void unite(Net *t, unsigned a, unsigned b)   /* b joins a */
{
    Comp *p = &t->comp[a], *q = &t->comp[b];
    unsigned k, nt = t->tags;
    p->n += q->n;
    p->sk += q->sk;
    p->so += q->so;
    p->sa1 += q->sa1;
    p->sa2 += q->sa2;
    p->sr += q->sr;
    p->sc += q->sc;
    if (q->r0 < p->r0) p->r0 = q->r0;
    if (q->r1 > p->r1) p->r1 = q->r1;
    if (q->c0 < p->c0) p->c0 = q->c0;
    if (q->c1 > p->c1) p->c1 = q->c1;
    q->parent = (uint16_t)a;
    for (k = 0; k < nt; k++) t->tsum[(size_t)a * nt + k] += t->tsum[(size_t)b * nt + k];   /* version 4 */
}

static int32_t floor_div(int32_t a, int32_t b)   /* b > 0 */
{
    int32_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static int32_t ceil_div(int32_t a, int32_t b)    /* b > 0 */
{
    int32_t q = a / b;
    return (a % b != 0 && a > 0) ? q + 1 : q;
}

/* The cap's order: the twin's key (-conf, -n, vertical, y, x, h, w, tag), smallest first, then tconf (the twin
 * sorts (key, box) pairs; the box's other fields are the key's). */
static int cap_less(const Cand *a, const Cand *b)
{
    if (a->conf != b->conf) return a->conf > b->conf;
    if (a->u.n != b->u.n) return a->u.n > b->u.n;
    if (a->v != b->v) return a->v < b->v;
    if (a->y != b->y) return a->y < b->y;
    if (a->x != b->x) return a->x < b->x;
    if (a->h != b->h) return a->h < b->h;
    if (a->w != b->w) return a->w < b->w;
    if (a->tag != b->tag) return a->tag < b->tag;
    return a->tconf < b->tconf;
}

/* A candidate into the sorted list; the CANDS smallest by the cap key are kept (a tie with the last one is the
 * same box, so either may go). */
static void keep(Net *t, const Cand *v)
{
    unsigned j;
    if (t->ncand == CANDS) {
        t->ctrunc = 1;
        if (!cap_less(v, &t->cand[CANDS - 1u])) return;
        t->ncand--;
    }
    for (j = t->ncand; j > 0 && cap_less(v, &t->cand[j - 1u]); j--) t->cand[j] = t->cand[j - 1u];
    t->cand[j] = *v;
    t->ncand++;
}

/* Version 4: the class of a component of n cells with tag sums ts[0 .. T) (textspot6_twin._tag_of): class 0
 * scores 0 and class k + 1 scores ts[k]; the largest score wins (the lowest class on a tie); tconf =
 * min(127, (best - second) / n), second the largest score of the other classes. */
static void tag_of(const int32_t *ts, unsigned nt, int32_t n, Cand *v)
{
    int32_t best = 0, second = 0, d;
    unsigned k, cls = 0;
    for (k = 0; k < nt; k++)
        if (ts[k] > best) {
            best = ts[k];
            cls = k + 1u;
        }
    second = cls ? 0 : ts[0];                 /* class 0's score when it is not the best */
    for (k = 0; k < nt; k++)
        if (k + 1u != cls && ts[k] > second) second = ts[k];
    d = (best - second) / n;                  /* best >= second, n > 0: the floor */
    v->tag = (uint8_t)cls;
    v->tconf = (uint8_t)(d > TCONF_MAX ? TCONF_MAX : d);
}

/* A complete component (slot sl): its box (twin: candidates), if it has one. */
static void finish(Net *t, unsigned sl)
{
    const Comp *c = &t->comp[sl];
    int32_t s = (int32_t)t->m->stride, n = c->n, f = (int32_t)t->f, base, near_, far_, across, pad, lo, hi;
    int32_t x0, x1, y0, y1, conf;
    int vertical;
    Cand v;
    if (n < (int32_t)t->m->min_cells) return;
    conf = c->sk / n;                                          /* sum K >= n > 0: the floor */
    if (conf < (int32_t)t->m->conf_min) return;
    vertical = c->so > 0;
    base = 2 * s * (vertical ? c->sc : c->sr) + n * (s - 1);   /* sum of c2 = 2 s i + s - 1 */
    near_ = floor_div(f * (base - c->sa1), 2 * n);
    far_ = ceil_div(f * (base + c->sa2), 2 * n);
    across = far_ - near_;
    if (across < MIN_ACROSS * f) return;
    pad = (across * PAD_PCT + 50) / 100;
    if (pad < PAD_MIN) pad = PAD_MIN;
    if (vertical) {
        lo = f * s * (int32_t)c->r0;
        hi = f * s * ((int32_t)c->r1 + 1);
        x0 = near_ - pad;
        x1 = far_ + pad;
        y0 = lo - pad;
        y1 = hi + pad;
    } else {
        lo = f * s * (int32_t)c->c0;
        hi = f * s * ((int32_t)c->c1 + 1);
        y0 = near_ - pad;
        y1 = far_ + pad;
        x0 = lo - pad;
        x1 = hi + pad;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)t->fw) x1 = (int32_t)t->fw;
    if (y1 > (int32_t)t->fh) y1 = (int32_t)t->fh;
    if (x1 <= x0 || y1 <= y0) return;
    v.x = (uint16_t)x0;
    v.y = (uint16_t)y0;
    v.w = (uint16_t)(x1 - x0);
    v.h = (uint16_t)(y1 - y0);
    v.v = (uint8_t)vertical;
    v.conf = (uint8_t)conf;
    v.tag = v.tconf = 0;
    if (t->tags) tag_of(t->tsum + (size_t)sl * t->tags, t->tags, n, &v);
    v.u.n = (uint32_t)n;
    keep(t, &v);
}

/* Frees every slot but the live roots (stamp); a root no run of the row continued is complete. */
static void sweep(Net *t, unsigned stamp)
{
    unsigned s;
    for (s = 0; s < t->nc; s++) {
        Comp *c = &t->comp[s];
        if (c->parent == NONE) continue;
        if (c->parent == s && c->stamp == stamp) continue;
        if (c->parent == s) finish(t, s);
        c->parent = NONE;
        t->freel[t->nfree++] = (uint16_t)s;
    }
}

/* Map row i (maps: K, O, A1, A2 planes of wm cells, then version 4's T tag planes) into the streaming components. */
static void decode_row(Net *t, const int8_t *maps, unsigned i)
{
    const int8_t *kk = maps, *oo = kk + t->wm, *a1 = oo + t->wm, *a2 = a1 + t->wm;
    Comp *cp = t->comp;
    Seg *prev = t->prev, *cur = t->cur;
    unsigned wm = t->wm, j = 0, k = 0, q, nc = 0, np = t->np, nt = t->tags;
    while (j < wm) {
        unsigned a, root = NONE;
        int32_t n = 0, sk = 0, so = 0, sa1 = 0, sa2 = 0, sc = 0;
        Comp *c;
        if (kk[j] <= 0) {
            j++;
            continue;
        }
        a = j;
        for (; j < wm && kk[j] > 0; j++) {
            sk += kk[j];
            so += oo[j];
            sa1 += a1[j];
            sa2 += a2[j];
            sc += (int32_t)j;
            n++;
        }
        while (k < np && prev[k].b <= a) k++;
        for (q = k; q < np && prev[q].a < j; q++) {   /* the runs above that this one touches */
            unsigned r = root_of(cp, prev[q].comp);
            if (root == NONE) root = r;
            else if (r != root) unite(t, root, r);
        }
        if (root == NONE) {
            root = t->freel[--t->nfree];
            c = &cp[root];
            c->n = c->sk = c->so = c->sa1 = c->sa2 = c->sr = c->sc = 0;
            c->r0 = (uint16_t)i;
            c->c0 = (uint16_t)a;
            c->c1 = (uint16_t)(j - 1u);
            c->parent = (uint16_t)root;
            c->stamp = 0;
            for (q = 0; q < nt; q++) t->tsum[(size_t)root * nt + q] = 0;
        }
        c = &cp[root];
        c->n += n;
        c->sk += sk;
        c->so += so;
        c->sa1 += sa1;
        c->sa2 += sa2;
        c->sr += (int32_t)i * n;
        c->sc += sc;
        c->r1 = (uint16_t)i;
        if (a < c->c0) c->c0 = (uint16_t)a;
        if (j - 1u > c->c1) c->c1 = (uint16_t)(j - 1u);
        for (q = 0; q < nt; q++) {             /* version 4: the run's tag codes, map by map */
            const int8_t *tm = maps + (size_t)(MAPS + q) * wm;
            int32_t st = 0;
            unsigned x;
            for (x = a; x < j; x++) st += tm[x];
            t->tsum[(size_t)root * nt + q] += st;
        }
        cur[nc].a = (uint16_t)a;
        cur[nc].b = (uint16_t)j;
        cur[nc].comp = (uint16_t)root;
        nc++;
    }
    for (q = 0; q < nc; q++) {
        unsigned r = root_of(cp, cur[q].comp);
        cur[q].comp = (uint16_t)r;
        cp[r].stamp = (uint16_t)(i + 1u);
    }
    sweep(t, i + 1u);
    t->prev = cur;
    t->cur = prev;
    t->np = nc;
}

/* ---------------------------------------------------------------- the row pipeline */

static int8_t *ring_row(const Net *t, unsigned l, unsigned y)
{
    const vocr_textspot_layer *L = &t->m->layers[l];
    const Run *r = &t->run[l];
    return (int8_t *)(void *)(t->base + r->ring + (size_t)(y % (2u * L->dil + 1u)) * L->cin * r->plane);
}

/* Where the segment ending before layer `next` writes pixel 0 of channel 0 of its row (channel c at
 * + c * dest_stride): the next 3x3 layer's newest ring row, after its left padding, or the map row. */
static int8_t *dest_row(const Net *t, unsigned next)
{
    if (next < t->m->n_layers) return ring_row(t, next, t->run[next].received) + t->m->layers[next].dil;
    return t->head;
}

static size_t dest_stride(const Net *t, unsigned next)
{
    return next < t->m->n_layers ? t->run[next].plane : t->wn;
}

/* One decode-map plane of map row 2 i + dy from two planes of the head's row i (e: the sub-cell (dy, 0)'s channel,
 * o: (dy, 1)'s): cell J = head cell J / 2 of e for an even J, of o for an odd one, J < wm. */
static void sub_plane(int8_t *d, const int8_t *e, const int8_t *o, unsigned wm)
{
    unsigned j;
    for (j = 0; j + 1u < wm; j += 2u) {
        d[j] = e[j / 2u];
        d[j + 1u] = o[j / 2u];
    }
    if (j < wm) d[j] = e[j / 2u];
}

/* Versions 2 and 4 (a trunk): map row 2 i + dy of the decode from the head's row i (16 + 4 T planes of wn cells)
 * into t->drow: map mp, cell J = channel 4 (2 dy + J % 2) + mp of head cell J / 2 (textspot2_twin.head_to_maps); tag
 * map k, channel 16 + T (2 dy + J % 2) + k (textspot6_twin.head_to_maps); J < wm. */
static void sub_row(const Net *t, unsigned dy)
{
    unsigned mp, k, nt = t->tags;
    for (mp = 0; mp < MAPS; mp++) {
        const int8_t *e = t->head + (size_t)(8u * dy + mp) * t->wn;
        sub_plane(t->drow + (size_t)mp * t->wm, e, e + (size_t)4u * t->wn, t->wm);
    }
    for (k = 0; k < nt; k++) {
        const int8_t *e = t->head + (size_t)(D2S_OUT + 2u * dy * nt + k) * t->wn;
        sub_plane(t->drow + (size_t)(MAPS + k) * t->wm, e, e + (size_t)nt * t->wn, t->wm);
    }
}

#ifndef VOCR_TEXTSPOT6_NO_MS
/* A VTMS network: the stream's output row i (the head row: its c8 planes of wn cells) into F8's row i, cell-major. */
NOINLINE static void to_f8(const Net *t, unsigned i)
{
    unsigned j = 0, c, wn = t->wn, c8 = t->m->map_chans[0];
    size_t cap = t->f8cap;
    const int8_t *h = t->head;
    int8_t *row = t->f8 + (size_t)i * wn * cap;
#if TS_NEON
    for (; j + 8u <= wn; j += 8u)           /* 8 cells x 8 channels at a time: an 8 x 8 byte transpose */
        for (c = 0; c < c8; c += 8u) {
            const int8_t *q = h + (size_t)c * wn + j;
            int8x8x2_t a = vtrn_s8(vld1_s8(q), vld1_s8(q + wn)), b = vtrn_s8(vld1_s8(q + 2u * wn), vld1_s8(q + 3u * wn));
            int8x8x2_t e = vtrn_s8(vld1_s8(q + 4u * wn), vld1_s8(q + 5u * wn));
            int8x8x2_t f = vtrn_s8(vld1_s8(q + 6u * wn), vld1_s8(q + 7u * wn));
            int16x4x2_t ab0 = vtrn_s16(vreinterpret_s16_s8(a.val[0]), vreinterpret_s16_s8(b.val[0]));
            int16x4x2_t ab1 = vtrn_s16(vreinterpret_s16_s8(a.val[1]), vreinterpret_s16_s8(b.val[1]));
            int16x4x2_t ef0 = vtrn_s16(vreinterpret_s16_s8(e.val[0]), vreinterpret_s16_s8(f.val[0]));
            int16x4x2_t ef1 = vtrn_s16(vreinterpret_s16_s8(e.val[1]), vreinterpret_s16_s8(f.val[1]));
            int32x2x2_t r0 = vtrn_s32(vreinterpret_s32_s16(ab0.val[0]), vreinterpret_s32_s16(ef0.val[0]));
            int32x2x2_t r1 = vtrn_s32(vreinterpret_s32_s16(ab1.val[0]), vreinterpret_s32_s16(ef1.val[0]));
            int32x2x2_t r2 = vtrn_s32(vreinterpret_s32_s16(ab0.val[1]), vreinterpret_s32_s16(ef0.val[1]));
            int32x2x2_t r3 = vtrn_s32(vreinterpret_s32_s16(ab1.val[1]), vreinterpret_s32_s16(ef1.val[1]));
            int8_t *o = row + (size_t)j * cap + c;
            vst1_s8(o, vreinterpret_s8_s32(r0.val[0]));
            vst1_s8(o + cap, vreinterpret_s8_s32(r1.val[0]));
            vst1_s8(o + 2u * cap, vreinterpret_s8_s32(r2.val[0]));
            vst1_s8(o + 3u * cap, vreinterpret_s8_s32(r3.val[0]));
            vst1_s8(o + 4u * cap, vreinterpret_s8_s32(r0.val[1]));
            vst1_s8(o + 5u * cap, vreinterpret_s8_s32(r1.val[1]));
            vst1_s8(o + 6u * cap, vreinterpret_s8_s32(r2.val[1]));
            vst1_s8(o + 7u * cap, vreinterpret_s8_s32(r3.val[1]));
        }
#endif
    for (; j < wn; j++)
        for (c = 0; c < c8; c++) row[(size_t)j * cap + c] = h[(size_t)c * wn + j];
}
#endif /* !VOCR_TEXTSPOT6_NO_MS */

/* The row has gone into layer `next` (a ring) or is the head's row i: version 1's map row i, or a d2s head's map
 * rows 2 i and 2 i + 1 (versions 2 and 4's trunk; the second only while it lies inside the cropped maps); a VTMS
 * network's stream row i of F8. */
static void delivered(Net *t, unsigned next, unsigned i)
{
    unsigned dy;
    if (next < t->m->n_layers) {
        t->run[next].received++;
        return;
    }
#ifndef VOCR_TEXTSPOT6_NO_MS
    if (is_ms(t->m)) {
        to_f8(t, i);
        return;
    }
#endif
    if (t->m->outputs == MAPS) {
        if (t->hook) t->hook(t->user, i, (const signed char *)(const void *)t->head, t->wm);
        decode_row(t, t->head, i);
        return;
    }
    for (dy = 0; dy < 2u && 2u * i + dy < t->hm; dy++) {
        sub_row(t, dy);
        if (t->hook) t->hook(t->user, 2u * i + dy, (const signed char *)(const void *)t->drow, t->wm);
        decode_row(t, t->drow, 2u * i + dy);
    }
}

/* Output row next_out of the 3x3 layer l, through the 1x1 layers after it, BLK pixels at a time. */
static void produce(Net *t, unsigned l)
{
    const vocr_textspot_layer *L = &t->m->layers[l];
    Run *r = &t->run[l];
    unsigned s = L->stride, d = L->dil, nx = r->next, i = r->next_out, ky, j0;
    const int8_t *rows[3];
    int valid[3];
    const Chan *ch = t->chan + r->chan;
    int32_t lo = L->relu ? 0 : -128;
    int8_t *dst = dest_row(t, nx), *blk = t->tmp;
    size_t ds = dest_stride(t, nx), bs = (size_t)BLK * t->maxch;
    for (ky = 0; ky < 3u; ky++) {             /* input row i s + (ky - 1) d; the centre one always exists */
        unsigned y = i * s + ky * d;
        valid[ky] = y >= d && y - d < r->h_in;
        rows[ky] = ring_row(t, l, valid[ky] ? y - d : i * s);
    }
    for (j0 = 0; j0 < r->w_out; j0 += BLK) {
        unsigned n = min_u(BLK, r->w_out - j0);
        int8_t *o = nx == l + 1u ? dst + j0 : blk;
        if (s == 1u)
            k3_block_s1(weights(L), ch, lo, L->cin, L->cout, L->kind == 1u, rows, valid, r->plane, d, j0, o,
                        nx == l + 1u ? ds : BLK, nx == l + 1u ? n : BLK);
        else
            k3_block_s2(weights(L), ch, lo, L->cin, L->cout, L->kind == 1u, rows, valid, r->plane, d, j0, o,
                        nx == l + 1u ? ds : BLK, nx == l + 1u ? n : BLK);
        if (nx != l + 1u) chain(t, l + 1u, nx, blk, blk + bs, blk + 2u * bs, dst + j0, ds, n);
    }
    r->next_out++;
    delivered(t, nx, i);
}

static int producible(const Net *t, unsigned l)
{
    const Run *r = &t->run[l];
    unsigned need = (unsigned)r->next_out * t->m->layers[l].stride + t->m->layers[l].dil + 1u;
    if (r->next_out >= r->h_out) return 0;
    return r->received >= min_u(need, r->h_in);
}

/* Computes every row that can be computed, deepest layer first. */
static void drain(Net *t)
{
    int p = 0;
    while (p >= 0) {
        unsigned l = t->rl[p];
        if (producible(t, l)) {
            produce(t, l);
            if ((unsigned)p + 1u < t->nrl) p++;
        } else {
            p--;
        }
    }
}

static unsigned grey_px(const unsigned char *row, unsigned x, unsigned bpp)
{
    const unsigned char *p = row + (size_t)x * bpp;
    if (bpp == VOCR_LINES_GREY) return p[0];
    return (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
}

#if TS_NEON
/* The grey of 8 RGBA pixels, (77 R + 150 G + 29 B) >> 8 (at most 256 * 255: no u16 overflow). */
static uint8x8_t grey8(const unsigned char *p)
{
    uint8x8x4_t a = vld4_u8(p);
    uint16x8_t s = vmull_u8(a.val[0], vdup_n_u8(77));
    s = vmlal_u8(s, a.val[1], vdup_n_u8(150));
    s = vmlal_u8(s, a.val[2], vdup_n_u8(29));
    return vshrn_n_u16(s, 8);
}
#endif

/* The halving's pair sums of a frame row: ps[j] (+)= g[2j] + g[min(2j + 1, W - 1)], j < ceil(W / 2). */
static void pair_sums(uint16_t *ps, const unsigned char *row, unsigned fw, unsigned bpp, int add)
{
    unsigned j = 0, n = fw / 2u;
#if TS_NEON
    if (bpp == VOCR_LINES_GREY) {
        for (; j + 8u <= n; j += 8u) {        /* 16 pixels: 8 even, 8 odd */
            uint8x8x2_t p = vld2_u8(row + 2u * j);
            uint16x8_t s = vaddl_u8(p.val[0], p.val[1]);
            if (add) s = vaddq_u16(s, vld1q_u16(ps + j));
            vst1q_u16(ps + j, s);
        }
    } else {
        for (; j + 8u <= n; j += 8u) {        /* 16 pixels, two grey8 of 8; pairwise sums */
            uint16x8_t s = vcombine_u16(vpaddl_u8(grey8(row + 8u * j)), vpaddl_u8(grey8(row + 8u * j + 32u)));
            if (add) s = vaddq_u16(s, vld1q_u16(ps + j));
            vst1q_u16(ps + j, s);
        }
    }
#endif
    if (bpp == VOCR_LINES_GREY) {
        if (add)
            for (; j < n; j++) ps[j] = (uint16_t)(ps[j] + row[2u * j] + row[2u * j + 1u]);
        else
            for (; j < n; j++) ps[j] = (uint16_t)(row[2u * j] + row[2u * j + 1u]);
    } else {
        for (; j < n; j++) {
            unsigned v = grey_px(row, 2u * j, bpp) + grey_px(row, 2u * j + 1u, bpp);
            ps[j] = (uint16_t)(add ? ps[j] + v : v);
        }
    }
    if (fw & 1u) {                            /* the last column repeats */
        unsigned v = 2u * grey_px(row, fw - 1u, bpp);
        ps[n] = (uint16_t)(add ? ps[n] + v : v);
    }
}

/* n network-input codes: x - 128, x = (ps + 2) >> 2 (sums) or the row's grey (row). Only [0, n) is written: the
 * ring's padding past the row stays zero. */
static void input_codes(int8_t *dst, const uint16_t *ps, const unsigned char *row, unsigned n, unsigned bpp)
{
    unsigned x = 0;
#if TS_NEON
    if (ps) {
        for (; x + 8u <= n; x += 8u)
            vst1_s8(dst + x, vreinterpret_s8_u8(veor_u8(vrshrn_n_u16(vld1q_u16(ps + x), 2), vdup_n_u8(0x80))));
    } else if (bpp == VOCR_LINES_GREY) {
        for (; x + 16u <= n; x += 16u)
            vst1q_s8(dst + x, vreinterpretq_s8_u8(veorq_u8(vld1q_u8(row + x), vdupq_n_u8(0x80))));
    } else {
        for (; x + 8u <= n; x += 8u) vst1_s8(dst + x, vreinterpret_s8_u8(veor_u8(grey8(row + 4u * x), vdup_n_u8(0x80))));
    }
#endif
    if (ps)
        for (; x < n; x++) dst[x] = (int8_t)((int)((ps[x] + 2u) >> 2) - 128);
    else
        for (; x < n; x++) dst[x] = (int8_t)((int)grey_px(row, x, bpp) - 128);
}

/* Frame row y into the network: at f = 1 the row itself; at f = 2 every second row (and the last) completes an
 * input row of 2x2 means. The input row goes into the first 3x3 layer, through the 1x1 layers before it. Out of line
 * (as it is with two callers): with VOCR_TEXTSPOT6_NO_MS, inlined into spot, its locals would add 208 B to the frame
 * the dark pass's call chain also passes through. */
NOINLINE static int feed(Net *t, const Src *s, unsigned y, unsigned bpp)
{
    const unsigned char *row = src_row(s, y);
    unsigned first = t->rl[0], x, p;
    int8_t *dst;
    if (!row) return VOCR_LINES_E_SOURCE;
    if (t->f == 2u) {
        pair_sums(t->psum, row, t->fw, bpp, (int)(y & 1u));
        if (!(y & 1u)) {
            if (y + 1u < t->fh) return VOCR_LINES_OK;   /* the pair's second row comes next */
            pair_sums(t->psum, row, t->fw, bpp, 1);      /* an odd height: the last row repeats */
        }
    }
    dst = dest_row(t, first);
    if (!first) {
        input_codes(dst, t->f == 2u ? t->psum : NULL, row, t->width, bpp);
    } else {
        size_t bs = (size_t)BLK * t->maxch;
        for (x = 0; x < t->width; x += BLK) {
            unsigned n = min_u(BLK, t->width - x);
            for (p = 0; p < BLK; p++) {
                unsigned g = p >= n ? 128u : t->f == 2u ? (t->psum[x + p] + 2u) >> 2 : grey_px(row, x + p, bpp);
                t->tmp[p] = (int8_t)((int)g - 128);
            }
            chain(t, 0, first, t->tmp, t->tmp + bs, t->tmp + 2u * bs, dst + x, t->run[first].plane, n);
        }
    }
    delivered(t, first, y / t->f);
    drain(t);
    return VOCR_LINES_OK;
}

/* The per-layer state and the requantisation constants of one call. */
static void setup(Net *t, const Plan *p)
{
    const vocr_textspot6 *m = t->m;
    unsigned i, c, ci = 0, w = t->width, h = t->height, next = m->n_layers;
    uint32_t roff = (uint32_t)p->rings;
    t->run = (Run *)(void *)(t->base + p->run);
    t->chan = (Chan *)(void *)(t->base + p->chan);
    t->head = (int8_t *)(void *)(t->base + p->head);
    t->drow = (int8_t *)(void *)(t->base + p->drow);
    t->tmp = (int8_t *)(void *)(t->base + p->tmp);
    t->comp = (Comp *)(void *)(t->base + p->comp);
    t->tsum = (int32_t *)(void *)(t->base + p->tsum);
    t->tags = m->tags;
    t->freel = (uint16_t *)(void *)(t->base + p->freel);
    t->prev = (Seg *)(void *)(t->base + p->prev);
    t->cur = (Seg *)(void *)(t->base + p->cur);
    t->psum = (uint16_t *)(void *)(t->base + p->psum);
    t->cand = (Cand *)(void *)(t->base + p->cand);
    t->grp = t->base;
    t->nrl = 0;
    t->maxch = 1;
    for (i = 0; i < m->n_layers; i++) {
        const vocr_textspot_layer *L = &m->layers[i];
        Run *r = &t->run[i];
        r->w_in = (uint16_t)w;
        r->h_in = (uint16_t)h;
        w = (w + L->stride - 1u) / L->stride;
        h = (h + L->stride - 1u) / L->stride;
        r->w_out = (uint16_t)w;
        r->h_out = (uint16_t)h;
        r->received = r->next_out = 0;
        r->chan = (uint16_t)ci;
        for (c = 0; c < L->cout; c++, ci++) {
            Chan *q = &t->chan[ci];
            q->b = rds32(L->b + 4u * c);
            q->m = rds32(L->m + 4u * c);
            q->sh = L->sh[c];
            /* |sum| < 2^24 (576 products of 2^14), so bias + sum fits int32 when |bias| <= 2^31 - 1 - 2^24 */
            q->fast = q->b >= -(INT32_MAX - (1 << 24)) && q->b <= INT32_MAX - (1 << 24);
        }
        r->ring = r->plane = 0;
        if (L->cout > t->maxch) t->maxch = L->cout;
        if (L->k == 3u) {
            size_t bytes;
            r->ring = roff;
            r->plane = (uint32_t)plane_bytes(r->w_in, L);
            bytes = (size_t)(2u * L->dil + 1u) * L->cin * r->plane;
            memset(t->base + roff, 0, bytes);   /* the padding and slack columns stay zero */
            roff += (uint32_t)bytes;
            t->rl[t->nrl++] = i;
        }
    }
    for (i = m->n_layers; i-- > 0;) {
        t->run[i].next = (uint16_t)next;
        if (m->layers[i].k == 3u) next = i;
    }
    t->wn = w;
    t->hn = h;
    if (m->outputs != MAPS) {                 /* a d2s head: the stride-4 maps of the network's input */
        t->wm = (t->width + 3u) / 4u;
        t->hm = (t->height + 3u) / 4u;
    } else {
        t->wm = w;
        t->hm = h;
    }
    t->nc = p->nc;
    for (i = 0; i < p->nc; i++) {
        t->comp[i].parent = NONE;
        t->freel[i] = (uint16_t)(p->nc - 1u - i);
    }
    t->nfree = p->nc;
    t->np = 0;
    t->ncand = 0;
    t->ctrunc = 0;
}

/* ---------------------------------------------------------------- blocks, reading order, pick */

/* The sort keys of textspot6_twin.group_and_order, as field codes: 0 x, 1 y, 2 w, 3 h, 4 -(x + w), 5 vertical,
 * 6 conf, 7 tag, 8 tconf; 9 ends a key. Python's sorts are stable, so each is an insertion sort from the order
 * before it. With every tag 0 (versions 1-3) they are textspot_twin's keys. */
#define K_END 9u
enum { K_FULL, K_HSORT, K_HROW, K_VSORT, K_VROW, K_BSORT, K_BHROW, K_BVROW };
static const unsigned char KEYS[8][9] = {
    {0, 1, 2, 3, 5, 6, 7, 8, K_END},   /* sorted(boxes): (x, y, w, h, vertical, conf, tag, tconf); dark follows
                                          from the box */
    {1, 0, 3, 2, K_END},               /* horizontal members: (y, x, h, w) */
    {0, 1, 3, 2, K_END},               /* a row of horizontal members: (x, y, h, w) */
    {4, 1, 0, 2, 3, K_END},            /* vertical members: (-(x + w), y, x, w, h) */
    {1, 4, 3, 2, K_END},               /* a group of vertical members: (y, -(x + w), h, w) */
    {1, 0, 3, 2, 7, K_END},            /* blocks: (y, x, h, w, tag) */
    {0, 1, 3, 2, 7, K_END},            /* a row of blocks: (x, y, h, w, tag) */
    {4, 1, 3, 2, 7, K_END},            /* a row of vertical blocks: (-(x + w), y, h, w, tag) */
};

static int32_t field(const Cand *c, unsigned f)
{
    switch (f) {
    case 0: return c->x;
    case 1: return c->y;
    case 2: return c->w;
    case 3: return c->h;
    case 4: return -((int32_t)c->x + c->w);
    case 5: return c->v;
    case 6: return c->conf;
    case 7: return c->tag;
    default: return c->tconf;
    }
}

static int key_less(const Cand *a, const Cand *b, unsigned kind)
{
    const unsigned char *k = KEYS[kind];
    for (; *k != K_END; k++) {
        int32_t p = field(a, *k), q = field(b, *k);
        if (p != q) return p < q;
    }
    return 0;
}

/* A stable sort of ix[0 .. n) (indices into c) by the key. */
static void isort(uint8_t *ix, unsigned n, const Cand *c, unsigned kind)
{
    unsigned i, j;
    for (i = 1; i < n; i++) {
        uint8_t v = ix[i];
        for (j = i; j > 0 && key_less(&c[v], &c[ix[j - 1u]], kind); j--) ix[j] = ix[j - 1u];
        ix[j] = v;
    }
}

/* textspot6_twin._joins: one class and textspot_twin._joins (one orientation, overlap along, gap across <= the
 * larger size across, and the larger size across <= 3/2 of the smaller). */
static int joins(const Cand *a, const Cand *b)
{
    int32_t ov, gap, sa, sb, mx, mn;
    if (a->v != b->v || a->tag != b->tag) return 0;
    if (a->v) {
        ov = (int32_t)min_u((unsigned)a->y + a->h, (unsigned)b->y + b->h) - (a->y > b->y ? a->y : b->y);
        gap = (a->x > b->x ? a->x : b->x) - (int32_t)min_u((unsigned)a->x + a->w, (unsigned)b->x + b->w);
        sa = a->w;
        sb = b->w;
    } else {
        ov = (int32_t)min_u((unsigned)a->x + a->w, (unsigned)b->x + b->w) - (a->x > b->x ? a->x : b->x);
        gap = (a->y > b->y ? a->y : b->y) - (int32_t)min_u((unsigned)a->y + a->h, (unsigned)b->y + b->h);
        sa = a->h;
        sb = b->h;
    }
    mx = sa > sb ? sa : sb;
    mn = sa > sb ? sb : sa;
    return ov > 0 && gap <= mx && 2 * mx <= 3 * mn;
}

/* Overlap of [a, a + n) and [b, b + m), at least half the smaller of n and m (the twin's row rule). */
static int same_row(unsigned a, unsigned n, unsigned b, unsigned m)
{
    int32_t ov = (int32_t)min_u(a + n, b + m) - (int32_t)(a > b ? a : b);
    return 2 * ov >= (int32_t)min_u(n, m);
}

/* Gathers ix[0 .. n) (sorted already) into rows (an item joins the row when it overlaps the row's first item by
 * half the smaller size, along y and h or, vertical, x and w) and sorts each row by the key. */
static void rows_of(uint8_t *ix, unsigned n, const Cand *c, int vertical, unsigned kind)
{
    unsigned a = 0, b;
    for (b = 1; b <= n; b++) {
        if (b < n) {
            const Cand *f = &c[ix[a]], *it = &c[ix[b]];
            if (vertical ? same_row(it->x, it->w, f->x, f->w) : same_row(it->y, it->h, f->y, f->h)) continue;
        }
        isort(ix + a, b - a, c, kind);
        a = b;
    }
}

static uint8_t find_root(uint8_t *parent, uint8_t a)
{
    while (parent[a] != a) {
        parent[a] = parent[parent[a]];
        a = parent[a];
    }
    return a;
}

/* The kept candidates into out: blocks, reading order, the pick and presence (dark flags 0;
 * textspot6_twin.group_and_order and spot_maps). */
NOINLINE static void group(const Net *t, VocrTextspot6Set *out)
{
    Group *g = (Group *)(void *)t->grp;
    const Cand *c = t->cand;
    unsigned n = t->ncand, i, j, nb = 0, nm = 0, k, b, nbox = 0, presence = 0;
    uint32_t best = 0;
    unsigned best_y = 0;
    for (i = 0; i < n; i++) {
        g->sorted[i] = (uint8_t)i;
        g->parent[i] = (uint8_t)i;
    }
    isort(g->sorted, n, c, K_FULL);
    for (i = 0; i < n; i++)                   /* union-find: a root is its group's first box */
        for (j = i + 1u; j < n; j++)
            if (joins(&c[g->sorted[i]], &c[g->sorted[j]])) {
                uint8_t ri = find_root(g->parent, (uint8_t)i), rj = find_root(g->parent, (uint8_t)j);
                if (ri != rj) g->parent[ri > rj ? ri : rj] = ri > rj ? rj : ri;
            }
    for (i = 0; i < n; i++) {                 /* blocks in the order of their first box */
        Cand *bk;
        unsigned x1 = 0, y1 = 0, first = nm;
        if (find_root(g->parent, (uint8_t)i) != i) continue;
        bk = &g->blk[nb];
        bk->x = bk->y = 0xFFFFu;
        for (j = i; j < n; j++) {
            const Cand *m;
            if (find_root(g->parent, (uint8_t)j) != i) continue;
            m = &c[g->sorted[j]];
            g->member[nm++] = g->sorted[j];
            if (m->x < bk->x) bk->x = m->x;
            if (m->y < bk->y) bk->y = m->y;
            if ((unsigned)m->x + m->w > x1) x1 = (unsigned)m->x + m->w;
            if ((unsigned)m->y + m->h > y1) y1 = (unsigned)m->y + m->h;
        }
        bk->w = (uint16_t)(x1 - bk->x);
        bk->h = (uint16_t)(y1 - bk->y);
        bk->v = c[g->sorted[i]].v;
        bk->conf = 0;
        bk->tag = c[g->sorted[i]].tag;        /* every box of a block has its first box's class */
        bk->tconf = 0;
        bk->u.n = 0;
        bk->u.b.first = (uint8_t)first;
        bk->u.b.count = (uint8_t)(nm - first);
        /* the block's boxes in reading order: rows top to bottom (left to right), or columns right to left */
        isort(g->member + first, nm - first, c, bk->v ? K_VSORT : K_HSORT);
        rows_of(g->member + first, nm - first, c, bk->v, bk->v ? K_VROW : K_HROW);
        g->order[nb] = (uint8_t)nb;
        nb++;
    }
    /* blocks: rows of blocks top to bottom; a row left to right, or right to left when all its blocks are vertical */
    isort(g->order, nb, g->blk, K_BSORT);
    for (i = 0, j = 1; j <= nb; j++) {
        int all_v = 1;
        if (j < nb) {
            const Cand *f = &g->blk[g->order[i]], *it = &g->blk[g->order[j]];
            if (same_row(it->y, it->h, f->y, f->h)) continue;
        }
        for (k = i; k < j; k++) all_v &= g->blk[g->order[k]].v;
        isort(g->order + i, j - i, g->blk, all_v ? K_BVROW : K_BHROW);
        i = j;
    }
    out->pick = -1;
    for (k = 0; k < nb; k++) {
        const Cand *bk = &g->blk[g->order[k]];
        VocrTextspot6Block *ob = &out->blocks[k];
        unsigned first = bk->u.b.first, count = bk->u.b.count;
        uint32_t score = 0;
        ob->x = bk->x;
        ob->y = bk->y;
        ob->w = bk->w;
        ob->h = bk->h;
        ob->vertical = bk->v;
        ob->first = (unsigned char)nbox;
        ob->count = (unsigned char)count;
        ob->tag = bk->tag;
        for (b = 0; b < count; b++) {
            const Cand *m = &c[g->member[first + b]];
            VocrTextspot6Box *ox = &out->boxes[nbox++];
            ox->x = m->x;
            ox->y = m->y;
            ox->w = m->w;
            ox->h = m->h;
            ox->vertical = m->v;
            ox->conf = m->conf;
            ox->dark = 0;
            ox->block = (unsigned char)k;
            ox->tag = m->tag;
            ox->tconf = m->tconf;
            score += (uint32_t)m->w * m->h;
            if (m->tag == VOCR_TEXTSPOT6_DIALOGUE && m->conf > presence) presence = m->conf;
        }
        if (bk->tag != VOCR_TEXTSPOT6_DIALOGUE) continue;   /* the pick is a dialogue block */
        if (2u * bk->y + bk->h >= t->fh) score *= 2u;       /* the block's centre in the lower half */
        if (out->pick < 0 || score > best || (score == best && bk->y > best_y)) {
            out->pick = (int)k;
            best = score;
            best_y = bk->y;
        }
    }
    out->count = nbox;
    out->block_count = nb;
    out->truncated = t->ctrunc;
    out->presence = presence;
}

/* ---------------------------------------------------------------- versions 3 and 4: the map ops */

#ifndef VOCR_TEXTSPOT6_NO_MS

typedef struct {
    int8_t *base;               /* cell (0, 0) */
    uint32_t rows, cols, cap;   /* cap: bytes per cell */
} MapRef;

typedef struct {
    uint32_t woff, len;         /* the source's first weight column in the op's rows, its channels */
    uint32_t at, cols, cap, u;  /* its map: cell (0, 0)'s arena offset, columns, bytes per cell; its upsampling */
} PwSeg;

static MapRef map_ref(const Net *t, unsigned k)
{
    const vocr_textspot6 *m = t->m;
    MapRef r;
    unsigned s = m->map_slot[k], st = m->map_stride[k];
    r.base = (int8_t *)(void *)(t->stack + t->slot_off[s]);
    r.rows = cdiv(t->height, st);
    r.cols = cdiv(t->width, st);
    r.cap = m->slot_cap[s];
    return r;
}

/* Op k's requantisation constants: par[0, cout) bias, [cout, 2 cout) multiplier, [2 cout, 3 cout) shift; per 4
 * channels the NEON form (rq4v): 0 when every bias is fast and every shift >= 32, 1 when every bias is fast, else 2. */
static void op_params(const vocr_textspot6_op *op, int32_t *par, uint8_t *mode)
{
    unsigned c, q, cout = op->cout;
    for (c = 0; c < cout; c++) {
        par[c] = rds32(op->b + 4u * c);
        par[cout + c] = rds32(op->m + 4u * c);
        par[2u * cout + c] = op->sh[c];
    }
    for (c = 0; c < cout; c += 4u) {
        unsigned fast = 1, big = 1;
        for (q = c; q < c + 4u; q++) {
            if (par[q] < -(INT32_MAX - (1 << 24)) || par[q] > INT32_MAX - (1 << 24)) fast = 0;
            if (par[2u * cout + q] < 32) big = 0;
        }
        mode[c / 4u] = (uint8_t)(fast ? (big ? 0u : 1u) : 2u);
    }
}

/* A 1x1 op's sources, in the order of its weight columns. */
static void pw_segs(const Net *t, const vocr_textspot6_op *op, PwSeg *seg)
{
    unsigned q;
    uint32_t woff = 0;
    for (q = 0; q < op->nsrc; q++) {
        MapRef r = map_ref(t, op->src[q]);
        seg[q].woff = woff;
        seg[q].len = t->m->map_chans[op->src[q]];
        seg[q].at = (uint32_t)((unsigned char *)(void *)r.base - t->base);
        seg[q].cols = r.cols;
        seg[q].cap = r.cap;
        seg[q].u = op->up[q];
        woff += seg[q].len;
    }
}

/* xp[p * ns + s]: the arena offset of source s's cell for cell k0 + p (raster order, cols columns) of the output:
 * source cell (i >> u, j >> u) of output cell (i, j). */
static void pw_offsets(const PwSeg *seg, unsigned ns, uint32_t k0, uint32_t n, uint32_t cols, uint32_t *xp)
{
    uint32_t i = k0 / cols, j = k0 % cols, p;
    unsigned s;
    for (p = 0; p < n; p++) {
        for (s = 0; s < ns; s++) {
            const PwSeg *g = &seg[s];
            xp[p * ns + s] = g->at + ((i >> g->u) * g->cols + (j >> g->u)) * g->cap;
        }
        if (++j == cols) {
            j = 0;
            i++;
        }
    }
}

#if TS_NEON
/* 4 channels' sums (acc) -> their requantised values before the clamp, b / mm / sh their constants, in one of the
 * module comment's three forms (op_params' mode); every form is requant()'s value. */
static inline int32x4_t rq4v(int32x4_t acc, int32x4_t b, int32x4_t mm, int32x4_t sh, unsigned mode)
{
    if (mode == 0u) {                       /* VQDMULH: floor(v m / 2^31); VRSHL by 31 - shift */
        int32x4_t v = vqdmulhq_s32(vaddq_s32(acc, b), mm);
        return vrshlq_s32(v, vsubq_s32(vdupq_n_s32(31), sh));
    } else if (mode == 1u) {                /* (v m + 2^(shift - 1)) >> shift in int64 */
        int32x4_t v = vaddq_s32(acc, b), ns = vnegq_s32(sh);
        int64x2_t lo = vrshlq_s64(vmull_s32(vget_low_s32(v), vget_low_s32(mm)), vmovl_s32(vget_low_s32(ns)));
        int64x2_t hi = vrshlq_s64(vmull_s32(vget_high_s32(v), vget_high_s32(mm)), vmovl_s32(vget_high_s32(ns)));
        return vcombine_s32(vqmovn_s64(lo), vqmovn_s64(hi));
    } else {                                /* bias m + 2^(shift - 1) + sum m, floor-shifted */
        int32x4_t ns = vnegq_s32(sh), s1 = vsubq_s32(sh, vdupq_n_s32(1));
        int64x2_t one = vdupq_n_s64(1);
        int64x2_t lo = vaddq_s64(vmull_s32(vget_low_s32(b), vget_low_s32(mm)), vshlq_s64(one, vmovl_s32(vget_low_s32(s1))));
        int64x2_t hi = vaddq_s64(vmull_s32(vget_high_s32(b), vget_high_s32(mm)),
                                 vshlq_s64(one, vmovl_s32(vget_high_s32(s1))));
        lo = vshlq_s64(vmlal_s32(lo, vget_low_s32(acc), vget_low_s32(mm)), vmovl_s32(vget_low_s32(ns)));
        hi = vshlq_s64(vmlal_s32(hi, vget_high_s32(acc), vget_high_s32(mm)), vmovl_s32(vget_high_s32(ns)));
        return vcombine_s32(vqmovn_s64(lo), vqmovn_s64(hi));
    }
}

/* (sum of a, of b, of c, of d) */
static int32x4_t hsum4(int32x4_t a, int32x4_t b, int32x4_t c, int32x4_t d)
{
    int32x2_t ab = vpadd_s32(vpadd_s32(vget_low_s32(a), vget_high_s32(a)), vpadd_s32(vget_low_s32(b), vget_high_s32(b)));
    int32x2_t cd = vpadd_s32(vpadd_s32(vget_low_s32(c), vget_high_s32(c)), vpadd_s32(vget_low_s32(d), vget_high_s32(d)));
    return vcombine_s32(ab, cd);
}

/* A += the products of 16 weights W and 16 codes X: two per int16 lane (<= 32,512), folded into 4 int32 lanes */
#define PW2(A, W, X) do { \
        int16x8_t t_ = vmull_s8(vget_low_s8(W), vget_low_s8(X)); \
        t_ = vmlal_s8(t_, vget_high_s8(W), vget_high_s8(X)); \
        (A) = vpadalq_s16((A), t_); \
    } while (0)
#define PW1(A, W, X) (A) = vpadalq_s16((A), vmull_s8((W), (X)))

/* Cells are 8-byte aligned (slots and cells are multiples of 8 bytes from the 8-aligned arena): the loads of a
 * cell's channels may say so (VLD1 with :64), where the compiler can be told. */
#if defined(__GNUC__)
#define CELL8(p) ((const int8_t *)__builtin_assume_aligned((const void *)(p), 8))
#else
#define CELL8(p) ((const int8_t *)(const void *)(p))
#endif

/* The 1x1 microkernel (the module comment's "Kernels"): 4 output channels (weight rows W0, W0 + cin, W0 + 2 cin,
 * W0 + 3 cin) for two cells (codes X0, X1) over the LEN channels of a source (a multiple of 8), adding into the
 * 8 int32x4 sums a<channel><cell>: per 16 channels 6 loads (each weight row and each cell once) and 24 multiply-adds,
 * every pointer post-incremented. */
#define PW_SRC(X0, X1, W0, LEN) do { \
        const int8_t *x0_ = (X0), *x1_ = (X1), *w0_ = (W0); \
        const int8_t *w1_ = w0_ + cin, *w2_ = w1_ + cin, *w3_ = w2_ + cin; \
        uint32_t q_; \
        for (q_ = (LEN) >> 4; q_ != 0u; q_--) { \
            int8x16_t xa = vld1q_s8(x0_), xb = vld1q_s8(x1_), wv; \
            x0_ += 16; \
            x1_ += 16; \
            wv = vld1q_s8(w0_); \
            w0_ += 16; \
            PW2(a00, wv, xa); \
            PW2(a01, wv, xb); \
            wv = vld1q_s8(w1_); \
            w1_ += 16; \
            PW2(a10, wv, xa); \
            PW2(a11, wv, xb); \
            wv = vld1q_s8(w2_); \
            w2_ += 16; \
            PW2(a20, wv, xa); \
            PW2(a21, wv, xb); \
            wv = vld1q_s8(w3_); \
            w3_ += 16; \
            PW2(a30, wv, xa); \
            PW2(a31, wv, xb); \
        } \
        if ((LEN) & 8u) {                   /* the source's last 8 channels */ \
            int8x8_t xa = vld1_s8(x0_), xb = vld1_s8(x1_), wv; \
            wv = vld1_s8(w0_); \
            PW1(a00, wv, xa); \
            PW1(a01, wv, xb); \
            wv = vld1_s8(w1_); \
            PW1(a10, wv, xa); \
            PW1(a11, wv, xb); \
            wv = vld1_s8(w2_); \
            PW1(a20, wv, xa); \
            PW1(a21, wv, xb); \
            wv = vld1_s8(w3_); \
            PW1(a30, wv, xa); \
            PW1(a31, wv, xb); \
        } \
    } while (0)

/* The first 16 (cin >= 16) or 8 (cin == 8) channels start the sums: the products' pairs' sums (VPADDL), so the 8
 * sums need no clearing. */
#define PW_FIRST16(X0, X1, W0) do { \
        int8x16_t xa = vld1q_s8(X0), xb = vld1q_s8(X1), wv; \
        const int8_t *w_ = (W0); \
        wv = vld1q_s8(w_); \
        a00 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xa)), vget_high_s8(wv), vget_high_s8(xa))); \
        a01 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xb)), vget_high_s8(wv), vget_high_s8(xb))); \
        wv = vld1q_s8(w_ + cin); \
        a10 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xa)), vget_high_s8(wv), vget_high_s8(xa))); \
        a11 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xb)), vget_high_s8(wv), vget_high_s8(xb))); \
        wv = vld1q_s8(w_ + 2u * cin); \
        a20 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xa)), vget_high_s8(wv), vget_high_s8(xa))); \
        a21 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xb)), vget_high_s8(wv), vget_high_s8(xb))); \
        wv = vld1q_s8(w_ + 3u * cin); \
        a30 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xa)), vget_high_s8(wv), vget_high_s8(xa))); \
        a31 = vpaddlq_s16(vmlal_s8(vmull_s8(vget_low_s8(wv), vget_low_s8(xb)), vget_high_s8(wv), vget_high_s8(xb))); \
    } while (0)
#define PW_FIRST8(X0, X1, W0) do { \
        int8x8_t xa = vld1_s8(X0), xb = vld1_s8(X1), wv; \
        const int8_t *w_ = (W0); \
        wv = vld1_s8(w_); \
        a00 = vpaddlq_s16(vmull_s8(wv, xa)); \
        a01 = vpaddlq_s16(vmull_s8(wv, xb)); \
        wv = vld1_s8(w_ + cin); \
        a10 = vpaddlq_s16(vmull_s8(wv, xa)); \
        a11 = vpaddlq_s16(vmull_s8(wv, xb)); \
        wv = vld1_s8(w_ + 2u * cin); \
        a20 = vpaddlq_s16(vmull_s8(wv, xa)); \
        a21 = vpaddlq_s16(vmull_s8(wv, xb)); \
        wv = vld1_s8(w_ + 3u * cin); \
        a30 = vpaddlq_s16(vmull_s8(wv, xa)); \
        a31 = vpaddlq_s16(vmull_s8(wv, xb)); \
    } while (0)

/* The sums of 4 output channels (weight rows wc, wc + cin, ...) for n cells whose cin codes lie at x, x + xcap, ...,
 * cells in pairs (an odd last cell twice), into sums[4 p .. 4 p + 4). No NEON value lives across the pairs but the
 * sums themselves, so nothing spills to the stack (an extension-register load or store between NEON instructions
 * costs a pipeline switch on the A9). The cells are indexed (no pointer is formed past a block's end after an odd
 * last cell). */
static void pw_sums(const int8_t *x, uint32_t xcap, const int8_t *wc, uint32_t cin, uint32_t n, int32_t *sums)
{
    uint32_t p;
    for (p = 0; p < n; p += 2u) {
        const int8_t *xp = x + (size_t)p * xcap;
        const int8_t *x0 = CELL8(xp), *x1 = CELL8(p + 1u < n ? xp + xcap : xp);
        int32_t *s = sums + 4u * p;
        int32x4_t a00, a01, a10, a11, a20, a21, a30, a31;
        if (cin >= 16u) {
            PW_FIRST16(x0, x1, wc);
            PW_SRC(x0 + 16, x1 + 16, wc + 16, cin - 16u);
        } else {
            PW_FIRST8(x0, x1, wc);
        }
        vst1q_s32(s, hsum4(a00, a10, a20, a30));
        vst1q_s32(s + 4, hsum4(a01, a11, a21, a31));
    }
}

/* The n cells' sums (sums[4 p ..]) of 4 channels (constants at pb, pm, ps) -> their codes at dst + p ds, in the
 * channels' rq4v form (md); the clamp's floor is lo (0 with ReLU, else -128). */
static void pw_store(const int32_t *sums, uint32_t n, const int32_t *pb, const int32_t *pm, const int32_t *ps,
                     unsigned md, int32_t lo, int8_t *dst, uint32_t ds)
{
    int32x4_t b = vld1q_s32(pb), mm = vld1q_s32(pm), sh = vld1q_s32(ps);
    int8x8_t floor8 = vdup_n_s8((int8_t)lo);
    uint32_t p;
    if (md == 0u) {                         /* the usual form: VQDMULH, VRSHL by 31 - shift */
        int32x4_t k = vsubq_s32(vdupq_n_s32(31), sh);
        for (p = 0; p < n; p += 2u) {
            const int32_t *s = sums + 4u * p;
            int8_t *d = dst + (size_t)p * ds;
            int32x4_t v0 = vrshlq_s32(vqdmulhq_s32(vaddq_s32(vld1q_s32(s), b), mm), k);
            int32x4_t v1 = vrshlq_s32(vqdmulhq_s32(vaddq_s32(vld1q_s32(s + 4), b), mm), k);
            int8x8_t v = vmax_s8(vqmovn_s16(vcombine_s16(vqmovn_s32(v0), vqmovn_s32(v1))), floor8);
            vst1_lane_u32((uint32_t *)(void *)d, vreinterpret_u32_s8(v), 0);
            if (p + 1u < n) vst1_lane_u32((uint32_t *)(void *)(d + ds), vreinterpret_u32_s8(v), 1);
        }
        return;
    }
    for (p = 0; p < n; p += 2u) {
        const int32_t *s = sums + 4u * p;
        int8_t *d = dst + (size_t)p * ds;
        int8x8_t v = vqmovn_s16(vcombine_s16(vqmovn_s32(rq4v(vld1q_s32(s), b, mm, sh, md)),
                                             vqmovn_s32(rq4v(vld1q_s32(s + 4), b, mm, sh, md))));
        v = vmax_s8(v, floor8);
        vst1_lane_u32((uint32_t *)(void *)d, vreinterpret_u32_s8(v), 0);
        if (p + 1u < n) vst1_lane_u32((uint32_t *)(void *)(d + ds), vreinterpret_u32_s8(v), 1);
    }
}

/* A 1x1 op on n cells whose cin codes lie at x, x + xcap, ...: cell p's output at dst + p ds; sums 4 (n + 1) int32
 * of scratch. Channels in blocks of 4: their weights are read once per block of cells. */
static void pw_cells(const vocr_textspot6_op *op, const int8_t *x, uint32_t xcap, uint32_t n, const int32_t *par,
                     const uint8_t *mode, int32_t *sums, int8_t *dst, uint32_t ds)
{
    const int8_t *w = (const int8_t *)(const void *)op->w;
    uint32_t cout = op->cout, cin = op->cin, c;
    for (c = 0; c < cout; c += 4u) {
        pw_sums(x, xcap, w + (size_t)c * cin, cin, n, sums);
        pw_store(sums, n, par + c, par + cout + c, par + 2u * cout + c, mode[c / 4u], op->relu ? 0 : -128, dst + c,
                 ds);
    }
}

/* The int32 sums of one cell's 8 channels from the taps whose source lies inside the map (a cell at the left or
 * right border, or every cell of a narrow map): wt the weights by tap, row[ky] the source rows (vr[ky]: inside). */
static void dw_border(const int8_t *const *row, const unsigned *vr, const int8_t *wt, uint32_t cout, uint32_t c,
                      uint32_t j, uint32_t s, uint32_t d, uint32_t cols, uint32_t cap, int32_t *a)
{
    int32x4_t lo = vdupq_n_s32(0), hi = lo;
    uint32_t ky, kx;
    for (ky = 0; ky < 3u; ky++) {
        if (!vr[ky]) continue;
        for (kx = 0; kx < 3u; kx++) {
            uint32_t xx = j * s + kx * d;
            int16x8_t t;
            if (xx < d || xx - d >= cols) continue;
            t = vmull_s8(vld1_s8(wt + (size_t)(3u * ky + kx) * cout + c), vld1_s8(row[ky] + (size_t)(xx - d) * cap + c));
            lo = vaddw_s16(lo, vget_low_s16(t));
            hi = vaddw_s16(hi, vget_high_s16(t));
        }
    }
    vst1q_s32(a, lo);
    vst1q_s32(a + 4, hi);
}

/* Output row i of a depthwise op (cols cells to dst, ds bytes apart) from the map src; wt its weights by tap
 * ([9][cout]); acc a row's sums (8 int32 per cell). Per 8 channels, two passes: the sums of every cell (the cells
 * whose taps lie inside the map with the 9 taps' weights in registers, two products per int16 lane, a source row
 * outside the map read as the centre row with its weights 0), then their requantisation (constants in registers). */
NOINLINE static void dw_row(const vocr_textspot6_op *op, const MapRef *src, uint32_t i, uint32_t cols, const int8_t *wt,
                            const int32_t *par, const uint8_t *mode, int32_t *acc, int8_t *dst, uint32_t ds)
{
    uint32_t s = op->stride, d = op->dil, cout = op->cout, cap = src->cap, c, j, ky, jlo, jend;
    size_t rowb = (size_t)src->cols * cap, step = (size_t)s * cap, st = (size_t)d * cap;
    const int8_t *row[3];
    unsigned vr[3];
    int8x8_t floor8 = vdup_n_s8((int8_t)(op->relu ? 0 : -128));
    for (ky = 0; ky < 3u; ky++) {           /* source row i s + (ky - 1) d; the centre one always exists */
        uint32_t yy = i * s + ky * d;
        vr[ky] = yy >= d && yy - d < src->rows ? 1u : 0u;
        row[ky] = src->base + (vr[ky] ? (size_t)(yy - d) : (size_t)i * s) * rowb;
    }
    jlo = (d + s - 1u) / s;                                              /* j s >= d */
    jend = src->cols > d ? (src->cols - 1u - d) / s + 1u : 0u;           /* j s + d < src->cols */
    if (jend > cols) jend = cols;
    if (jlo > jend) jlo = jend;
    for (c = 0; c < cout; c += 8u) {
        const int8x8_t z = vdup_n_s8(0);
        for (j = 0; j < jlo; j++) dw_border(row, vr, wt, cout, c, j, s, d, src->cols, cap, acc + 8u * j);
        if (jlo < jend) {
            int8x8_t w0 = vr[0] ? vld1_s8(wt + c) : z, w1 = vr[0] ? vld1_s8(wt + cout + c) : z;
            int8x8_t w2 = vr[0] ? vld1_s8(wt + 2u * cout + c) : z, w3 = vr[1] ? vld1_s8(wt + 3u * cout + c) : z;
            int8x8_t w4 = vr[1] ? vld1_s8(wt + 4u * cout + c) : z, w5 = vr[1] ? vld1_s8(wt + 5u * cout + c) : z;
            int8x8_t w6 = vr[2] ? vld1_s8(wt + 6u * cout + c) : z, w7 = vr[2] ? vld1_s8(wt + 7u * cout + c) : z;
            int8x8_t w8 = vr[2] ? vld1_s8(wt + 8u * cout + c) : z;
            size_t x = (size_t)(jlo * s - d) * cap + c;
            const int8_t *r0 = row[0] + x, *r1 = row[1] + x, *r2 = row[2] + x;
            int32_t *a = acc + 8u * jlo;
            for (j = jlo; j < jend; j++, r0 += step, r1 += step, r2 += step, a += 8) {
                int16x8_t t = vmlal_s8(vmull_s8(w0, vld1_s8(r0)), w1, vld1_s8(r0 + st));
                int32x4_t lo = vmovl_s16(vget_low_s16(t)), hi = vmovl_s16(vget_high_s16(t));
                t = vmlal_s8(vmull_s8(w2, vld1_s8(r0 + 2u * st)), w3, vld1_s8(r1));
                lo = vaddw_s16(lo, vget_low_s16(t));
                hi = vaddw_s16(hi, vget_high_s16(t));
                t = vmlal_s8(vmull_s8(w4, vld1_s8(r1 + st)), w5, vld1_s8(r1 + 2u * st));
                lo = vaddw_s16(lo, vget_low_s16(t));
                hi = vaddw_s16(hi, vget_high_s16(t));
                t = vmlal_s8(vmull_s8(w6, vld1_s8(r2)), w7, vld1_s8(r2 + st));
                lo = vaddw_s16(lo, vget_low_s16(t));
                hi = vaddw_s16(hi, vget_high_s16(t));
                t = vmull_s8(w8, vld1_s8(r2 + 2u * st));
                vst1q_s32(a, vaddw_s16(lo, vget_low_s16(t)));
                vst1q_s32(a + 4, vaddw_s16(hi, vget_high_s16(t)));
            }
        }
        for (j = jend > jlo ? jend : jlo; j < cols; j++)
            dw_border(row, vr, wt, cout, c, j, s, d, src->cols, cap, acc + 8u * j);
        {
            int32x4_t b0 = vld1q_s32(par + c), b1 = vld1q_s32(par + c + 4u);
            int32x4_t m0 = vld1q_s32(par + cout + c), m1 = vld1q_s32(par + cout + c + 4u);
            int32x4_t s0 = vld1q_s32(par + 2u * cout + c), s1 = vld1q_s32(par + 2u * cout + c + 4u);
            unsigned md0 = mode[c / 4u], md1 = mode[c / 4u + 1u];
            const int32_t *a = acc;
            int8_t *o = dst + c;
            if (md0 == 0u && md1 == 0u) {   /* the usual form: VQDMULH, VRSHL by 31 - shift */
                int32x4_t k0 = vsubq_s32(vdupq_n_s32(31), s0), k1 = vsubq_s32(vdupq_n_s32(31), s1);
                for (j = 0; j < cols; j++, a += 8, o += ds) {
                    int32x4_t v0 = vrshlq_s32(vqdmulhq_s32(vaddq_s32(vld1q_s32(a), b0), m0), k0);
                    int32x4_t v1 = vrshlq_s32(vqdmulhq_s32(vaddq_s32(vld1q_s32(a + 4), b1), m1), k1);
                    vst1_s8(o, vmax_s8(vqmovn_s16(vcombine_s16(vqmovn_s32(v0), vqmovn_s32(v1))), floor8));
                }
            } else {
                for (j = 0; j < cols; j++, a += 8, o += ds) {
                    int8x8_t v = vqmovn_s16(vcombine_s16(vqmovn_s32(rq4v(vld1q_s32(a), b0, m0, s0, md0)),
                                                         vqmovn_s32(rq4v(vld1q_s32(a + 4), b1, m1, s1, md1))));
                    vst1_s8(o, vmax_s8(v, floor8));
                }
            }
        }
    }
}
#else
/* requant() on separate constants (the twin's floor((acc + bias) * mult + 2^(shift - 1)) / 2^shift), clamped */
static int8_t rq1(int32_t acc, int32_t b, int32_t mult, int32_t sh, int32_t lo)
{
    int64_t r = shr64(((int64_t)acc + b) * mult + ((int64_t)1 << (sh - 1)), (unsigned)sh);
    return (int8_t)(r < lo ? lo : r > 127 ? 127 : r);
}

static void pw_cells(const vocr_textspot6_op *op, const int8_t *x, uint32_t xcap, uint32_t n, const int32_t *par,
                     const uint8_t *mode, int32_t *sums, int8_t *dst, uint32_t ds)
{
    const int8_t *w = (const int8_t *)(const void *)op->w;
    uint32_t cout = op->cout, cin = op->cin, c, p, q;
    int32_t lo = op->relu ? 0 : -128;
    (void)mode;
    (void)sums;
    for (p = 0; p < n; p++) {
        const int8_t *xc = x + (size_t)p * xcap;
        int8_t *o = dst + (size_t)p * ds;
        for (c = 0; c < cout; c++) {
            const int8_t *wc = w + (size_t)c * cin;
            int32_t acc = 0;
            for (q = 0; q < cin; q++) acc += wc[q] * xc[q];
            o[c] = rq1(acc, par[c], par[cout + c], par[2u * cout + c], lo);
        }
    }
}

NOINLINE static void dw_row(const vocr_textspot6_op *op, const MapRef *src, uint32_t i, uint32_t cols, const int8_t *wt,
                            const int32_t *par, const uint8_t *mode, int32_t *accs, int8_t *dst, uint32_t ds)
{
    uint32_t s = op->stride, d = op->dil, cout = op->cout, cap = src->cap, c, j, ky, kx;
    const int8_t *row[3];
    unsigned vr[3];
    int32_t lo = op->relu ? 0 : -128;
    (void)mode;
    (void)accs;
    for (ky = 0; ky < 3u; ky++) {
        uint32_t yy = i * s + ky * d;
        vr[ky] = yy >= d && yy - d < src->rows ? 1u : 0u;
        row[ky] = src->base + (vr[ky] ? (size_t)(yy - d) * src->cols * cap : 0u);
    }
    for (j = 0; j < cols; j++) {
        int8_t *o = dst + (size_t)j * ds;
        for (c = 0; c < cout; c++) {
            int32_t acc = 0;
            for (ky = 0; ky < 3u; ky++) {
                if (!vr[ky]) continue;
                for (kx = 0; kx < 3u; kx++) {
                    uint32_t xx = j * s + kx * d;
                    if (xx < d || xx - d >= src->cols) continue;
                    acc += wt[(size_t)(3u * ky + kx) * cout + c] * row[ky][(size_t)(xx - d) * cap + c];
                }
            }
            o[c] = rq1(acc, par[c], par[cout + c], par[2u * cout + c], lo);
        }
    }
}
#endif

/* n cells of scap bytes at src -> cells of dcap bytes at dst */
static void copy_cells(int8_t *dst, uint32_t dcap, const int8_t *src, uint32_t scap, uint32_t n)
{
    uint32_t p;
    if (dcap == scap) {
        memcpy(dst, src, (size_t)n * scap);
        return;
    }
    for (p = 0; p < n; p++) memcpy(dst + (size_t)p * dcap, src + (size_t)p * scap, scap);
}

/* The n cells' codes of a 1x1 op's sources (xp: their offsets, pw_offsets), concatenated in source order, into blk
 * (cin bytes per cell). */
static void pw_gather(const unsigned char *base, const PwSeg *seg, unsigned ns, const uint32_t *xp, uint32_t n,
                      int8_t *blk, uint32_t cin)
{
    uint32_t p;
    unsigned s;
    for (p = 0; p < n; p++, xp += ns, blk += cin)
        for (s = 0; s < ns; s++) memcpy(blk + seg[s].woff, base + xp[s], seg[s].len);
}

/* A block of n cells of a 1x1 op from cell k0 (cols cells per row of its output) to dst (ds bytes per cell): from
 * its source's slot where its codes lie, or from the gathered block. A gathered block holds every code the block's
 * outputs read, so the outputs may overwrite their own source cells (in place); otherwise an op in place writes
 * through its block of outputs (tmp). */
static void pw_block_run(const Net *t, const vocr_textspot6_op *op, const PwSeg *seg, uint32_t k0, uint32_t n,
                         uint32_t cols, const int32_t *par, const uint8_t *mode, int32_t *sums, uint32_t *xp, int8_t *tmp,
                         int8_t *dst, uint32_t ds)
{
    if (pw_gathers(op)) {
        pw_offsets(seg, op->nsrc, k0, n, cols, xp);
        pw_gather(t->base, seg, op->nsrc, xp, n, tmp, op->cin);
        pw_cells(op, tmp, op->cin, n, par, mode, sums, dst, ds);
    } else {
        const int8_t *x = (const int8_t *)(const void *)(t->base + seg[0].at) + (size_t)k0 * seg[0].cap;
        if (op->inplace) {
            pw_cells(op, x, seg[0].cap, n, par, mode, sums, tmp, op->cout);
            copy_cells(dst, ds, tmp, op->cout, n);
        } else {
            pw_cells(op, x, seg[0].cap, n, par, mode, sums, dst, ds);
        }
    }
}

/* Op k, a 1x1 op, with its scratch at sc: blocks of pw_block cells in raster order. */
NOINLINE static void run_pw(const Net *t, unsigned k, unsigned char *sc)
{
    const vocr_textspot6 *m = t->m;
    const vocr_textspot6_op *op = &m->ops[k];
    OpLay L;
    PwSeg seg[3];
    MapRef o = map_ref(t, k + 1u);
    int32_t *par = (int32_t *)(void *)sc;
    uint8_t *mode;
    uint32_t *xp, cells = o.rows * o.cols, k0;
    int32_t *sums;
    int8_t *tmp;
    op_lay(m, k, t->width, t->height, &L);
    mode = sc + (size_t)L.mode;
    sums = (int32_t *)(void *)(sc + (size_t)L.sums);
    xp = (uint32_t *)(void *)(sc + (size_t)L.xp);
    tmp = (int8_t *)(void *)(sc + (size_t)L.tmp);
    op_params(op, par, mode);
    pw_segs(t, op, seg);
    for (k0 = 0; k0 < cells; k0 += L.pb) {
        uint32_t n = min_u(L.pb, cells - k0);
        pw_block_run(t, op, seg, k0, n, o.cols, par, mode, sums, xp, tmp, o.base + (size_t)k0 * o.cap, o.cap);
    }
}

/* Op k, a depthwise op, with its scratch at sc: row by row (in place: through a ring of d + 1 rows). */
NOINLINE static void run_dw(const Net *t, unsigned k, unsigned char *sc)
{
    const vocr_textspot6 *m = t->m;
    const vocr_textspot6_op *op = &m->ops[k];
    OpLay L;
    MapRef src = map_ref(t, op->src[0]), o = map_ref(t, k + 1u);
    const int8_t *w = (const int8_t *)(const void *)op->w;
    int32_t *par = (int32_t *)(void *)sc;
    uint8_t *mode;
    int8_t *wt, *ring;
    int32_t *acc;
    uint32_t c, tap, i, d = op->dil, cout = op->cout;
    size_t rowb = (size_t)o.cols * o.cap;
    op_lay(m, k, t->width, t->height, &L);
    mode = sc + (size_t)L.mode;
    wt = (int8_t *)(void *)(sc + (size_t)L.wt);
    acc = (int32_t *)(void *)(sc + (size_t)L.acc);
    ring = (int8_t *)(void *)(sc + (size_t)L.ring);
    op_params(op, par, mode);
    for (c = 0; c < cout; c++)
        for (tap = 0; tap < 9u; tap++) wt[(size_t)tap * cout + c] = w[(size_t)c * 9u + tap];
    for (i = 0; i < o.rows; i++) {
        int8_t *dst = op->inplace ? ring + (size_t)(i % (d + 1u)) * rowb : o.base + (size_t)i * rowb;
        dw_row(op, &src, i, o.cols, wt, par, mode, acc, dst, o.cap);
        if (op->inplace && i >= d)          /* no later row reads source row i - d */
            memcpy(o.base + (size_t)(i - d) * rowb, ring + (size_t)((i - d) % (d + 1u)) * rowb, rowb);
    }
    if (op->inplace)
        for (i = o.rows > d ? o.rows - d : 0u; i < o.rows; i++)
            memcpy(o.base + (size_t)i * rowb, ring + (size_t)(i % (d + 1u)) * rowb, rowb);
}

/* One decode-map plane of map row 2 i + dy from a cell-major head row (oc codes per cell): e the sub-cell (dy, 0)'s
 * channel in the first cell, o (dy, 1)'s; cell J = head cell J / 2 of e for an even J, of o for an odd one, J < wm. */
static void sub_plane3(int8_t *d, const int8_t *e, const int8_t *o, unsigned wm, size_t oc)
{
    unsigned j;
    for (j = 0; j + 1u < wm; j += 2u) {
        d[j] = e[(size_t)(j / 2u) * oc];
        d[j + 1u] = o[(size_t)(j / 2u) * oc];
    }
    if (j < wm) d[j] = e[(size_t)(j / 2u) * oc];
}

/* A VTMS network: map row 2 i + dy of the decode from the head's row i, cell-major (16 + 4 T codes per cell) in hwc:
 * map mp, cell J = channel 4 (2 dy + J % 2) + mp of head cell J / 2 (textspot2_twin.head_to_maps); tag map k,
 * channel 16 + T (2 dy + J % 2) + k (textspot6_twin.head_to_maps); J < wm. */
static void sub_row3(const Net *t, const int8_t *hwc, unsigned dy)
{
    unsigned mp, k, nt = t->tags;
    size_t oc = t->m->outputs;
    for (mp = 0; mp < MAPS; mp++) {
        const int8_t *e = hwc + 8u * dy + mp;
        sub_plane3(t->drow + (size_t)mp * t->wm, e, e + 4u, t->wm, oc);
    }
    for (k = 0; k < nt; k++) {
        const int8_t *e = hwc + D2S_OUT + 2u * dy * nt + k;
        sub_plane3(t->drow + (size_t)(MAPS + k) * t->wm, e, e + nt, t->wm, oc);
    }
}

/* The head (the last op) row by row into the decode, with its scratch and the decode's state at sc. */
NOINLINE static void run_head(Net *t, unsigned char *sc)
{
    const vocr_textspot6 *m = t->m;
    const vocr_textspot6_op *op = &m->ops[m->n_ops - 1u];
    HeadLay L;
    PwSeg seg[3];
    int32_t *par = (int32_t *)(void *)sc;
    uint8_t *mode;
    uint32_t *xp, i, j0, s, dy;
    int32_t *sums;
    int8_t *hwc, *gat;
    head_lay(m, t->width, &L);
    mode = sc + (size_t)L.mode;
    xp = (uint32_t *)(void *)(sc + (size_t)L.xp);
    sums = (int32_t *)(void *)(sc + (size_t)L.sums);
    gat = (int8_t *)(void *)(sc + (size_t)L.gat);
    hwc = (int8_t *)(void *)(sc + (size_t)L.hwc);
    t->drow = (int8_t *)(void *)(sc + (size_t)L.drow);
    t->comp = (Comp *)(void *)(sc + (size_t)L.comp);
    t->tsum = (int32_t *)(void *)(sc + (size_t)L.tsum);
    t->tags = m->tags;
    t->freel = (uint16_t *)(void *)(sc + (size_t)L.freel);
    t->prev = (Seg *)(void *)(sc + (size_t)L.prev);
    t->cur = (Seg *)(void *)(sc + (size_t)L.cur);
    t->wn = L.wn;
    t->hn = cdiv(t->height, D2S_STRIDE);
    t->wm = L.wm;
    t->hm = cdiv(t->height, D2S_STRIDE / 2u);
    t->nc = L.nc;
    for (s = 0; s < L.nc; s++) {
        t->comp[s].parent = NONE;
        t->freel[s] = (uint16_t)(L.nc - 1u - s);
    }
    t->nfree = L.nc;
    t->np = 0;
    op_params(op, par, mode);
    pw_segs(t, op, seg);
    for (i = 0; i < t->hn; i++) {
        for (j0 = 0; j0 < t->wn; j0 += L.pb) {
            uint32_t n = min_u(L.pb, t->wn - j0);
            pw_block_run(t, op, seg, i * t->wn + j0, n, t->wn, par, mode, sums, xp, gat,
                         hwc + (size_t)j0 * m->outputs, m->outputs);
        }
        for (dy = 0; dy < 2u && 2u * i + dy < t->hm; dy++) {   /* two map rows, the second inside the maps */
            sub_row3(t, hwc, dy);
            if (t->hook) t->hook(t->user, 2u * i + dy, (const signed char *)(const void *)t->drow, t->wm);
            decode_row(t, t->drow, 2u * i + dy);
        }
    }
}

/* The stream's per-layer state and requantisation constants (setup's, version 1's rings and blocks) at sc. */
NOINLINE static void setup3(Net *t, unsigned char *sc, const StreamLay *L)
{
    const vocr_textspot6 *m = t->m;
    unsigned i, c, ci = 0, w = t->width, h = t->height, next = m->n_layers;
    uint32_t roff = (uint32_t)(sc - t->base) + (uint32_t)L->rings;
    t->run = (Run *)(void *)(sc + (size_t)L->run);
    t->chan = (Chan *)(void *)(sc + (size_t)L->chan);
    t->tmp = (int8_t *)(void *)(sc + (size_t)L->tmp);
    t->head = (int8_t *)(void *)(sc + (size_t)L->head);
    t->psum = (uint16_t *)(void *)(sc + (size_t)L->psum);
    t->drow = NULL;
    t->comp = NULL;
    t->tsum = NULL;
    t->tags = m->tags;
    t->freel = NULL;
    t->prev = t->cur = NULL;
    t->nrl = 0;
    t->maxch = 1;
    for (i = 0; i < m->n_layers; i++) {
        const vocr_textspot_layer *Ly = &m->layers[i];
        Run *r = &t->run[i];
        r->w_in = (uint16_t)w;
        r->h_in = (uint16_t)h;
        w = (w + Ly->stride - 1u) / Ly->stride;
        h = (h + Ly->stride - 1u) / Ly->stride;
        r->w_out = (uint16_t)w;
        r->h_out = (uint16_t)h;
        r->received = r->next_out = 0;
        r->chan = (uint16_t)ci;
        for (c = 0; c < Ly->cout; c++, ci++) {
            Chan *q = &t->chan[ci];
            q->b = rds32(Ly->b + 4u * c);
            q->m = rds32(Ly->m + 4u * c);
            q->sh = Ly->sh[c];
            q->fast = q->b >= -(INT32_MAX - (1 << 24)) && q->b <= INT32_MAX - (1 << 24);
        }
        r->ring = r->plane = 0;
        if (Ly->cout > t->maxch) t->maxch = Ly->cout;
        if (Ly->k == 3u) {
            size_t bytes;
            r->ring = roff;
            r->plane = (uint32_t)plane_bytes(r->w_in, Ly);
            bytes = (size_t)(2u * Ly->dil + 1u) * Ly->cin * r->plane;
            memset(t->base + roff, 0, bytes);
            roff += (uint32_t)bytes;
            t->rl[t->nrl++] = i;
        }
    }
    for (i = m->n_layers; i-- > 0;) {
        t->run[i].next = (uint16_t)next;
        if (m->layers[i].k == 3u) next = i;
    }
    t->wn = w;
    t->hn = h;
    t->wm = (t->width + 3u) / 4u;
    t->hm = (t->height + 3u) / 4u;
    t->ncand = 0;
    t->ctrunc = 0;
    t->np = 0;
}

/* A VTMS network (version 3, version 4's multi-scale form), up to the decoded candidates: the stream into F8, the map
 * ops, the head into the decode. */
NOINLINE static int spot3(Net *t, const Src *s, unsigned bpp, unsigned force, void *arena, size_t arena_bytes)
{
    const vocr_textspot6 *m = t->m;
    Plan3 p;
    Stack st;
    StreamLay sl;
    unsigned y, k;
    int rc;
    plan3(m, t->fw, t->fh, force, &p);
    if (p.over || arena_bytes < (size_t)p.total + 7u) return VOCR_LINES_E_ARENA;
    t->base = (unsigned char *)arena + ((8u - (unsigned)((uintptr_t)arena & 7u)) & 7u);
    t->f = p.f;
    t->width = p.win;
    t->height = p.hin;
    t->cand = (Cand *)(void *)t->base;
    t->slot_off = (uint32_t *)(void *)(t->base + (size_t)p.slots);
    t->stack = t->base + (size_t)p.stack;
    t->grp = t->stack;
    stk_init(&st);
    stk_push(&st, 0, slot_bytes(m, 0, p.win, p.hin), t->slot_off);
    t->f8 = (int8_t *)(void *)(t->stack + t->slot_off[0]);
    t->f8cap = m->slot_cap[0];
    stream_lay(m, p.win, p.f, &sl);
    setup3(t, t->stack + (size_t)st.top, &sl);
    for (y = 0; y < t->fh; y++) {
        rc = feed(t, s, y, bpp);
        if (rc) return rc;
    }
    for (k = 0; k + 1u < m->n_ops; k++) {
        unsigned char *sc = t->stack + (size_t)op_begin(&st, m, k, p.win, p.hin, t->slot_off);
        if (m->ops[k].kind == OP_PW)
            run_pw(t, k, sc);
        else
            run_dw(t, k, sc);
        op_end(&st, m, k);
    }
    run_head(t, t->stack + (size_t)st.top);
    return VOCR_LINES_OK;
}
#endif /* !VOCR_TEXTSPOT6_NO_MS */

/* ---------------------------------------------------------------- the calls */

static void clear(VocrTextspot6Set *out)
{
    out->count = out->block_count = out->truncated = out->presence = 0;
    out->pick = -1;
}

static int spot(const vocr_textspot6 *m, const Src *s, unsigned fw, unsigned fh, unsigned bpp, unsigned flags,
                void *arena, size_t arena_bytes, VocrTextspot6Set *out, vocr_textspot_map_row hook, void *user)
{
    Plan p;
    Net t;
    unsigned y, k, force = forced(flags);
    int rc;
    clear(out);
    if (!loaded(m) || !side_ok(fw, fh) || (bpp != VOCR_LINES_GREY && bpp != VOCR_LINES_RGBA) || !arena ||
        (flags & ~VOCR_TEXTSPOT6_FLAGS) || force == 3u)
        return VOCR_LINES_E_ARG;
#ifndef VOCR_TEXTSPOT6_NO_MS
    if (is_ms(m)) {
        t.m = m;
        t.fw = fw;
        t.fh = fh;
        t.hook = hook;
        t.user = user;
        rc = spot3(&t, s, bpp, force, arena, arena_bytes);
        if (rc) return rc;
    } else
#endif
    {
        plan(m, fw, fh, force, &p);
        if (arena_bytes < p.total + 7u) return VOCR_LINES_E_ARENA;
        t.m = m;
        t.base = (unsigned char *)arena + ((8u - (unsigned)((uintptr_t)arena & 7u)) & 7u);
        t.f = p.f;
        t.fw = fw;
        t.fh = fh;
        t.width = p.win;
        t.height = p.f == 2u ? (fh + 1u) / 2u : fh;
        t.hook = hook;
        t.user = user;
        setup(&t, &p);
        for (y = 0; y < fh; y++) {
            rc = feed(&t, s, y, bpp);
            if (rc) return rc;
        }
    }
    sweep(&t, NONE);   /* the components still open after the last map row */
    group(&t, out);
    if (flags & VOCR_TEXTSPOT_NO_DARK) return VOCR_LINES_OK;
    for (k = 0; k < out->count; k++) {
        VocrTextspot6Box *b = &out->boxes[k];
        int d = s->im ? vocr_line_dark(s->im, b->x, b->y, b->w, b->h)
                      : vocr_line_dark_rows(s->rows, b->x, b->y, b->w, b->h);
        if (d < 0) {
            clear(out);
            return d;
        }
        b->dark = (unsigned char)d;
    }
    return VOCR_LINES_OK;
}

static int image_ok(const VocrLinesImage *im)
{
    return im && im->pixels && (im->bpp == VOCR_LINES_GREY || im->bpp == VOCR_LINES_RGBA) &&
           im->stride >= (size_t)im->width * im->bpp;
}

int vocr_textspot6_find(const vocr_textspot6 *m, const VocrLinesImage *im, unsigned flags, void *arena,
                        size_t arena_bytes, VocrTextspot6Set *out)
{
    Src s;
    if (!out) return VOCR_LINES_E_ARG;
    if (!image_ok(im)) {
        clear(out);
        return VOCR_LINES_E_ARG;
    }
    s.im = im;
    s.rows = NULL;
    return spot(m, &s, im->width, im->height, im->bpp, flags, arena, arena_bytes, out, NULL, NULL);
}

int vocr_textspot6_find_rows(const vocr_textspot6 *m, const VocrLinesRows *src, unsigned flags, void *arena,
                             size_t arena_bytes, VocrTextspot6Set *out)
{
    Src s;
    if (!out) return VOCR_LINES_E_ARG;
    if (!src || !src->row) {
        clear(out);
        return VOCR_LINES_E_ARG;
    }
    s.im = NULL;
    s.rows = src;
    return spot(m, &s, src->width, src->height, src->bpp, flags, arena, arena_bytes, out, NULL, NULL);
}

int vocr_textspot6_maps(const vocr_textspot6 *m, const VocrLinesImage *im, unsigned flags, void *arena,
                        size_t arena_bytes, vocr_textspot_map_row row, void *user)
{
    Src s;
    VocrTextspot6Set out;
    if (!image_ok(im) || !row) return VOCR_LINES_E_ARG;
    s.im = im;
    s.rows = NULL;
    return spot(m, &s, im->width, im->height, im->bpp, flags | VOCR_TEXTSPOT_NO_DARK, arena, arena_bytes, &out, row,
                user);
}
