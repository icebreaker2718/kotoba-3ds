/* The dialogue line finder of vocr_lines.h, reading its region row by row.
 *
 * Same finders, constants, arena (vocr_lines_arena_bytes) and results as
 * vocr_find_lines / vocr_line_dark; the region comes from a callback instead of
 * an image in memory, so a caller can stream it (vita-jp-overlay reads it from the
 * game's framebuffer in strips and never holds the whole region). vocr_lines.c is
 * frozen by pre-registered protocols, so this form is a separate file,
 * vocr_lines_rows.c; tests/test_vocr_lines_rows.c checks that the two agree.
 */
#ifndef VOCR_LINES_ROWS_H
#define VOCR_LINES_ROWS_H

#include "vocr_lines.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOCR_LINES_E_SOURCE (-4) /* the row callback returned NULL */

/* A region read row by row. row(user, y) returns row y of the region: width * bpp
 * bytes, the leftmost pixel first. The finders ask for rows in increasing order
 * within one pass over the region or over a line's box and make several passes
 * (two over the region, one for the legacy finder, which may stop early; then one
 * over each line's box); the returned bytes need to stay valid only until the next
 * call. NULL ends the call with VOCR_LINES_E_SOURCE. */
typedef struct {
    unsigned width, height;
    unsigned bpp;                /* VOCR_LINES_GREY or VOCR_LINES_RGBA */
    const unsigned char *(*row)(void *user, unsigned y);
    void *user;
} VocrLinesRows;

/* vocr_find_lines for a region read through src, with the same arena
 * (vocr_lines_arena_bytes(src->width, src->height)): the same lines for the same
 * pixels, or VOCR_LINES_E_SOURCE (and no lines) when a row could not be read. */
int vocr_find_lines_rows(const VocrLinesRows *src, int finder, void *arena, size_t arena_bytes,
                         VocrLineSet *out);

/* vocr_line_dark through src: reads the box's rows once each, top to bottom;
 * VOCR_LINES_E_SOURCE when one could not be read. */
int vocr_line_dark_rows(const VocrLinesRows *src, unsigned x, unsigned y, unsigned w, unsigned h);

#ifdef __cplusplus
}
#endif

#endif
