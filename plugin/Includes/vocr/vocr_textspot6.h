/* The full-frame text spotter for VTSP version 1, 2, 3 and 4 blobs (issue #128, docs/textspot6.md): version 4 is the
 * TAGGED spotter, whose network gives every box a class (dialogue, speaker name, choice, UI text, scene text).
 *
 * Versions 1, 2 and 3 are textspot's, textspot2's and textspot3's spotters (vocr_textspot.h, vocr_textspot2.h,
 * vocr_textspot3.h): for a version-1, 2 or 3 blob every call here gives what vocr_textspot3.c gives, bit for bit
 * (boxes, blocks, order, pick, truncated flag, dark flags, the arena sizes), with every tag 0, every tconf 0 and
 * presence the largest box confidence. Version 4 is textspot3's stride-8 trunk (a VTDT network, as version 2) or its
 * multi-scale network (a VTMS section, as version 3) whose d2s head carries, beside version 2's 16 channels, T tag
 * maps per sub-cell (T = 1..4): channel 16 + T (2 dy + dx) + t is tag map t of the sub-cell (dy, dx). The decode
 * reads 4 + T maps at stride 4 and gives each box a class: class t + 1 scores the sum of tag map t over the box's
 * kernel cells, class 0 (dialogue) scores 0, the largest score wins (the lowest class on a tie), and tconf is
 * min(127, floor((best - second) / n)) over the n cells. Two boxes join a block only when they have one class; the
 * pick is the dialogue (class 0) block of the largest score (-1 when there is none) and presence the largest
 * confidence of a dialogue box (0 when there is none). A version-4 frame is halved when it is wider than half_above
 * or taller than half_above_h (0: never by that side), in both forms; VOCR_TEXTSPOT6_SCALE1 / _SCALE2 force the scale
 * instead (any version: a region read at the scale of the frame it was cut from). synth/textspot6_twin.py specifies
 * the format (its docstring) and is the bit-exact Python twin of all four versions.
 *
 * The flags, limits and error codes are vocr_textspot.h's (VOCR_TEXTSPOT_NO_DARK, VOCR_TEXTSPOT_MAX_BOXES,
 * VOCR_TEXTSPOT_MAX_SIDE, VOCR_TEXTSPOT_E_BLOB, ...). The result types are new (VocrTextspot6Box, VocrTextspot6Block,
 * VocrTextspot6Set: the fields of vocr_textspot.h's with the class added); vocr_textspot.h is unchanged.
 * vocr_textspot.c, vocr_textspot2.c and vocr_textspot3.c are not linked: this file is self-contained (link
 * vocr_lines.c and vocr_lines_rows.c for the dark flags).
 *
 * VOCR_TEXTSPOT6_NO_MS (a compile-time option): leaves out every VTMS (multi-scale) path, the map ops, their kernels
 * and their arena plan. Versions 3 and 4's multi-scale form are then refused at load (VOCR_TEXTSPOT_E_BLOB);
 * versions 1, 2 and 4's trunk form give the same bits as without it. A console build for a family-T (trunk) blob
 * needs no map-op code. The option changes no type here: vocr_textspot6's layout is the same in both builds.
 *
 * C99, no allocation, no recursion, no global state, integer arithmetic only. The caller passes an arena of
 * vocr_textspot6_arena_bytes() bytes (no alignment needed). Versions 1 and 2 read the frame row by row in one pass,
 * so their arena depends on the frame's width only; a version-4 trunk's also on whether the frame is halved (by its
 * height too); a version-3 or version-4 multi-scale arena holds whole maps, so it depends on both sides.
 */
#ifndef VOCR_TEXTSPOT6_H
#define VOCR_TEXTSPOT6_H

#include <stddef.h>

#include "vocr_textspot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOCR_TEXTSPOT6_VERSION_D2S 2u   /* the VTSP version of a 2 x 2 sub-pixel head on a stride-8 trunk */
#define VOCR_TEXTSPOT6_VERSION_MS 3u    /* the VTSP version of textspot3's multi-scale family M */
#define VOCR_TEXTSPOT6_VERSION_TAG 4u   /* the VTSP version of the tagged spotter (a trunk or a multi-scale network) */
#define VOCR_TEXTSPOT6_MAX_TAGS 4u      /* tag maps of a version-4 head: classes 0 .. T */
#define VOCR_TEXTSPOT6_MAX_OPS 64u      /* map ops of a VTMS network (the head is the last) */
#define VOCR_TEXTSPOT6_MAX_MAPS 65u     /* maps: the stream's output, then one per op */

