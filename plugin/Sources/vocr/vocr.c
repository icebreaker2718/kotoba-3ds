/* vocr.c - see vocr.h. C99; libc use is limited to memcpy, memset and math.h. */
#include "vocr.h"

#include <math.h>
#include <string.h>

#if defined(__ARM_NEON) && !defined(VOCR_NO_NEON)
#include <arm_neon.h>
#define VOCR_NEON 1
#endif
/* PSP: the float matrix products on the VFPU, opt-in with -DVOCR_PSP_VFPU (psp/Makefile
 * VFPU=1). It does the scalar code's multiplies and adds in the same order, 4 lanes at a time,
 * which gives the same bits in PPSSPP; the real VFPU flushes denormals to zero, so a console
 * run has to confirm it (psp/README.md). The thread that runs vocr needs PSP_THREAD_ATTR_VFPU. */
#if defined(VOCR_PSP_VFPU) && defined(__psp__)
#define VOCR_VFPU 1
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

/* Phase markers for the per-layer cycle profile (vita/a9_layers.py): built with
 * -DVOCR_PROF=name, vocr_run() calls name(id) as each phase starts: id = the op's index for its
 * step (the head's for run_head; the attention op's for its projections), VOCR_PROF_SRC for the input columns,
 * VOCR_PROF_GATE for the head gate's per-tile step, VOCR_PROF_ATTN for the attention's scores, softmax and
 * weighted sums, VOCR_PROF_RUN for the rest. Without VOCR_PROF they compile to nothing, so
 * the console build has none. */
#ifdef VOCR_PROF
void VOCR_PROF(unsigned id);
#define VOCR_MARK(id) VOCR_PROF(id)
#else
#define VOCR_MARK(id) ((void)0)
#endif
enum { VOCR_PROF_RUN = 100, VOCR_PROF_SRC = 101, VOCR_PROF_GATE = 102, VOCR_PROF_ATTN = 103 };

enum { T_CONV = 1, T_DW = 2, T_PW = 3, T_HEAD = 4, T_ATTN = 5 };
/* VOCR_PREFETCH(p): a cache hint for the line at p (PLD on ARM), which never changes a result:
 * the int8 weights stream through the L1 once per tile (a layer's matrix is larger than the
 * L1, or evicted by the layers between), so the matrix kernel asks for them two passes ahead. */
#if defined(__GNUC__) && !defined(VOCR_NO_PREFETCH)
#define VOCR_PREFETCH(p) __builtin_prefetch(p)
#else
#define VOCR_PREFETCH(p) ((void)(p))
#endif
/* The NEON kernels stay out of step_op: inlined, their loop state spills to the stack (gcc 14/15).
 * Other builds (the PSP's, the scalar ones) inline them as before. */
#if defined(__GNUC__) && defined(VOCR_NEON)
#define VOCR_NOINLINE __attribute__((noinline))
#else
#define VOCR_NOINLINE
#endif
enum { F_RELU = 1, F_RES = 2, F_BIAS = 4, F_INT8 = 8, F_COLLAPSE = 16 };
enum { DST_RING = 1, DST_TRANS = 2, DST_ACC = 3, DST_LINE = 4 };
#define HEADER_BYTES 64u
#define OP_BYTES 64u
#define MIN_WIDTH 8u
/* Kotoba3DS addition: ARMv6 SIMD (the 3DS's ARM11 has no NEON). vocr_pack_simd32() reorders
 * each int8 weight block so that one 32-bit load holds two outputs' weights for two k, and
 * SMLAD does two multiply-accumulates at once. Integer sums are exact, so the text is
 * bit-identical to the scalar path. VOCR_SIMD32_EMULATE builds the same path with C
 * stand-ins for the instructions, to test it on a PC. */
#if !defined(VOCR_NEON) && !defined(VOCR_NO_SIMD32) && \
    (defined(__ARM_FEATURE_SIMD32) || defined(VOCR_SIMD32_EMULATE))
#define VOCR_SIMD32 1
#if defined(__ARM_FEATURE_SIMD32) && !defined(VOCR_SIMD32_EMULATE)
#include <arm_acle.h>
#define vocr_sxtb16(x) __sxtb16(x)
#define vocr_ror8(x) (((uint32_t)(x) >> 8) | ((uint32_t)(x) << 24))
#define vocr_smlad(a, b, acc) __smlad((a), (b), (acc))
#else
static uint32_t vocr_sxtb16(uint32_t x)
{
    return (uint32_t)(uint16_t)(int16_t)(int8_t)(x & 0xFF) | ((uint32_t)(uint16_t)(int16_t)(int8_t)((x >> 16) & 0xFF) << 16);
}
static uint32_t vocr_ror8(uint32_t x) { return (x >> 8) | (x << 24); }
static int32_t vocr_smlad(uint32_t a, uint32_t b, int32_t acc)
{
    int32_t p0 = (int32_t)(int16_t)(a & 0xFFFF) * (int32_t)(int16_t)(b & 0xFFFF);
    int32_t p1 = (int32_t)(int16_t)(a >> 16) * (int32_t)(int16_t)(b >> 16);
    return (int32_t)((uint32_t)acc + (uint32_t)p0 + (uint32_t)p1);
}
#endif
#endif

int vocr_use_pairs = 1;

#define W8I_CHUNK 512u /* int32 sums of at most 512 int8 x int16 products: 512 * 127 * 32767 < 2^31 */
#ifdef VOCR_NEON
#define VOCR_HEAD_CHUNK 32u /* head frames per mm_i16 call on the NEON W8I path (1 KB of stack) */
/* run_head keeps its lanes (int32 keys, uint32 blocks) in the float accumulator region once
 * quant16 has read it. may_alias keeps those accesses ordered against the region's float ones,
 * should quant16 or step_op ever be inlined into run_head. */
#if defined(__GNUC__)
typedef int32_t vocr_lane_i32 __attribute__((may_alias));
typedef uint32_t vocr_lane_u32 __attribute__((may_alias));
#else
typedef int32_t vocr_lane_i32;
typedef uint32_t vocr_lane_u32;
#endif
#endif

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static size_t al16(size_t n) { return (n + 15u) & ~(size_t)15u; }
static int is_magic(const unsigned char *b)
{
    return b[0] == 'V' && b[1] == 'O' && b[2] == 'C' && b[3] == 'R';
}
/* the head gate section (vocr.h, train/headgate.py): 64-byte header, then its arrays */
#define GATE_HEADER_BYTES 64u
static int is_gate_magic(const unsigned char *b)
{
    return b[0] == 'V' && b[1] == 'O' && b[2] == 'C' && b[3] == 'G';
}
static unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }

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

#define TWO_HEADER_BYTES 128u
static uint32_t find_trailer(const unsigned char *b, size_t bytes, uint32_t total, const char *magic);

int vocr_check_crc(const void *blob, size_t bytes)
{
    const unsigned char *b = (const unsigned char *)blob;
    uint32_t total, t2;
    size_t g_off;
    if (!b || bytes < HEADER_BYTES || !is_magic(b)) return VOCR_E_FORMAT;
    total = rd32(b + 8);
    if (total < HEADER_BYTES || total > bytes) return VOCR_E_FORMAT;
    if (crc32_ieee(b + HEADER_BYTES, total - HEADER_BYTES) != rd32(b + 40)) return VOCR_E_FORMAT;
    g_off = al16(total); /* a gate section after the blob: its own CRC-32 too */
    if (g_off <= bytes && bytes - g_off >= 4 && is_gate_magic(b + g_off)) {
        const unsigned char *g = b + g_off;
        uint32_t n;
        if (bytes - g_off < GATE_HEADER_BYTES) return VOCR_E_FORMAT;
        n = rd32(g + 8);
        if (n < GATE_HEADER_BYTES || n > bytes - g_off ||
            crc32_ieee(g + GATE_HEADER_BYTES, n - GATE_HEADER_BYTES) != rd32(g + 52))
            return VOCR_E_FORMAT;
    }
    t2 = find_trailer(b, bytes, total, "VOC2");
    if (t2) {
        const uint32_t n = rd32(b + t2 + 8);
        if (n < TWO_HEADER_BYTES || n > bytes - t2 || crc32_ieee(b + t2 + 16, n - 16) != rd32(b + t2 + 12))
            return VOCR_E_FORMAT;
    }
    return VOCR_OK;
}

/* Offset of the trailer with this 4-byte magic after the blob, or 0. Trailers sit one after another from
 * al16(total), each starting with its magic and, at +8, its u32 size (a multiple of 16); a trailer of another
 * kind is skipped by its size. */
static uint32_t find_trailer(const unsigned char *b, size_t bytes, uint32_t total, const char *magic)
{
    size_t off = al16(total);
    while (off + 16u <= bytes) {
        const uint32_t n = rd32(b + off + 8);
        if (!memcmp(b + off, magic, 4)) return (uint32_t)off;
        if (n < 16u || (n & 15u) || n > bytes - off) break;
        off += n;
    }
    return 0;
}

/* [off, off + n) inside a trailer of `size` bytes, off 16-byte aligned (and the pointer then too) */
static int in_trailer(uint32_t size, uint32_t off, size_t n)
{
    return off >= TWO_HEADER_BYTES && !(off & 15u) && off <= size && n <= size - off;
}

/* The 'VOC2' trailer: the two-level head (vocr.h vocr_2level, train/head2level.py write_trailer).
 *   0 "VOC2"  4 u16 version (1)  6 u16 header bytes (128)  8 u32 trailer bytes  12 u32 CRC-32 of [16, end)
 *  16 u32 base blob bytes  20 u32 base blob CRC  24 u16 l1_blocks  26 u16 hidden  28 u16 rare_k (0)  30 u16 0
 *  32 f32 t  36 f32 e_b2  40.. u32 offsets from the trailer's start: e_q e_scale e_bias e_wl1 e_wb0 e_w2
 *  (40-60), zeros to 128. Level 2 is the head's own rows: rare_k and the offsets at 64-84 are the trial's
 *  48-wide rare block (docs/head2level.md, rejected), which this runtime does not read, so they must be 0. */
static int parse_two(vocr_model *m, const unsigned char *b, size_t bytes, uint32_t total)
{
    vocr_2level *tw = &m->two;
    const vocr_op *hd = &m->op[m->n_ops - 1];
    const uint32_t t2 = find_trailer(b, bytes, total, "VOC2");
    const unsigned char *p;
    uint32_t size, o[6];
    unsigned i, K = hd->k, NB = hd->cout_pad / 8u;
    if (!t2) return VOCR_OK;
    p = b + t2;
    size = rd32(p + 8);
    if (rd16(p + 4) != 1 || rd16(p + 6) != TWO_HEADER_BYTES || size < TWO_HEADER_BYTES || (size & 15u) ||
        size > bytes - t2 || rd32(p + 16) != total || rd32(p + 20) != rd32(b + 40) || rd16(p + 30))
        return VOCR_E_FORMAT;
    for (i = 64; i < TWO_HEADER_BYTES; i++) /* the rare block's offsets (64-84) and the reserved bytes */
        if (p[i]) return VOCR_E_FORMAT;
    tw->l1_blocks = rd16(p + 24);
    tw->hidden = rd16(p + 26);
    memcpy(&tw->t, p + 32, 4);
    memcpy(&tw->e_b2, p + 36, 4);
    for (i = 0; i < 6; i++) o[i] = rd32(p + 40 + 4 * i);
    if (!m->int8 || rd16(p + 28) || tw->l1_blocks < 1 || tw->l1_blocks >= NB || tw->hidden < 8 ||
        tw->hidden > 64 || (tw->hidden & 7u) || !(tw->t == tw->t) || !(tw->e_b2 == tw->e_b2))
        return VOCR_E_FORMAT;
    if (!in_trailer(size, o[0], (size_t)tw->hidden * K))
        return VOCR_E_FORMAT;
    for (i = 1; i < 6; i++)
        if (!in_trailer(size, o[i], 4u * tw->hidden)) return VOCR_E_FORMAT;
    tw->e_q = (const int8_t *)(const void *)(p + o[0]);
    tw->e_scale = (const float *)(const void *)(p + o[1]);
    tw->e_bias = (const float *)(const void *)(p + o[2]);
    tw->e_wl1 = (const float *)(const void *)(p + o[3]);
    tw->e_wb0 = (const float *)(const void *)(p + o[4]);
    tw->e_w2 = (const float *)(const void *)(p + o[5]);
    return VOCR_OK;
}

/* [off, off + n) lies inside the blob and off is 16-byte aligned */
static int in_blob(uint32_t total, uint32_t off, uint32_t n)
{
    return off >= HEADER_BYTES && !(off & 15u) && off <= total && n <= total - off;
}

static int parse_op(vocr_model *m, unsigned i, const unsigned char *r, unsigned *rows,
                    unsigned *chans, uint32_t total)
{
    vocr_op *op = &m->op[i];
    const unsigned char *b = m->blob;
    uint32_t w_off = rd32(r + 16), w_bytes = rd32(r + 20), b_off = rd32(r + 24),
             s_off = rd32(r + 28);
    unsigned j, last = i + 1 == m->n_ops;
    op->type = r[0];
    op->flags = r[1];
    op->kh = r[2];
    op->kw = r[3];
    op->sh = r[4];
    op->sw = r[5];
    op->ph = r[6];
    op->pw = r[7];
    op->cin = (uint16_t)rd16(r + 8);
    op->cout = (uint16_t)rd16(r + 10);
    op->h_in = (uint16_t)rd16(r + 12);
    op->h_out = (uint16_t)rd16(r + 14);
    op->k = (uint16_t)rd16(r + 32);
    op->cout_pad = (uint16_t)rd16(r + 34);
    for (j = op->type == T_ATTN ? 56u : 36u; j < OP_BYTES; j++) /* ATTN: its O and heads at 36-55 */
        if (r[j]) return VOCR_E_FORMAT;
    if (op->type < T_CONV || op->type > T_ATTN || (op->flags & ~31u)) return VOCR_E_FORMAT;
    if ((op->type == T_HEAD) != (int)last) return VOCR_E_FORMAT;
    /* width strides up to 3: a stride-12 model (train/model.py stage_strides) downsamples once by 3 */
    if (op->kh < 1 || op->kh > 7 || op->kw < 1 || op->kw > 7 || op->sh < 1 || op->sh > 2 ||
        op->sw < 1 || op->sw > 3 || op->ph >= op->kh || op->pw >= op->kw)
        return VOCR_E_FORMAT;
    /* every input column must fall in some window (kw >= sw): plan()'s column ring carries
     * lag * sw + pw - (the previous op's lag) columns from step to step, which is at least kw - sw,
     * so a kernel narrower than its stride could make it negative and the ring overflow */
    if (op->kw < op->sw) return VOCR_E_FORMAT;
    if (op->h_in != *rows || op->h_in + 2u * op->ph < op->kh ||
        op->h_out != (op->h_in + 2u * op->ph - op->kh) / op->sh + 1u)
        return VOCR_E_FORMAT;
    if (!op->cout || (op->type != T_HEAD && op->cout > 1024) || op->cin != *chans)
        return VOCR_E_FORMAT;
    if (op->flags & F_RES) {
        const vocr_op *d = i ? &m->op[i - 1] : NULL;
        if (!d || d->type != T_DW || d->sh != 1 || d->sw != 1 || d->h_in != d->h_out ||
            op->type != T_PW || op->cin != op->cout || (op->flags & F_COLLAPSE))
            return VOCR_E_FORMAT;
    }
    if (op->type == T_CONV || op->type == T_DW) {
        if ((op->type == T_CONV) != (i == 0) || (op->type == T_CONV && op->cin != 1) ||
            (op->type == T_DW && op->cin != op->cout) || (op->flags & (F_INT8 | F_COLLAPSE)) ||
            op->k != op->kh * op->kw || op->cout_pad != op->cout ||
            w_bytes != 4u * op->k * op->cout)
            return VOCR_E_FORMAT;
    } else if (op->type == T_ATTN) {
        /* train/export.py "ATTN": right before the head, one row, cin = cout = d (x8), its record's matrix QKV
         * ([3 dim / 8][d][8], bias always), dim / heads a multiple of 8, O [d / 8][dim][8] at 40-55 */
        const unsigned heads = rd16(r + 36), dim = rd16(r + 38), wsz = op->flags & F_INT8 ? 1u : 4u;
        const uint32_t ow = rd32(r + 40), owb = rd32(r + 44), ob = rd32(r + 48), os = rd32(r + 52);
        if (i == 0 || i + 2 != m->n_ops || m->attn.dim || op->kh != 1 || op->kw != 1 || op->sh != 1 ||
            op->sw != 1 || op->ph || op->pw || op->h_in != 1 || op->cin != op->cout || (op->cout & 7u) ||
            op->k != op->cin || (op->flags & ~(unsigned)(F_BIAS | F_INT8)) || !(op->flags & F_BIAS) || !heads ||
            dim < 8u || dim > 1024u || dim % heads || ((dim / heads) & 7u) || op->cout_pad != 3u * dim ||
            w_bytes != wsz * op->cout_pad * op->k || owb != wsz * op->cout * dim || !in_blob(total, ow, owb) ||
            !in_blob(total, ob, 4u * op->cout) || ((op->flags & F_INT8) ? !in_blob(total, os, 4u * op->cout) : os != 0))
            return VOCR_E_FORMAT;
        m->attn.heads = heads;
        m->attn.dim = dim;
        m->attn.o_bias = (const float *)(const void *)(b + ob);
        if (op->flags & F_INT8) {
            m->attn.o_q = (const int8_t *)(const void *)(b + ow);
            m->attn.o_scale = (const float *)(const void *)(b + os);
        } else {
            m->attn.o_w = (const float *)(const void *)(b + ow);
        }
    } else {
        unsigned collapse = (op->flags & F_COLLAPSE) != 0;
        if (op->sh != 1 || op->sw != 1 || op->ph || op->pw || op->kw != 1 ||
            op->kh != (collapse ? op->h_in : 1u) || (collapse && op->h_out != 1) ||
            op->k != (collapse ? op->h_in * op->cin : op->cin) || !op->k || op->k > 4096)
            return VOCR_E_FORMAT;
        if (op->type == T_HEAD ? (op->h_in != 1 || (op->flags & (F_RELU | F_RES | F_COLLAPSE)) ||
                                  op->cout != m->n_classes || op->cout_pad != ((op->cout + 7u) & ~7u))
                               : ((op->cout & 7u) || op->cout_pad != op->cout))
            return VOCR_E_FORMAT;
        if (w_bytes != (op->flags & F_INT8 ? 1u : 4u) * op->cout_pad * op->k) return VOCR_E_FORMAT;
        if (i == 0) return VOCR_E_FORMAT;
    }
    if (!in_blob(total, w_off, w_bytes)) return VOCR_E_FORMAT;
    if (op->flags & F_INT8) {
        if (!in_blob(total, s_off, 4u * op->cout_pad)) return VOCR_E_FORMAT;
        op->q = (const int8_t *)(const void *)(b + w_off);
        op->scale = (const float *)(const void *)(b + s_off);
    } else {
        if (s_off) return VOCR_E_FORMAT;
        op->w = (const float *)(const void *)(b + w_off);
    }
    if (op->flags & F_BIAS) {
        if (!in_blob(total, b_off, 4u * op->cout_pad)) return VOCR_E_FORMAT;
        op->bias = (const float *)(const void *)(b + b_off);
    } else if (b_off) {
        return VOCR_E_FORMAT;
    }
    *rows = op->h_out;
    *chans = op->cout;
    return VOCR_OK;
}

/* The head gate section at g (avail bytes from it to the end of the buffer), after a parsed version-1
 * blob (m complete). Layout (little-endian, train/headgate.py gate_section):
 *    0 char[4] "VOCG"     4 u16 version (1)     6 u16 header bytes (64)    8 u32 section bytes (x16)
 *   12 u32 flags (0)     16 u16 k (the head's K) 18 u16 hidden (x8, <= 64)
 *   20 u32 the blob's bytes (header field 8)    24 u32 the blob's CRC-32 (header field 40)
 *   28 f32 t (threshold)  32 f32 b2 (layer 2 bias)
 *   36 u32 w1 offset: int8 [hidden/8][k][8]      40 u32 s1 offset: f32 [hidden] (layer 1 scales)
 *   44 u32 b1 offset: f32 [hidden]               48 u32 w2 offset: f32 [hidden]
 *   52 u32 CRC-32 of the section's bytes [64, section bytes)    56..63 zero
 * Offsets are from the section's start, 16-byte aligned, inside the section. */
