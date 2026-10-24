/* vocr.h - a tiny CTC text-line recognizer for visual-novel dialogue on PS Vita / PSP.
 *
 * Plain C99. No malloc and no libc beyond memcpy/memset and <math.h>.
 * The weights blob (written by train/export.py) is used in place; all working memory
 * comes from one caller-provided arena whose exact size vocr_arena_bytes() reports.
 *
 *   vocr_model m;  vocr_ctx c;  vocr_opts o = {0};          // defaults
 *   vocr_load(&m, blob, blob_bytes);                         // blob 16-byte aligned
 *   size_t need = vocr_arena_bytes(&m, &o);
 *   vocr_init(&c, &m, &o, arena, need);                      // arena 16-byte aligned
 *   vocr_run(&c, &img, text, sizeof text, NULL);             // one line crop -> UTF-8
 *
 * The line is processed in tiles of `tile_frames` CTC frames (8 input columns each),
 * carrying each layer's left context between tiles, so the arena does not grow with
 * the line width and the output is bit-identical for every tile size.
 *
 * Preprocessing matches train/train.py exactly: grey = (77 R + 150 G + 29 B) >> 8
 * (then 255 - grey with VOCR_INVERT, as synth/common.line_crops does for dark text),
 * OpenCV INTER_LINEAR resize to the model height with width round(w * H / h) clamped
 * to [8, 1024], right-pad to a multiple of 8 by repeating the last column, then
 * x / 127.5 - 1. Greedy CTC (blank 0) gives the text.
 *
 * Results are bit-identical across the scalar and NEON paths and across platforms
 * when the compiler does not contract a*b+c into fused multiply-adds (build with
 * -ffp-contract=off, which ISO -std=c99 already implies on GCC). The one exception:
 * ARMv7 NEON flushes denormals to zero, so a model producing denormal activations
 * could differ in those values on the NEON path.
 */
#ifndef VOCR_H
#define VOCR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOCR_BLOB_VERSION 1
#define VOCR_MAX_OPS 48
#define VOCR_MAX_HEIGHT 64

enum {
    VOCR_OK = 0,
    VOCR_E_ARG = -1,       /* bad argument or image */
    VOCR_E_FORMAT = -2,    /* not a valid version-1 blob, or an unsupported layer */
    VOCR_E_ALIGN = -3,     /* blob or arena not 16-byte aligned */
    VOCR_E_ARENA = -4,     /* arena smaller than vocr_arena_bytes() */
    VOCR_E_WIDTH = -5,     /* resized line wider than opts.max_width */
    VOCR_E_TEXT = -6,      /* text buffer too small; text holds whole characters only */
    VOCR_E_MODE = -7,      /* arithmetic mode not available for this blob */
    VOCR_E_CANCELLED = -8  /* the cancel callback returned nonzero */
};

/* Arithmetic. F32: float weights. W8F: int8 weights, float activations and
 * accumulation. W8I: int8 weights, activations quantised per position to int16,
 * exact int32 sums over chunks of 512 terms (so they cannot overflow), chunk sums
 * added in float. Stem, depthwise layers and biases are float in every mode. */
enum { VOCR_MODE_AUTO = 0, VOCR_MODE_F32 = 1, VOCR_MODE_W8F = 2, VOCR_MODE_W8I = 3 };

enum { VOCR_GREY8 = 1, VOCR_RGBA8 = 4 };   /* pixel formats (RGBA: R, G, B, A bytes) */
/* Or'ed into vocr_image.format: read every pixel as 255 - grey, before the resize, as
 * training does for dark text (synth/common.line_crops). Set it when the line finder
 * reports VocrLine.dark = 1 (docs/linefind.md, "Polarity contract"); it needs no copy
 * of the crop. */
enum { VOCR_INVERT = 0x100 };

typedef struct {
    uint8_t type, flags, kh, kw, sh, sw, ph, pw;
    uint16_t cin, cout, h_in, h_out, k, cout_pad;
    const float *w;          /* float weights, or NULL */
    const int8_t *q;         /* int8 weights, or NULL */
    const float *bias;       /* NULL if the op has none */
    const float *scale;      /* per-output-channel scales of q */
} vocr_op;

typedef struct {             /* a parsed blob; points into it, so keep the blob alive */
    const unsigned char *blob;
    size_t bytes;
    unsigned height, max_width, stride, n_ops, n_classes, int8, params;
    const uint16_t *vocab_off;   /* n_classes + 1 offsets into vocab */
    const char *vocab;           /* UTF-8; class c is vocab[off[c] .. off[c+1]) */
    unsigned packed;             /* Kotoba3DS: weights reordered by vocr_pack_simd32() */
    vocr_op op[VOCR_MAX_OPS];
} vocr_model;

typedef struct {
    unsigned tile_frames;    /* CTC frames per tile (0: default 4); >= max_width / 8 runs
                                the whole line as one tile, i.e. without streaming */
    unsigned head_frames;    /* frames per output-head batch (0: default 16) */
    unsigned max_width;      /* widest resized line in pixels (0: the model's clamp) */
    int mode;                /* VOCR_MODE_*; AUTO = F32 for float blobs, W8F for int8 */
} vocr_opts;
/* A zeroed vocr_opts gives the defaults. Larger tiles and head batches trade arena
 * bytes for speed; the output does not change. */

