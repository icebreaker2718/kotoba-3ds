/* The full-frame text spotter (issue #104, docs/textspot.md): every text box of a whole frame (PS Vita 960x544,
 * PSP 480x272, Adrenaline's PSP picture at 2x, a PC screenshot scaled to the Vita), each with an orientation, a
 * confidence and a dark flag, grouped into blocks in reading order, with one block picked as the likely dialogue.
 *
 * The network is #97's int8 detector (vocr_textdet.h's VTDT layout, unchanged) inside a VTSP blob: a 32-byte
 * header with two decode parameters (half_above: a frame wider than this is halved by an exact 2x2 mean before
 * the network; conf_min: candidates below this confidence are dropped) around a VTDT blob.
 * synth/textspot_twin.py is the specification and a bit-exact Python twin: spot() gives the same boxes, blocks,
 * order, pick and truncated flag for every frame and every blob its Model accepts.
 *
 * C99, no allocation, no recursion, no global state, integer arithmetic only. The caller passes an arena of
 * vocr_textspot_arena_bytes() bytes (no alignment needed). The frame is read row by row in one pass (halving
 * keeps one frame row of partial sums), so the arena depends on the frame's width, not its height, and the
 * row-callback form (vocr_lines_rows.h) needs no copy of the frame. Self-contained: link vocr_lines.c and
 * vocr_lines_rows.c for the dark flags (vocr_line_dark, vocr_line_dark_rows); vocr_textdet.c is not needed.
 */
#ifndef VOCR_TEXTSPOT_H
#define VOCR_TEXTSPOT_H

#include <stddef.h>

#include "vocr_lines.h"
#include "vocr_lines_rows.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A blob vocr_textspot_load refuses (every check of textspot_twin.Model and of its inner textdet_twin.Model);
 * the same value as VOCR_TEXTDET_E_BLOB. */
#define VOCR_TEXTSPOT_E_BLOB (-5)

/* Largest frame side (width and height). It keeps every sum of the decode within int32; the proof is at the top
 * of vocr_textspot.c. Larger frames are VOCR_LINES_E_ARG. */
#define VOCR_TEXTSPOT_MAX_SIDE 2048u

#define VOCR_TEXTSPOT_MAX_LAYERS 32u
#define VOCR_TEXTSPOT_MAX_BOXES 64u   /* boxes (and so blocks) per frame: textspot_twin.BOX_MAX */