static int parse_gate(vocr_model *m, const unsigned char *g, size_t avail)
{
    vocr_gate *gt = &m->gate;
    const vocr_op *head = &m->op[m->n_ops - 1];
    uint32_t n, w1, s1, b1, w2;
    unsigned k, hid, j;
    float t, b2;
    if (avail < GATE_HEADER_BYTES || rd16(g + 4) != VOCR_GATE_VERSION || rd16(g + 6) != GATE_HEADER_BYTES)
        return VOCR_E_FORMAT;
    n = rd32(g + 8);
    if (n < GATE_HEADER_BYTES || (n & 15u) || n > avail || rd32(g + 12)) return VOCR_E_FORMAT;
    for (j = 56; j < GATE_HEADER_BYTES; j++)
        if (g[j]) return VOCR_E_FORMAT;
    k = rd16(g + 16);
    hid = rd16(g + 18);
    if (k != head->k || !hid || hid > VOCR_GATE_MAX_HIDDEN || (hid & 7u)) return VOCR_E_FORMAT;
    if (rd32(g + 20) != m->bytes || rd32(g + 24) != rd32(m->blob + 40)) return VOCR_E_FORMAT;
    memcpy(&t, g + 28, sizeof t);
    memcpy(&b2, g + 32, sizeof b2);
    if (t != t || b2 != b2) return VOCR_E_FORMAT; /* NaN */
    w1 = rd32(g + 36);
    s1 = rd32(g + 40);
    b1 = rd32(g + 44);
    w2 = rd32(g + 48);
    if (!in_blob(n, w1, hid * k) || !in_blob(n, s1, 4u * hid) || !in_blob(n, b1, 4u * hid) ||
        !in_blob(n, w2, 4u * hid))
        return VOCR_E_FORMAT;
    gt->q = (const int8_t *)(const void *)(g + w1);
    gt->scale = (const float *)(const void *)(g + s1);
    gt->bias = (const float *)(const void *)(g + b1);
    gt->w2 = (const float *)(const void *)(g + w2);
    gt->b2 = b2;
    gt->t = t;
    gt->k = k;
    gt->hidden = hid;
    gt->bytes = n;
    return VOCR_OK;
}

int vocr_load(vocr_model *m, const void *blob, size_t bytes)
{
    const unsigned char *b = (const unsigned char *)blob;
    const uint16_t one = 1;
    uint32_t total, ops_off, v_off, v_bytes;
    unsigned i, rows, chans = 1, stride = 1, n_int8 = 0, n_mat = 0;
    int rc;
    if (!m || !b) return VOCR_E_ARG;
    memset(m, 0, sizeof *m);
    if (*(const unsigned char *)&one != 1) return VOCR_E_FORMAT; /* the blob is little-endian */
    if ((uintptr_t)b & 15u) return VOCR_E_ALIGN;
    if (bytes < HEADER_BYTES || !is_magic(b) || rd16(b + 4) != VOCR_BLOB_VERSION ||
        rd16(b + 6) != HEADER_BYTES)
        return VOCR_E_FORMAT;
    total = rd32(b + 8);
    if (total < HEADER_BYTES || total > bytes || (rd32(b + 12) & ~1u)) return VOCR_E_FORMAT;
    m->blob = b;
    m->bytes = total;
    m->height = rd16(b + 16);
    m->max_width = rd16(b + 18);
    m->stride = rd16(b + 20);
    m->n_ops = rd16(b + 22);
    m->n_classes = rd32(b + 24);
    ops_off = rd32(b + 28);
    v_off = rd32(b + 32);
    v_bytes = rd32(b + 36);
    m->params = rd32(b + 44);
    if (!m->height || m->height > VOCR_MAX_HEIGHT || m->n_ops < 2 || m->n_ops > VOCR_MAX_OPS ||
        m->n_classes < 2 || m->n_classes > 65535u || !m->stride || m->stride > 64 ||
        m->max_width < MIN_WIDTH || m->max_width > 8192 || m->max_width % m->stride ||
        !in_blob(total, ops_off, OP_BYTES * m->n_ops))
        return VOCR_E_FORMAT;
    rows = m->height;
    for (i = 0; i < m->n_ops; i++) {
        rc = parse_op(m, i, b + ops_off + OP_BYTES * i, &rows, &chans, total);
        if (rc) return rc;
        stride *= m->op[i].sw;
        if (m->op[i].type >= T_PW) {
            n_mat++;
            n_int8 += (m->op[i].flags & F_INT8) != 0;
        }
    }
    if (rows != 1 || stride != m->stride || (n_int8 && n_int8 != n_mat) ||
        (n_int8 != 0) != ((rd32(b + 12) & 1u) != 0))
        return VOCR_E_FORMAT;
    m->int8 = n_int8 != 0;
    if (!in_blob(total, v_off, 2u * (m->n_classes + 1u)) ||
        v_bytes > total - v_off - 2u * (m->n_classes + 1u))
        return VOCR_E_FORMAT;
    m->vocab_off = (const uint16_t *)(const void *)(b + v_off);
    m->vocab = (const char *)(b + v_off + 2u * (m->n_classes + 1u));
    if (m->vocab_off[0] || m->vocab_off[1] || m->vocab_off[m->n_classes] != v_bytes)
        return VOCR_E_FORMAT;
    for (i = 0; i < m->n_classes; i++)
        if (m->vocab_off[i] > m->vocab_off[i + 1]) return VOCR_E_FORMAT;
    {
        const size_t g_off = al16(total);
        if (g_off <= bytes && bytes - g_off >= 4 && is_gate_magic(b + g_off)) {
            rc = parse_gate(m, b + g_off, bytes - g_off);
            if (rc) {
                memset(&m->gate, 0, sizeof m->gate);
                return rc;
            }
        }
    }
    rc = parse_two(m, b, bytes, total);
    if (rc) memset(&m->two, 0, sizeof m->two);
    return rc;
}

int vocr_has_gate(const vocr_model *m)
{
    return m && m->blob && m->gate.hidden ? 1 : 0;
}

int vocr_has_two_level(const vocr_model *m)
{
    return m && m->blob && m->two.l1_blocks ? 1 : 0;
}

const char *vocr_class_text(const vocr_model *m, unsigned c, unsigned *len)
{
    if (!m || !m->vocab_off || c >= m->n_classes) {
        if (len) *len = 0;
        return "";
    }
    if (len) *len = (unsigned)(m->vocab_off[c + 1] - m->vocab_off[c]);
    return m->vocab_off[c + 1] == m->vocab_off[c] ? "" : m->vocab + m->vocab_off[c];
}

const char *vocr_strerror(int rc)
{
    switch (rc) {
    case VOCR_OK: return "ok";
    case VOCR_E_ARG: return "bad argument";
    case VOCR_E_FORMAT: return "bad or unsupported model blob";
    case VOCR_E_ALIGN: return "blob or arena not 16-byte aligned";
    case VOCR_E_ARENA: return "arena too small";
    case VOCR_E_WIDTH: return "line wider than the planned max width";
    case VOCR_E_TEXT: return "text buffer too small";
    case VOCR_E_MODE: return "mode not available for this blob";
    case VOCR_E_CANCELLED: return "cancelled";
    default: return "unknown error";
    }
}

/* ---------------------------------------------------------------- plan
 *
 * Every op computes a fixed range of its output columns per tile, a constant `lag`
 * behind the source, so each layer's input is a sliding window (a ring) holding the
 * new columns plus `halo` columns carried over from the previous tile. Columns
 * outside the line are written as zeros, which is exactly the convolution padding.
 * The leading CONV/DW ops (the "front") run one output column of the last front op
 * at a time, so their large high-resolution rings stay a few columns wide.
 * PW inputs are transient: produced and consumed inside one tile. The op feeding the
 * head appends its columns to an accumulator; the head runs once it holds
 * head_frames frames. Every byte the plan places is reported by vocr_arena_bytes().
 */
static int resolve_mode(const vocr_model *m, int mode)
{
    if (mode == VOCR_MODE_AUTO) mode = m->int8 ? VOCR_MODE_W8F : VOCR_MODE_F32;
    if (mode == VOCR_MODE_F32 && !m->int8) return mode;
    if ((mode == VOCR_MODE_W8F || mode == VOCR_MODE_W8I) && m->int8) return mode;
    return VOCR_E_MODE;
}

static size_t op_scratch(const vocr_ctx *c, const vocr_op *op, unsigned n_tile)
{
    size_t pos;
    if (op->type < T_PW || op->type == T_ATTN) return 0; /* the attention's own scratch: plan(), after the tiles */
    pos = op->type == T_HEAD ? c->acc_frames
                             : (size_t)n_tile * (op->flags & F_COLLAPSE ? 1u : op->h_in);
    if (c->mode == VOCR_MODE_W8F) {
        /* the two-level head's escape reads the head input in int16 in every mode (run_head2), then a
         * block's dequantised weights */
        if (op->type == T_HEAD && c->two_level)
            return al16(pos * op->k * 2u) + al16(pos * sizeof(float)) + al16((size_t)op->k * 8u * sizeof(float));
        return al16((size_t)op->k * 8u * sizeof(float));
    }
    if (c->mode == VOCR_MODE_W8I) return al16(pos * op->k * 2u) + al16(pos * sizeof(float));
    return 0;
}

static int plan(vocr_ctx *c, const vocr_model *m, const vocr_opts *o)
{
    static const vocr_opts defaults = {0, 0, 0, VOCR_MODE_AUTO, VOCR_HEAD_GATE_OFF, 0};
    unsigned L, S, i, f, t_max, F, hb, s = 1, s_out[VOCR_MAX_OPS];
    size_t off, base, R = 0, tin[VOCR_MAX_OPS], tout[VOCR_MAX_OPS], scr[VOCR_MAX_OPS];
    int prev = 0, mode, in_top = 0;
    if (!c || !m || !m->blob) return VOCR_E_ARG;
    if (!o) o = &defaults;
    memset(c, 0, sizeof *c);
    L = m->n_ops;
    S = m->stride;
    mode = resolve_mode(m, o->mode);
    if (mode < 0) return mode;
    if (o->head_gate > VOCR_HEAD_GATE_SCORE) return VOCR_E_ARG;
    /* the gate runs in every mode, the two-level head in W8I and W8F (it needs an int8 blob: parse_two) */
    if (o->head_gate && !m->gate.hidden) return VOCR_E_MODE;
    if (o->two_level && (mode == VOCR_MODE_F32 || !m->two.l1_blocks)) return VOCR_E_MODE;
    c->two_level = o->two_level ? 1u : 0u;
    c->attn = m->attn.dim ? m->n_ops - 2u : 0u;
    c->model = m;
    c->mode = mode;
    c->gate = o->head_gate;
    c->max_width = o->max_width && o->max_width < m->max_width ? o->max_width : m->max_width;
    c->max_width = (c->max_width + S - 1) / S * S;
    t_max = c->max_width / S;
    /* default tile: 4 frames, or for a stride over 8 the whole frames nearest 32 input columns (3 at
     * stride 12, 2 at 16), so a coarser-stride model's zeroed options keep about a 32-column tile's
     * arena instead of growing with the stride */
    F = umin(o->tile_frames ? o->tile_frames : S <= 8u ? 4u : S >= 64u ? 1u : (32u + S / 2u) / S, t_max);
    hb = umin(o->head_frames ? o->head_frames : 16u, t_max);
    c->tile = F;
    c->head = hb;
    c->acc_frames = hb + F - 1;
    for (f = 0; f < L && (m->op[f].type == T_CONV || m->op[f].type == T_DW); f++) {
    }
    if (f < L && (m->op[f].flags & F_RES)) f--;
    if (f < 1) return VOCR_E_FORMAT;
    f--;
    c->front = f;
    for (i = 0; i < L; i++) {
        const vocr_op *op = &m->op[i];
        vocr_plan_op *p = &c->op[i];
        int r = (int)op->kw - 1 - (int)op->pw;
        s *= op->sw;
        s_out[i] = s;
        p->lag = (prev + r + 1 + (int)op->sw - 1) / (int)op->sw - 1;
        prev = p->lag;
        p->in_col = (uint32_t)op->h_in * op->cin;
        p->out_col = (uint32_t)op->h_out * op->cout;
        p->dst = i + 1 == L || op->type == T_ATTN ? 0 /* the attention runs after the tiles */
                 : m->op[i + 1].type == T_PW   ? DST_TRANS
                 : m->op[i + 1].type == T_HEAD ? DST_ACC
                 : m->op[i + 1].type == T_ATTN ? DST_LINE
                                               : DST_RING;
    }
    if ((F * S) % s_out[L - 1]) return VOCR_E_FORMAT;
    c->n_src = s_out[f];
    for (i = 0; i < L; i++) {
        c->op[i].n_tile = F * S / s_out[i];
        c->op[i].n_step = i <= f ? s_out[f] / s_out[i] : c->op[i].n_tile;
    }
    off = al16(8u * m->height); /* resize row table */
    c->rows_off = 0;
    c->ring_off = (uint32_t)off;
    for (i = 0; i < L; i++) {
        const vocr_op *op = &m->op[i];
        vocr_plan_op *p = &c->op[i];
        unsigned n_prev;
        int halo;
        if (op->type != T_CONV && op->type != T_DW) continue;
        n_prev = i == 0 ? c->n_src : i <= f ? c->op[i - 1].n_step : c->op[i - 1].n_tile;
        halo = p->lag * (int)op->sw + (int)op->pw - (i ? c->op[i - 1].lag : 0);
        if (halo < 0) return VOCR_E_FORMAT; /* kw >= sw (parse_op) keeps it >= kw - sw >= 0 */
        p->halo = (uint32_t)halo;
        p->cap = n_prev + p->halo;
        p->in_off = (uint32_t)off;
        off += al16((size_t)p->cap * p->in_col * sizeof(float));
    }
    c->ring_end = (uint32_t)off;
    c->acc_off = (uint32_t)off;
    c->op[L - 1].in_off = (uint32_t)off;
    off += al16((size_t)c->acc_frames * m->op[L - 1].k * sizeof(float));
    c->bestv_off = (uint32_t)off;
    off += al16((size_t)c->acc_frames * sizeof(float));
    c->besti_off = (uint32_t)off;
    off += al16((size_t)c->acc_frames * sizeof(uint32_t));
    if (c->gate == VOCR_HEAD_GATE_ON) { /* the line index of each accumulated frame */
        c->fidx_off = (uint32_t)off;
        off += al16((size_t)c->acc_frames * sizeof(uint16_t));
    }
    if (c->two_level) { /* level 1's non-blank maxima and blank logits, the escape's hidden units, the fired frames */
        const size_t A = c->acc_frames;
        c->nbv_off = (uint32_t)off;
        off += al16(A * sizeof(float));
        c->b0_off = (uint32_t)off;
        off += al16(A * sizeof(float));
        c->hid_off = (uint32_t)off;
        off += al16(A * m->two.hidden * sizeof(float));
        c->fmap_off = (uint32_t)off;
        off += al16(A * sizeof(uint32_t));
    }
    base = off;
    for (i = 0; i < L; i++) {
        const vocr_op *op = &m->op[i];
        size_t sum;
        tin[i] = op->type == T_PW ? al16((size_t)c->op[i - 1].n_tile * c->op[i - 1].out_col * 4u) : 0;
        tout[i] = c->op[i].dst == DST_TRANS ? al16((size_t)c->op[i].n_tile * c->op[i].out_col * 4u) : 0;
        scr[i] = op_scratch(c, op, c->op[i].n_tile);
        sum = tin[i] + tout[i] + scr[i];
        if (sum > R) R = sum;
    }
    /* the gate runs between a tile's last layer and the head, in the head's scratch (at base): a tile's
     * new frames as int16 with their scales, and their hidden units; in W8F also the head's first block,
     * dequantised, for a skipped frame's blank logit */
    if (c->gate) {
        size_t g = al16((size_t)F * m->op[L - 1].k * 2u) + al16((size_t)F * sizeof(float)) +
                   al16((size_t)F * m->gate.hidden * sizeof(float));
        if (mode == VOCR_MODE_W8F) g += al16((size_t)m->op[L - 1].k * 8u * sizeof(float));
        if (g > R) R = g;
    }
    /* transients alternate between the bottom and the top of the region, scratch
     * sits above the bottom one */
    for (i = 0; i < L; i++) {
        int out_top = 0;
        size_t bottom = 0;
        if (tin[i] && !in_top) bottom = tin[i];
        if (tout[i]) {
            out_top = tin[i] && !in_top;
            c->op[i + 1].in_off = (uint32_t)(base + (out_top ? R - tout[i] : 0));
            if (!out_top) bottom = tout[i];
        }
        c->op[i].scr_off = (uint32_t)(base + bottom);
        in_top = out_top;
    }
    off = base + R;
    if (off > 0xFFFFFFF0u) return VOCR_E_ARG; /* before any offset below is cast to 32 bits */
    /* the attention op (docs/vocr6-attn.md "Arena"): its working set (K and V of every frame, a head batch of queries'
     * Q, scores and outputs, the projections' scratch) overlays the rings, the head's accumulator and state and the
     * transients, all idle between the tiles and the head; the line of z, every frame's bottleneck output, which
     * lives through all three, comes after both. train/export.py plan_arena mirrors it. */
    if (c->attn) {
        const size_t d = m->op[c->attn].cin, da = m->attn.dim, P = hb, kmax = d > da ? d : da;
        const size_t tp = ((size_t)t_max + 7u) & ~(size_t)7u;
        const size_t line = al16((size_t)t_max * d * sizeof(float));
        size_t w = c->ring_off, lo;
        c->att_tp = (uint32_t)tp;
        c->kp_off = (uint32_t)w;
        w += al16(tp * da * sizeof(float));
        c->vp_off = (uint32_t)w;
        w += al16(tp * da * sizeof(float));
        c->qc_off = (uint32_t)w;
        w += al16(P * da * sizeof(float));
        c->ac_off = (uint32_t)w;
        w += al16(P * da * sizeof(float));
        c->sc_off = (uint32_t)w;
        w += al16(P * tp * sizeof(float));
        c->as_off = (uint32_t)w;
        if (mode == VOCR_MODE_W8F) w += al16(kmax * 8u * sizeof(float));
        else if (mode == VOCR_MODE_W8I) w += al16(P * kmax * 2u) + al16(P * sizeof(float));
        if (w > 0xFFFFFFF0u) return VOCR_E_ARG;
        lo = off > w ? off : w;
        if (line > 0xFFFFFFF0u - lo) return VOCR_E_ARG; /* no wrap of a 32-bit size_t either */
        c->line_off = (uint32_t)lo;
        off = lo + line;
    }
    if (off > 0xFFFFFFF0u) return VOCR_E_ARG;
    c->arena_bytes = off;
    return VOCR_OK;
}

size_t vocr_arena_bytes(const vocr_model *m, const vocr_opts *o)
{
    vocr_ctx c;
    return plan(&c, m, o) == VOCR_OK ? c.arena_bytes : 0;
}

int vocr_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes)
{
    int rc;
    if (!c || !arena) return VOCR_E_ARG;
    if ((uintptr_t)arena & 15u) return VOCR_E_ALIGN;
    rc = plan(c, m, o);
    if (rc) return rc;
    if (bytes < c->arena_bytes) return VOCR_E_ARENA;
    c->arena = (unsigned char *)arena;
    return VOCR_OK;
}

/* ---------------------------------------------------------------- preprocessing
 *
 * Bit-exact with cv2.resize(grey, (nw, H), interpolation=cv2.INTER_LINEAR) on uint8:
 * 11-bit fixed-point coefficients from the same float/double arithmetic, horizontal
 * pass into int, vertical pass with OpenCV's (b * (v >> 4)) >> 16 rounding. The
 * vertical coefficients are not clamped at the borders, the rows are.
 */
typedef struct {
    const vocr_image *img;
    unsigned nw, padded;
    int rgba, inv;        /* RGBA input; inv = 255 with VOCR_INVERT (v ^ 255 == 255 - v) */
    double scale_x;
    const uint16_t *rows; /* per output row: y0, y1, b0, b1 */
} line_t;

static int floor_i(float v)
{
    int s = (int)v;
    return (float)s > v ? s - 1 : s;
}

static int round_even(float v) /* v >= 0, like cvRound */
{
    int n = (int)v;
    float fr = v - (float)n;
    if (fr > 0.5f || (fr == 0.5f && (n & 1))) n++;
    return n;
}

unsigned vocr_resized_width(const vocr_model *m, unsigned w, unsigned h)
{
    uint32_t num, q;
    if (!m || !m->blob || !w || !h || w > 65535u || h > 65535u) return 0;
    num = (uint32_t)w * m->height; /* round(w * H / h), half to even like Python */
    q = (2u * num + h) / (2u * h);
    if ((2u * num + h) % (2u * h) == 0 && (q & 1u)) q--;
    if (q < MIN_WIDTH) q = MIN_WIDTH;
    if (q > m->max_width) q = m->max_width;
    return q;
}

static void row_table(uint16_t *rows, unsigned H, unsigned h)
{
    double scale = 1.0 / ((double)H / (double)h);
    unsigned y;
    for (y = 0; y < H; y++) {
        volatile double t = ((double)y + 0.5) * scale; /* volatile: never fused */
        float fy = (float)(t - 0.5);
        int sy = floor_i(fy);
        fy -= (float)sy;
        rows[4 * y + 0] = (uint16_t)(sy < 0 ? 0 : sy > (int)h - 1 ? (int)h - 1 : sy);
        rows[4 * y + 1] = (uint16_t)(sy + 1 < 0 ? 0 : sy + 1 > (int)h - 1 ? (int)h - 1 : sy + 1);
        rows[4 * y + 2] = (uint16_t)round_even((1.0f - fy) * 2048.0f); /* 0..2048 */
        rows[4 * y + 3] = (uint16_t)round_even(fy * 2048.0f);
    }
}

static int check_image(const vocr_image *im)
{
    int fmt;
    unsigned bpp;
    if (!im || !im->pixels || !im->width || !im->height || im->width > 65535u ||
        im->height > 65535u)
        return VOCR_E_ARG;
    fmt = im->format & ~VOCR_INVERT;
    if (fmt != VOCR_GREY8 && fmt != VOCR_RGBA8) return VOCR_E_ARG;
    bpp = fmt == VOCR_GREY8 ? 1u : 4u;
    if (im->stride < (size_t)im->width * bpp) return VOCR_E_ARG;
    return VOCR_OK;
}