typedef struct {
    const unsigned char *pixels;   /* top-left pixel of the line crop */
    unsigned width, height;        /* crop size in pixels */
    size_t stride;                 /* bytes from one row to the next */
    int format;                    /* VOCR_GREY8 or VOCR_RGBA8, optionally | VOCR_INVERT */
} vocr_image;

#define VOCR_TOPK_MAX 16

typedef struct {             /* optional per-line extras for vocr_run() */
    float *logits;           /* in: frames x n_classes floats, or NULL */
    uint16_t *best;          /* in: argmax class per frame, or NULL */
    unsigned cap_frames;     /* in: capacity of logits / best / topk_* / blank_logit in frames */
    int (*cancel)(void *user);   /* in: polled once per tile; nonzero stops */
    void *user;
    unsigned width;          /* out: resized width before padding */
    unsigned frames;         /* out: CTC frames */
    unsigned chars;          /* out: decoded characters */
    /* Per-frame top-K and a confidence for the greedy text, for vocr_lm_decode() (vocr_lm.h) and for
     * callers that re-read a doubtful line. topk = 0 (zeroed extras) is today's path exactly. Neither
     * the text nor any logit depends on topk, and the arena does not either: the top-K state lives in
     * the caller's buffers below, which must hold cap_frames >= the line's frames
     * (vocr_resized_width(m, w, h) / m->stride + 1 always suffices; else VOCR_E_ARG). */
    unsigned topk;           /* in: 0 off, 1..VOCR_TOPK_MAX: keep the K largest logits per frame */
    float *topk_logit;       /* in: cap_frames x topk floats, per frame decreasing (ties: the lower
                                class first, so topk_class[f * topk] is the argmax of frame f) */
    uint16_t *topk_class;    /* in: cap_frames x topk classes, aligned with topk_logit */
    float *blank_logit;      /* in: cap_frames floats, class 0's logit per frame, or NULL */
    float conf_min;          /* out (topk >= 1): over the frames that emit a character of the text,
                                the smallest softmax-max probability, computed over that frame's
                                top-K logits (an upper bound of the full softmax's, equal to it when
                                the classes outside the top K carry no mass); 1 with no character */
    float conf_mean;         /* out (topk >= 1): the mean of the same per-character probabilities */
} vocr_extra;

typedef struct {             /* per-op plan; private */
    int32_t lag;
    uint32_t n_tile, n_step, cap, halo, in_col, out_col, in_off, scr_off, dst;
} vocr_plan_op;

typedef struct {             /* a planned arena; private apart from the first fields */
    const vocr_model *model;
    unsigned char *arena;
    size_t arena_bytes;      /* bytes the plan uses (== vocr_arena_bytes()) */
    int mode;
    unsigned tile, head, max_width;
    unsigned front, n_src, acc_frames;
    uint32_t rows_off, ring_off, ring_end, acc_off, bestv_off, besti_off;
    vocr_plan_op op[VOCR_MAX_OPS];
} vocr_ctx;

/* Parse and validate a blob already in memory. Nothing is copied. */
int vocr_load(vocr_model *m, const void *blob, size_t bytes);
/* Kotoba3DS addition: reorder the int8 weights for the ARMv6 SIMD kernel (in place; the
 * blob must be writable). 1 = done, 0 = not available in this build. */
int vocr_pack_simd32(vocr_model *m);

/* Check the blob's CRC-32 (optional; vocr_load checks structure, not content). */
int vocr_check_crc(const void *blob, size_t bytes);
/* Exact arena bytes for lines up to o->max_width (0 if the options are invalid). */
size_t vocr_arena_bytes(const vocr_model *m, const vocr_opts *o);
/* Plan the arena. bytes must be >= vocr_arena_bytes(m, o). */
int vocr_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes);
/* Recognise one line crop. text gets NUL-terminated UTF-8; extra may be NULL. */
int vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t text_cap, vocr_extra *extra);
/* Resized width (before padding) the runtime uses for a w x h crop. */
unsigned vocr_resized_width(const vocr_model *m, unsigned w, unsigned h);
/* The preprocessed grey line, height x padded width bytes, row-major, exactly as
 * vocr_run() sees it. With out == NULL only *width and *padded_width are set. */
int vocr_preprocess(const vocr_model *m, const vocr_image *img, unsigned char *out,
                    size_t cap, unsigned *width, unsigned *padded_width);
/* UTF-8 bytes of class c, *len of them (not NUL-terminated; "" for the blank, class 0). */
const char *vocr_class_text(const vocr_model *m, unsigned c, unsigned *len);
const char *vocr_strerror(int rc);
/* sizeof(vocr_model) and sizeof(vocr_ctx), for foreign-function bindings. */
void vocr_sizes(size_t *model_bytes, size_t *ctx_bytes);

#ifdef __cplusplus
}
#endif
#endif