/* The classes of a version-4 box (synth/textspot6_twin.CLASSES); a blob of T tag maps has the classes 0 .. T. */
#define VOCR_TEXTSPOT6_DIALOGUE 0u      /* dialogue and narration */
#define VOCR_TEXTSPOT6_NAME 1u          /* a speaker name */
#define VOCR_TEXTSPOT6_CHOICE 2u        /* a choice */
#define VOCR_TEXTSPOT6_UI 3u            /* UI text */
#define VOCR_TEXTSPOT6_SCENE 4u         /* scene text: signs, writing in a CG */

/* vocr_textspot6.form */
#define VOCR_TEXTSPOT6_FORM_NONE 0u     /* versions 1, 2 and 3 */
#define VOCR_TEXTSPOT6_FORM_TRUNK 1u    /* version 4, a VTDT trunk */
#define VOCR_TEXTSPOT6_FORM_MS 2u       /* version 4, a VTMS multi-scale network */

/* Flags of vocr_textspot6_find(_rows), beside VOCR_TEXTSPOT_NO_DARK (vocr_textspot.h): the scale f forced instead of
 * the blob's rule (synth/textspot6_twin.spot(model, img, scale=f)), for a region cut from a frame and read at the
 * scale the rule gives that frame (every console frame is 960 x 544, so a half-scale blob reads every region of it
 * halved). One of the two at most; any version. */
#define VOCR_TEXTSPOT6_SCALE1 0x2u      /* f = 1: the frame (region) read at its own size */
#define VOCR_TEXTSPOT6_SCALE2 0x4u      /* f = 2: the frame (region) halved by textspot's exact 2x2 mean */
#define VOCR_TEXTSPOT6_FLAGS 0x7u       /* every defined flag: VOCR_TEXTSPOT_NO_DARK, SCALE1, SCALE2 */

/* A map op of a VTMS network, as loaded (pointers into the blob), with the buffer plan of its output. */
typedef struct {
    const unsigned char *w;     /* int8 weights [cout][cin, or 1 if depthwise][k][k], in the blob */
    const unsigned char *b;     /* little-endian int32 bias[cout] */
    const unsigned char *m;     /* little-endian int32 multiplier[cout] (> 0) */
    const unsigned char *sh;    /* u8 shift[cout] (1..62) */
    unsigned short cin, cout;   /* cin: the sum of its sources' channels (a depthwise op: cout) */
    unsigned short out_stride;  /* network-input px per cell of its output map */
    unsigned char kind;         /* 0: a 1x1 layer over 1..3 sources, 1: a 3x3 depthwise layer */
    unsigned char stride, dil, relu, nsrc;
    unsigned char src[3], up[3];   /* its source maps and their upsampling exponents */
    unsigned char inplace;      /* 1: its output overwrites its dying source's buffer */
    unsigned char slot;         /* its output's buffer (0xFF: the head, whose rows go to the decode) */
} vocr_textspot6_op;

/* A loaded blob: about 4 KB on 32-bit ARM. It points into the blob, which must stay alive and unchanged while the
 * model is used; the blob needs no alignment. Read-only after vocr_textspot6_load, so threads can share it (each with
 * its own arena). */
typedef struct {
    vocr_textspot_layer layers[VOCR_TEXTSPOT_MAX_LAYERS];   /* a VTDT network; a VTMS network's stream */
    unsigned n_layers;
    unsigned version;           /* the VTSP version: 1, 2, 3 or 4 */
    unsigned outputs;           /* the head's channels: 4 (version 1), 16 (versions 2, 3), 16 + 4 tags (version 4) */
    unsigned net_stride;        /* the network's output stride: 4 or 8 (versions 2 to 4: 8) */
    unsigned stride;            /* the stride of the maps the decode reads: net_stride, or 4 for versions 2 to 4 */
    unsigned min_cells;         /* the decode drops kernel components of fewer cells */
    unsigned channels;          /* output channels of all layers (a VTMS network: of the stream's layers) */
    unsigned half_above;        /* a frame wider than this is halved before the network (0: never by its width) */
    unsigned half_above_h;      /* versions 3, 4: a frame taller than this is halved (0: never by its height); else 0 */
    unsigned conf_min;          /* candidates of a lower confidence are dropped (1..127) */
    unsigned tags;              /* version 4: the head's tag maps T, 1..4 (classes 0 .. T); versions 1-3: 0 */
    unsigned form;              /* VOCR_TEXTSPOT6_FORM_*: version 4's network, a trunk or a multi-scale one */
    unsigned long blob_bytes;   /* the VTSP blob */
    unsigned long net_bytes;    /* its network (a VTDT blob or a VTMS section) */
    unsigned char ckpt[8];      /* sha256 prefix of the network's source checkpoint */
    char name[33];              /* the network's shape name, NUL-terminated */
    /* a VTMS network (version 3, version 4's multi-scale form) */
    unsigned n_ops, n_slots;
    vocr_textspot6_op ops[VOCR_TEXTSPOT6_MAX_OPS];
    unsigned short map_stride[VOCR_TEXTSPOT6_MAX_MAPS];  /* map 0: the stream's output, map k + 1: op k's */
    unsigned short map_chans[VOCR_TEXTSPOT6_MAX_MAPS];
    signed char last_use[VOCR_TEXTSPOT6_MAX_MAPS];       /* the last op reading the map; -1: none */
    unsigned char map_slot[VOCR_TEXTSPOT6_MAX_MAPS];     /* the buffer holding it (0xFF: the head's output) */
    unsigned short slot_cap[VOCR_TEXTSPOT6_MAX_MAPS];    /* a buffer's channels per cell: its maps' most */
    unsigned short slot_stride[VOCR_TEXTSPOT6_MAX_MAPS]; /* the stride of its maps */
    unsigned loaded;
    unsigned packed;            /* Kotoba3DS: the 1x1 layers' weights reordered by vocr_textspot6_pack_simd32 */
} vocr_textspot6;