static void line_setup(line_t *ln, const vocr_model *m, const vocr_image *im, uint16_t *rows)
{
    ln->img = im;
    ln->nw = vocr_resized_width(m, im->width, im->height);
    ln->padded = (ln->nw + m->stride - 1) / m->stride * m->stride;
    ln->rgba = (im->format & ~VOCR_INVERT) == VOCR_RGBA8;
    ln->inv = (im->format & VOCR_INVERT) ? 255 : 0;
    ln->scale_x = 1.0 / ((double)ln->nw / (double)im->width);
    ln->rows = rows;
    row_table(rows, m->height, im->height);
}

/* The grey value training sees for source pixel x of a row: integer luma for RGBA
 * (synth/common.to_gray), inverted with VOCR_INVERT. */
static int grey_px(const line_t *ln, const unsigned char *row, unsigned x)
{
    if (ln->rgba) {
        row += 4u * x;
        return ((77 * row[0] + 150 * row[1] + 29 * row[2]) >> 8) ^ ln->inv;
    }
    return row[x] ^ ln->inv;
}

/* One resized column x (x < nw): its source columns and coefficients. */
typedef struct {
    unsigned s0, s1;
    int a0, a1;
} xcoef_t;

static xcoef_t col_coef(const line_t *ln, unsigned x)
{
    xcoef_t k;
    int w = (int)ln->img->width, sx;
    volatile double t = ((double)x + 0.5) * ln->scale_x;
    float fx = (float)(t - 0.5);
    sx = floor_i(fx);
    fx -= (float)sx;
    if (sx < 0) {
        fx = 0.0f;
        sx = 0;
    }
    if (sx >= w - 1) {
        fx = 0.0f;
        sx = w - 1;
    }
    k.a0 = round_even((1.0f - fx) * 2048.0f);
    k.a1 = round_even(fx * 2048.0f);
    k.s0 = (unsigned)sx;
    k.s1 = sx + 1 < w ? (unsigned)sx + 1u : (unsigned)sx;
    return k;
}

static unsigned resized_px(const line_t *ln, unsigned y, xcoef_t k)
{
    const vocr_image *im = ln->img;
    const uint16_t *r = ln->rows + 4u * y;
    const unsigned char *p0 = im->pixels + (size_t)r[0] * im->stride;
    const unsigned char *p1 = im->pixels + (size_t)r[1] * im->stride;
    int h0 = grey_px(ln, p0, k.s0) * k.a0 + grey_px(ln, p0, k.s1) * k.a1;
    int h1 = grey_px(ln, p1, k.s0) * k.a0 + grey_px(ln, p1, k.s1) * k.a1;
    return (unsigned)((((r[2] * (h0 >> 4)) >> 16) + ((r[3] * (h1 >> 4)) >> 16) + 2) >> 2);
}

/* Source columns [j0, j0 + n) of the normalised network input, [col][row] floats. */
static void src_cols(const line_t *ln, unsigned H, long j0, unsigned n, float *dst)
{
    unsigned t, y;
    for (t = 0; t < n; t++) {
        long j = j0 + (long)t;
        float *col = dst + (size_t)t * H;
        xcoef_t k;
        if (j < 0 || j >= (long)ln->padded) {
            memset(col, 0, H * sizeof(float));
            continue;
        }
        k = col_coef(ln, j < (long)ln->nw ? (unsigned)j : ln->nw - 1u);
        for (y = 0; y < H; y++) col[y] = (float)resized_px(ln, y, k) / 127.5f - 1.0f;
    }
}

int vocr_preprocess(const vocr_model *m, const vocr_image *img, unsigned char *out,
                    size_t cap, unsigned *width, unsigned *padded_width)
{
    uint16_t rows[4 * VOCR_MAX_HEIGHT];
    line_t ln;
    unsigned x, y;
    int rc = check_image(img);
    if (rc) return rc;
    if (!m || !m->blob) return VOCR_E_ARG;
    line_setup(&ln, m, img, rows);
    if (width) *width = ln.nw;
    if (padded_width) *padded_width = ln.padded;
    if (!out) return VOCR_OK;
    if (cap < (size_t)ln.padded * m->height) return VOCR_E_ARG;
    for (x = 0; x < ln.padded; x++) {
        xcoef_t k = col_coef(&ln, x < ln.nw ? x : ln.nw - 1u);
        for (y = 0; y < m->height; y++) out[(size_t)y * ln.padded + x] = (unsigned char)resized_px(&ln, y, k);
    }
    return VOCR_OK;
}

/* ---------------------------------------------------------------- kernels
 *
 * Activations are [column][row][channel], channels innermost. Every output element
 * is computed as acc = bias, then acc = acc + w * x over the taps (dy, then dx) or
 * over k in ascending order, each product and sum rounded separately; the NEON
 * paths vectorise across output channels only, so they give the same bits as the
 * scalar code. Taps that fall in the top/bottom padding are skipped; columns
 * outside the line are zero-filled by the producer, so they need no checks here.
 */
/* max(v, 0), +0 for v <= 0. Without NEON as (v + |v|) / 2 (Kotoba3DS change): the same bits for
 * every value below 2^127 (v > 0: v + v and the halving are exact; v <= 0: -|v| + |v| = +0), also
 * with flush-to-zero, and three VFP operations that pipeline, where the compare has to wait for
 * the VFP and move its flags to the ARM core (on the ARM11 several cycles a value). */
#ifdef VOCR_NEON
static float relu_f(float v) { return v > 0.0f ? v : 0.0f; }
#else
static float relu_f(float v) { return (v + fabsf(v)) * 0.5f; }
#endif

/* Kotoba3DS addition: the taps of the depthwise layers and of the first convolution for 8
 * channels at a time in ARM VFP assembly (the ARM11's VFP11): each tap's 8 inputs and 8 weights
 * in two VLDMs (two registers a cycle) instead of 16 VLDRs, and the 8 sums in s16-s23 throughout.
 * VMLA rounds the product before the add, as the C code's a = a + w * x, in the same tap order,
 * so the sums are the C code's bits. acc[8] in and out; t: the first tap's input and weights, the
 * byte steps to the next tap of a row (dx) and to the next row (dy), taps per row, rows (>= 1). */
#if defined(__arm__) && !defined(__thumb__) && !defined(VOCR_NEON) && defined(__VFP_FP__) && !defined(__SOFTFP__) && \
    !defined(VOCR_NO_VFP_ASM)
#define VOCR_VFP_ASM 1
typedef struct {
    const float *x, *w;
    uint32_t xs, ws, kw, ndy, xr, wr;
} vocr_taps;
void vocr__dw8(float *acc, const vocr_taps *t);   /* x: 8 channels of input per tap */
void vocr__cv8(float *acc, const vocr_taps *t);   /* x: one input value per tap (one input channel) */
__asm__(".pushsection .text.vocr__dw8,\"ax\",%progbits\n"
        ".syntax unified\n"
        ".arm\n"
        ".fpu vfp\n"
        ".align 2\n"
        ".macro VOCR_TAPS name, scalar\n"
        ".global \\name\n"
        ".type \\name, %function\n"
        "\\name:\n"
        "    push    {r4-r9, lr}\n"
        "    vpush   {d8-d11}\n"
        "    vldmia  r0, {s16-s23}\n"
        "    ldm     r1, {r2-r9}\n"
        "1:\n"
        "    mov     ip, r6\n"
        "    mov     r1, r2\n"
        "    mov     lr, r3\n"
        "2:\n"
        "    vldmia  lr, {s8-s15}\n"
        ".if \\scalar\n"
        "    vldr    s0, [r1]\n"
        "    add     r1, r1, r4\n"
        "    add     lr, lr, r5\n"
        "    vmla.f32 s16, s8, s0\n"
        "    vmla.f32 s17, s9, s0\n"
        "    vmla.f32 s18, s10, s0\n"
        "    vmla.f32 s19, s11, s0\n"
        "    vmla.f32 s20, s12, s0\n"
        "    vmla.f32 s21, s13, s0\n"
        "    vmla.f32 s22, s14, s0\n"
        "    vmla.f32 s23, s15, s0\n"
        ".else\n"
        "    vldmia  r1, {s0-s7}\n"
        "    add     r1, r1, r4\n"
        "    add     lr, lr, r5\n"
        "    vmla.f32 s16, s8, s0\n"
        "    vmla.f32 s17, s9, s1\n"
        "    vmla.f32 s18, s10, s2\n"
        "    vmla.f32 s19, s11, s3\n"
        "    vmla.f32 s20, s12, s4\n"
        "    vmla.f32 s21, s13, s5\n"
        "    vmla.f32 s22, s14, s6\n"
        "    vmla.f32 s23, s15, s7\n"
        ".endif\n"
        "    subs    ip, ip, #1\n"
        "    bne     2b\n"
        "    add     r2, r2, r8\n"
        "    add     r3, r3, r9\n"
        "    subs    r7, r7, #1\n"
        "    bne     1b\n"
        "    vstmia  r0, {s16-s23}\n"
        "    vpop    {d8-d11}\n"
        "    pop     {r4-r9, pc}\n"
        ".size \\name, .-\\name\n"
        ".endm\n"
        "    VOCR_TAPS vocr__dw8, 0\n"
        "    VOCR_TAPS vocr__cv8, 1\n"
        ".purgem VOCR_TAPS\n"
        ".popsection\n");
#endif

/* CONV with one input channel (the stem). win: [col][h_in]; output column t reads
 * window columns t * sw + dx. Writes output columns [t0, t1) to out. */
static VOCR_NOINLINE void k_conv1(const vocr_op *op, const float *win, float *out, unsigned t0, unsigned t1)
{
    const unsigned C = op->cout, kw = op->kw, Ho = op->h_out, Hi = op->h_in;
    const int relu = (op->flags & F_RELU) != 0;
    unsigned t, y, c0, c, dy, dx;
    for (t = t0; t < t1; t++)
        for (y = 0; y < Ho; y++) {
            const int r0 = (int)(y * op->sh) - (int)op->ph;
            const unsigned dy0 = r0 < 0 ? (unsigned)-r0 : 0u;
            const unsigned dy1 = umin(op->kh, (unsigned)((int)Hi - r0));
            float *o = out + ((size_t)(t - t0) * Ho + y) * C;
            c0 = 0;
#ifdef VOCR_NEON
            for (; c0 + 16 <= C; c0 += 16) {
                float32x4_t a0, a1, a2, a3, z = vdupq_n_f32(0.0f);
                if (op->bias) {
                    a0 = vld1q_f32(op->bias + c0);
                    a1 = vld1q_f32(op->bias + c0 + 4);
                    a2 = vld1q_f32(op->bias + c0 + 8);
                    a3 = vld1q_f32(op->bias + c0 + 12);
                } else {
                    a0 = a1 = a2 = a3 = z;
                }
                /* the same taps in the same order, the input value loaded straight into a NEON
                 * lane (VLD1 to all lanes) instead of through an ARM register, and the tap's
                 * input and weights by stepping pointers */
                for (dy = dy0; dy < dy1; dy++) {
                    const float *xp = win + (size_t)(t * op->sw) * Hi + (unsigned)(r0 + (int)dy);
                    const float *wt = op->w + (size_t)(dy * kw) * C + c0;
                    for (dx = 0; dx < kw; dx++, xp += Hi, wt += C) {
                        const float32x2_t x = vld1_dup_f32(xp);
                        const float32x4x2_t wa = vld1q_f32_x2(wt), wb = vld1q_f32_x2(wt + 8);
                        a0 = vmlaq_lane_f32(a0, wa.val[0], x, 0);
                        a1 = vmlaq_lane_f32(a1, wa.val[1], x, 0);
                        a2 = vmlaq_lane_f32(a2, wb.val[0], x, 0);
                        a3 = vmlaq_lane_f32(a3, wb.val[1], x, 0);
                    }
                }
                if (relu) {
                    a0 = vmaxq_f32(a0, z);
                    a1 = vmaxq_f32(a1, z);
                    a2 = vmaxq_f32(a2, z);
                    a3 = vmaxq_f32(a3, z);
                }
                vst1q_f32(o + c0, a0);
                vst1q_f32(o + c0 + 4, a1);
                vst1q_f32(o + c0 + 8, a2);
                vst1q_f32(o + c0 + 12, a3);
            }
#endif
            /* Scalar: 8 channels per pass with the sums in registers (see mm_f32). */
            for (; c0 + 8 <= C; c0 += 8) {
                float a0, a1, a2, a3, a4, a5, a6, a7;
                if (op->bias) {
                    const float *b = op->bias + c0;
                    a0 = b[0], a1 = b[1], a2 = b[2], a3 = b[3], a4 = b[4], a5 = b[5], a6 = b[6], a7 = b[7];
                } else {
                    a0 = a1 = a2 = a3 = a4 = a5 = a6 = a7 = 0.0f;
                }
#ifdef VOCR_VFP_ASM
                if (dy1 > dy0) {
                    float acc[8];
                    vocr_taps tp;
                    acc[0] = a0, acc[1] = a1, acc[2] = a2, acc[3] = a3, acc[4] = a4, acc[5] = a5, acc[6] = a6, acc[7] = a7;
                    tp.x = win + (size_t)(t * op->sw) * Hi + (unsigned)(r0 + (int)dy0);
                    tp.w = op->w + (size_t)(dy0 * kw) * C + c0;
                    tp.xs = Hi * 4u;
                    tp.ws = C * 4u;
                    tp.kw = kw;
                    tp.ndy = dy1 - dy0;
                    tp.xr = 4u;
                    tp.wr = kw * C * 4u;
                    vocr__cv8(acc, &tp);
                    a0 = acc[0], a1 = acc[1], a2 = acc[2], a3 = acc[3], a4 = acc[4], a5 = acc[5], a6 = acc[6], a7 = acc[7];
                }
#else
                for (dy = dy0; dy < dy1; dy++)
                    for (dx = 0; dx < kw; dx++) {
                        const float x = win[(size_t)(t * op->sw + dx) * Hi + (unsigned)(r0 + (int)dy)];
                        const float *wt = op->w + (size_t)(dy * kw + dx) * C + c0;
                        a0 = a0 + wt[0] * x;
                        a1 = a1 + wt[1] * x;
                        a2 = a2 + wt[2] * x;
                        a3 = a3 + wt[3] * x;
                        a4 = a4 + wt[4] * x;
                        a5 = a5 + wt[5] * x;
                        a6 = a6 + wt[6] * x;
                        a7 = a7 + wt[7] * x;
                    }
#endif
                if (relu) { /* straight from the registers (through s[], gcc 13 stores and reloads each) */
                    a0 = relu_f(a0), a1 = relu_f(a1), a2 = relu_f(a2), a3 = relu_f(a3);
                    a4 = relu_f(a4), a5 = relu_f(a5), a6 = relu_f(a6), a7 = relu_f(a7);
                }
                o[c0] = a0, o[c0 + 1] = a1, o[c0 + 2] = a2, o[c0 + 3] = a3;
                o[c0 + 4] = a4, o[c0 + 5] = a5, o[c0 + 6] = a6, o[c0 + 7] = a7;
            }
            for (; c0 < C; c0 += 32) {
                const unsigned cn = umin(32u, C - c0);
                float acc[32];
                for (c = 0; c < cn; c++) acc[c] = op->bias ? op->bias[c0 + c] : 0.0f;
                for (dy = dy0; dy < dy1; dy++)
                    for (dx = 0; dx < kw; dx++) {
                        const float x = win[(size_t)(t * op->sw + dx) * Hi + (unsigned)(r0 + (int)dy)];
                        const float *wt = op->w + (size_t)(dy * kw + dx) * C + c0;
                        for (c = 0; c < cn; c++) acc[c] = acc[c] + wt[c] * x;
                    }
                for (c = 0; c < cn; c++) o[c0 + c] = relu ? relu_f(acc[c]) : acc[c];
            }
        }
}

/* Depthwise kh x kw. win: [col][h_in][C]. */
static VOCR_NOINLINE void k_dw(const vocr_op *op, const float *win, float *out, unsigned t0, unsigned t1)
{
    const unsigned C = op->cout, kw = op->kw, Ho = op->h_out, Hi = op->h_in;
    const int relu = (op->flags & F_RELU) != 0;
    unsigned t, y, c0, c, dy, dx;
    for (t = t0; t < t1; t++)
        for (y = 0; y < Ho; y++) {
            const int r0 = (int)(y * op->sh) - (int)op->ph;
            const unsigned dy0 = r0 < 0 ? (unsigned)-r0 : 0u;
            const unsigned dy1 = umin(op->kh, (unsigned)((int)Hi - r0));
            float *o = out + ((size_t)(t - t0) * Ho + y) * C;
            c0 = 0;
#ifdef VOCR_NEON
            for (; c0 + 16 <= C; c0 += 16) {
                float32x4_t a0, a1, a2, a3, z = vdupq_n_f32(0.0f);
                if (op->bias) {
                    a0 = vld1q_f32(op->bias + c0);
                    a1 = vld1q_f32(op->bias + c0 + 4);
                    a2 = vld1q_f32(op->bias + c0 + 8);
                    a3 = vld1q_f32(op->bias + c0 + 12);
                } else {
                    a0 = a1 = a2 = a3 = z;
                }
                /* the same taps in the same order, by stepping pointers: one input column is
                 * Hi * C floats, one tap row of weights kw * C */
#define VOCR_DWTAP(X, W)                                                                     \
    do {                                                                                     \
        const float32x4x2_t xa_ = vld1q_f32_x2(X), wa_ = vld1q_f32_x2(W);                    \
        const float32x4x2_t xb_ = vld1q_f32_x2((X) + 8), wb_ = vld1q_f32_x2((W) + 8);        \
        a0 = vmlaq_f32(a0, wa_.val[0], xa_.val[0]);                                          \
        a1 = vmlaq_f32(a1, wa_.val[1], xa_.val[1]);                                          \
        a2 = vmlaq_f32(a2, wb_.val[0], xb_.val[0]);                                          \
        a3 = vmlaq_f32(a3, wb_.val[1], xb_.val[1]);                                          \
    } while (0)
                for (dy = dy0; dy < dy1; dy++) {
                    const size_t cs = (size_t)Hi * C;
                    const float *x = win + ((size_t)(t * op->sw) * Hi + (unsigned)(r0 + (int)dy)) * C + c0;
                    const float *wt = op->w + (size_t)(dy * kw) * C + c0;
                    if (kw == 3) {
                        VOCR_DWTAP(x, wt);
                        VOCR_DWTAP(x + cs, wt + C);
                        VOCR_DWTAP(x + 2 * cs, wt + 2 * C);
                    } else {
                        for (dx = 0; dx < kw; dx++, x += cs, wt += C) VOCR_DWTAP(x, wt);
                    }
                }
#undef VOCR_DWTAP
                if (relu) {
                    a0 = vmaxq_f32(a0, z);
                    a1 = vmaxq_f32(a1, z);
                    a2 = vmaxq_f32(a2, z);
                    a3 = vmaxq_f32(a3, z);
                }
                vst1q_f32(o + c0, a0);
                vst1q_f32(o + c0 + 4, a1);
                vst1q_f32(o + c0 + 8, a2);
                vst1q_f32(o + c0 + 12, a3);
            }
#endif
            /* Scalar: 8 channels per pass with the sums in registers (see mm_f32). */
            for (; c0 + 8 <= C; c0 += 8) {
                float a0, a1, a2, a3, a4, a5, a6, a7;
                if (op->bias) {
                    const float *b = op->bias + c0;
                    a0 = b[0], a1 = b[1], a2 = b[2], a3 = b[3], a4 = b[4], a5 = b[5], a6 = b[6], a7 = b[7];
                } else {
                    a0 = a1 = a2 = a3 = a4 = a5 = a6 = a7 = 0.0f;
                }
#ifdef VOCR_VFP_ASM
                if (dy1 > dy0) {
                    float acc[8];
                    vocr_taps tp;
                    acc[0] = a0, acc[1] = a1, acc[2] = a2, acc[3] = a3, acc[4] = a4, acc[5] = a5, acc[6] = a6, acc[7] = a7;
                    tp.x = win + ((size_t)(t * op->sw) * Hi + (unsigned)(r0 + (int)dy0)) * C + c0;
                    tp.w = op->w + (size_t)(dy0 * kw) * C + c0;
                    tp.xs = Hi * C * 4u;
                    tp.ws = C * 4u;
                    tp.kw = kw;
                    tp.ndy = dy1 - dy0;
                    tp.xr = C * 4u;
                    tp.wr = kw * C * 4u;
                    vocr__dw8(acc, &tp);
                    a0 = acc[0], a1 = acc[1], a2 = acc[2], a3 = acc[3], a4 = acc[4], a5 = acc[5], a6 = acc[6], a7 = acc[7];
                }
#else
                for (dy = dy0; dy < dy1; dy++)
                    for (dx = 0; dx < kw; dx++) {
                        const float *x = win + ((size_t)(t * op->sw + dx) * Hi + (unsigned)(r0 + (int)dy)) * C + c0;
                        const float *wt = op->w + (size_t)(dy * kw + dx) * C + c0;
                        a0 = a0 + wt[0] * x[0];
                        a1 = a1 + wt[1] * x[1];
                        a2 = a2 + wt[2] * x[2];
                        a3 = a3 + wt[3] * x[3];
                        a4 = a4 + wt[4] * x[4];
                        a5 = a5 + wt[5] * x[5];
                        a6 = a6 + wt[6] * x[6];
                        a7 = a7 + wt[7] * x[7];
                    }
#endif
                if (relu) { /* straight from the registers (through s[], gcc 13 stores and reloads each) */
                    a0 = relu_f(a0), a1 = relu_f(a1), a2 = relu_f(a2), a3 = relu_f(a3);
                    a4 = relu_f(a4), a5 = relu_f(a5), a6 = relu_f(a6), a7 = relu_f(a7);
                }
                o[c0] = a0, o[c0 + 1] = a1, o[c0 + 2] = a2, o[c0 + 3] = a3;
                o[c0 + 4] = a4, o[c0 + 5] = a5, o[c0 + 6] = a6, o[c0 + 7] = a7;
            }
            for (; c0 < C; c0 += 32) {
                const unsigned cn = umin(32u, C - c0);
                float acc[32];
                for (c = 0; c < cn; c++) acc[c] = op->bias ? op->bias[c0 + c] : 0.0f;
                for (dy = dy0; dy < dy1; dy++)
                    for (dx = 0; dx < kw; dx++) {
                        const float *x = win + ((size_t)(t * op->sw + dx) * Hi + (unsigned)(r0 + (int)dy)) * C + c0;
                        const float *wt = op->w + (size_t)(dy * kw + dx) * C + c0;
                        for (c = 0; c < cn; c++) acc[c] = acc[c] + wt[c] * x[c];
                    }
                for (c = 0; c < cn; c++) o[c0 + c] = relu ? relu_f(acc[c]) : acc[c];
            }
        }
}

