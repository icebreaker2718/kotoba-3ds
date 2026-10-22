/* Post-finder refinement of the dialogue line finder's boxes (vocr_lines.h).
 *
 * The GRAD finder's boxes are taller than their text: its vertical growth and padding
 * pull in dialogue-box frame lines, the edges of neighbouring lines, furigana and
 * scenery, and a box can hold two text rows set close together. The recognizer was
 * trained on crops cut close around the ink, so those rows cost it accuracy
 * (docs/finder-tighten.md). vocr_refine_lines() takes the finder's VocrLineSet and,
 * per box, measures the ink of the line's own polarity and:
 *   - VOCR_REFINE_TIGHTEN: cuts the box's rows to its main text band plus a margin of
 *     16% of the band's height (its columns are kept);
 *   - VOCR_REFINE_FRAMES: first ignores frame rows (one run of ink over half the box
 *     width or more: dialogue-box borders, rules, stripes);
 *   - VOCR_REFINE_SPLIT: splits a box holding several text rows of about the same
 *     height (and glyph height: furigana stay with their line) into one line per row;
 *   - VOCR_REFINE_CURSOR: drops an isolated trailing blob that is not shaped like text
 *     (the "next page" cursor: a speck or a solid blob) beyond a gap of 1.5 band heights;
 *   - VOCR_REFINE_PORTRAIT: drops a part at either end beyond a gap of one band height
 *     whose ink runs on far above and below the band, or that is wide and solidly filled
 *     (portrait edges, ornaments).
 * Flags 0 leave the set untouched, so a caller can A/B the refinement bit for bit.
 * The lines stay in order, keep their dark flags and never grow; no line is dropped.
 *
 * C99, no allocation, integer arithmetic: the caller passes an arena of at least
 * vocr_refine_arena_bytes() bytes, which never exceeds vocr_lines_arena_bytes() for the
 * same region, so the finder's arena serves both. synth/lines_refine.py is a bit-exact
 * Python twin.
 */
#ifndef VOCR_LINES_REFINE_H
#define VOCR_LINES_REFINE_H

#include <stddef.h>

#include "vocr_lines.h"
#include "vocr_lines_rows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOCR_REFINE_TIGHTEN 0x01u
#define VOCR_REFINE_FRAMES 0x02u
#define VOCR_REFINE_SPLIT 0x04u
#define VOCR_REFINE_CURSOR 0x08u
#define VOCR_REFINE_PORTRAIT 0x10u
#define VOCR_REFINE_ALL 0x1Fu
/* The set measured on the development cohorts (docs/finder-tighten.md): every step. */
#define VOCR_REFINE_DEFAULT VOCR_REFINE_ALL

/* Arena bytes vocr_refine_lines needs for any region up to max_width x max_height (0 if
 * either is 0). Never more than vocr_lines_arena_bytes(max_width, max_height). A larger
 * arena lets a box be read once instead of twice (rows_read below). */
size_t vocr_refine_arena_bytes(unsigned max_width, unsigned max_height);

/* Refines set (the finder's output for im) in place. Returns VOCR_LINES_OK, VOCR_LINES_E_ARG
 * (bad image, flags or a line outside the image) or VOCR_LINES_E_ARENA; on an error the set
 * is unchanged. rows_read, if not NULL, receives the number of image rows read. */
int vocr_refine_lines(const VocrLinesImage *im, unsigned flags, void *arena, size_t arena_bytes,
                      VocrLineSet *set, unsigned long *rows_read);

/* The same through a row callback (vocr_lines_rows.h): each box's rows are asked in
 * increasing order, once (twice when the arena holds only vocr_refine_arena_bytes() and
 * the box's grey pixels do not fit), boxes in set order. VOCR_LINES_E_SOURCE when a row
 * could not be read (the set is then unchanged). */
int vocr_refine_lines_rows(const VocrLinesRows *src, unsigned flags, void *arena, size_t arena_bytes,
                           VocrLineSet *set, unsigned long *rows_read);

#ifdef __cplusplus
}
#endif

#endif
