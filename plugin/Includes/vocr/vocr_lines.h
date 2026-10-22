/* Dialogue line finder for the Vita/PSP VN OCR: finds horizontal text lines in a
 * user-selected region (a tight box around the dialogue or a whole text box) for
 * any text and background polarity, coloured names and outlined text.
 *
 * C99, no allocation: the caller passes an arena of vocr_lines_arena_bytes() bytes.
 * synth/common.py find_lines() is a bit-exact Python twin, so training crops are
 * the crops the console sees. See docs/linefind.md for the method and results.
 */
#ifndef VOCR_LINES_H
#define VOCR_LINES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOCR_LINES_MAX 16 /* lines returned per region, top to bottom */

/* Pixel formats (bytes per pixel). RGBA is R, G, B, A bytes; alpha is ignored. */
#define VOCR_LINES_GREY 1
#define VOCR_LINES_RGBA 4

/* Finders. */
#define VOCR_LINES_GRAD 0   /* polarity-agnostic local-contrast projection (default) */
#define VOCR_LINES_LEGACY 1 /* vita-jp-overlay's bright-pixel projection, for comparison */
#define VOCR_LINES_TEXTBOX 2 /* preserve GRAD rows; repair proven textbox topology */
#define VOCR_LINES_TEXTBOX_RECOVERY 3 /* TEXTBOX plus independently supported missing rows */

/* Return codes. */
#define VOCR_LINES_OK 0
#define VOCR_LINES_E_ARG (-1)    /* null pointer, empty image or unknown format/finder */
#define VOCR_LINES_E_ARENA (-2)  /* arena smaller than vocr_lines_arena_bytes(w, h) */
#define VOCR_LINES_E_REGION (-3) /* legacy finder only: region over 960x544, light box,
                                    line over 88 px or more than 8 lines (the overlay's
                                    VJO_E_OCR_REGION) */

typedef struct {
    const unsigned char *pixels; /* top-left pixel of the region */
    unsigned width, height;
    size_t stride;               /* bytes from one row to the next (>= width * bpp) */
    unsigned bpp;                /* VOCR_LINES_GREY or VOCR_LINES_RGBA */
} VocrLinesImage;

typedef struct {
    unsigned x, y, w, h; /* crop box in region pixels, padded as the training crops are */
    unsigned dark;       /* 1: text darker than its background (invert for a light-text
                            recognizer); always 0 from the legacy finder */
} VocrLine;

typedef struct {
    VocrLine lines[VOCR_LINES_MAX];
    unsigned count;
    unsigned truncated; /* 1 when more than VOCR_LINES_MAX lines were found */
} VocrLineSet;

/* Exact arena bytes needed by either finder for any region up to max_width x
 * max_height (0 if either is 0): 83,323 bytes at 960x544, 25,355 at 480x272. */
size_t vocr_lines_arena_bytes(unsigned max_width, unsigned max_height);

/* Finds the lines of `im`. `arena` needs no particular alignment and is scratch
 * only: nothing in it is used after the call returns. */
int vocr_find_lines(const VocrLinesImage *im, int finder, void *arena, size_t arena_bytes,
                    VocrLineSet *out);

/* Polarity of any box of `im` (the rule vocr_find_lines uses for VocrLine.dark):
 * 1 if the text is darker than its background, 0 if not, VOCR_LINES_E_ARG for a
 * bad image or a box outside it. Needs no arena. */
int vocr_line_dark(const VocrLinesImage *im, unsigned x, unsigned y, unsigned w, unsigned h);

#ifdef __cplusplus
}
#endif

#endif