#ifdef VOCR_VFPU
/* s[q * 8 + j] = bias[j] (or 0) + w[0][j] x_q[0] + w[1][j] x_q[1] + ... for 4 positions
 * x_q = x + q * xs and the 8 outputs of w ([K][8]): per k, vscl.q rounds each w * x and vadd.q
 * each sum, lane by lane, like the scalar code. M1 / M2 hold the 4 x 4 low / high sums, M0 the
 * weights (C000, C010) and the 4 inputs (S020..S023), M3 / M4 the products. w, bias and s
 * must be 16-byte aligned. */
static void vfpu_4x8(const float *w, const float *bias, const float *x, size_t xs, unsigned K,
                     float *s)
{
    const float *x0 = x, *x1 = x + xs, *x2 = x1 + xs, *x3 = x2 + xs;
    unsigned k;
    if (bias)
        __asm__ volatile("lv.q C100, 0(%0)\n\tlv.q C200, 16(%0)\n\t"
                         "vmov.q C110, C100\n\tvmov.q C120, C100\n\tvmov.q C130, C100\n\t"
                         "vmov.q C210, C200\n\tvmov.q C220, C200\n\tvmov.q C230, C200\n"
                         : : "r"(bias) : "memory");
    else
        __asm__ volatile("vmzero.q M100\n\tvmzero.q M200\n" : : : "memory");
    for (k = 0; k < K; k++)
        __asm__ volatile("lv.q C000, 0(%0)\n\tlv.q C010, 16(%0)\n\t"
                         "lv.s S020, 0(%1)\n\tlv.s S021, 0(%2)\n\t"
                         "lv.s S022, 0(%3)\n\tlv.s S023, 0(%4)\n\t"
                         "vscl.q C300, C000, S020\n\tvscl.q C310, C000, S021\n\t"
                         "vscl.q C320, C000, S022\n\tvscl.q C330, C000, S023\n\t"
                         "vscl.q C400, C010, S020\n\tvscl.q C410, C010, S021\n\t"
                         "vscl.q C420, C010, S022\n\tvscl.q C430, C010, S023\n\t"
                         "vadd.q C100, C100, C300\n\tvadd.q C110, C110, C310\n\t"
                         "vadd.q C120, C120, C320\n\tvadd.q C130, C130, C330\n\t"
                         "vadd.q C200, C200, C400\n\tvadd.q C210, C210, C410\n\t"
                         "vadd.q C220, C220, C420\n\tvadd.q C230, C230, C430\n"
                         : : "r"(w + (size_t)k * 8), "r"(x0 + k), "r"(x1 + k), "r"(x2 + k), "r"(x3 + k)
                         : "memory");
    __asm__ volatile("sv.q C100, 0(%0)\n\tsv.q C200, 16(%0)\n\tsv.q C110, 32(%0)\n\t"
                     "sv.q C210, 48(%0)\n\tsv.q C120, 64(%0)\n\tsv.q C220, 80(%0)\n\t"
                     "sv.q C130, 96(%0)\n\tsv.q C230, 112(%0)\n"
                     : : "r"(s) : "memory");
}
#endif

/* Float block: y[p][j] = act(bias[j] + sum_k w[k][j] x[p][k]) (+ res[p][j]) for the
 * 8 outputs of one column block w ([K][8]) and positions p < P. */
static void mm_f32(const float *w, const float *bias, const float *x, size_t xs, unsigned K,
                   unsigned P, float *y, size_t ys, const float *res, size_t rs, int relu)
{
    unsigned p = 0, q, j, k;
#ifdef VOCR_VFPU
    if (!(((uintptr_t)w | (uintptr_t)bias) & 15u))
        for (; p + 4 <= P; p += 4) {
            float buf[32 + 4], *s = (float *)(((uintptr_t)buf + 15u) & ~(uintptr_t)15u);
            vfpu_4x8(w, bias, x + p * xs, xs, K, s);
            for (q = 0; q < 4; q++)
                for (j = 0; j < 8; j++) {
                    float v = relu ? relu_f(s[q * 8 + j]) : s[q * 8 + j];
                    if (res) v = v + res[(p + q) * rs + j];
                    y[(p + q) * ys + j] = v;
                }
        }
#endif
#ifdef VOCR_NEON
    const float32x4_t z = vdupq_n_f32(0.0f);
    const float32x4_t bl = bias ? vld1q_f32(bias) : z, bh = bias ? vld1q_f32(bias + 4) : z;
    for (; p + 4 <= P; p += 4) {
        const float *x0 = x + p * xs, *x1 = x0 + xs, *x2 = x1 + xs, *x3 = x2 + xs;
        float32x4_t a0l = bl, a0h = bh, a1l = bl, a1h = bh, a2l = bl, a2h = bh, a3l = bl, a3h = bh;
        float32x4_t r[8];
        for (k = 0; k + 4 <= K; k += 4) {
            const float32x4_t v0 = vld1q_f32(x0 + k), v1 = vld1q_f32(x1 + k);
            const float32x4_t v2 = vld1q_f32(x2 + k), v3 = vld1q_f32(x3 + k);
            const float *wk = w + (size_t)k * 8;
#define VOCR_STEP(LANEV, LANE, OFF)                                                         \
    do {                                                                                     \
        const float32x4_t wl = vld1q_f32(wk + (OFF)), wh = vld1q_f32(wk + (OFF) + 4);       \
        a0l = vmlaq_lane_f32(a0l, wl, LANEV(v0), LANE);                                      \
        a0h = vmlaq_lane_f32(a0h, wh, LANEV(v0), LANE);                                      \
        a1l = vmlaq_lane_f32(a1l, wl, LANEV(v1), LANE);                                      \
        a1h = vmlaq_lane_f32(a1h, wh, LANEV(v1), LANE);                                      \
        a2l = vmlaq_lane_f32(a2l, wl, LANEV(v2), LANE);                                      \
        a2h = vmlaq_lane_f32(a2h, wh, LANEV(v2), LANE);                                      \
        a3l = vmlaq_lane_f32(a3l, wl, LANEV(v3), LANE);                                      \
        a3h = vmlaq_lane_f32(a3h, wh, LANEV(v3), LANE);                                      \
    } while (0)
            VOCR_STEP(vget_low_f32, 0, 0);
            VOCR_STEP(vget_low_f32, 1, 8);
            VOCR_STEP(vget_high_f32, 0, 16);
            VOCR_STEP(vget_high_f32, 1, 24);
#undef VOCR_STEP
        }
        for (; k < K; k++) {
            const float32x4_t wl = vld1q_f32(w + (size_t)k * 8), wh = vld1q_f32(w + (size_t)k * 8 + 4);
            a0l = vmlaq_n_f32(a0l, wl, x0[k]);
            a0h = vmlaq_n_f32(a0h, wh, x0[k]);
            a1l = vmlaq_n_f32(a1l, wl, x1[k]);
            a1h = vmlaq_n_f32(a1h, wh, x1[k]);
            a2l = vmlaq_n_f32(a2l, wl, x2[k]);
            a2h = vmlaq_n_f32(a2h, wh, x2[k]);
            a3l = vmlaq_n_f32(a3l, wl, x3[k]);
            a3h = vmlaq_n_f32(a3h, wh, x3[k]);
        }
        r[0] = a0l, r[1] = a0h, r[2] = a1l, r[3] = a1h, r[4] = a2l, r[5] = a2h, r[6] = a3l, r[7] = a3h;
        for (q = 0; q < 4; q++) {
            float32x4_t lo = r[2 * q], hi = r[2 * q + 1];
            if (relu) {
                lo = vmaxq_f32(lo, z);
                hi = vmaxq_f32(hi, z);
            }
            if (res) {
                lo = vaddq_f32(lo, vld1q_f32(res + (p + q) * rs));
                hi = vaddq_f32(hi, vld1q_f32(res + (p + q) * rs + 4));
            }
            vst1q_f32(y + (p + q) * ys, lo);
            vst1q_f32(y + (p + q) * ys + 4, hi);
        }
    }
    for (; p < P; p++) {
        const float *x0 = x + p * xs;
        float32x4_t lo = bl, hi = bh;
        for (k = 0; k < K; k++) {
            lo = vmlaq_n_f32(lo, vld1q_f32(w + (size_t)k * 8), x0[k]);
            hi = vmlaq_n_f32(hi, vld1q_f32(w + (size_t)k * 8 + 4), x0[k]);
        }
        if (relu) {
            lo = vmaxq_f32(lo, z);
            hi = vmaxq_f32(hi, z);
        }
        if (res) {
            lo = vaddq_f32(lo, vld1q_f32(res + p * rs));
            hi = vaddq_f32(hi, vld1q_f32(res + p * rs + 4));
        }
        vst1q_f32(y + p * ys, lo);
        vst1q_f32(y + p * ys + 4, hi);
    }
#endif
#ifndef VOCR_NEON /* the NEON loops above leave p == P */
    /* Scalar: two positions x 8 outputs per pass with all 16 sums in registers. With the sums
     * in an array (the loop below, now only the odd tail) gcc keeps them in memory, a load and
     * a store per multiply-add: 8 instructions each on the PSP's Allegrex instead of about 3.
     * The order of every sum is the same, so are the bits. */
    for (; p + 2 <= P; p += 2) {
        const float *x0 = x + p * xs, *x1 = x0 + xs;
        float a0, a1, a2, a3, a4, a5, a6, a7, b0, b1, b2, b3, b4, b5, b6, b7, s[16];
        if (bias) {
            a0 = b0 = bias[0];
            a1 = b1 = bias[1];
            a2 = b2 = bias[2];
            a3 = b3 = bias[3];
            a4 = b4 = bias[4];
            a5 = b5 = bias[5];
            a6 = b6 = bias[6];
            a7 = b7 = bias[7];
        } else {
            a0 = a1 = a2 = a3 = a4 = a5 = a6 = a7 = 0.0f;
            b0 = b1 = b2 = b3 = b4 = b5 = b6 = b7 = 0.0f;
        }
        for (k = 0; k < K; k++) {
            const float *wk = w + (size_t)k * 8;
            const float v0 = x0[k], v1 = x1[k];
            a0 = a0 + wk[0] * v0;
            b0 = b0 + wk[0] * v1;
            a1 = a1 + wk[1] * v0;
            b1 = b1 + wk[1] * v1;
            a2 = a2 + wk[2] * v0;
            b2 = b2 + wk[2] * v1;
            a3 = a3 + wk[3] * v0;
            b3 = b3 + wk[3] * v1;
            a4 = a4 + wk[4] * v0;
            b4 = b4 + wk[4] * v1;
            a5 = a5 + wk[5] * v0;
            b5 = b5 + wk[5] * v1;
            a6 = a6 + wk[6] * v0;
            b6 = b6 + wk[6] * v1;
            a7 = a7 + wk[7] * v0;
            b7 = b7 + wk[7] * v1;
        }
        s[0] = a0, s[1] = a1, s[2] = a2, s[3] = a3, s[4] = a4, s[5] = a5, s[6] = a6, s[7] = a7;
        s[8] = b0, s[9] = b1, s[10] = b2, s[11] = b3, s[12] = b4, s[13] = b5, s[14] = b6, s[15] = b7;
        for (q = 0; q < 2; q++)
            for (j = 0; j < 8; j++) {
                float v = relu ? relu_f(s[q * 8 + j]) : s[q * 8 + j];
                if (res) v = v + res[(p + q) * rs + j];
                y[(p + q) * ys + j] = v;
            }
    }
#endif
    for (; p < P; p += 4) {
        const unsigned pn = umin(4u, P - p);
        float acc[4][8];
        for (q = 0; q < pn; q++)
            for (j = 0; j < 8; j++) acc[q][j] = bias ? bias[j] : 0.0f;
        for (k = 0; k < K; k++) {
            const float *wk = w + (size_t)k * 8;
            for (q = 0; q < pn; q++) {
                const float xv = x[(p + q) * xs + k];
                for (j = 0; j < 8; j++) acc[q][j] = acc[q][j] + wk[j] * xv;
            }
        }
        for (q = 0; q < pn; q++)
            for (j = 0; j < 8; j++) {
                float v = relu ? relu_f(acc[q][j]) : acc[q][j];
                if (res) v = v + res[(p + q) * rs + j];
                y[(p + q) * ys + j] = v;
            }
    }
}

/* int8 column block -> float: wf[k][j] = q[k][j] * scale[j]. */
static void dequant8(const int8_t *qw, const float *scale, unsigned K, float *wf)
{
    unsigned k = 0;
#ifdef VOCR_NEON
    const float32x4_t sl = vld1q_f32(scale), sh = vld1q_f32(scale + 4);
    for (; k < K; k++) {
        const int16x8_t v = vmovl_s8(vld1_s8(qw + (size_t)k * 8));
        vst1q_f32(wf + (size_t)k * 8, vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(v))), sl));
        vst1q_f32(wf + (size_t)k * 8 + 4, vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(v))), sh));
    }
#endif
#ifndef VOCR_NEON /* the NEON loop above leaves k == K */
    if (k < K) { /* scalar: the 8 scales in registers, 5 instructions per weight on Allegrex */
        const float s0 = scale[0], s1 = scale[1], s2 = scale[2], s3 = scale[3];
        const float s4 = scale[4], s5 = scale[5], s6 = scale[6], s7 = scale[7];
        for (; k < K; k++) {
            const int8_t *q = qw + (size_t)k * 8;
            float *f = wf + (size_t)k * 8;
            f[0] = (float)q[0] * s0;
            f[1] = (float)q[1] * s1;
            f[2] = (float)q[2] * s2;
            f[3] = (float)q[3] * s3;
            f[4] = (float)q[4] * s4;
            f[5] = (float)q[5] * s5;
            f[6] = (float)q[6] * s6;
            f[7] = (float)q[7] * s7;
        }
    }
#endif
}

/* Per-position int16 quantisation: q = trunc(x * (32767 / max|x|) +- 0.5), and the
 * dequantisation scale max|x| / 32767 (0 for an all-zero position). */
static void quant16_plain(const float *x, size_t xs, unsigned K, unsigned P, int16_t *qx, float *sx)
{
    unsigned p, k;
    for (p = 0; p < P; p++) {
        const float *xp = x + p * xs;
        int16_t *qp = qx + (size_t)p * K;
        float mx = 0.0f, inv;
        k = 0;
#ifdef VOCR_NEON
        if (K >= 4) {
            float32x4_t m4 = vabsq_f32(vld1q_f32(xp));
            float32x2_t m2;
            for (k = 4; k + 4 <= K; k += 4) m4 = vmaxq_f32(m4, vabsq_f32(vld1q_f32(xp + k)));
            m2 = vpmax_f32(vget_low_f32(m4), vget_high_f32(m4));
            m2 = vpmax_f32(m2, m2);
            mx = vget_lane_f32(m2, 0);
        }
#endif
        for (; k < K; k++) {
            const float a = fabsf(xp[k]);
            if (a > mx) mx = a;
        }
        if (!(mx > 0.0f)) {
            memset(qp, 0, K * sizeof(int16_t));
            sx[p] = 0.0f;
            continue;
        }
        inv = 32767.0f / mx;
        sx[p] = mx / 32767.0f;
        k = 0;
#ifdef VOCR_NEON
        {
            const float32x4_t vinv = vdupq_n_f32(inv);
            const uint32x4_t sign = vdupq_n_u32(0x80000000u), half = vreinterpretq_u32_f32(vdupq_n_f32(0.5f));
            for (; k + 4 <= K; k += 4) {
                const float32x4_t t = vmulq_f32(vld1q_f32(xp + k), vinv);
                const float32x4_t h = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(vreinterpretq_u32_f32(t), sign), half));
                vst1_s16(qp + k, vqmovn_s32(vcvtq_s32_f32(vaddq_f32(t, h))));
            }
        }
#endif
        for (; k < K; k++) {
            const float t = xp[k] * inv;
            int v = (int)(t + (signbit(t) ? -0.5f : 0.5f));
            qp[k] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
        }
    }
}

#ifdef VOCR_SIMD32
#if defined(__GNUC__)
typedef uint32_t vocr_word __attribute__((may_alias));
#else
typedef uint32_t vocr_word;
#endif
/* quant16_plain's numbers for an even K, laid out for vocr__k2x8 (Kotoba3DS addition): positions
 * 2i and 2i + 1 interleaved by pairs of k, each pair (q(k), q(k + 1)) one word, and an odd last
 * position stored as usual. Element k of position p is at (p & ~1) K + (k & ~1) 2 + (p & 1) 2
 * + (k & 1). The same numbers by other steps, without the VFP compares (each waits for the VFP
 * pipeline on the ARM11):
 *  - max |x| as the largest of the values' bits with the sign cleared (the bits of non-negative
 *    floats order as the floats do);
 *  - t + (+-0.5 by the sign of t) as +-(|t| + 0.5): IEEE rounding is symmetric, and the sign of
 *    t = x * inv is x's, as inv > 0;
 *  - no clamp to +-32767: |x| <= max|x| keeps |t| below 32767.01, so the sum truncates to at
 *    most 32767. */
static void quant16_pairs(const float *x, size_t xs, unsigned K, unsigned P, int16_t *qx, float *sx)
{
    unsigned p, k;
    for (p = 0; p < P; p++) {
        const float *xp = x + p * xs;
        const vocr_word *xb = (const vocr_word *)(const void *)xp;
        const int il = (p | 1u) < P;
        vocr_word *qw = (vocr_word *)(void *)(il ? qx + (size_t)(p & ~1u) * K + (p & 1u) * 2u : qx + (size_t)p * K);
        const unsigned step = il ? 2u : 1u; /* words from one pair of k to the next */
        uint32_t mb = 0;
        float mx, inv;
        for (k = 0; k < K; k++) {
            const uint32_t a = xb[k] & 0x7FFFFFFFu;
            if (a > mb) mb = a;
        }
        memcpy(&mx, &mb, sizeof mx);
        if (!(mx > 0.0f)) {
            for (k = 0; k < K; k += 2) qw[(k / 2u) * step] = 0;
            sx[p] = 0.0f;
            continue;
        }
        inv = 32767.0f / mx;
        sx[p] = mx / 32767.0f;
        for (k = 0; k < K; k += 2, qw += step) {
            int v0 = (int)(fabsf(xp[k] * inv) + 0.5f), v1 = (int)(fabsf(xp[k + 1] * inv) + 0.5f);
            if ((int32_t)xb[k] < 0) v0 = -v0;
            if ((int32_t)xb[k + 1] < 0) v1 = -v1;
            *qw = (uint32_t)(uint16_t)v0 | (uint32_t)v1 << 16;
        }
    }
}
#endif

static void quant16(const float *x, size_t xs, unsigned K, unsigned P, int16_t *qx, float *sx, int pairs)
{
#ifdef VOCR_SIMD32
    if (pairs) {
        quant16_pairs(x, xs, K, P, qx, sx);
        return;
    }
#endif
    (void)pairs;
    quant16_plain(x, xs, K, P, qx, sx);
}

/* Integer block: y[p][j] = act(S[p][j] * (scale[j] * sx[p]) + bias[j]), where
 * S = 0 + float(c_0) + float(c_1) + ... (float adds, in order) and c_i is the exact int32
 * sum of q[k][j] * qx[p][k] over k in [512 i, 512 i + 512). 512 * 127 * 32767 < 2^31, so
 * no chunk can overflow, and for K <= 512 S is just float(sum). */
#ifdef VOCR_SIMD32
/* Two positions at once (Kotoba3DS addition): out[0..7] and out[8..15] = the exact int32 sums of
 * w(k, j) * x(k) over npairs pairs of k for two positions p, p + 1, from packed weights w (16
 * bytes per pair of k, as vocr_pack_simd32 leaves them) and a pair-interleaved input x: for each
 * pair of k, one word (x_p(k), x_p(k + 1)) then one word (x_p+1(k), x_p+1(k + 1)). Each weight is
 * sign-extended once for both positions: 16 instructions per 16 multiply-accumulates where one
 * position at a time takes 23. The ARM version keeps both positions' sums for 4 outputs in r4-r11
 * and runs the 8 outputs as two halves; its loop is software-pipelined, so it reads (and ignores)
 * up to 16 bytes past the weights and 8 past x. */