/* Flags of vocr_textspot_find(_rows). 0 is textspot_twin.spot exactly. */
#define VOCR_TEXTSPOT_NO_DARK 0x1u    /* leave every dark flag 0 and read no box: the frame is read exactly once,
                                         top to bottom (with a row callback, no second read of any box's rows).
                                         The caller computes vocr_line_dark on a box's pixels when it reads it
                                         (the same flag, bit for bit) */
#define VOCR_TEXTSPOT_FLAGS 0x1u      /* every defined flag */

typedef struct {
    const unsigned char *w;     /* int8 weights [cout][cin, or 1 if depthwise][k][k], in the blob */
    const unsigned char *b;     /* little-endian int32 bias[cout], in the blob */
    const unsigned char *m;     /* little-endian int32 multiplier[cout] (> 0), in the blob */
    const unsigned char *sh;    /* u8 shift[cout] (1..62), in the blob */
    unsigned short cin, cout;
    unsigned char kind;         /* 0: dense conv, 1: depthwise */
    unsigned char k, stride, dil, relu;
} vocr_textspot_layer;

/* A loaded blob: about 1 KB on 32-bit ARM. It points into the blob, which must stay alive and unchanged while
 * the model is used; the blob needs no alignment. Read-only after vocr_textspot_load, so threads can share it
 * (each with its own arena). */
typedef struct {
    vocr_textspot_layer layers[VOCR_TEXTSPOT_MAX_LAYERS];
    unsigned n_layers;
    unsigned stride;            /* output stride: 4 or 8 network-input pixels per map cell */
    unsigned min_cells;         /* the decode drops kernel components of fewer cells */
    unsigned channels;          /* output channels of all layers */
    unsigned half_above;        /* a frame wider than this is halved before the network (0: never) */
    unsigned conf_min;          /* candidates of a lower confidence are dropped (1..127) */
    unsigned long blob_bytes;   /* the VTSP blob */
    unsigned long net_bytes;    /* its VTDT network */
    unsigned char ckpt[8];      /* sha256 prefix of the network's source checkpoint */
    char name[33];              /* the network's shape name, NUL-terminated */
    unsigned loaded;
} vocr_textspot;

/* A text box in frame pixels, padded as #97's boxes are (max(2, 18% of the size across) on every side, clipped
 * to the frame). */
typedef struct {
    unsigned short x, y, w, h;
    unsigned char vertical;     /* 1: a vertical column (read it turned 90 degrees, by VOCR5-V) */
    unsigned char conf;         /* 1..127: the component's mean kernel code */
    unsigned char dark;         /* 1: text darker than its background (vocr_line_dark of the box); 0 with
                                   VOCR_TEXTSPOT_NO_DARK */
    unsigned char block;        /* its block, an index into blocks[] */
} VocrTextspotBox;

/* A block: boxes of one orientation that belong together (lines of one text box, columns of one balloon), its
 * boxes boxes[first .. first + count) in reading order; x, y, w, h their union. */
typedef struct {
    unsigned short x, y, w, h;
    unsigned char vertical;
    unsigned char first, count;
    unsigned char reserved;     /* 0 */
} VocrTextspotBlock;

/* The spotter's result: blocks in reading order (rows of blocks top to bottom; a row left to right, or right to
 * left when all its blocks are vertical), each block's boxes in its reading order (horizontal lines top to
 * bottom, side by side ones left to right; vertical columns right to left). */
typedef struct {
    VocrTextspotBox boxes[VOCR_TEXTSPOT_MAX_BOXES];
    VocrTextspotBlock blocks[VOCR_TEXTSPOT_MAX_BOXES];
    unsigned count;             /* boxes; 0: no text on the frame */
    unsigned block_count;
    int pick;                   /* the block picked as the likely dialogue, -1 when there is no box */
    unsigned truncated;         /* 1 when candidates were dropped (more than VOCR_TEXTSPOT_MAX_BOXES) */
} VocrTextspotSet;

/* Checks the blob (the VTSP header: magic, version, size, CRC-32, reserved fields, the network's offset and size,
 * conf_min 1..127; then every check of the inner VTDT blob: header fields, the layer chain, section bounds and
 * alignment, multipliers > 0, shifts 1..62) and points m into it. VOCR_LINES_OK, VOCR_LINES_E_ARG (a NULL
 * pointer) or VOCR_TEXTSPOT_E_BLOB; on an error m is cleared (unusable). */
int vocr_textspot_load(vocr_textspot *m, const void *blob, size_t bytes);

/* Exact arena bytes vocr_textspot_find / _find_rows / _maps need for every frame up to max_width x max_height with
 * this model (it depends on the width only): the network's row rings, the halving's row, the map row, the
 * streaming decode, the 64 candidates and the blocks' scratch, plus 7 bytes of alignment slack. With half_above
 * h > 0 and max_width > h it is the larger of the needs of an h-wide frame (read at full size) and a
 * max_width-wide one (halved). 0 for a NULL or unloaded model, a zero side or a side over VOCR_TEXTSPOT_MAX_SIDE. */
size_t vocr_textspot_arena_bytes(const vocr_textspot *m, unsigned max_width, unsigned max_height);

/* Exact arena bytes for a frame of exactly width x height (what the calls check): at most
 * vocr_textspot_arena_bytes(m, width, height), and less for a halved frame when a narrower one would be read at
 * full size. 0 as vocr_textspot_arena_bytes. */
size_t vocr_textspot_frame_arena_bytes(const vocr_textspot *m, unsigned width, unsigned height);

/* The spotter on a frame in memory (VOCR_LINES_GREY or VOCR_LINES_RGBA: grey = (77 R + 150 G + 29 B) >> 8).
 * Returns VOCR_LINES_OK, VOCR_LINES_E_ARG (NULL pointer, unloaded model, bad format, stride or flags, a zero
 * side or one over VOCR_TEXTSPOT_MAX_SIDE) or VOCR_LINES_E_ARENA; on an error out->count and block_count are 0,
 * pick -1 and truncated 0. The arena is scratch only. */
int vocr_textspot_find(const vocr_textspot *m, const VocrLinesImage *im, unsigned flags, void *arena,
                       size_t arena_bytes, VocrTextspotSet *out);

/* The same through a row callback (vocr_lines_rows.h): the frame's rows are asked once each in increasing order
 * in one pass, then (without VOCR_TEXTSPOT_NO_DARK) each box's rows once, top to bottom, boxes in output order,
 * for its dark flag (vocr_line_dark_rows); a returned row only needs to stay valid until the next call. A NULL
 * row ends the call with VOCR_LINES_E_SOURCE and no boxes. The same result as vocr_textspot_find for the same
 * pixels. */
int vocr_textspot_find_rows(const vocr_textspot *m, const VocrLinesRows *src, unsigned flags, void *arena,
                            size_t arena_bytes, VocrTextspotSet *out);

/* Debug and test hook: runs the network on the frame (halved when the model says so) and calls
 * row(user, i, maps, width) for each row i of the head's output, in increasing order; maps holds the row of the
 * four maps one after the other (K at [0, width), O, A1, then A2 at [3 width, 4 width)) and is valid only during
 * the call. Same arena and return codes as vocr_textspot_find (without its dark flags). */
typedef void (*vocr_textspot_map_row)(void *user, unsigned i, const signed char *maps, unsigned width);
int vocr_textspot_maps(const vocr_textspot *m, const VocrLinesImage *im, void *arena, size_t arena_bytes,
                       vocr_textspot_map_row row, void *user);

#ifdef __cplusplus
}
#endif

#endif