/* A text box in frame pixels, padded as #97's boxes are (max(2, 18% of the size across) on every side, clipped to
 * the frame): VocrTextspotBox's fields and the box's class. */
typedef struct {
    unsigned short x, y, w, h;
    unsigned char vertical;     /* 1: a vertical column (read it turned 90 degrees, by VOCR5-V) */
    unsigned char conf;         /* 1..127: the component's mean kernel code */
    unsigned char dark;         /* 1: text darker than its background (vocr_line_dark of the box); 0 with
                                   VOCR_TEXTSPOT_NO_DARK */
    unsigned char block;        /* its block, an index into blocks[] */
    unsigned char tag;          /* its class, 0 .. tags (VOCR_TEXTSPOT6_DIALOGUE ...); 0 for versions 1-3 */
    unsigned char tconf;        /* 0..127: min(127, (best - second class score) / cells); 0 for versions 1-3 */
} VocrTextspot6Box;

/* A block: boxes of one orientation and one class that belong together (lines of one text box, columns of one
 * balloon), its boxes boxes[first .. first + count) in reading order; x, y, w, h their union. */
typedef struct {
    unsigned short x, y, w, h;
    unsigned char vertical;
    unsigned char first, count;
    unsigned char tag;          /* its boxes' class */
} VocrTextspot6Block;

/* The spotter's result: blocks in reading order (rows of blocks top to bottom; a row left to right, or right to left
 * when all its blocks are vertical), each block's boxes in its reading order (horizontal lines top to bottom, side by
 * side ones left to right; vertical columns right to left). */
typedef struct {
    VocrTextspot6Box boxes[VOCR_TEXTSPOT_MAX_BOXES];
    VocrTextspot6Block blocks[VOCR_TEXTSPOT_MAX_BOXES];
    unsigned count;             /* boxes; 0: no text on the frame */
    unsigned block_count;
    int pick;                   /* the dialogue (class 0) block picked as the likely dialogue, -1 when there is none */
    unsigned truncated;         /* 1 when candidates were dropped (more than VOCR_TEXTSPOT_MAX_BOXES) */
    unsigned presence;          /* the largest confidence of a dialogue box (1..127), 0 when there is none */
} VocrTextspot6Set;

/* Checks the blob: a version-1, 2 or 3 blob exactly as vocr_textspot3_load does; a version-4 blob every check of
 * synth/textspot6_twin.Model (the VTSP header: magic, version, size, CRC-32, conf_min 1..127, tags 1..4, the
 * network's offset and size, half_above_h, the reserved field; then by the network's magic a VTDT trunk with every
 * check of textspot2_twin.Net and outputs 16 + 4 tags, or a VTMS section with every check of textspot3_twin.Net and
 * outputs 16 + 4 tags, a multiple of 8, in its header and its head op). With VOCR_TEXTSPOT6_NO_MS a version-3 blob
 * and a version-4 VTMS network are refused. Points m into the blob. VOCR_LINES_OK, VOCR_LINES_E_ARG (a NULL pointer)
 * or VOCR_TEXTSPOT_E_BLOB; on an error m is cleared. */
int vocr_textspot6_load(vocr_textspot6 *m, const void *blob, size_t bytes);

/* Kotoba3DS addition: reorders the 1x1 layers' int8 weights in place (the blob must be writable; its CRC no longer
 * matches afterwards) for the ARMv6 SIMD kernel of the ARM11, which reads 8 output channels x 2 pixels per pass.
 * 1: packed; 0: this build has no such kernel (the weights are left alone). The boxes are the same bits. */
int vocr_textspot6_pack_simd32(vocr_textspot6 *m);