#if defined(__ARM_FEATURE_SIMD32) && defined(__arm__) && !defined(__thumb__) && !defined(VOCR_SIMD32_EMULATE)
void vocr__k2x8(const void *w, const void *x, unsigned npairs, int32_t *out);
/* r0 w, r1 x, r2 xa, r3 xb, r12 a weight word, lr its sign-extended halves; r4-r7 position p's
 * sums for the half's 4 outputs, r8-r11 position p + 1's; [sp] out, w, x, x end */
__asm__(".pushsection .text.vocr__k2x8,\"ax\",%progbits\n"
        ".syntax unified\n"
        ".arm\n"
        ".align 2\n"
        ".global vocr__k2x8\n"
        ".type vocr__k2x8, %function\n"
        "vocr__k2x8:\n"
        "    push    {r4-r11, lr}\n"
        "    sub     sp, sp, #16\n"
        "    add     r2, r1, r2, lsl #3\n"
        "    str     r3, [sp, #0]\n"
        "    str     r0, [sp, #4]\n"
        "    str     r1, [sp, #8]\n"
        "    str     r2, [sp, #12]\n"
        ".macro VOCR_K2X8_PAIR\n"
        "    sxtb16  lr, r12\n"
        "    smlad   r6, lr, r2, r6\n"
        "    smlad   r10, lr, r3, r10\n"
        "    sxtb16  lr, r12, ror #8\n"
        "    ldr     r12, [r0], #16\n"
        "    smlad   r7, lr, r2, r7\n"
        "    smlad   r11, lr, r3, r11\n"
        "    sxtb16  lr, r12\n"
        "    smlad   r4, lr, r2, r4\n"
        "    smlad   r8, lr, r3, r8\n"
        "    sxtb16  lr, r12, ror #8\n"
        "    ldr     r12, [r0, #4]\n"
        "    smlad   r5, lr, r2, r5\n"
        "    ldr     r2, [r1], #4\n"
        "    smlad   r9, lr, r3, r9\n"
        "    ldr     r3, [r1], #4\n"
        ".endm\n"
        ".macro VOCR_K2X8_HALF off\n"
        "    ldr     r0, [sp, #4]\n"
        "    ldr     r1, [sp, #8]\n"
        "    ldr     lr, [sp, #12]\n"
        "    add     r0, r0, #(\\off / 2)\n"
        "    sub     lr, lr, r1\n"
        "    mov     r4, #0\n"
        "    mov     r5, #0\n"
        "    mov     r6, #0\n"
        "    mov     r7, #0\n"
        "    mov     r8, #0\n"
        "    mov     r9, #0\n"
        "    mov     r10, #0\n"
        "    mov     r11, #0\n"
        "    ldr     r12, [r0, #4]\n"
        "    ldr     r2, [r1], #4\n"
        "    ldr     r3, [r1], #4\n"
        "    tst     lr, #8\n"
        "    bne     2f\n"
        "1:\n"
        "    VOCR_K2X8_PAIR\n"
        "2:\n"
        "    VOCR_K2X8_PAIR\n"
        "    ldr     lr, [sp, #12]\n"
        "    cmp     r1, lr\n"
        "    bls     1b\n"
        "    ldr     lr, [sp, #0]\n"
        "    add     lr, lr, #\\off\n"
        "    stmia   lr, {r4-r7}\n"
        "    add     lr, lr, #32\n"
        "    stmia   lr, {r8-r11}\n"
        ".endm\n"
        "    VOCR_K2X8_HALF 0\n"
        "    VOCR_K2X8_HALF 16\n"
        "    add     sp, sp, #16\n"
        "    pop     {r4-r11, pc}\n"
        ".purgem VOCR_K2X8_PAIR\n"
        ".purgem VOCR_K2X8_HALF\n"
        ".size vocr__k2x8, .-vocr__k2x8\n"
        ".popsection\n");
#else
static void vocr__k2x8(const void *w, const void *x, unsigned npairs, int32_t *out)
{
    const uint32_t *wp = (const uint32_t *)w, *xp = (const uint32_t *)x;
    unsigned i, h;
    for (h = 0; h < 16; h++) out[h] = 0;
    for (i = 0; i < npairs; i++, wp += 4, xp += 2)
        for (h = 0; h < 4; h++) {
            const uint32_t lo = vocr_sxtb16(wp[h]), hi = vocr_sxtb16(vocr_ror8(wp[h]));
            out[2 * h] = vocr_smlad(lo, xp[0], out[2 * h]);
            out[2 * h + 1] = vocr_smlad(hi, xp[0], out[2 * h + 1]);
            out[8 + 2 * h] = vocr_smlad(lo, xp[1], out[8 + 2 * h]);
            out[8 + 2 * h + 1] = vocr_smlad(hi, xp[1], out[8 + 2 * h + 1]);
        }
}
#endif
#endif

#ifdef VOCR_SIMD32
/* mm_i16 for packed weights (vocr_pack_simd32): for each pair of k, 16 bytes hold
 * [w(k,j) w(k,j+1) w(k+1,j) w(k+1,j+1)] for j = 0, 2, 4, 6; an odd last k is unpacked. */
static void mm_i16_simd32(const int8_t *qw, const float *scale, const float *bias, const int16_t *qx,
                          const float *sx, unsigned K, unsigned P, float *y, size_t ys, const float *res,
                          size_t rs, int relu)
{
    unsigned p, j, k, k0;
    for (p = 0; p < P; p++) {
        const int16_t *xp = qx + (size_t)p * K;
        float fs[8];
        for (j = 0; j < 8; j++) fs[j] = 0.0f;
        for (k0 = 0; k0 < K; k0 += W8I_CHUNK) { /* W8I_CHUNK is even: pairs never straddle */
            const unsigned k1 = umin(K, k0 + W8I_CHUNK);
            int32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0, c6 = 0, c7 = 0;
            const uint32_t *w = (const uint32_t *)(const void *)(qw + (size_t)k0 * 8);
#if defined(__ARM_FEATURE_SIMD32) && defined(__arm__) && !defined(__thumb__) && !defined(VOCR_SIMD32_EMULATE)
            /* the same loop in ARM assembly: SXTB16's built-in ROR #8, no register spills */
            {
                const int16_t *xk = xp + k0;
                const int16_t *xe = xp + k0 + ((k1 - k0) & ~1u);
                while (xk < xe) {
                    uint32_t t0, t1, t2;
                    __asm__ volatile(
                        "ldrh   %[t0], [%[x]], #2\n\t"
                        "ldrh   %[t1], [%[x]], #2\n\t"
                        "pkhbt  %[t0], %[t0], %[t1], lsl #16\n\t"
                        "ldr    %[t1], [%[w]], #4\n\t"
                        "sxtb16 %[t2], %[t1]\n\t"
                        "smlad  %[c0], %[t2], %[t0], %[c0]\n\t"
                        "sxtb16 %[t2], %[t1], ror #8\n\t"
                        "smlad  %[c1], %[t2], %[t0], %[c1]\n\t"
                        "ldr    %[t1], [%[w]], #4\n\t"
                        "sxtb16 %[t2], %[t1]\n\t"
                        "smlad  %[c2], %[t2], %[t0], %[c2]\n\t"
                        "sxtb16 %[t2], %[t1], ror #8\n\t"
                        "smlad  %[c3], %[t2], %[t0], %[c3]\n\t"
                        "ldr    %[t1], [%[w]], #4\n\t"
                        "sxtb16 %[t2], %[t1]\n\t"
                        "smlad  %[c4], %[t2], %[t0], %[c4]\n\t"
                        "sxtb16 %[t2], %[t1], ror #8\n\t"
                        "smlad  %[c5], %[t2], %[t0], %[c5]\n\t"
                        "ldr    %[t1], [%[w]], #4\n\t"
                        "sxtb16 %[t2], %[t1]\n\t"
                        "smlad  %[c6], %[t2], %[t0], %[c6]\n\t"
                        "sxtb16 %[t2], %[t1], ror #8\n\t"
                        "smlad  %[c7], %[t2], %[t0], %[c7]\n\t"
                        : [c0] "+r"(c0), [c1] "+r"(c1), [c2] "+r"(c2), [c3] "+r"(c3), [c4] "+r"(c4),
                          [c5] "+r"(c5), [c6] "+r"(c6), [c7] "+r"(c7), [x] "+r"(xk), [w] "+r"(w),
                          [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2)
                        :
                        : "memory");
                }
                k = k0 + ((k1 - k0) & ~1u);
            }
#else
            for (k = k0; k + 2 <= k1; k += 2, w += 4) {
                const uint32_t xv = (uint32_t)(uint16_t)xp[k] | ((uint32_t)(uint16_t)xp[k + 1] << 16);
                const uint32_t a = w[0], b = w[1], c = w[2], d = w[3];
                c0 = vocr_smlad(vocr_sxtb16(a), xv, c0);
                c1 = vocr_smlad(vocr_sxtb16(vocr_ror8(a)), xv, c1);
                c2 = vocr_smlad(vocr_sxtb16(b), xv, c2);
                c3 = vocr_smlad(vocr_sxtb16(vocr_ror8(b)), xv, c3);
                c4 = vocr_smlad(vocr_sxtb16(c), xv, c4);
                c5 = vocr_smlad(vocr_sxtb16(vocr_ror8(c)), xv, c5);
                c6 = vocr_smlad(vocr_sxtb16(d), xv, c6);
                c7 = vocr_smlad(vocr_sxtb16(vocr_ror8(d)), xv, c7);
            }
#endif
            if (k < k1) { /* odd K: the last k as it was */
                const int8_t *wk = qw + (size_t)k * 8;
                const int32_t xv = xp[k];
                c0 += (int32_t)wk[0] * xv;
                c1 += (int32_t)wk[1] * xv;
                c2 += (int32_t)wk[2] * xv;
                c3 += (int32_t)wk[3] * xv;
                c4 += (int32_t)wk[4] * xv;
                c5 += (int32_t)wk[5] * xv;
                c6 += (int32_t)wk[6] * xv;
                c7 += (int32_t)wk[7] * xv;
            }
            fs[0] = fs[0] + (float)c0;
            fs[1] = fs[1] + (float)c1;
            fs[2] = fs[2] + (float)c2;
            fs[3] = fs[3] + (float)c3;
            fs[4] = fs[4] + (float)c4;
            fs[5] = fs[5] + (float)c5;
            fs[6] = fs[6] + (float)c6;
            fs[7] = fs[7] + (float)c7;
        }
        for (j = 0; j < 8; j++) {
            float v = fs[j] * (scale[j] * sx[p]);
            if (bias) v = v + bias[j];
            if (relu) v = relu_f(v);
            if (res) v = v + res[p * rs + j];
            y[p * ys + j] = v;
        }
    }
}
#define VOCR_MM_I16(m, ...) ((m)->packed ? mm_i16_simd32(__VA_ARGS__) : mm_i16(__VA_ARGS__))
#else
#define VOCR_MM_I16(m, ...) mm_i16(__VA_ARGS__)
#endif
#ifdef VOCR_SIMD32
/* mm_i16_simd32 for a pair-interleaved qx (quant16 with pairs): positions two at a time through
 * vocr__k2x8; an odd last position is stored as it was and goes through mm_i16_simd32. The
 * integer sums are the same exact sums, and the float steps are mm_i16_simd32's in its order. */
/* the outputs of one pair of positions from their 16 float sums, for each kind of layer (the
 * flags are constants where it is inlined, so each kind gets its own loop) */
#if defined(__GNUC__)
#define VOCR_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define VOCR_ALWAYS_INLINE
#endif
static VOCR_ALWAYS_INLINE void pair_out(const float *fs, const int32_t *is, const float *scale, const float *bias,
                                        const float *sx, float *y, size_t ys, const float *res, size_t rs, int relu)
{
    const float s0 = sx[0], s1 = sx[1];
    unsigned j;
    for (j = 0; j < 8; j++) { /* the sums as floats (fs), or the one chunk's ints (is): float(c) */
        float v = (is ? (float)is[j] : fs[j]) * (scale[j] * s0);
        float u = (is ? (float)is[8 + j] : fs[8 + j]) * (scale[j] * s1);
        if (bias) v = v + bias[j], u = u + bias[j];
        if (relu) v = relu_f(v), u = relu_f(u);
        if (res) v = v + res[j], u = u + res[rs + j];
        y[j] = v;
        y[ys + j] = u;
    }
}

static void mm_i16_pairs(const int8_t *qw, const float *scale, const float *bias, const int16_t *qx,
                         const float *sx, unsigned K, unsigned P, float *y, size_t ys, const float *res,
                         size_t rs, int relu)
{
    const int kind = (bias ? 1 : 0) | (relu ? 2 : 0) | (res ? 4 : 0);
    unsigned p, j, k0;
/* pair_out for this kind of layer, with constant flags (each case its own loop) */
#define VOCR_PAIR_OUT(FS, IS)                                                                        \
    do {                                                                                             \
        float *yp_ = y + p * ys;                                                                     \
        const float *rp_ = res ? res + p * rs : NULL;                                                \
        switch (kind) {                                                                              \
        case 0: pair_out(FS, IS, scale, NULL, sx + p, yp_, ys, NULL, rs, 0); break;                  \
        case 1: pair_out(FS, IS, scale, bias, sx + p, yp_, ys, NULL, rs, 0); break;                  \
        case 2: pair_out(FS, IS, scale, NULL, sx + p, yp_, ys, NULL, rs, 1); break;                  \
        case 3: pair_out(FS, IS, scale, bias, sx + p, yp_, ys, NULL, rs, 1); break;                  \
        case 4: pair_out(FS, IS, scale, NULL, sx + p, yp_, ys, rp_, rs, 0); break;                   \
        case 5: pair_out(FS, IS, scale, bias, sx + p, yp_, ys, rp_, rs, 0); break;                   \
        case 6: pair_out(FS, IS, scale, NULL, sx + p, yp_, ys, rp_, rs, 1); break;                   \
        default: pair_out(FS, IS, scale, bias, sx + p, yp_, ys, rp_, rs, 1); break;                  \
        }                                                                                            \
    } while (0)
    for (p = 0; p + 2 <= P; p += 2) {
        const int16_t *xp = qx + (size_t)p * K;
        int32_t s[16];
        if (K <= W8I_CHUNK) { /* one chunk: S = 0 + float(c) = float(c) (an int's float is never -0) */
            vocr__k2x8(qw, xp, K / 2, s);
            VOCR_PAIR_OUT(NULL, s);
        } else {
            float fs[16];
            for (j = 0; j < 16; j++) fs[j] = 0.0f;
            for (k0 = 0; k0 < K; k0 += W8I_CHUNK) {
                const unsigned k1 = umin(K, k0 + W8I_CHUNK);
                vocr__k2x8(qw + (size_t)k0 * 8, xp + (size_t)k0 * 2, (k1 - k0) / 2, s);
                for (j = 0; j < 16; j++) fs[j] = fs[j] + (float)s[j];
            }
            VOCR_PAIR_OUT(fs, NULL);
        }
    }
#undef VOCR_PAIR_OUT
    if (p < P)
        mm_i16_simd32(qw, scale, bias, qx + (size_t)p * K, sx + p, K, 1, y + p * ys, ys, res ? res + p * rs : NULL,
                      rs, relu);
}
/* pair-interleaved input for this layer: the SIMD32 weights and an even K */
#define VOCR_PAIRS(m, K) (vocr_use_pairs && (m)->packed && !((K) & 1u))
#define VOCR_MM_I16P(m, pairs, ...) ((pairs) ? mm_i16_pairs(__VA_ARGS__) : VOCR_MM_I16(m, __VA_ARGS__))
#else
#define VOCR_PAIRS(m, K) 0
#define VOCR_MM_I16P(m, pairs, ...) VOCR_MM_I16(m, __VA_ARGS__)
#endif

