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
 * The line is processed in tiles of `tile_frames` CTC frames (`stride` input columns each:
 * 8, or 12 / 16 for the coarser-frame models of train2.py --stride),
 * carrying each layer's left context between tiles, so the arena does not grow with
 * the line width and the output is bit-identical for every tile size. A blob with an attention op (VOCR6,
 * vocr_attn below) keeps every frame's bottleneck output for the attention over the whole line, which the head
 * then reads: its arena grows with max_width by that line (still independent of the tile and batch sizes' output).
 *
 * Preprocessing matches train/train.py exactly: grey = (77 R + 150 G + 29 B) >> 8
 * (then 255 - grey with VOCR_INVERT, as synth/common.line_crops does for dark text),
 * OpenCV INTER_LINEAR resize to the model height with width round(w * H / h) clamped
 * to [8, max_width] (1024; 1020 at stride 12), right-pad to a multiple of the stride by
 * repeating the last column, then
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

/* The head gate (docs/headgate.md, issue #117): an optional section after the version-1 blob, at its
 * 16-byte aligned end ("VOCG", train/headgate.py writes it), with a tiny network on the head's input:
 *   h = relu(int8 layer 1 on the head's input quantised to int16, exactly as a W8I pointwise layer computes it,
 *            in every mode)
 *   score = b2 + w2[0] h[0] + w2[1] h[1] + ...   (float, in this order)
 * and a threshold t: with vocr_opts.head_gate = VOCR_HEAD_GATE_ON a frame whose score > t skips the output
 * head and reads as the blank (its blank logit computed as the run's head computes it, every other class -inf).
 * A blob without the section loads and runs as before; a runtime without gate support ignores it.
 * vocr_check_crc checks the section's CRC, which covers its arrays but not its 64-byte header: t and b2 are
 * content outside it, as the base blob's header is outside the base CRC (vocr_load rejects a NaN in either;
 * another changed value is not detected; audit-head-gate N5). */
#define VOCR_GATE_VERSION 1
#define VOCR_GATE_MAX_HIDDEN 64

typedef struct {             /* a blob's head gate; hidden = 0: the blob has none */
    const int8_t *q;         /* layer 1: [hidden / 8][k][8] int8, packed like a pointwise layer */
    const float *scale;      /* layer 1: hidden per-unit scales */
    const float *bias;       /* layer 1: hidden biases */
    const float *w2;         /* layer 2: hidden weights */
    float b2;                /* layer 2: bias */
    float t;                 /* threshold: skip iff score > t */
    unsigned k, hidden;      /* input width (the head's K) and hidden units (8..VOCR_GATE_MAX_HIDDEN, x8) */
    uint32_t bytes;          /* bytes of the gate section */
} vocr_gate;
/* The two-level output head (vita-vn-ocr #117 item 2; eval/head2level_protocol.json; W8F since #126): an optional
 * 'VOC2' trailer after the version-1 blob. The blob's head rows are in frequency order (class 0 the blank, then the most frequent
 * classes); level 1 is the head's first l1_blocks 8-row blocks, computed on every frame, with an escape that says
 * whether the frame needs the other (rare) rows, which then run as the head's own. Absent: l1_blocks == 0. */
typedef struct {
    unsigned l1_blocks;          /* level 1 = head blocks [0, l1_blocks); 0: no trailer */
    unsigned hidden;             /* escape hidden units (a multiple of 8) */
    float t, e_b2;               /* the escape fires iff its score > t; layer 2's bias */
    const int8_t *e_q;           /* escape layer 1: int8 [hidden / 8][K][8] on the head's quantised input */
    const float *e_scale, *e_bias, *e_wl1, *e_wb0, *e_w2;   /* [hidden] each */
} vocr_2level;

/* The attention op (VOCR6 stage 3, docs/vocr6-attn.md; train/export.py "ATTN"): one light global self-attention layer
 * over the line's frames, op[n_ops - 2], right before the head, on the d = op.cin values of each frame:
 *   z' = z + O(concat_h softmax(Q_h K_h^T) V_h)
 * The op's own matrix (w or q, scale, bias; cout_pad = 3 dim rows) is QKV, with 1 / sqrt(dim / heads) folded into Q;
 * O carries the trained residual gate. The runtime keeps the bottleneck's output of every frame of the line (so the
 * arena grows with max_width by max_width / stride x d floats), runs QKV and O in the blob's mode and the scores,
 * softmax and weighted sums in float32, then the head on z'. Absent: dim == 0 (every blob before VOCR6). */
typedef struct {
    unsigned heads, dim;     /* heads and the Q / K / V width; dim / heads is a multiple of 8 */
    const float *o_w;        /* O: float weights [d / 8][dim][8], or NULL */
    const int8_t *o_q;       /* O: int8 weights, or NULL */
    const float *o_scale;    /* O: per-output scales of o_q */
    const float *o_bias;     /* O: bias [d] */
} vocr_attn;

typedef struct {             /* a parsed blob; points into it, so keep the blob alive */
    const unsigned char *blob;
    size_t bytes;            /* the version-1 blob's bytes (without a gate section) */
    unsigned height, max_width, stride, n_ops, n_classes, int8, params;
    const uint16_t *vocab_off;   /* n_classes + 1 offsets into vocab */
    const char *vocab;           /* UTF-8; class c is vocab[off[c] .. off[c+1]) */
    unsigned packed;             /* Kotoba3DS: weights reordered by vocr_pack_simd32() */
    vocr_op op[VOCR_MAX_OPS];
    vocr_gate gate;              /* the head gate, if the blob has one (gate.hidden != 0) */
    vocr_2level two;             /* the 'VOC2' trailer's two-level head, if the blob has one */
    vocr_attn attn;              /* the attention op, if the blob has one (attn.dim != 0) */
} vocr_model;

/* vocr_opts.head_gate */
enum {
    VOCR_HEAD_GATE_OFF = 0,  /* no gate: today's run, arena and output exactly */
    VOCR_HEAD_GATE_ON = 1,   /* skip the head on the frames the gate calls blank */
    VOCR_HEAD_GATE_SCORE = 2 /* compute the gate's score (vocr_extra.gate_score) but skip nothing */
};

typedef struct {
    unsigned tile_frames;    /* CTC frames per tile (0: default 4; for a stride over 8 the
                                frames nearest 32 input columns: 3 at 12, 2 at 16);
                                >= max_width / stride runs the whole line as one tile */
    unsigned head_frames;    /* frames per output-head batch (0: default 16); with an attention op also its
                                query chunk */
    unsigned max_width;      /* widest resized line in pixels (0: the model's clamp) */
    int mode;                /* VOCR_MODE_*; AUTO = F32 for float blobs, W8F for int8 */
    unsigned head_gate;      /* VOCR_HEAD_GATE_*: ON and SCORE need a blob with a gate (else VOCR_E_MODE), in
                                any mode (the gate's own arithmetic is the same in every mode); ON skips
                                nothing in a run that asks for the logits */
    unsigned two_level;      /* 1: the two-level head (VOCR_MODE_W8I or W8F and a blob with a 'VOC2' trailer,
                                else VOCR_E_MODE); 0: the full head (default) */
} vocr_opts;
/* A zeroed vocr_opts gives the defaults. Larger tiles and head batches trade arena
 * bytes for speed; the output does not change. With head_gate ON the head's batches hold
 * only the frames it runs on, so they stay full; the output does not depend on the tile or
 * batch size either. */

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
    int (*cancel)(void *user);   /* in: polled once per tile (with an attention op also once per chunk of its
                                    frames and of the head's after it); nonzero stops */
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
    /* The head gate (docs/headgate.md). Analysis outputs, which change nothing (NULL: none): */
    float *head_in;          /* in: cap_frames x K floats (K = the head's input width), each frame's head input */
    float *gate_score;       /* in: cap_frames floats, each frame's gate score (head_gate ON or SCORE) */
    unsigned gated;          /* out: frames whose head the gate skipped (0 unless head_gate ON); such a frame's
                                best is 0 and its top-K is [its blank logit, -inf, ...] with classes 0 */
    unsigned ungated;        /* in: nonzero reads this line without skipping, as head_gate OFF does (e.g. again
                                for vocr_post_redecode, whose look-back reads blank frames' top-K), and with
                                the full head when two_level is on: the line as the default options read it */
    /* The two-level head (docs/head2level.md). Analysis outputs, which change nothing (NULL: none): */
    float *esc_score;        /* in: cap_frames floats: each frame's two-level escape score (two_level on; a
                                frame the gate skipped gets none) */
    unsigned escaped;        /* out: frames the two-level escape fired on (0 with the full head) */
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
    unsigned gate;           /* VOCR_HEAD_GATE_* of the plan */
    uint32_t fidx_off;       /* head_gate ON: each accumulated frame's index in the line (uint16) */
    unsigned two_level;      /* the two-level head is on (vocr_opts.two_level) */
    uint32_t nbv_off, b0_off, hid_off, fmap_off;   /* its arena (two_level only) */
    unsigned attn;           /* the attention op's index (n_ops - 2), or 0: the blob has none */
    uint32_t line_off;       /* attention: every frame's bottleneck output (max_width / stride x d floats) */
    uint32_t kp_off, vp_off, qc_off, ac_off, sc_off, as_off, att_tp;   /* its working set; att_tp: frames, x8 */
    vocr_plan_op op[VOCR_MAX_OPS];
} vocr_ctx;

/* Parse and validate a blob already in memory. Nothing is copied. A head gate section
 * (VOCG) at the version-1 blob's aligned end is parsed too, when bytes covers it. */
int vocr_load(vocr_model *m, const void *blob, size_t bytes);
/* Kotoba3DS addition: reorder the int8 weights for the ARMv6 SIMD kernel (in place; the
 * blob must be writable). 1 = done, 0 = not available in this build. */
int vocr_pack_simd32(vocr_model *m);
/* Kotoba3DS: nonzero (the default) = the SIMD32 path multiplies two positions at a time; 0 = one
 * at a time, as before (the same text, for timing them against each other). */
extern int vocr_use_pairs;
/* Check the blob's CRC-32, and the gate section's (optional; vocr_load checks structure, not content). */
int vocr_check_crc(const void *blob, size_t bytes);
/* 1 if the loaded blob has a head gate (vocr_opts.head_gate can be ON), else 0. */
int vocr_has_gate(const vocr_model *m);
/* 1 if the loaded blob has a two-level head (a VOC2 trailer: vocr_opts.two_level can be 1), else 0. */
int vocr_has_two_level(const vocr_model *m);
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