/* Exact arena bytes for every frame up to max_width x max_height read at the blob's scale (no SCALE flag): for a
 * version-1 or version-2 blob vocr_textspot3_arena_bytes' number (the width matters only); for version 3 and version
 * 4 the most any frame up to that size needs: the frame of max_width x max_height with each side clamped to
 * half_above / half_above_h where that field is > 0 and the side is larger (read at full size), or the max_width x
 * max_height frame itself when it is halved. 0 for a NULL or unloaded model, a zero side, a side over
 * VOCR_TEXTSPOT_MAX_SIDE, or (a VTMS network) a need over 2^31 - 256 bytes. */
size_t vocr_textspot6_arena_bytes(const vocr_textspot6 *m, unsigned max_width, unsigned max_height);

/* Exact arena bytes for a frame of exactly width x height read at the blob's scale (what the calls check). */
size_t vocr_textspot6_frame_arena_bytes(const vocr_textspot6 *m, unsigned width, unsigned height);

/* Exact arena bytes for a frame of exactly width x height read with these flags (what the calls check):
 * vocr_textspot6_frame_arena_bytes without a SCALE flag, else the need at the forced scale. 0 as
 * vocr_textspot6_frame_arena_bytes, and for undefined flags or both SCALE flags. */
size_t vocr_textspot6_frame_arena_bytes_flags(const vocr_textspot6 *m, unsigned width, unsigned height,
                                              unsigned flags);

/* Exact arena bytes for every frame up to max_width x max_height read with these flags: vocr_textspot6_arena_bytes
 * without a SCALE flag; with one, the max_width x max_height frame's need at that scale (at a fixed scale every
 * arena size grows with both sides), e.g. every region of a 960 x 544 frame read with VOCR_TEXTSPOT6_SCALE2 fits
 * vocr_textspot6_arena_bytes_flags(m, 960, 544, VOCR_TEXTSPOT6_SCALE2). 0 as vocr_textspot6_frame_arena_bytes_flags. */
size_t vocr_textspot6_arena_bytes_flags(const vocr_textspot6 *m, unsigned max_width, unsigned max_height,
                                        unsigned flags);

/* The spotter on a frame in memory (VOCR_LINES_GREY or VOCR_LINES_RGBA: grey = (77 R + 150 G + 29 B) >> 8), as
 * vocr_textspot_find: flags 0 is synth/textspot6_twin.spot exactly; VOCR_TEXTSPOT_NO_DARK leaves every dark flag 0
 * and reads no box; VOCR_TEXTSPOT6_SCALE1 / _SCALE2 force the scale (textspot6_twin.spot(..., scale=1 or 2)).
 * VOCR_LINES_OK, VOCR_LINES_E_ARG (NULL pointer, unloaded model, bad format, stride or flags (both SCALE flags), a
 * zero side or one over VOCR_TEXTSPOT_MAX_SIDE) or VOCR_LINES_E_ARENA; on an error out->count, block_count,
 * truncated and presence are 0 and pick -1. The arena is scratch only. */
int vocr_textspot6_find(const vocr_textspot6 *m, const VocrLinesImage *im, unsigned flags, void *arena,
                        size_t arena_bytes, VocrTextspot6Set *out);

/* The same through a row callback, as vocr_textspot_find_rows: the frame's rows once each in increasing order in
 * one pass, then (without VOCR_TEXTSPOT_NO_DARK) each box's rows once, top to bottom, boxes in output order. A NULL
 * row ends the call with VOCR_LINES_E_SOURCE and no boxes. */
int vocr_textspot6_find_rows(const vocr_textspot6 *m, const VocrLinesRows *src, unsigned flags, void *arena,
                             size_t arena_bytes, VocrTextspot6Set *out);

/* Debug and test hook: runs the network on the frame (halved when the model says so, or as VOCR_TEXTSPOT6_SCALE1 /
 * _SCALE2 in flags force) and calls row(user, i, maps, width) for each row i of the maps the decode reads, in
 * increasing order: the head's own rows for version 1, the stride-4 rows of the d2s head for versions 2 to 4; maps
 * holds the row's K, O, A1 and A2, then (version 4) tag maps 0 .. T - 1, one after the other ([0, width),
 * [width, 2 width), ...: 4 + m->tags planes), valid only during the call. Same flags, arena and return codes as
 * vocr_textspot6_find (without its dark flags: VOCR_TEXTSPOT_NO_DARK is implied). */
int vocr_textspot6_maps(const vocr_textspot6 *m, const VocrLinesImage *im, unsigned flags, void *arena,
                        size_t arena_bytes, vocr_textspot_map_row row, void *user);

#ifdef __cplusplus
}
#endif

#endif