static void mm_i16(const int8_t *qw, const float *scale, const float *bias, const int16_t *qx,
                   const float *sx, unsigned K, unsigned P, float *y, size_t ys, const float *res,
                   size_t rs, int relu)
{
    unsigned p = 0, q = 0, j, k, k0;
#ifdef VOCR_NEON
    /* NEON: 4 positions x 8 outputs per pass, the 32 int32 sums in eight named accumulators
     * (not an array: with int32x4_t a[8], gcc 14 and 15 store all eight to the stack on every
     * pass of the k loop). Each pass reads 8 k of every position's input (one 128-bit load
     * each) and the 8 weight rows of those k in four 128-bit loads, so 64 VMLAL share 8 loads;
     * nothing else is live in the loop (15 of the 16 Q registers), so nothing spills. The
     * integer sums are exact in any order and the float steps are the scalar code's, in its
     * order, so the bits are the scalar code's. With one chunk (K <= 512, every layer of the
     * VOCR models but the widest collapse) S = 0 + float(c_0) is float(c_0) itself: float()
     * of an int is never -0. */
#define VOCR_IROW(W, X0, X1, X2, X3, LANE)                                                   \
    do {                                                                                     \
        const int16x8_t wv_ = (W);                                                           \
        const int16x4_t wl_ = vget_low_s16(wv_), wh_ = vget_high_s16(wv_);                   \
        a0 = vmlal_lane_s16(a0, wl_, X0, LANE);                                              \
        a1 = vmlal_lane_s16(a1, wh_, X0, LANE);                                              \
        a2 = vmlal_lane_s16(a2, wl_, X1, LANE);                                              \
        a3 = vmlal_lane_s16(a3, wh_, X1, LANE);                                              \
        a4 = vmlal_lane_s16(a4, wl_, X2, LANE);                                              \
        a5 = vmlal_lane_s16(a5, wh_, X2, LANE);                                              \
        a6 = vmlal_lane_s16(a6, wl_, X3, LANE);                                              \
        a7 = vmlal_lane_s16(a7, wh_, X3, LANE);                                              \
    } while (0)
#define VOCR_IHALF(W8A, W8B, H)                                                              \
    do {                                                                                     \
        const int16x4_t h0_ = H(v0), h1_ = H(v1), h2_ = H(v2), h3_ = H(v3);                  \
        VOCR_IROW(vmovl_s8(vget_low_s8(W8A)), h0_, h1_, h2_, h3_, 0);                        \
        VOCR_IROW(vmovl_s8(vget_high_s8(W8A)), h0_, h1_, h2_, h3_, 1);                       \
        VOCR_IROW(vmovl_s8(vget_low_s8(W8B)), h0_, h1_, h2_, h3_, 2);                        \
        VOCR_IROW(vmovl_s8(vget_high_s8(W8B)), h0_, h1_, h2_, h3_, 3);                       \
    } while (0)
    /* the int32 sums of k in [K0, K1) of the 4 positions at P0 x0..x3 */
#define VOCR_ISUMS4(K0, K1)                                                                  \
    do {                                                                                     \
        const int16_t *p0_ = x0 + (K0), *p1_ = x1 + (K0), *p2_ = x2 + (K0), *p3_ = x3 + (K0); \
        const int8_t *wk_ = qw + (size_t)(K0) * 8;                                           \
        a0 = a1 = a2 = a3 = a4 = a5 = a6 = a7 = vdupq_n_s32(0);                              \
        for (k = (K0); k + 8 <= (K1); k += 8, wk_ += 64, p0_ += 8, p1_ += 8, p2_ += 8, p3_ += 8) { \
            int16x8_t v0, v1, v2, v3;                                                        \
            int8x16x2_t w_;                                                                  \
            VOCR_PREFETCH(wk_ + 128);                                                        \
            VOCR_PREFETCH(wk_ + 160);                                                        \
            v0 = vld1q_s16(p0_), v1 = vld1q_s16(p1_), v2 = vld1q_s16(p2_), v3 = vld1q_s16(p3_); \
            w_ = vld1q_s8_x2(wk_);                                                           \
            VOCR_IHALF(w_.val[0], w_.val[1], vget_low_s16);                                  \
            w_ = vld1q_s8_x2(wk_ + 32);                                                      \
            VOCR_IHALF(w_.val[0], w_.val[1], vget_high_s16);                                 \
        }                                                                                    \
        for (; k < (K1); k++, wk_ += 8) {                                                    \
            const int16x8_t wv_ = vmovl_s8(vld1_s8(wk_));                                    \
            const int16x4_t wl_ = vget_low_s16(wv_), wh_ = vget_high_s16(wv_);               \
            a0 = vmlal_n_s16(a0, wl_, x0[k]);                                                \
            a1 = vmlal_n_s16(a1, wh_, x0[k]);                                                \
            a2 = vmlal_n_s16(a2, wl_, x1[k]);                                                \
            a3 = vmlal_n_s16(a3, wh_, x1[k]);                                                \
            a4 = vmlal_n_s16(a4, wl_, x2[k]);                                                \
            a5 = vmlal_n_s16(a5, wh_, x2[k]);                                                \
            a6 = vmlal_n_s16(a6, wl_, x3[k]);                                                \
            a7 = vmlal_n_s16(a7, wh_, x3[k]);                                                \
        }                                                                                    \
    } while (0)
    /* position P's 8 outputs from its float sums LO / HI, S the position's scale as a lane of
     * SXH (a D register): the scalar code's steps, in order (scale[j] * sx[p], times S, + bias,
     * relu, + res); a multiply by a lane is the multiply by that scalar, bit for bit. */
#define VOCR_IOUT(P, LO, HI, SXH, LANE)                                                      \
    do {                                                                                     \
        float32x4_t lo_ = vmulq_f32(LO, vmulq_lane_f32(vld1q_f32(scale), SXH, LANE));        \
        float32x4_t hi_ = vmulq_f32(HI, vmulq_lane_f32(vld1q_f32(scale + 4), SXH, LANE));    \
        if (bias) {                                                                          \
            lo_ = vaddq_f32(lo_, vld1q_f32(bias));                                           \
            hi_ = vaddq_f32(hi_, vld1q_f32(bias + 4));                                       \
        }                                                                                    \
        if (relu) {                                                                          \
            lo_ = vmaxq_f32(lo_, vdupq_n_f32(0.0f));                                         \
            hi_ = vmaxq_f32(hi_, vdupq_n_f32(0.0f));                                         \
        }                                                                                    \
        if (res) {                                                                           \
            lo_ = vaddq_f32(lo_, vld1q_f32(res + (P) * rs));                                 \
            hi_ = vaddq_f32(hi_, vld1q_f32(res + (P) * rs + 4));                             \
        }                                                                                    \
        vst1q_f32(y + (P) * ys, lo_);                                                        \
        vst1q_f32(y + (P) * ys + 4, hi_);                                                    \
    } while (0)
    for (; p + 4 <= P; p += 4) {
        const int16_t *x0 = qx + (size_t)p * K, *x1 = x0 + K, *x2 = x1 + K, *x3 = x2 + K;
        int32x4_t a0, a1, a2, a3, a4, a5, a6, a7;
        if (K <= W8I_CHUNK) {
            VOCR_ISUMS4(0u, K);
            {
                const float32x4_t sxv = vld1q_f32(sx + p);
                VOCR_IOUT(p, vcvtq_f32_s32(a0), vcvtq_f32_s32(a1), vget_low_f32(sxv), 0);
                VOCR_IOUT(p + 1, vcvtq_f32_s32(a2), vcvtq_f32_s32(a3), vget_low_f32(sxv), 1);
                VOCR_IOUT(p + 2, vcvtq_f32_s32(a4), vcvtq_f32_s32(a5), vget_high_f32(sxv), 0);
                VOCR_IOUT(p + 3, vcvtq_f32_s32(a6), vcvtq_f32_s32(a7), vget_high_f32(sxv), 1);
            }
        } else {
            float32x4_t fs[8]; /* the float sums of the chunks so far: S = 0 + float(c_0) + ... */
            for (q = 0; q < 8; q++) fs[q] = vdupq_n_f32(0.0f);
            for (k0 = 0; k0 < K; k0 += W8I_CHUNK) {
                const unsigned k1 = umin(K, k0 + W8I_CHUNK);
                VOCR_ISUMS4(k0, k1);
                fs[0] = vaddq_f32(fs[0], vcvtq_f32_s32(a0));
                fs[1] = vaddq_f32(fs[1], vcvtq_f32_s32(a1));
                fs[2] = vaddq_f32(fs[2], vcvtq_f32_s32(a2));
                fs[3] = vaddq_f32(fs[3], vcvtq_f32_s32(a3));
                fs[4] = vaddq_f32(fs[4], vcvtq_f32_s32(a4));
                fs[5] = vaddq_f32(fs[5], vcvtq_f32_s32(a5));
                fs[6] = vaddq_f32(fs[6], vcvtq_f32_s32(a6));
                fs[7] = vaddq_f32(fs[7], vcvtq_f32_s32(a7));
            }
            {
                const float32x4_t sxv = vld1q_f32(sx + p);
                VOCR_IOUT(p, fs[0], fs[1], vget_low_f32(sxv), 0);
                VOCR_IOUT(p + 1, fs[2], fs[3], vget_low_f32(sxv), 1);
                VOCR_IOUT(p + 2, fs[4], fs[5], vget_high_f32(sxv), 0);
                VOCR_IOUT(p + 3, fs[6], fs[7], vget_high_f32(sxv), 1);
            }
        }
    }
    /* The last 1-3 positions (head batches and line ends): one position x 8 outputs, four
     * accumulators (two per k parity), added at the end of each chunk (exact). */
    for (; p < P; p++) {
        const int16_t *xp = qx + (size_t)p * K;
        float32x4_t f0 = vdupq_n_f32(0.0f), f1 = f0;
        for (k0 = 0; k0 < K; k0 += W8I_CHUNK) {
            const unsigned k1 = umin(K, k0 + W8I_CHUNK);
            const int8_t *wk = qw + (size_t)k0 * 8;
            const int16_t *xk = xp + k0;
            int32x4_t a0 = vdupq_n_s32(0), a1 = a0, a2 = a0, a3 = a0;
            for (k = k0; k + 8 <= k1; k += 8, wk += 64, xk += 8) {
                const int16x8_t v = vld1q_s16(xk);
                const int16x4_t vl = vget_low_s16(v), vh = vget_high_s16(v);
                int8x16x2_t w2 = vld1q_s8_x2(wk);
                int8x16_t wa = w2.val[0], wb = w2.val[1];
                int16x8_t w;
                VOCR_PREFETCH(wk + 128);
                VOCR_PREFETCH(wk + 160);
                w = vmovl_s8(vget_low_s8(wa));
                a0 = vmlal_lane_s16(a0, vget_low_s16(w), vl, 0);
                a1 = vmlal_lane_s16(a1, vget_high_s16(w), vl, 0);
                w = vmovl_s8(vget_high_s8(wa));
                a2 = vmlal_lane_s16(a2, vget_low_s16(w), vl, 1);
                a3 = vmlal_lane_s16(a3, vget_high_s16(w), vl, 1);
                w = vmovl_s8(vget_low_s8(wb));
                a0 = vmlal_lane_s16(a0, vget_low_s16(w), vl, 2);
                a1 = vmlal_lane_s16(a1, vget_high_s16(w), vl, 2);
                w = vmovl_s8(vget_high_s8(wb));
                a2 = vmlal_lane_s16(a2, vget_low_s16(w), vl, 3);
                a3 = vmlal_lane_s16(a3, vget_high_s16(w), vl, 3);
                w2 = vld1q_s8_x2(wk + 32);
                wa = w2.val[0];
                wb = w2.val[1];
                w = vmovl_s8(vget_low_s8(wa));
                a0 = vmlal_lane_s16(a0, vget_low_s16(w), vh, 0);
                a1 = vmlal_lane_s16(a1, vget_high_s16(w), vh, 0);
                w = vmovl_s8(vget_high_s8(wa));
                a2 = vmlal_lane_s16(a2, vget_low_s16(w), vh, 1);
                a3 = vmlal_lane_s16(a3, vget_high_s16(w), vh, 1);
                w = vmovl_s8(vget_low_s8(wb));
                a0 = vmlal_lane_s16(a0, vget_low_s16(w), vh, 2);
                a1 = vmlal_lane_s16(a1, vget_high_s16(w), vh, 2);
                w = vmovl_s8(vget_high_s8(wb));
                a2 = vmlal_lane_s16(a2, vget_low_s16(w), vh, 3);
                a3 = vmlal_lane_s16(a3, vget_high_s16(w), vh, 3);
            }
            for (; k < k1; k++, wk += 8) {
                const int16x8_t wv = vmovl_s8(vld1_s8(wk));
                a0 = vmlal_n_s16(a0, vget_low_s16(wv), xp[k]);
                a1 = vmlal_n_s16(a1, vget_high_s16(wv), xp[k]);
            }
            a0 = vaddq_s32(a0, a2);
            a1 = vaddq_s32(a1, a3);
            if (!k0) {
                f0 = vcvtq_f32_s32(a0);
                f1 = vcvtq_f32_s32(a1);
            } else {
                f0 = vaddq_f32(f0, vcvtq_f32_s32(a0));
                f1 = vaddq_f32(f1, vcvtq_f32_s32(a1));
            }
        }
        VOCR_IOUT(p, f0, f1, vld1_dup_f32(sx + p), 0);
    }
#undef VOCR_IOUT
#undef VOCR_ISUMS4
#undef VOCR_IHALF
#undef VOCR_IROW
#endif
    /* Scalar: one position x 8 outputs per pass, the 8 int32 sums in registers (see mm_f32;
     * two positions would need more integer registers than MIPS has). Integer sums are
     * exact, and each chunk's sums are added to fs in the same order, so the bits are the
     * same as with any blocking. */
    (void)q;
    for (; p < P; p++) {
        const int16_t *xp = qx + (size_t)p * K;
        float fs[8];
        for (j = 0; j < 8; j++) fs[j] = 0.0f;
        for (k0 = 0; k0 < K; k0 += W8I_CHUNK) {
            const unsigned k1 = umin(K, k0 + W8I_CHUNK);
            int32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0, c6 = 0, c7 = 0;
            for (k = k0; k < k1; k++) {
                const int8_t *wk = qw + (size_t)k * 8;
                const int32_t xv = xp[k];
                c0 += (int32_t)wk[0] * xv;
                c1 += (int32_t)wk[1] * xv;
                c2 += (int32_t)wk[2] * xv;
                c3 += (int32_t)wk[3] * xv;
                c4 += (int32_t)wk[4] * xv;
                c5 += (int32_t)wk[5] * xv;
                c6 += (int32_t)wk[6] * xv;
                c7 += (int32_t)wk[7] * xv;
            }
            fs[0] = fs[0] + (float)c0;
            fs[1] = fs[1] + (float)c1;
            fs[2] = fs[2] + (float)c2;
            fs[3] = fs[3] + (float)c3;
            fs[4] = fs[4] + (float)c4;
            fs[5] = fs[5] + (float)c5;
            fs[6] = fs[6] + (float)c6;
            fs[7] = fs[7] + (float)c7;
        }
        for (j = 0; j < 8; j++) {
            float v = fs[j] * (scale[j] * sx[p]);
            if (bias) v = v + bias[j];
            if (relu) v = relu_f(v);
            if (res) v = v + res[p * rs + j];
            y[p * ys + j] = v;
        }
    }
}

/* ---------------------------------------------------------------- run */

typedef struct {
    char *text;
    size_t cap, used;
    int overflow;
    unsigned last, chars, frame;
    vocr_extra *ex;
    unsigned topk;           /* ex->topk, or 0 */
    unsigned conf_n;         /* characters whose confidence is in conf_min / conf_sum */
    float conf_min, conf_sum;
    const uint16_t *fidx;    /* the run skips frames (head_gate ON, no logits): the accumulated frames' line
                                indices, else NULL */
    unsigned gated, headed;  /* frames the gate skipped; frames the head ran on */
    unsigned escaped;        /* frames the two-level escape fired on */
} out_t;

static float *at(const vocr_ctx *c, uint32_t off) { return (float *)(void *)(c->arena + off); }

/* exp(x) for x <= 0 in float arithmetic only (no libm): 2^(x / ln 2), the integer part from the float
 * exponent bits, 2^f for f in [0, 1) from its degree-6 Taylor polynomial (relative error < 2e-5).
 * x is clamped at -30, so every value stays a normal float. The confidence and the attention op's softmax use it
 * (softmax_row). train/headgate.py's exp_neg and runtime/tests/vocr_ref.py's exp_neg_exact are the same arithmetic. */
static float exp_neg(float x)
{
    union { float f; uint32_t u; } p2;
    float t, f, p;
    int32_t e;
    if (!(x > -30.0f)) x = -30.0f; /* also NaN */
    if (x > 0.0f) x = 0.0f;
    t = x * 1.44269504f;
    e = (int32_t)t;
    if ((float)e > t) e--;
    f = t - (float)e;
    p = 0.000154035304f;
    p = p * f + 0.00133335581f;
    p = p * f + 0.00961812911f;
    p = p * f + 0.0555041087f;
    p = p * f + 0.240226507f;
    p = p * f + 0.693147181f;
    p = p * f + 1.0f;
    p2.u = (uint32_t)(e + 127) << 23;
    return p * p2.f;
}

#ifndef VOCR_NEON
/* Whether any of v[0..7] > t, by the floats' bits (Kotoba3DS addition: the head's per-class compares
 * each wait for the VFP pipeline on the ARM11; most blocks of classes have nothing to add). For
 * t >= +0, v > t exactly when v's bits, as a signed int, exceed t's (a negative v, or -0, has
 * negative bits; NaN bits above t's only send the block to the exact compares). Otherwise each
 * value's order key (fkey's: -m for a negative value of magnitude bits m, so -0 = +0) is compared. */
static int head_above(const float *v, float t)
{
#if defined(__GNUC__)
    typedef int32_t i32a __attribute__((may_alias));
#else
    typedef int32_t i32a;
#endif
    const i32a *b = (const i32a *)(const void *)v;
    int32_t tb, mx;
    unsigned j;
    memcpy(&tb, &t, sizeof tb);
    if (tb >= 0 && (tb & 0x7FFFFFFF) <= 0x7F800000) {
        mx = b[0];
        for (j = 1; j < 8; j++) mx = b[j] > mx ? b[j] : mx;
        return mx > tb;
    }
    if ((tb & 0x7FFFFFFF) > 0x7F800000) return 1; /* t is NaN: let the compares decide */
    tb = (tb & 0x7FFFFFFF) ? -(tb & 0x7FFFFFFF) : 0;
    for (j = 0; j < 8; j++) {
        const int32_t m = b[j] & 0x7FFFFFFF;
        if (m > 0x7F800000) return 1; /* NaN */
        if ((b[j] < 0 ? -m : m) > tb) return 1;
    }
    return 0;
}
#endif

/* softmax-max probability over one frame's top-K logits (v[0] is the largest) */
static float topk_conf(const float *v, unsigned k)
{
    float s = 1.0f;
    unsigned j;
    for (j = 1; j < k; j++) s += exp_neg(v[j] - v[0]);
    return 1.0f / s;
}

/* PW over P positions of x (row stride K) into y (row stride cout). */
static void run_pw(const vocr_ctx *c, unsigned i, const float *x, unsigned P, float *y, const float *res)
{
    const vocr_op *op = &c->model->op[i];
    const unsigned K = op->k, N = op->cout;
    const int relu = (op->flags & F_RELU) != 0;
    unsigned char *scr = c->arena + c->op[i].scr_off;
    int16_t *qx = (int16_t *)(void *)scr;
    float *sx = (float *)(void *)(scr + al16((size_t)P * K * 2u));
    float *wf = (float *)(void *)scr;
    unsigned nb;
    if (!P) return;
    const int pairs = c->mode == VOCR_MODE_W8I && VOCR_PAIRS(c->model, K);
    if (c->mode == VOCR_MODE_W8I) quant16(x, K, K, P, qx, sx, pairs);
    for (nb = 0; nb < N / 8; nb++) {
        const float *b = op->bias ? op->bias + nb * 8 : NULL;
        const float *rb = res ? res + nb * 8 : NULL;
        const size_t wo = (size_t)nb * K * 8;
        if (c->mode == VOCR_MODE_F32) {
            mm_f32(op->w + wo, b, x, K, K, P, y + nb * 8, N, rb, N, relu);
        } else if (c->mode == VOCR_MODE_W8F) {
            dequant8(op->q + wo, op->scale + nb * 8, K, wf);
            mm_f32(wf, b, x, K, K, P, y + nb * 8, N, rb, N, relu);
        } else {
            VOCR_MM_I16P(c->model, pairs, op->q + wo, op->scale + nb * 8, b, qx, sx, K, P, y + nb * 8, N, rb, N, relu);
        }
    }
}

static void ctc_push(const vocr_model *m, out_t *o, unsigned tok)
{
    if (tok && tok != o->last) {
        unsigned n;
        const char *s = vocr_class_text(m, tok, &n);
        if (!o->overflow && o->used + n < o->cap) {
            memcpy(o->text + o->used, s, n);
            o->used += n;
            o->text[o->used] = 0;
            o->chars++;
        } else {
            o->overflow = 1;
        }
    }
    o->last = tok;
}

#ifdef VOCR_NEON
/* The head's argmax in NEON without a float compare: gcc lowers vcgtq_f32 to one VFP compare per
 * lane when it may not flush denormals, as NEON does. A key in int32 orders the values as the
 * scalar `v > best` does, exactly: the value's magnitude bits for v >= 0, their negation for
 * v < 0 (so -0 and +0 are equal), and INT32_MIN for a NaN (never greater, never kept). This
 * matches `v > best` under IEEE compares (FPSCR.FZ = 0); a VFP that flushed denormals to zero
 * would see a denormal logit as 0 where the key does not, and logits are never denormal. */
static int32x4_t fkey(float32x4_t v)
{
    const int32x4_t b = vreinterpretq_s32_f32(v);
    const int32x4_t m = vandq_s32(b, vdupq_n_s32(0x7FFFFFFF));
    const int32x4_t sgn = vshrq_n_s32(b, 31);
    const int32x4_t key = vsubq_s32(veorq_s32(m, sgn), sgn);
    return vbslq_s32(vcgtq_s32(m, vdupq_n_s32(0x7F800000)), vdupq_n_s32(INT32_MIN), key);
}

/* The first maximum of each frame from its lanes: lane j of frame f holds the key (fkey) of the
 * largest logit of the classes nb * 8 + j and the first block nb that had it. The frame's first
 * maximum is the largest key; of the lanes that hold it, the smallest class. With every logit
 * -inf or NaN all lanes still hold -inf's key and block 0, which gives class 0, as the scan.
 * bestv gets the value back (-0 as +0, which compares the same). */
static void lanes_argmax(const vocr_lane_i32 *lk, const vocr_lane_u32 *li, unsigned nf, float *bestv,
                         uint32_t *besti)
{
    unsigned f, j;
    for (f = 0; f < nf; f++) {
        const vocr_lane_i32 *v = lk + (size_t)f * 8;
        const vocr_lane_u32 *ix = li + (size_t)f * 8;
        int32_t bk = v[0];
        uint32_t bi = ix[0] * 8u, bits;
        for (j = 1; j < 8; j++) {
            const uint32_t cls = ix[j] * 8u + j;
            if (v[j] > bk || (v[j] == bk && cls < bi)) {
                bk = v[j];
                bi = cls;
            }
        }
        bits = bk >= 0 ? (uint32_t)bk : 0x80000000u | (uint32_t)-bk;
        memcpy(&bestv[f], &bits, sizeof bits);
        besti[f] = bi;
    }
}
#define VOCR_HEAD_STEP step
#else
#define VOCR_HEAD_STEP 4u
#endif

/* Output head over the nf accumulated frames: argmax (first maximum) and CTC. With o->topk, also the
 * K largest logits of every frame, straight into the caller's topk_* buffers (inserted with strict
 * comparisons, so equal logits keep the lower class first and [0] is the argmax), the blank's logit,
 * and the confidence of every frame that emits a character.
 * W8I with NEON, the logits are mm_i16's, the same bits, VOCR_HEAD_CHUNK frames per call, and only the
 * scan's bookkeeping moves to NEON:
 *  - no top-K: once quant16 has read the accumulated frames, their memory is not read again, so it
 *    holds each frame's argmax per lane (lanes_argmax) instead of a scalar compare per class;
 *  - top-K: a full block of 8 classes goes through the per-class insertion only if one of its logits
 *    is above the frame's K-th largest so far (fkey's order is `v > t`'s). That value only grows
 *    while the classes are inserted in order, and the argmax is never below it (it is [0]), so a
 *    block with none above it is one the per-class loop would leave as it is.
 * A last block of fewer than 8 classes goes through the per-class loop after the full ones. The
 * head gate (o->fidx) changes only which frames the batch holds: frame f of the batch is the line's frame
 * o->fidx[f] (else o->frame + f), and a frame the gate skipped between two of them reads as a blank. */
static void run_head(const vocr_ctx *c, unsigned nf, out_t *o)
{
    const vocr_model *m = c->model;
    const unsigned L = m->n_ops, N = m->n_classes;
    const vocr_op *op = &m->op[L - 1];
    const unsigned K = op->k;
#ifdef VOCR_NEON
    float *const x = at(c, c->acc_off);
#else
    const float *x = at(c, c->acc_off);
#endif
    float *bestv = at(c, c->bestv_off);
    uint32_t *besti = (uint32_t *)(void *)(c->arena + c->besti_off);
    unsigned char *scr = c->arena + c->op[L - 1].scr_off;
    int16_t *qx = (int16_t *)(void *)scr;
    float *sx = (float *)(void *)(scr + al16((size_t)nf * K * 2u));
    float *wf = (float *)(void *)scr;
    vocr_extra *ex = o->ex;
    const unsigned TK = o->topk;
    const uint16_t *fx = o->fidx;
    unsigned f, nb, f0, q, j;
/* the line's frame of the batch's frame I */
#define VOCR_FRAME(I) (fx ? (unsigned)fx[I] : o->frame + (I))
#ifdef VOCR_NEON
    /* frames per mm_* call: 4, or VOCR_HEAD_CHUNK on the NEON W8I paths (one call per block); the
     * blocks below n_lanes (no top-K) or n_tk (top-K) take those paths */
    unsigned n_lanes = 0, n_tk = 0, step = 4;
    float tmp[VOCR_HEAD_CHUNK * 8];
    vocr_lane_i32 *lk = (vocr_lane_i32 *)(void *)x;              /* nf x 8 lane maxima (keys) ... */
    vocr_lane_u32 *li = (vocr_lane_u32 *)(void *)(x + (size_t)nf * 8u); /* ... and blocks: 16 nf <= K nf */
    vocr_lane_u32 *ro = (vocr_lane_u32 *)(void *)x; /* top-K: each frame's row in the caller's buffers */
#endif
    for (f = 0; f < nf; f++) {
        bestv[f] = -INFINITY;
        besti[f] = 0;
    }
    if (TK)
        for (f = 0; f < nf; f++) {
            float *tv = ex->topk_logit + (size_t)VOCR_FRAME(f) * TK;
            uint16_t *tc = ex->topk_class + (size_t)VOCR_FRAME(f) * TK;
            for (j = 0; j < TK; j++) {
                tv[j] = -INFINITY;
                tc[j] = 0;
            }
        }
    const int pairs = c->mode == VOCR_MODE_W8I && VOCR_PAIRS(c->model, K); /* f0 below is always even */
    if (c->mode == VOCR_MODE_W8I) quant16(x, K, K, nf, qx, sx, pairs);
#ifdef VOCR_NEON
    if (c->mode == VOCR_MODE_W8I && K >= 16u) {
        step = VOCR_HEAD_CHUNK;
        if (TK) {
            n_tk = N / 8u;
            for (f = 0; f < nf; f++) ro[f] = (uint32_t)VOCR_FRAME(f) * TK; /* once per batch, not per block */
        } else {
            n_lanes = N / 8u;
            for (f = 0; f < nf * 8u; f++) {
                lk[f] = -0x7F800000; /* fkey(-inf) */
                li[f] = 0;
            }
        }
    }
#endif
    for (nb = 0; nb < op->cout_pad / 8u; nb++) {
        const float *b = op->bias ? op->bias + nb * 8 : NULL;
        const size_t wo = (size_t)nb * K * 8;
#ifdef VOCR_NEON
        if (n_lanes && nb == n_lanes) lanes_argmax(lk, li, nf, bestv, besti);
#endif
        if (c->mode == VOCR_MODE_W8F) dequant8(op->q + wo, op->scale + nb * 8, K, wf);
        for (f0 = 0; f0 < nf; f0 += VOCR_HEAD_STEP) {
            const unsigned pn = umin(VOCR_HEAD_STEP, nf - f0);
#ifndef VOCR_NEON
            float tmp[4 * 8];
#endif
            if (c->mode == VOCR_MODE_F32)
                mm_f32(op->w + wo, b, x + (size_t)f0 * K, K, K, pn, tmp, 8, NULL, 0, 0);
            else if (c->mode == VOCR_MODE_W8F)
                mm_f32(wf, b, x + (size_t)f0 * K, K, K, pn, tmp, 8, NULL, 0, 0);
            else
                VOCR_MM_I16P(c->model, pairs, op->q + wo, op->scale + nb * 8, b, qx + (size_t)f0 * K, sx + f0, K, pn,
                            tmp, 8, NULL, 0, 0);
#ifdef VOCR_NEON
            if (nb < n_lanes) {
                const uint32x4_t vnb = vdupq_n_u32(nb);
                for (q = 0; q < pn; q++) {
                    vocr_lane_i32 *sk = lk + (size_t)(f0 + q) * 8;
                    vocr_lane_u32 *si = li + (size_t)(f0 + q) * 8;
                    const int32x4_t kl = fkey(vld1q_f32(tmp + q * 8)), kh = fkey(vld1q_f32(tmp + q * 8 + 4));
                    const int32x4_t ol = vld1q_s32(sk), oh = vld1q_s32(sk + 4);
                    const uint32x4_t ml = vcgtq_s32(kl, ol), mh = vcgtq_s32(kh, oh);
                    vst1q_s32(sk, vbslq_s32(ml, kl, ol));
                    vst1q_s32(sk + 4, vbslq_s32(mh, kh, oh));
                    vst1q_u32(si, vbslq_u32(ml, vnb, vld1q_u32(si)));
                    vst1q_u32(si + 4, vbslq_u32(mh, vnb, vld1q_u32(si + 4)));
                }
                if (ex && ex->logits)
                    for (q = 0; q < pn; q++) {
                        const unsigned frame = VOCR_FRAME(f0 + q);
                        if (frame < ex->cap_frames)
                            memcpy(ex->logits + (size_t)frame * N + nb * 8, tmp + q * 8, 8 * sizeof(float));
                    }
                continue;
            }
            if (nb < n_tk) {
                for (q = 0; q < pn; q++) {
                    float *tv = ex->topk_logit + ro[f0 + q];
                    uint16_t *tc = ex->topk_class + ro[f0 + q];
                    const int32x4_t kt = fkey(vld1q_dup_f32(tv + TK - 1));
                    const uint32x4_t gt = vorrq_u32(vcgtq_s32(fkey(vld1q_f32(tmp + q * 8)), kt),
                                                    vcgtq_s32(fkey(vld1q_f32(tmp + q * 8 + 4)), kt));
                    const uint32x2_t g2 = vorr_u32(vget_low_u32(gt), vget_high_u32(gt));
                    if (ex->logits && VOCR_FRAME(f0 + q) < ex->cap_frames)
                        memcpy(ex->logits + (size_t)VOCR_FRAME(f0 + q) * N + nb * 8, tmp + q * 8,
                               8 * sizeof(float));
                    if (!nb && ex->blank_logit) ex->blank_logit[VOCR_FRAME(f0 + q)] = tmp[q * 8];
                    if (!vget_lane_u32(vpmax_u32(g2, g2), 0)) continue;
                    for (j = 0; j < 8; j++) { /* the per-class loop below, for this frame */
                        const float v = tmp[q * 8 + j];
                        if (v > bestv[f0 + q]) {
                            bestv[f0 + q] = v;
                            besti[f0 + q] = nb * 8 + j;
                        }
                        if (v > tv[TK - 1]) {
                            unsigned p = TK - 1;
                            while (p && v > tv[p - 1]) {
                                tv[p] = tv[p - 1];
                                tc[p] = tc[p - 1];
                                p--;
                            }
                            tv[p] = v;
                            tc[p] = (uint16_t)(nb * 8 + j);
                        }
                    }
                }
                continue;
            }
#endif
            if (TK) {
                for (q = 0; q < pn; q++) {
                    const unsigned frame = VOCR_FRAME(f0 + q);
                    float *tv = ex->topk_logit + (size_t)frame * TK;
                    uint16_t *tc = ex->topk_class + (size_t)frame * TK;
                    if (!ex->logits && nb * 8 + 8 <= N && !head_above(tmp + q * 8, tv[TK - 1])) {
                        /* none of the 8 is above the K-th largest (nor, so, the largest) */
                        if (!nb && ex->blank_logit) ex->blank_logit[frame] = tmp[q * 8];
                        continue;
                    }
                    for (j = 0; j < 8 && nb * 8 + j < N; j++) {
                        const float v = tmp[q * 8 + j];
                        if (ex->logits && frame < ex->cap_frames)
                            ex->logits[(size_t)frame * N + nb * 8 + j] = v;
                        if (v > bestv[f0 + q]) {
                            bestv[f0 + q] = v;
                            besti[f0 + q] = nb * 8 + j;
                        }
                        if (v > tv[TK - 1]) {
                            unsigned p = TK - 1;
                            while (p && v > tv[p - 1]) {
                                tv[p] = tv[p - 1];
                                tc[p] = tc[p - 1];
                                p--;
                            }
                            tv[p] = v;
                            tc[p] = (uint16_t)(nb * 8 + j);
                        }
                    }
                    if (!nb && ex->blank_logit) ex->blank_logit[frame] = tmp[q * 8];
                }
            } else {
                for (q = 0; q < pn; q++) {
                    const unsigned frame = VOCR_FRAME(f0 + q);
                    if (!(ex && ex->logits) && nb * 8 + 8 <= N && !head_above(tmp + q * 8, bestv[f0 + q])) continue;
                    for (j = 0; j < 8 && nb * 8 + j < N; j++) {
                        const float v = tmp[q * 8 + j];
                        if (ex && ex->logits && frame < ex->cap_frames)
                            ex->logits[(size_t)frame * N + nb * 8 + j] = v;
                        if (v > bestv[f0 + q]) {
                            bestv[f0 + q] = v;
                            besti[f0 + q] = nb * 8 + j;
                        }
                    }
                }
            }
        }
    }
#ifdef VOCR_NEON
    if (n_lanes && n_lanes == op->cout_pad / 8u) lanes_argmax(lk, li, nf, bestv, besti);
#endif
    for (f = 0; f < nf; f++, o->frame++) {
        if (fx && fx[f] != o->frame) { /* frames the gate skipped since the last one: blanks */
            o->frame = fx[f];
            o->last = 0;
        }
        if (ex && ex->best && o->frame < ex->cap_frames) ex->best[o->frame] = (uint16_t)besti[f];
        if (TK && besti[f] && besti[f] != o->last) {
            const float p = topk_conf(ex->topk_logit + (size_t)o->frame * TK, TK);
            if (!o->conf_n || p < o->conf_min) o->conf_min = p;
            o->conf_sum += p;
            o->conf_n++;
        }
        ctc_push(m, o, besti[f]);
    }
    o->headed += nf;
}
#undef VOCR_FRAME
#undef VOCR_HEAD_STEP

/* The head gate's step, after a tile's last layer appended frames [s0, s1) to the accumulator, the line's
 * frames a0, a0 + 1, ...: their head input to ex->head_in, and with the gate planned, their scores (to
 * ex->gate_score). When the run skips (o->fidx), a frame scoring above t leaves the accumulator and reads
 * as the blank: best 0, its blank logit as the head computes class 0 (its first block on the same input:
 * mm_i16 on the int16 input in W8I, mm_f32 on the float input with the block's float weights in W8F and
 * F32; computed only when the caller keeps the top-K, the one place it is read outside W8I), its top-K
 * [blank logit, -inf, ...] with classes 0. The gate itself computes the same in every mode: layer 1 on the
 * head input quantised to int16, exactly as a W8I pointwise layer. The frames that stay move down in
 * order, their line indices in fidx. Returns the accumulator's new fill. The scratch is the head's (free
 * between a tile's layers and the head), sized for a tile of frames in plan(). */
static unsigned gate_tile(const vocr_ctx *c, out_t *o, unsigned s0, unsigned s1, unsigned a0)
{
    const vocr_model *m = c->model;
    const vocr_op *head = &m->op[m->n_ops - 1];
    const vocr_gate *g = &m->gate;
    const unsigned K = head->k, H = g->hidden, n = s1 - s0, TK = o->topk;
    vocr_extra *ex = o->ex;
    float *acc = at(c, c->acc_off);
    unsigned char *scr = c->arena + c->op[m->n_ops - 1].scr_off;
    int16_t *qx = (int16_t *)(void *)scr;
    float *sx = (float *)(void *)(scr + al16((size_t)c->tile * K * 2u));
    float *hid = (float *)(void *)(scr + al16((size_t)c->tile * K * 2u) + al16((size_t)c->tile * sizeof(float)));
    /* W8F: the head's first block, dequantised once per tile on the first skipped frame that needs it */
    float *wf0 = (float *)(void *)(scr + al16((size_t)c->tile * K * 2u) + al16((size_t)c->tile * sizeof(float)) +
                                   al16((size_t)c->tile * H * sizeof(float)));
    const float *w0 = c->mode == VOCR_MODE_F32 ? head->w : NULL;
    uint16_t *fidx = (uint16_t *)(void *)(c->arena + c->fidx_off);
    unsigned i, j, nb, w = s0;
    if (ex && ex->head_in)
        for (i = 0; i < n; i++)
            if (a0 + i < ex->cap_frames)
                memcpy(ex->head_in + (size_t)(a0 + i) * K, acc + (size_t)(s0 + i) * K, K * sizeof(float));
    if (!c->gate || !n) return s1;
    quant16(acc + (size_t)s0 * K, K, K, n, qx, sx, 0);
    for (nb = 0; nb < H / 8u; nb++)
        mm_i16(g->q + (size_t)nb * K * 8u, g->scale + nb * 8u, g->bias + nb * 8u, qx, sx, K, n, hid + nb * 8u, H,
               NULL, 0, 1);
    for (i = 0; i < n; i++) {
        const unsigned a = a0 + i;
        const float *h = hid + (size_t)i * H;
        float s = g->b2;
        for (j = 0; j < H; j++) s = s + g->w2[j] * h[j];
        if (ex && ex->gate_score && a < ex->cap_frames) ex->gate_score[a] = s;
        if (!o->fidx) continue;
        if (s > g->t) {
            float b8[8];
            if (c->mode == VOCR_MODE_W8I) {
                VOCR_MM_I16(m, head->q, head->scale, head->bias, qx + (size_t)i * K, sx + i, K, 1, b8, 8, NULL, 0, 0);
            } else if (TK) {
                if (!w0) {
                    dequant8(head->q, head->scale, K, wf0);
                    w0 = wf0;
                }
                mm_f32(w0, head->bias, acc + (size_t)(s0 + i) * K, K, K, 1, b8, 8, NULL, 0, 0);
            }
            if (ex && ex->best && a < ex->cap_frames) ex->best[a] = 0;
            if (TK) { /* the caller's buffers hold every frame of the line (checked in vocr_run) */
                float *tv = ex->topk_logit + (size_t)a * TK;
                uint16_t *tc = ex->topk_class + (size_t)a * TK;
                tv[0] = b8[0];
                tc[0] = 0;
                for (j = 1; j < TK; j++) {
                    tv[j] = -INFINITY;
                    tc[j] = 0;
                }
                if (ex->blank_logit) ex->blank_logit[a] = b8[0];
            }
            o->gated++;
        } else {
            if (w != s0 + i) memcpy(acc + (size_t)w * K, acc + (size_t)(s0 + i) * K, K * sizeof(float));
            fidx[w++] = (uint16_t)a;
        }
    }
    return o->fidx ? w : s1;
}

/* ---------------------------------------------------------------- the two-level head (VOC2 trailer)
 *
 * W8I and W8F (vocr_opts.two_level; an int8 blob). Level 1, the head's blocks [0, l1_blocks), runs on every frame:
 * the blank and the most frequent classes. The escape (a small MLP on the head's input quantised to int16 as W8I
 * does, in either mode, level 1's largest non-blank logit and its blank logit) then says which frames also need
 * level 2, the head's other blocks. Those frames' inputs (quantised in W8I, float in W8F) are packed to the front
 * and level 2 runs on them as one batch. Each block's logits are the mode's head arithmetic (mm_i16 in W8I,
 * dequant8 + mm_f32 in W8F, as run_head). A frame's state (first
 * maximum, top-K, the blank's logit) is updated block by block in class order, so a fired frame gets today's
 * argmax and top-K bit for bit and the others
 * the same over the level-1 classes. A block whose 8 logits are none above the frame's threshold (below which no
 * logit can change its state: the non-blank maximum in level 1, the first maximum in level 2, and the top-K's
 * last value) is skipped without the per-class loop, as run_head's NEON top-K path does. */
#ifdef VOCR_NEON
#define H2_STEP VOCR_HEAD_CHUNK
#else
#define H2_STEP 4u
#endif

/* any of the 8 values above thr (a NaN never is) */
static int block_above(const float *v8, float thr)
{
#ifdef VOCR_NEON
    const int32x4_t kt = fkey(vdupq_n_f32(thr));
    const uint32x4_t gt = vorrq_u32(vcgtq_s32(fkey(vld1q_f32(v8)), kt), vcgtq_s32(fkey(vld1q_f32(v8 + 4)), kt));
    const uint32x2_t g2 = vorr_u32(vget_low_u32(gt), vget_high_u32(gt));
    return vget_lane_u32(vpmax_u32(g2, g2), 0) != 0;
#else
    unsigned j;
    for (j = 0; j < 8; j++)
        if (v8[j] > thr) return 1;
    return 0;
#endif
}

/* One block's 8 logits v (classes cls0 + j < N) into a frame's state: its first maximum (bestv, besti), in
 * level 1 its largest non-blank logit (nbv; NULL in level 2) and the top-K (tv, tc; TK = 0: none). */
static void h2_update(const float *v, unsigned cls0, unsigned N, float *bestv, uint32_t *besti, float *nbv,
                      float *tv, uint16_t *tc, unsigned TK)
{
    unsigned j;
    float thr = nbv ? *nbv : *bestv;
    if (TK && tv[TK - 1] < thr) thr = tv[TK - 1];
    if (!block_above(v, thr)) return;
    for (j = 0; j < 8 && cls0 + j < N; j++) {
        const float x = v[j];
        if (x > *bestv) {
            *bestv = x;
            *besti = cls0 + j;
        }
        if (nbv && cls0 + j && x > *nbv) *nbv = x;
        if (TK && x > tv[TK - 1]) {
            unsigned p = TK - 1;
            while (p && x > tv[p - 1]) {
                tv[p] = tv[p - 1];
                tc[p] = tc[p - 1];
                p--;
            }
            tv[p] = x;
            tc[p] = (uint16_t)(cls0 + j);
        }
    }
}

static void run_head2(const vocr_ctx *c, unsigned nf, out_t *o)
{
    const vocr_model *m = c->model;
    const vocr_2level *tw = &m->two;
    const unsigned L = m->n_ops, N = m->n_classes;
    const vocr_op *op = &m->op[L - 1];
    const unsigned K = op->k, B1 = tw->l1_blocks, NB = op->cout_pad / 8u, H = tw->hidden;
    /* W8F: level 1 and level 2 are W8F's head rows (dequant8 + mm_f32 on the float input, as run_head), the
     * escape the same as in W8I (layer 1 on the int16 input); the fired frames' float inputs are packed to
     * the front of the accumulator, which nothing reads after the head */
    const int fl = c->mode == VOCR_MODE_W8F;
    float *x = at(c, c->acc_off);
    float *bestv = at(c, c->bestv_off), *nbv = at(c, c->nbv_off), *b0v = at(c, c->b0_off);
    float *hid = at(c, c->hid_off);
    uint32_t *besti = (uint32_t *)(void *)(c->arena + c->besti_off);
    uint32_t *fm = (uint32_t *)(void *)(c->arena + c->fmap_off);
    unsigned char *scr = c->arena + c->op[L - 1].scr_off;
    int16_t *qx = (int16_t *)(void *)scr;
    float *sx = (float *)(void *)(scr + al16((size_t)nf * K * 2u));
    float *wf = (float *)(void *)(scr + al16((size_t)c->acc_frames * K * 2u) + al16((size_t)c->acc_frames * sizeof(float)));
    vocr_extra *ex = o->ex;
    const unsigned TK = o->topk;
    const uint16_t *fx = o->fidx;
    unsigned f, nb, f0, q, j, nfire = 0;
    float tmp[H2_STEP * 8];
/* the line's frame of the batch's frame I (the head gate removes the frames it skips from the batch) */
#define H2_FRAME(I) (fx ? (unsigned)fx[I] : o->frame + (I))
    for (f = 0; f < nf; f++) {
        bestv[f] = -INFINITY;
        besti[f] = 0;
        nbv[f] = -INFINITY;
    }
    if (TK)
        for (f = 0; f < nf; f++) {
            float *tv = ex->topk_logit + (size_t)H2_FRAME(f) * TK;
            uint16_t *tc = ex->topk_class + (size_t)H2_FRAME(f) * TK;
            for (j = 0; j < TK; j++) {
                tv[j] = -INFINITY;
                tc[j] = 0;
            }
        }
    quant16(x, K, K, nf, qx, sx, 0);
    /* level 1: every frame */
    for (nb = 0; nb < B1; nb++) {
        const float *b = op->bias ? op->bias + nb * 8 : NULL;
        if (fl) dequant8(op->q + (size_t)nb * K * 8, op->scale + nb * 8, K, wf);
        for (f0 = 0; f0 < nf; f0 += H2_STEP) {
            const unsigned pn = umin(H2_STEP, nf - f0);
            if (fl)
                mm_f32(wf, b, x + (size_t)f0 * K, K, K, pn, tmp, 8, NULL, 0, 0);
            else
                VOCR_MM_I16(c->model, op->q + (size_t)nb * K * 8, op->scale + nb * 8, b, qx + (size_t)f0 * K, sx + f0, K, pn, tmp, 8,
                       NULL, 0, 0);
            for (q = 0; q < pn; q++) {
                const unsigned fr = f0 + q, frame = H2_FRAME(fr);
                float *tv = TK ? ex->topk_logit + (size_t)frame * TK : NULL;
                uint16_t *tc = TK ? ex->topk_class + (size_t)frame * TK : NULL;
                if (!nb) {
                    b0v[fr] = tmp[q * 8];
                    if (TK && ex->blank_logit) ex->blank_logit[frame] = tmp[q * 8];
                }
                h2_update(tmp + q * 8, nb * 8, N, bestv + fr, besti + fr, nbv + fr, tv, tc, TK);
            }
        }
    }
    /* the escape: layer 1 through the head's integer kernel, then its two level-1 inputs, ReLU and layer 2 */
    for (nb = 0; nb < H / 8u; nb++)
        mm_i16(tw->e_q + (size_t)nb * K * 8, tw->e_scale + nb * 8, tw->e_bias + nb * 8, qx, sx, K, nf, hid + nb * 8,
               H, NULL, 0, 0);
    for (f = 0; f < nf; f++) {
        const float *h = hid + (size_t)f * H;
        float s = tw->e_b2;
        for (j = 0; j < H; j++) {
            float a = h[j];
            a = a + tw->e_wl1[j] * nbv[f];
            a = a + tw->e_wb0[j] * b0v[f];
            s = s + tw->e_w2[j] * relu_f(a);
        }
        if (ex && ex->esc_score && H2_FRAME(f) < ex->cap_frames) ex->esc_score[H2_FRAME(f)] = s;
        if (s > tw->t) fm[nfire++] = f;
    }
    o->escaped += nfire;
    /* level 2: the fired frames, packed to the front of the quantised input (forwards: fm[j] >= j), or in W8F
     * of the float input */
    if (nfire) {
        for (j = 0; j < nfire; j++)
            if (fm[j] != j) {
                if (fl) {
                    memcpy(x + (size_t)j * K, x + (size_t)fm[j] * K, K * sizeof(float));
                } else {
                    memcpy(qx + (size_t)j * K, qx + (size_t)fm[j] * K, K * sizeof(int16_t));
                    sx[j] = sx[fm[j]];
                }
            }
        for (nb = B1; nb < NB; nb++) {
            const float *b = op->bias ? op->bias + nb * 8 : NULL;
            if (fl) dequant8(op->q + (size_t)nb * K * 8, op->scale + nb * 8, K, wf);
            for (f0 = 0; f0 < nfire; f0 += H2_STEP) {
                const unsigned pn = umin(H2_STEP, nfire - f0);
                if (fl)
                    mm_f32(wf, b, x + (size_t)f0 * K, K, K, pn, tmp, 8, NULL, 0, 0);
                else
                    VOCR_MM_I16(c->model, op->q + (size_t)nb * K * 8, op->scale + nb * 8, b, qx + (size_t)f0 * K, sx + f0, K, pn,
                           tmp, 8, NULL, 0, 0);
                for (q = 0; q < pn; q++) {
                    const unsigned fr = fm[f0 + q], frame = H2_FRAME(fr);
                    h2_update(tmp + q * 8, nb * 8, N, bestv + fr, besti + fr, NULL,
                              TK ? ex->topk_logit + (size_t)frame * TK : NULL,
                              TK ? ex->topk_class + (size_t)frame * TK : NULL, TK);
                }
            }
        }
    }
    for (f = 0; f < nf; f++, o->frame++) {
        if (fx && fx[f] != o->frame) { /* frames the gate skipped since the last one: blanks */
            o->frame = fx[f];
            o->last = 0;
        }
        if (ex && ex->best && o->frame < ex->cap_frames) ex->best[o->frame] = (uint16_t)besti[f];
        if (TK && besti[f] && besti[f] != o->last) {
            const float p = topk_conf(ex->topk_logit + (size_t)o->frame * TK, TK);
            if (!o->conf_n || p < o->conf_min) o->conf_min = p;
            o->conf_sum += p;
            o->conf_n++;
        }
        ctc_push(m, o, besti[f]);
    }
    o->headed += nf;
#undef H2_FRAME
}
#undef H2_STEP

/* ---------------------------------------------------------------- the attention op (docs/vocr6-attn.md)
 *
 * After the tiles, the line buffer holds z, every frame's bottleneck output (d floats). attn_line turns it into
 * z' = z + O(concat_h softmax(Q_h K_h^T) V_h) in place, in chunks of P frames (the head batch):
 *  1. K and V of every frame, from the QKV matrix's K and V rows in the blob's mode. K is stored per head as blocks of
 *     8 keys, [Tp / 8][dh][8], so that a block's 8 scores are one mm_f32 block (the keys are its 8 "outputs"); V as
 *     blocks of 8 channels, [dim / 8][Tp][8], the "weights" of the weighted sums.
 *  2. per chunk of queries: Q (the matrix's Q rows, 1 / sqrt(dh) folded in), then per head the scores of the line's
 *     T keys (mm_f32: 0 + Q[0] K[0] + Q[1] K[1] + ...), softmax_row, and the weighted sums of V over the T keys in
 *     order (mm_f32); then O and the residual (z' = O(A) + z) in the blob's mode, over the chunk's own z, which no
 *     later chunk reads (their K and V are in 1).
 * The scores, the softmax and the weighted sums are float32 in every mode; only the projections follow it. Each value
 * is computed in a fixed order whatever P, the tile or the build (scalar, NEON, VFPU), so the logits are the same
 * for every tile and batch size; runtime/tests/vocr_ref.py attn_exact is the bit-exact twin. */

/* softmax over a row's first T values, in place: the first maximum, e = exp_neg(s - max) (in [e^-30, 1]),
 * sum = 0 + e_0 + e_1 + ..., then e * (1 / sum). */
static void softmax_row(float *s, unsigned T)
{
    float mx = s[0], sum = 0.0f, inv;
    unsigned k;
    for (k = 1; k < T; k++)
        if (s[k] > mx) mx = s[k];
    for (k = 0; k < T; k++) {
        s[k] = exp_neg(s[k] - mx);
        sum = sum + s[k];
    }
    inv = 1.0f / sum;
    for (k = 0; k < T; k++) s[k] = s[k] * inv;
}

/* block nb (8 outputs) of a matrix ([rows / 8][K][8]: w float, or q int8 with scales; bias [rows]) on P positions of
 * x (row stride xs) into y (row stride ys), + res, in the context's mode: as run_pw does a pointwise layer's, with x
 * already quantised to qx / sx (W8I) and wf one dequantised block (W8F) */
static void mat_block(const vocr_ctx *c, const float *w, const int8_t *q, const float *scale, const float *bias,
                      unsigned nb, unsigned K, const float *x, size_t xs, const int16_t *qx, const float *sx,
                      unsigned P, float *y, size_t ys, const float *res, size_t rs, float *wf)
{
    const size_t wo = (size_t)nb * K * 8u;
    if (c->mode == VOCR_MODE_F32) {
        mm_f32(w + wo, bias + nb * 8u, x, xs, K, P, y, ys, res, rs, 0);
    } else if (c->mode == VOCR_MODE_W8F) {
        dequant8(q + wo, scale + nb * 8u, K, wf);
        mm_f32(wf, bias + nb * 8u, x, xs, K, P, y, ys, res, rs, 0);
    } else {
        mm_i16(q + wo, scale + nb * 8u, bias + nb * 8u, qx, sx, K, P, y, ys, res, rs, 0);
    }
}

static int cancelled(vocr_extra *ex) { return ex && ex->cancel && ex->cancel(ex->user); }

static int attn_line(const vocr_ctx *c, unsigned T, vocr_extra *ex)
{
    const vocr_model *m = c->model;
    const vocr_op *op = &m->op[c->attn];
    const vocr_attn *a = &m->attn;
    const unsigned d = op->cin, da = a->dim, nh = a->heads, dh = da / nh, NB = da / 8u, P = c->head;
    const unsigned tp = c->att_tp, kmax = d > da ? d : da, kblocks = (T + 7u) / 8u;
    float *z = at(c, c->line_off), *kp = at(c, c->kp_off), *vp = at(c, c->vp_off);
    float *qc = at(c, c->qc_off), *ac = at(c, c->ac_off), *sc = at(c, c->sc_off);
    unsigned char *scr = c->arena + c->as_off;
    int16_t *qx = (int16_t *)(void *)scr;
    float *sx = (float *)(void *)(scr + al16((size_t)P * kmax * 2u));
    float *wf = (float *)(void *)scr;
    unsigned t0, n, h, cb, p, j;
    /* the last key block's lanes past the line's end: zero keys (their scores are computed, never read) */
    if (T & 7u)
        for (h = 0; h < nh; h++)
            for (j = 0; j < dh; j++)
                for (p = T & 7u; p < 8u; p++) kp[(((size_t)h * (tp / 8u) + T / 8u) * dh + j) * 8u + p] = 0.0f;
    for (t0 = 0; t0 < T; t0 += P) { /* 1. K and V of every frame */
        const float *zc = z + (size_t)t0 * d;
        n = umin(P, T - t0);
        if (cancelled(ex)) return VOCR_E_CANCELLED;
        if (c->mode == VOCR_MODE_W8I) quant16(zc, d, d, n, qx, sx, 0);
        for (cb = 0; cb < NB; cb++) {
            const unsigned h0 = cb * 8u / dh, j0 = cb * 8u - h0 * dh;
            mat_block(c, op->w, op->q, op->scale, op->bias, NB + cb, d, zc, d, qx, sx, n, qc, 8, NULL, 0, wf);
            for (p = 0; p < n; p++) { /* K channels j0 .. j0 + 7 of head h0, key t0 + p */
                const unsigned t = t0 + p;
                float *dst = kp + (((size_t)h0 * (tp / 8u) + t / 8u) * dh + j0) * 8u + t % 8u;
                for (j = 0; j < 8u; j++) dst[(size_t)j * 8u] = qc[p * 8u + j];
            }
            mat_block(c, op->w, op->q, op->scale, op->bias, 2u * NB + cb, d, zc, d, qx, sx, n,
                      vp + ((size_t)cb * tp + t0) * 8u, 8, NULL, 0, wf);
        }
    }
    for (t0 = 0; t0 < T; t0 += P) { /* 2. the queries, chunk by chunk */
        float *zc = z + (size_t)t0 * d;
        n = umin(P, T - t0);
        if (cancelled(ex)) return VOCR_E_CANCELLED;
        if (c->mode == VOCR_MODE_W8I) quant16(zc, d, d, n, qx, sx, 0);
        for (cb = 0; cb < NB; cb++)
            mat_block(c, op->w, op->q, op->scale, op->bias, cb, d, zc, d, qx, sx, n, qc + cb * 8u, da, NULL, 0, wf);
        VOCR_MARK(VOCR_PROF_ATTN);
        for (h = 0; h < nh; h++) {
            const float *kh = kp + (size_t)h * (tp / 8u) * dh * 8u;
            unsigned kb;
            for (kb = 0; kb < kblocks; kb++)
                mm_f32(kh + (size_t)kb * dh * 8u, NULL, qc + h * dh, da, dh, n, sc + kb * 8u, tp, NULL, 0, 0);
            for (p = 0; p < n; p++) softmax_row(sc + (size_t)p * tp, T);
            for (cb = h * dh / 8u; cb < (h + 1u) * dh / 8u; cb++)
                mm_f32(vp + (size_t)cb * tp * 8u, NULL, sc, tp, T, n, ac + cb * 8u, da, NULL, 0, 0);
        }
        VOCR_MARK(c->attn);
        if (c->mode == VOCR_MODE_W8I) quant16(ac, da, da, n, qx, sx, 0);
        for (cb = 0; cb < d / 8u; cb++)
            mat_block(c, a->o_w, a->o_q, a->o_scale, a->o_bias, cb, da, ac, da, qx, sx, n, zc + cb * 8u, d,
                      zc + cb * 8u, d, wf);
    }
    return VOCR_OK;
}

/* Keep the last `halo` columns of a ring as its first ones. The ranges can overlap
 * (fewer new columns than halo), so copy forwards: the destination is below the source. */
static void ring_shift(float *ring, unsigned cap, unsigned halo, unsigned col)
{
    const float *src = ring + (size_t)(cap - halo) * col;
    size_t i = 0, n = (size_t)halo * col;
    if (!halo || src == ring) return;
#ifdef VOCR_NEON
    /* 4 floats at a time, still forwards: each load comes before the store that could reach it */
    for (; i + 4 <= n; i += 4) vst1q_f32(ring + i, vld1q_f32(src + i));
#endif
    for (; i < n; i++) ring[i] = src[i];
}

/* Compute output columns [j0, j0 + n) of op i (wout of them exist) into its
 * destination; u is the mini-tile index for the last front op, else 0. */
static void step_op(const vocr_ctx *c, unsigned i, long j0, unsigned n, unsigned wout,
                    unsigned u, unsigned *acc_fill)
{
    const vocr_model *m = c->model;
    const vocr_op *op = &m->op[i];
    const vocr_plan_op *p = &c->op[i];
    const unsigned oc = p->out_col;
    unsigned t0 = 0, t1 = n, t;
    float *dst, *out;
    if (j0 < 0) t0 = (unsigned)(-j0) < n ? (unsigned)(-j0) : n;
    if (j0 + (long)n > (long)wout) t1 = (long)wout > j0 ? (unsigned)((long)wout - j0) : 0u;
    if (t1 < t0) t1 = t0;
    if (p->dst == DST_ACC || p->dst == DST_LINE) { /* the head's accumulator, or the attention's line of z */
        out = at(c, p->dst == DST_ACC ? c->acc_off : c->line_off) + (size_t)*acc_fill * oc;
        dst = NULL;
        *acc_fill += t1 - t0;
    } else {
        const vocr_plan_op *nx = &c->op[i + 1];
        dst = at(c, nx->in_off);
        if (p->dst == DST_RING) {
            if (!u) ring_shift(dst, nx->cap, nx->halo, nx->in_col);
            dst += (size_t)(nx->halo + u) * nx->in_col;
        } else {
            dst += (size_t)u * oc;
        }
        out = dst + (size_t)t0 * oc;
    }
    if (t1 > t0) {
        if (op->type == T_CONV) {
            k_conv1(op, at(c, p->in_off), out, t0, t1);
        } else if (op->type == T_DW) {
            k_dw(op, at(c, p->in_off), out, t0, t1);
        } else {
            const unsigned rows = op->flags & F_COLLAPSE ? 1u : op->h_in;
            const float *res = NULL;
            if (op->flags & F_RES)
                res = at(c, c->op[i - 1].in_off) + (size_t)(t0 + m->op[i - 1].pw) * oc;
            run_pw(c, i, at(c, p->in_off) + (size_t)t0 * p->in_col, (t1 - t0) * rows, out, res);
        }
    }
    if (dst) {
        for (t = 0; t < t0; t++) memset(dst + (size_t)t * oc, 0, oc * sizeof(float));
        for (t = t1; t < n; t++) memset(dst + (size_t)t * oc, 0, oc * sizeof(float));
    }
}

int vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t text_cap, vocr_extra *ex)
{
    const vocr_model *m;
    line_t ln;
    out_t o;
    unsigned L, S, f, i, u, T, wout[VOCR_MAX_OPS], acc_fill = 0, n_front, produced = 0;
    long k, ntiles;
    int rc, hook, two;
    if (!c || !c->arena || !c->model || !text || !text_cap) return VOCR_E_ARG;
    text[0] = 0;
    rc = check_image(img);
    if (rc) return rc;
    m = c->model;
    /* the two-level head, unless the caller wants every logit of every frame or a full read (ungated) */
    two = c->two_level && !(ex && (ex->logits || ex->ungated));
    L = m->n_ops;
    S = m->stride;
    f = c->front;
    line_setup(&ln, m, img, (uint16_t *)(void *)(c->arena + c->rows_off));
    if (ln.nw > c->max_width) return VOCR_E_WIDTH;
    T = ln.padded / S;
    memset(&o, 0, sizeof o);
    o.text = text;
    o.cap = text_cap;
    o.ex = ex;
    if (ex) {
        ex->width = ln.nw;
        ex->frames = T;
        ex->chars = 0;
        ex->gated = 0;
        ex->escaped = 0;
        if (ex->topk) {
            if (ex->topk > VOCR_TOPK_MAX || ex->topk > m->n_classes || !ex->topk_logit ||
                !ex->topk_class || ex->cap_frames < T)
                return VOCR_E_ARG;
            o.topk = ex->topk;
            ex->conf_min = ex->conf_mean = 1.0f;
        }
    }
    /* the head gate: skip frames unless the run asks for every logit or for no skipping; its step runs per
     * tile when the gate is planned or the head's input is asked for */
    if (c->gate == VOCR_HEAD_GATE_ON && !(ex && (ex->logits || ex->ungated)))
        o.fidx = (const uint16_t *)(const void *)(c->arena + c->fidx_off);
    hook = c->gate || (ex && ex->head_in);
    for (i = 0, u = ln.padded; i < L; i++) wout[i] = u /= m->op[i].sw;
    VOCR_MARK(VOCR_PROF_RUN);
    memset(c->arena + c->ring_off, 0, c->ring_end - c->ring_off);
    n_front = c->op[f].n_tile;
    ntiles = ((long)T + c->op[L - 1].lag + (long)c->tile - 1) / (long)c->tile;
    for (k = 0; k < ntiles; k++) {
        /* the frames this tile appends, from its first op on: a head right after the front's last op (no
         * frame-row layer between them) has the front append them (audit-head-gate F1) */
        const unsigned s0 = acc_fill, a0 = produced;
        if (ex && ex->cancel && ex->cancel(ex->user)) {
            text[0] = 0;
            return VOCR_E_CANCELLED;
        }
        for (u = 0; u < n_front; u++) {
            const long g = k * (long)n_front + (long)u;
            const vocr_plan_op *p0 = &c->op[0];
            float *ring0 = at(c, p0->in_off);
            VOCR_MARK(VOCR_PROF_SRC);
            ring_shift(ring0, p0->cap, p0->halo, p0->in_col);
            src_cols(&ln, m->height, g * (long)c->n_src, c->n_src, ring0 + (size_t)p0->halo * p0->in_col);
            for (i = 0; i <= f; i++) {
                VOCR_MARK(i);
                step_op(c, i, g * (long)c->op[i].n_step - c->op[i].lag, c->op[i].n_step, wout[i],
                        i == f ? u : 0u, &acc_fill);
            }
        }
        for (i = f + 1; i + 1 + (c->attn ? 1u : 0u) < L; i++) {
            VOCR_MARK(i);
            step_op(c, i, k * (long)c->op[i].n_tile - c->op[i].lag, c->op[i].n_tile, wout[i], 0u, &acc_fill);
        }
        if (c->attn) { /* the frames went to the line of z (acc_fill counts them); the attention and the head
                        * run once the line is complete */
            VOCR_MARK(VOCR_PROF_RUN);
            continue;
        }
        produced += acc_fill - s0;
        if (hook && acc_fill > s0) {
            VOCR_MARK(VOCR_PROF_GATE);
            acc_fill = gate_tile(c, &o, s0, acc_fill, a0);
        }
        if (acc_fill >= c->head) {
            VOCR_MARK(L - 1);
            if (two) run_head2(c, acc_fill, &o);
            else run_head(c, acc_fill, &o);
            acc_fill = 0;
        }
        VOCR_MARK(VOCR_PROF_RUN);
    }
    if (c->attn) { /* the attention over the line (z -> z' in place), then the head over z' as the tiles feed it */
        const unsigned K = m->op[L - 1].k;
        const float *zl = at(c, c->line_off);
        float *acc = at(c, c->acc_off);
        unsigned t0;
        if (acc_fill != T) {
            text[0] = 0;
            return VOCR_E_FORMAT; /* cannot happen for a validated blob */
        }
        VOCR_MARK(c->attn);
        rc = attn_line(c, T, ex);
        if (rc) {
            text[0] = 0;
            return rc;
        }
        acc_fill = 0;
        for (t0 = 0; t0 < T; t0 += c->tile) {
            const unsigned n = umin(c->tile, T - t0), s0 = acc_fill, a0 = produced;
            VOCR_MARK(VOCR_PROF_RUN);
            if (cancelled(ex)) {
                text[0] = 0;
                return VOCR_E_CANCELLED;
            }
            memcpy(acc + (size_t)acc_fill * K, zl + (size_t)t0 * K, (size_t)n * K * sizeof(float));
            acc_fill += n;
            produced += n;
            if (hook) {
                VOCR_MARK(VOCR_PROF_GATE);
                acc_fill = gate_tile(c, &o, s0, acc_fill, a0);
            }
            if (acc_fill >= c->head) {
                VOCR_MARK(L - 1);
                if (two) run_head2(c, acc_fill, &o);
                else run_head(c, acc_fill, &o);
                acc_fill = 0;
            }
        }
        VOCR_MARK(VOCR_PROF_RUN);
    }
    if (acc_fill) {
        VOCR_MARK(L - 1);
        if (two) run_head2(c, acc_fill, &o);
        else run_head(c, acc_fill, &o);
        VOCR_MARK(VOCR_PROF_RUN);
    }
    if (o.fidx && o.frame < produced) { /* the line ends in frames the gate skipped */
        o.frame = produced;
        o.last = 0;
    }
    if (ex) {
        ex->chars = o.chars;
        ex->gated = o.gated;
        ex->escaped = o.escaped;
        if (o.conf_n) {
            ex->conf_min = o.conf_min;
            ex->conf_mean = o.conf_sum / (float)o.conf_n;
        }
    }
    if (o.frame != T || produced != T || o.headed + o.gated != T) {
        text[0] = 0;
        return VOCR_E_FORMAT; /* cannot happen for a validated blob */
    }
    return o.overflow ? VOCR_E_TEXT : VOCR_OK;
}

void vocr_sizes(size_t *model_bytes, size_t *ctx_bytes)
{
    if (model_bytes) *model_bytes = sizeof(vocr_model);
    if (ctx_bytes) *ctx_bytes = sizeof(vocr_ctx);
}

/* Kotoba3DS addition (see VOCR_SIMD32 above). Reorders the int8 weights in place, once; the
 * blob's CRC no longer matches afterwards (check it first). Returns 1 if packed, 0 if this
 * build has no SIMD32 path (the weights are then left alone). */
int vocr_pack_simd32(vocr_model *m)
{
#ifdef VOCR_SIMD32
    unsigned i, nb, k, j;
    if (!m || !m->blob) return 0;
    if (m->packed) return 1;
    for (i = 0; i < m->n_ops; i++) {
        const vocr_op *op = &m->op[i];
        const unsigned K = op->k;
        if (!op->q) continue;
        if (m->attn.dim && i + 2u == m->n_ops) continue; /* the attention op: read by mat_block's mm_i16 */
        for (nb = 0; nb < op->cout_pad / 8u; nb++) {
            int8_t *w = (int8_t *)(void *)(uintptr_t)(op->q + (size_t)nb * K * 8);
            for (k = 0; k + 2 <= K; k += 2) {
                int8_t t[16];
                for (j = 0; j < 8; j += 2) {
                    t[2 * j + 0] = w[k * 8 + j];
                    t[2 * j + 1] = w[k * 8 + j + 1];
                    t[2 * j + 2] = w[(k + 1) * 8 + j];
                    t[2 * j + 3] = w[(k + 1) * 8 + j + 1];
                }
                memcpy(w + (size_t)k * 8, t, 16);
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
