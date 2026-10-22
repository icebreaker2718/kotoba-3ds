/* Dialogue line finder: see vocr_lines.h and docs/linefind.md.
 *
 * GRAD finder, in order:
 *  1. grey  g = (77 R + 150 G + 29 B) >> 8 (the recognizer's own grey)
 *  2. local contrast c = max - min of g over each 3x3 window (edges replicated):
 *     text strokes are high-contrast whether they are bright, dark or outlined
 *  3. threshold T = max(40, 2/5 of the 99th percentile of c); on = c >= T
 *  4. per row, join on-pixels at most 10 px apart into runs and keep runs of at least
 *     16 px (a text line becomes long runs; specks, vertical borders and the edges of
 *     big shapes do not); prof(y) = kept run length in row y
 *  5. rows with prof >= max(16, max(prof) / 16) are active; active rows with gaps of
 *     at most 2 rows form bands; bands under 8 rows are dropped. A bridged gap that
 *     holds an empty row (prof 0) between two runs of at least 8 active rows ends
 *     the band instead: lines set 1-2 px apart (PSP dialogue) stay separate lines
 *  6. per band: the column cluster with the most kept pixels (clusters split at gaps
 *     over twice the band height) is the line; it is widened over on-pixels at most
 *     max(4, h) columns away (punctuation, small kana) and grown by up to h/2 rows
 *     (never past half the gap to the next band) while rows hold 2+ on-pixels
 *  7. lines under 55% of the median line height are dropped (furigana, icons)
 *  8. boxes are padded by 3 px, which after the 1 px dilation of step 2 matches the
 *     legacy finder's 4 px around bright ink, and each gets a polarity flag
 *
 * Memory: a 1-bit on-mask of the region plus a few rows and per-row/column counters,
 * all carved from the caller's arena. Integer arithmetic only.
 */
#include "vocr_lines.h"

#include <stdint.h>
#include <string.h>

#define T_MIN 40u
#define GAP 10u
#define LMIN 16u
#define ROW_DIV 16u
#define MIN_H 8u
#define MERGE 2u
#define GAPC 2u
#define PAD 3u
#define FURI_PCT 55u

/* legacy (vjo_ocr_find_lines) limits */
#define LEG_MAX_W 960u
#define LEG_MAX_H 544u
#define LEG_MAX_LINES 8u
#define LEG_MAX_LINE_H 96u

typedef struct {
    unsigned a, b;          /* band rows [a, b) */
} Band;

typedef struct {
    unsigned x1, top, x2, bot;
} Cand;

static unsigned band_cap(unsigned h)
{
    /* stored bands are at least MIN_H rows tall and at least one row apart, so
     * n * MIN_H + (n - 1) <= h */
    return (h + 1u) / (MIN_H + 1u) + 1u;
}

static size_t up4(size_t n)
{
    return (n + 3u) & ~(size_t)3u;
}

static size_t grad_bytes(unsigned w, unsigned h)
{
    size_t words = ((size_t)w + 31u) / 32u;
    return up4(words * h * 4u)              /* on-mask */
         + up4((size_t)h * 4u)              /* prof */
         + up4((size_t)w) * 7u              /* row max and row min rings; contrast row */
         + up4((size_t)w * 4u) * 2u         /* kc, oc */
         + up4(sizeof(Band) * band_cap(h))
         + up4(sizeof(Cand) * band_cap(h));
}

static size_t legacy_bytes(unsigned w, unsigned h)
{
    (void)w;
    return up4((size_t)h * 4u) * 2u;        /* left, right */
}

size_t vocr_lines_arena_bytes(unsigned max_width, unsigned max_height)
{
    size_t g, l;
    if (!max_width || !max_height) return 0;
    g = grad_bytes(max_width, max_height);
    l = legacy_bytes(max_width, max_height);
    return 3u + (g > l ? g : l);            /* 3: worst-case alignment of the base */
}

static unsigned char *align4(void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (unsigned char *)p + ((4u - (v & 3u)) & 3u);
}

static unsigned grey_at(const VocrLinesImage *im, unsigned x, unsigned y)
{
    const unsigned char *p = im->pixels + (size_t)y * im->stride + (size_t)x * im->bpp;
    if (im->bpp == VOCR_LINES_GREY) return p[0];
    return (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
}

/* One step of the horizontal 3-max / 3-min: a, b, c are grey at x - 1, x, x + 1. */
static void put3(unsigned char *mx, unsigned char *mn, unsigned x, unsigned a, unsigned b, unsigned c)
{
    unsigned hi = a > b ? a : b, lo = a < b ? a : b;
    mx[x] = (unsigned char)(c > hi ? c : hi);
    mn[x] = (unsigned char)(c < lo ? c : lo);
}

/* Streams the 3x3 contrast of every row from rings of 3 rows of horizontal max / min. */
typedef struct {
    const VocrLinesImage *im;
    unsigned char *mx[3], *mn[3];
    unsigned next;  /* next source row to load into the ring */
} Contrast;

/* Grey of source row y and its horizontal 3-max and 3-min (edges replicated) into ring
 * slot y % 3. The grey row itself is never stored. */
static void contrast_load(Contrast *cs, unsigned y)
{
    const VocrLinesImage *im = cs->im;
    const unsigned char *p = im->pixels + (size_t)y * im->stride;
    unsigned char *mx = cs->mx[y % 3u], *mn = cs->mn[y % 3u];
    unsigned w = im->width, x, a, b, c;
    if (im->bpp == VOCR_LINES_GREY) {
        a = b = p[0];
        for (x = 0; x + 1 < w; x++, a = b, b = c) {
            c = p[x + 1];
            put3(mx, mn, x, a, b, c);
        }
    } else {
        a = b = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
        for (x = 0; x + 1 < w; x++, a = b, b = c) {
            const unsigned char *q = p + 4u * (x + 1u);
            c = (77u * q[0] + 150u * q[1] + 29u * q[2]) >> 8;
            put3(mx, mn, x, a, b, c);
        }
    }
    put3(mx, mn, w - 1u, a, b, b);
}

static void contrast_begin(Contrast *cs)
{
    contrast_load(cs, 0);
    if (cs->im->height > 1) contrast_load(cs, 1);
    cs->next = 2;
}

/* Contrast of row y (rows must be requested in order 0, 1, 2, ...). */
static void contrast_row(Contrast *cs, unsigned y, unsigned char *out)
{
    unsigned h = cs->im->height, w = cs->im->width, x;
    unsigned ya = y ? y - 1 : 0, yc = y + 1 < h ? y + 1 : h - 1;
    const unsigned char *ma, *mb, *mc, *na, *nb, *nc;
    if (yc >= cs->next && cs->next < h) {
        contrast_load(cs, cs->next);
        cs->next++;
    }
    ma = cs->mx[ya % 3u]; mb = cs->mx[y % 3u]; mc = cs->mx[yc % 3u];
    na = cs->mn[ya % 3u]; nb = cs->mn[y % 3u]; nc = cs->mn[yc % 3u];
    for (x = 0; x < w; x++) {
        unsigned hi = ma[x] > mb[x] ? ma[x] : mb[x], lo = na[x] < nb[x] ? na[x] : nb[x];
        hi = mc[x] > hi ? mc[x] : hi;
        lo = nc[x] < lo ? nc[x] : lo;
        out[x] = (unsigned char)(hi - lo);
    }
}

static int bit(const uint32_t *row, unsigned x)
{
    return (int)((row[x >> 5] >> (x & 31u)) & 1u);
}

static unsigned close_run(unsigned s, unsigned e, uint32_t *kc)
{
    unsigned x;
    if (e - s < LMIN) return 0;
    if (kc)
        for (x = s; x < e; x++) kc[x]++;
    return e - s;
}

/* Joins on-pixels at most GAP apart; returns the length of runs >= LMIN and, if kc
 * is given, counts their pixels per column. */
static unsigned row_kept(const uint32_t *row, unsigned w, uint32_t *kc)
{
    unsigned x, s = 0, last = 0, total = 0;
    int have = 0;
    for (x = 0; x < w; x++) {
        if (!row[x >> 5]) {             /* skip empty words */
            x |= 31u;
            continue;
        }
        if (!bit(row, x)) continue;
        if (have && x - last <= GAP) {
            last = x;
            continue;
        }
        if (have) total += close_run(s, last + 1, kc);
        s = last = x;
        have = 1;
    }
    if (have) total += close_run(s, last + 1, kc);
    return total;
}

static unsigned row_on(const uint32_t *row, unsigned x1, unsigned x2)
{
    unsigned x, n = 0;
    for (x = x1; x < x2; x++) n += (unsigned)bit(row, x);
    return n;
}

/* A textbox row has runs of >= MIN_H pixels separated by >= GAP empty pixels.
 * Keep only four rows of runs and their live union-find components. This fits
 * inside GRAD's row rings/counters, which are dead once the on-mask is built. */
typedef struct { unsigned x1, x2, label; } TextRun;
typedef struct {
    unsigned x1, top, x2, bot, last, parent, mass;
} TextComponent;
typedef struct { unsigned x1, top, x2, bot, mass; } TextCand;
#define TEXT_STRONG (1u << 28)
#define TEXT_DUP (1u << 29)
#define TEXT_USE (1u << 30)
#define TEXT_INFO_MASK (TEXT_STRONG - 1u)

static int text_same_box(unsigned ax, unsigned ay, unsigned aw, unsigned ah,
                         unsigned bx, unsigned by, unsigned bw, unsigned bh)
{
    unsigned left = ax > bx ? ax : bx, top = ay > by ? ay : by;
    unsigned right = ax + aw < bx + bw ? ax + aw : bx + bw;
    unsigned bottom = ay + ah < by + bh ? ay + ah : by + bh;
    if (right <= left || bottom <= top) return 0;
    return 10u * (right - left) >= 9u * aw && 10u * (right - left) >= 9u * bw &&
           10u * (bottom - top) >= 9u * ah && 10u * (bottom - top) >= 9u * bh;
}

static unsigned text_old_height(const VocrLine *line)
{
    unsigned ch = line->h > 2u * PAD ? line->h - 2u * PAD : line->h;
    return ch < MIN_H ? MIN_H : ch;
}

static int text_coherent(const TextCand *cand, unsigned main)
{
    unsigned ch = cand->mass & 255u;
    return !(cand->mass & TEXT_DUP) && (cand->mass & TEXT_STRONG) &&
           100u * ch >= FURI_PCT * main && ch <= GAPC * main;
}

/* Repeated ordinary GRAD row heights anchor the scale before new scene or
 * border components can influence it. A unique geometry-matched old row also
 * anchors it; incompatible single-row anchors request conservative fallback. */
static unsigned text_main_height(const VocrLineSet *old, const TextCand *cand,
                                  unsigned nc, unsigned proposed)
{
    unsigned i, j, best = 0, height = 0, single = 0;
    for (i = 0; i < old->count; i++) {
        unsigned ch = text_old_height(&old->lines[i]), count = 0;
        if (ch > LEG_MAX_LINE_H) continue;
        for (j = 0; j < old->count; j++) {
            unsigned other = text_old_height(&old->lines[j]);
            unsigned small = ch < other ? ch : other, big = ch > other ? ch : other;
            if (other <= LEG_MAX_LINE_H && 4u * big <= 5u * small) count++;
        }
        if (count > best || (count == best && ch > height)) { best = count; height = ch; }
    }
    if (best >= 2u) return height;
    for (i = 0; i < old->count; i++) {
        const VocrLine *line = &old->lines[i];
        unsigned ch = text_old_height(line);
        if (ch > LEG_MAX_LINE_H) continue;
        for (j = 0; j < nc; j++) {
            unsigned small, big;
            if (cand[j].mass & TEXT_DUP) continue;
            if (!text_same_box(line->x, line->y, line->w, line->h,
                               cand[j].x1, cand[j].top, cand[j].x2 - cand[j].x1,
                               cand[j].bot - cand[j].top)) continue;
            small = ch < single ? ch : single; big = ch > single ? ch : single;
            if (single && 4u * big > 5u * small) return 0;
            if (ch > single) single = ch;
            break;
        }
    }
    return single ? single : proposed;
}

static int text_inside(const TextCand *cand, const VocrLine *line)
{
    unsigned center = (cand->top + cand->bot) / 2u;
    unsigned left = cand->x1 > line->x ? cand->x1 : line->x;
    unsigned right = cand->x2 < line->x + line->w ? cand->x2 : line->x + line->w;
    return center >= line->y && center < line->y + line->h && right > left &&
           2u * (right - left) >= cand->x2 - cand->x1;
}

static void text_route(const VocrLinesImage *im, TextCand *cand, unsigned nc,
                        unsigned proposed, VocrLineSet *out)
{
    unsigned main = text_main_height(out, cand, nc, proposed), i, j, k, replace = 0;
    unsigned missing = out->count == 0, old_count = out->count, kept = 0;
    if (!main) return;
    for (i = 0; i < old_count; i++) {
        const VocrLine *line = &out->lines[i];
        if (10u * line->w >= 9u * im->width && 2u * text_old_height(line) >= 3u * main)
            missing = 1;
        if (line->h <= 2u * main + 2u * PAD) continue;
        for (j = 0; j < nc; j++) {
            if (!text_coherent(&cand[j], main) || !text_inside(&cand[j], line)) continue;
            for (k = j + 1u; k < nc; k++) {
                unsigned first = (cand[j].top + cand[j].bot) / 2u;
                unsigned second = (cand[k].top + cand[k].bot) / 2u;
                unsigned distance = first > second ? first - second : second - first;
                if (distance >= main && text_coherent(&cand[k], main) && text_inside(&cand[k], line)) {
                    replace |= 1u << i;
                    break;
                }
            }
            if (replace & (1u << i)) break;
        }
        if (replace & (1u << i))
            for (j = 0; j < nc; j++)
                if (text_coherent(&cand[j], main) && text_inside(&cand[j], line)) cand[j].mass |= TEXT_USE;
    }
    /* Preserve retained old coordinates, padding and polarity byte-for-byte. */
    for (i = 0; i < old_count; i++)
        if (!(replace & (1u << i))) out->lines[kept++] = out->lines[i];
    out->count = kept;
    for (i = 0; i < nc; i++) {
        unsigned ch = cand[i].mass & 255u, overlap = 0;
        VocrLine *line;
        if (cand[i].mass & TEXT_DUP) continue;
        if (!(cand[i].mass & TEXT_USE) &&
            !(missing && (old_count ? text_coherent(&cand[i], main) :
              (100u * ch >= FURI_PCT * main && ch <= GAPC * main)))) continue;
        for (j = 0; j < out->count; j++) {
            unsigned top = cand[i].top > out->lines[j].y ? cand[i].top : out->lines[j].y;
            unsigned bot = cand[i].bot < out->lines[j].y + out->lines[j].h ?
                           cand[i].bot : out->lines[j].y + out->lines[j].h;
            unsigned small = cand[i].bot - cand[i].top < out->lines[j].h ?
                             cand[i].bot - cand[i].top : out->lines[j].h;
            if (bot > top && 2u * (bot - top) >= small) { overlap = 1; break; }
        }
        if (overlap) continue;
        if (out->count == VOCR_LINES_MAX) { out->truncated = 1; break; }
        line = &out->lines[out->count++];
        line->x = cand[i].x1; line->y = cand[i].top;
        line->w = cand[i].x2 - cand[i].x1; line->h = cand[i].bot - cand[i].top;
        line->dark = (unsigned)vocr_line_dark(im, line->x, line->y, line->w, line->h);
    }
    for (i = 1; i < out->count; i++) {
        VocrLine value = out->lines[i]; j = i;
        while (j && (out->lines[j - 1u].y > value.y ||
                     (out->lines[j - 1u].y == value.y && out->lines[j - 1u].x > value.x))) {
            out->lines[j] = out->lines[j - 1u]; j--;
        }
        out->lines[j] = value;
    }
}

/* Recovery uses component core heights, not the height of a projection crop
 * which may include a portrait or a speaker banner. The run/component storage
 * is dead here: its first 97 bytes hold a bounded height histogram. Even the
 * smallest 8x8 scratch layout has 160 bytes before the candidate array. */
static unsigned recovery_height(const TextCand *cand, unsigned nc, const VocrLineSet *old,
                                 unsigned char *heights, unsigned *independent)
{
    unsigned i, j, best = nc, anchor = nc, main = 0, count = 0, total = 0;
    memset(heights, 0, LEG_MAX_LINE_H + 1u);
    *independent = nc;
    for (i = 0; i < nc; i++) {
        unsigned ch = cand[i].mass & 255u, weight = (cand[i].mass & TEXT_INFO_MASK) >> 8;
        if ((cand[i].mass & TEXT_DUP) || !(cand[i].mass & TEXT_STRONG)) continue;
        if (best == nc || weight * (cand[best].mass & 255u) >
                         ((cand[best].mass & TEXT_INFO_MASK) >> 8) * ch ||
            (weight * (cand[best].mass & 255u) ==
             ((cand[best].mass & TEXT_INFO_MASK) >> 8) * ch && ch > (cand[best].mass & 255u))) best = i;
        for (j = 0; j < old->count; j++) {
            const VocrLine *line = &old->lines[j];
            if (!text_same_box(line->x, line->y, line->w, line->h,
                               cand[i].x1, cand[i].top, cand[i].x2 - cand[i].x1,
                               cand[i].bot - cand[i].top)) continue;
            if (heights[ch] < 255u) heights[ch]++;
            total++;
            break;
        }
    }
    if (best == nc) return 0;
    /* A repeated supported height class beats a single wide scene component.
     * Count heights within a 5:4 ratio, with taller classes winning ties. */
    for (i = MIN_H; i <= LEG_MAX_LINE_H; i++) {
        unsigned upper = 5u * i / 4u, n = 0, largest = 0;
        if (upper > LEG_MAX_LINE_H) upper = LEG_MAX_LINE_H;
        for (j = i; j <= upper; j++) {
            n += heights[j];
            if (heights[j]) largest = j;
        }
        if (n > count || (n == count && largest > main)) { count = n; main = largest; }
    }
    if (count >= 2u) return main;
    if (total > 1u) return 0; /* incompatible supported scales: retain TEXTBOX */
    if (count) {
        for (i = 0; i < nc; i++) {
            if ((cand[i].mass & TEXT_DUP) || !(cand[i].mass & TEXT_STRONG) ||
                (cand[i].mass & 255u) != main) continue;
            for (j = 0; j < old->count; j++) {
                const VocrLine *line = &old->lines[j];
                if (text_same_box(line->x, line->y, line->w, line->h,
                                  cand[i].x1, cand[i].top, cand[i].x2 - cand[i].x1,
                                  cand[i].bot - cand[i].top)) { anchor = i; break; }
            }
            if (anchor != nc) break;
        }
        /* A lower row with at least 5:4 horizontal support per core-height can
         * override one tall scene-like anchor. Ruby above an ordinary row
         * cannot reset the scale. Nothing is removed from the old output. */
        if (anchor != nc && cand[best].top >= cand[anchor].bot &&
            4u * ((cand[best].mass & TEXT_INFO_MASK) >> 8) * main >=
            5u * ((cand[anchor].mass & TEXT_INFO_MASK) >> 8) * (cand[best].mass & 255u)) {
            unsigned ch = cand[best].mass & 255u;
            if (4u * (ch > main ? ch : main) > 5u * (ch < main ? ch : main)) *independent = best;
            return ch;
        }
        return main;
    }
    /* Without a supported old row, require one unambiguous line-like scale.
     * A competing scale with >=3/4 of the best aspect support is ambiguous. */
    main = cand[best].mass & 255u;
    for (i = 0; i < nc; i++) {
        unsigned ch = cand[i].mass & 255u, small = ch < main ? ch : main, big = ch > main ? ch : main;
        if ((cand[i].mass & TEXT_DUP) || !(cand[i].mass & TEXT_STRONG) || 4u * big <= 5u * small) continue;
        if (4u * ((cand[i].mass & TEXT_INFO_MASK) >> 8) * main >=
            3u * ((cand[best].mass & TEXT_INFO_MASK) >> 8) * ch) return 0;
    }
    *independent = best;
    return main;
}

static int recovery_neighbor(const TextCand *a, const TextCand *b, unsigned main)
{
    unsigned ah = a->mass & 255u, bh = b->mass & 255u;
    unsigned ac = (a->top + a->bot) / 2u, bc = (b->top + b->bot) / 2u;
    unsigned distance = ac > bc ? ac - bc : bc - ac;
    unsigned left = a->x1 > b->x1 ? a->x1 : b->x1;
    unsigned right = a->x2 < b->x2 ? a->x2 : b->x2;
    unsigned aw = a->x2 - a->x1, bw = b->x2 - b->x1, small = aw < bw ? aw : bw;
    unsigned start = a->x1 > b->x1 ? a->x1 - b->x1 : b->x1 - a->x1;
    if (4u * (ah > bh ? ah : bh) > 5u * (ah < bh ? ah : bh)) return 0;
    return distance >= main && distance <= 4u * main &&
           (start <= main || (right > left && 2u * (right - left) >= small));
}

static int recovery_covered(const TextCand *cand, const VocrLineSet *out)
{
    unsigned i, ch = cand->mass & 255u, center = (cand->top + cand->bot) / 2u;
    unsigned top = center >= ch / 2u ? center - ch / 2u : 0, bot = top + ch;
    for (i = 0; i < out->count; i++) {
        const VocrLine *line = &out->lines[i];
        unsigned left = cand->x1 > line->x ? cand->x1 : line->x;
        unsigned right = cand->x2 < line->x + line->w ? cand->x2 : line->x + line->w;
        unsigned y1 = top > line->y ? top : line->y, y2 = bot < line->y + line->h ? bot : line->y + line->h;
        unsigned small = cand->x2 - cand->x1 < line->w ? cand->x2 - cand->x1 : line->w;
        if (right > left && 2u * (right - left) >= small && y2 > y1 && 2u * (y2 - y1) >= ch) return 1;
    }
    return 0;
}

/* Small disconnected strokes may be shorter than the component run minimum.
 * Only newly recovered crops get this bounded attachment. Join pixels with the
 * same horizontal gap rule; a row-wide ruby/border run is not a small mark. */
static int recovery_mark_row(const uint32_t *row, unsigned x1, unsigned x2, unsigned main)
{
    unsigned x, start = 0, last = 0, on = 0;
    for (x = x1; x < x2; x++) {
        if (!bit(row, x)) continue;
        if (on && x - last > GAP) start = x;
        else if (!on) start = x;
        last = x; on++;
        if (2u * (last + 1u - start) > main) return 0;
    }
    return on >= 2u;
}

static int recovery_mark_span(const uint32_t *bits, unsigned words, unsigned h,
                               unsigned x1, unsigned x2, unsigned y, unsigned main,
                               unsigned fence, int up, unsigned *outer)
{
    unsigned n, limit = main / 4u;
    if (limit < PAD + 2u) limit = PAD + 2u;
    /* Probe one row beyond the complete permitted span. A component which
     * continues past that bound, an image edge or another row fence is not a
     * small detached mark. Never accept just its nearest horizontal slices. */
    for (n = 0; n <= limit; n++) {
        unsigned row;
        if (up) {
            if (n > y || y - n < fence) return 0;
            row = y - n;
        } else {
            if (n >= h - y || y + n >= fence) return 0;
            row = y + n;
        }
        if (!row_on(bits + (size_t)row * words, x1, x2)) {
            if (!n) return 0;
            *outer = up ? row + 1u : row - 1u;
            return 1;
        }
        if (n == limit || !recovery_mark_row(bits + (size_t)row * words, x1, x2, main)) return 0;
    }
    return 0;
}

static void recovery_marks(const VocrLinesImage *im, const uint32_t *bits,
                            const TextCand *all, unsigned nc, unsigned index,
                            TextCand *cand, unsigned main)
{
    unsigned words = (im->width + 31u) / 32u, ch = cand->mass & 255u;
    unsigned center = (cand->top + cand->bot) / 2u, top = center >= ch / 2u ? center - ch / 2u : 0;
    unsigned bot = top + ch, step, empty = 0, first = top, last = bot, lower = 0, upper = im->height, i;
    /* Other candidate cores retain their original geometry during every probe.
     * A midpoint fence prevents a new crop from claiming a neighbour's tail. */
    for (i = 0; i < nc; i++) {
        unsigned other, och, otop, obot, left, right, small;
        if (i == index || (all[i].mass & TEXT_DUP)) continue;
        other = (all[i].top + all[i].bot) / 2u;
        och = all[i].mass & 255u;
        otop = other >= och / 2u ? other - och / 2u : 0; obot = otop + och;
        left = cand->x1 > all[i].x1 ? cand->x1 : all[i].x1;
        right = cand->x2 < all[i].x2 ? cand->x2 : all[i].x2;
        small = cand->x2 - cand->x1 < all[i].x2 - all[i].x1 ? cand->x2 - cand->x1 : all[i].x2 - all[i].x1;
        if (right <= left || 2u * (right - left) < small) continue;
        if (obot <= top) {
            unsigned fence = (obot + top + 1u) / 2u;
            if (fence > lower) lower = fence;
        }
        if (otop >= bot) {
            unsigned fence = (bot + otop) / 2u;
            if (fence < upper) upper = fence;
        }
    }
    for (step = 1; step <= main / 3u && step <= top; step++) {
        unsigned y = top - step, outer, count;
        if (y < lower) break;
        count = row_on(bits + (size_t)y * words, cand->x1, cand->x2);
        if (!count) { if (++empty > PAD) break; continue; }
        if (recovery_mark_span(bits, words, im->height, cand->x1, cand->x2, y, main, lower, 1, &outer))
            first = outer;
        break;
    }
    empty = 0;
    for (step = 0; step < main / 3u && bot + step < im->height; step++) {
        unsigned y = bot + step, outer, count;
        if (y >= upper) break;
        count = row_on(bits + (size_t)y * words, cand->x1, cand->x2);
        if (!count) { if (++empty > PAD) break; continue; }
        if (recovery_mark_span(bits, words, im->height, cand->x1, cand->x2, y, main, upper, 0, &outer))
            last = outer + 1u;
        break;
    }
    first = first > PAD ? first - PAD : 0;
    last = last + PAD < im->height ? last + PAD : im->height;
    if (first < cand->top) cand->top = first;
    if (last > cand->bot) cand->bot = last;
}

static void text_recover(const VocrLinesImage *im, TextCand *cand, unsigned nc,
                          const uint32_t *bits, unsigned char *scratch, VocrLineSet *out)
{
    unsigned independent, main = recovery_height(cand, nc, out, scratch, &independent), i, j, added = 0;
    unsigned old_count = out->count, staged = 0, kept = old_count;
    if (!main) return;
    for (i = 0; i < nc; i++) cand[i].mass &= ~TEXT_USE;
    for (i = 0; i < nc; i++) {
        unsigned ch = cand[i].mass & 255u, supported = i == independent;
        if ((cand[i].mass & TEXT_DUP) || 100u * ch < FURI_PCT * main || 4u * ch > 5u * main ||
            recovery_covered(&cand[i], out)) continue;
        /* Both broad and short rows require a coherent neighbour, except for
         * the independently selected unambiguous scale below a scene-like old
         * anchor. Never borrow from a newly accepted weak component. */
        if (!supported)
            for (j = 0; j < nc; j++) {
                if (i == j || (cand[j].mass & TEXT_DUP) || !(cand[j].mass & TEXT_STRONG) ||
                    100u * (cand[j].mass & 255u) < FURI_PCT * main ||
                    4u * (cand[j].mass & 255u) > 5u * main) continue;
                if (recovery_neighbor(&cand[i], &cand[j], main)) { supported = 1; break; }
            }
        if (!supported) continue;
        /* Final geometries have already been coalesced. Do not spend output
         * slots on a second core row produced by nearby punctuation fragments. */
        for (j = 0; j < i; j++) {
            if (!(cand[j].mass & TEXT_USE)) continue;
            if (text_same_box(cand[i].x1, cand[i].top, cand[i].x2 - cand[i].x1, cand[i].bot - cand[i].top,
                              cand[j].x1, cand[j].top, cand[j].x2 - cand[j].x1, cand[j].bot - cand[j].top)) break;
        }
        if (j < i) continue;
        cand[i].mass |= TEXT_USE;
        added++;
    }
    if (added > VOCR_LINES_MAX - out->count) return; /* all-or-nothing safe fallback */
    for (i = 0; i < nc; i++) {
        TextCand value;
        VocrLine *line;
        if (!(cand[i].mass & TEXT_USE)) continue;
        value = cand[i];
        recovery_marks(im, bits, cand, nc, i, &value, main);
        /* Unused output slots stage final rectangles; candidate cores stay
         * immutable, and no old output coordinate/polarity is touched. */
        line = &out->lines[old_count + staged++];
        line->x = value.x1; line->y = value.top;
        line->w = value.x2 - value.x1; line->h = value.bot - value.top;
        line->dark = 0;
    }
    /* Small-mark attachment can change geometry: deduplicate AGAIN afterwards,
     * before polarity work or recognition. Old C3 crops always win a duplicate. */
    for (i = old_count; i < old_count + staged; i++) {
        VocrLine *line = &out->lines[i];
        for (j = 0; j < kept; j++)
            if (text_same_box(line->x, line->y, line->w, line->h,
                              out->lines[j].x, out->lines[j].y, out->lines[j].w, out->lines[j].h)) break;
        if (j < kept) continue;
        if (kept != i) out->lines[kept] = *line;
        line = &out->lines[kept++];
        line->dark = (unsigned)vocr_line_dark(im, line->x, line->y, line->w, line->h);
    }
    out->count = kept;
    for (i = 1; i < out->count; i++) {
        VocrLine value = out->lines[i]; j = i;
        while (j && (out->lines[j - 1u].y > value.y ||
                     (out->lines[j - 1u].y == value.y && out->lines[j - 1u].x > value.x))) {
            out->lines[j] = out->lines[j - 1u]; j--;
        }
        out->lines[j] = value;
    }
}

static unsigned component_root(TextComponent *components, unsigned label)
{
    unsigned root = label;
    while (components[root - 1u].parent != root) root = components[root - 1u].parent;
    while (label != root) {
        unsigned next = components[label - 1u].parent;
        components[label - 1u].parent = root;
        label = next;
    }
    return root;
}

static unsigned component_join(TextComponent *components, unsigned first, unsigned second)
{
    TextComponent *a, *b;
    first = component_root(components, first);
    second = component_root(components, second);
    if (first == second) return first;
    a = &components[first - 1u]; b = &components[second - 1u];
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->top < a->top) a->top = b->top;
    if (b->x2 > a->x2) a->x2 = b->x2;
    if (b->bot > a->bot) a->bot = b->bot;
    if (b->last > a->last) a->last = b->last;
    a->mass += b->mass;
    b->parent = first;
    return first;
}

static unsigned textbox_runs(const uint32_t *row, unsigned w, TextRun *runs)
{
    unsigned x, start = 0, last = 0, count = 0;
    int have = 0;
    for (x = 0; x <= w; x++) {
        if (x < w && !bit(row, x)) continue;
        if (x < w && have && x - last <= GAP) { last = x; continue; }
        if (have && last + 1u - start >= MIN_H) {
            runs[count].x1 = start; runs[count].x2 = last + 1u;
            runs[count++].label = 0;
        }
        if (x == w) break;
        start = last = x; have = 1;
    }
    return count;
}

static void textbox_emit(const TextComponent *component, TextCand *cand, unsigned *nc,
                         unsigned cap, unsigned *truncated)
{
    unsigned ch = component->bot - component->top;
    unsigned i;
    TextCand value;
    /* The legacy finder's native line-height ceiling excludes frame-sized
     * structures from the height estimate. Boundary contact and narrow glyph
     * shape are not reasons to discard an otherwise line-sized component. */
    if (ch < MIN_H || ch > LEG_MAX_LINE_H) return;
    value.x1 = component->x1; value.top = component->top;
    value.x2 = component->x2; value.bot = component->bot;
    value.mass = component->mass;
    /* Coalesce on emission, before imposing the bounded line capacity: a line
     * may have more disconnected word fragments than there are text rows. */
    for (i = 0; i < *nc;) {
        unsigned ah = value.bot - value.top, bh = cand[i].bot - cand[i].top;
        unsigned low = value.top > cand[i].top ? value.top : cand[i].top;
        unsigned high = value.bot < cand[i].bot ? value.bot : cand[i].bot;
        unsigned small = ah < bh ? ah : bh, big = ah > bh ? ah : bh;
        unsigned gap = value.x2 < cand[i].x1 ? cand[i].x1 - value.x2 :
                       cand[i].x2 < value.x1 ? value.x1 - cand[i].x2 : 0;
        if (high > low && 2u * (high - low) >= small && big <= 2u * small && gap <= GAPC * big) {
            if (cand[i].x1 < value.x1) value.x1 = cand[i].x1;
            if (cand[i].top < value.top) value.top = cand[i].top;
            if (cand[i].x2 > value.x2) value.x2 = cand[i].x2;
            if (cand[i].bot > value.bot) value.bot = cand[i].bot;
            value.mass += cand[i].mass;
            cand[i] = cand[--(*nc)]; i = 0;
        } else i++;
    }
    if (*nc == cap) { *truncated = 1; return; }
    cand[(*nc)++] = value;
}

/* Preserve the old close-line rule locally: a blank row separates two tall
 * components when the following MIN_H rows all contain ink in the same span. */
static int textbox_separate(const uint32_t *bits, unsigned words, unsigned h,
                            unsigned y, const TextComponent *component, const TextRun *run)
{
    unsigned x1 = component->x1 < run->x1 ? component->x1 : run->x1;
    unsigned x2 = component->x2 > run->x2 ? component->x2 : run->x2;
    unsigned k, empty = 0;
    if (component->bot - component->top < MIN_H || y + MIN_H > h) return 0;
    for (k = component->bot; k < y; k++)
        if (!row_on(bits + (size_t)k * words, x1, x2)) empty = 1;
    if (!empty) return 0;
    for (k = y; k < y + MIN_H; k++)
        if (row_on(bits + (size_t)k * words, x1, x2) < 2) return 0;
    return 1;
}

static int find_textbox(const VocrLinesImage *im, const uint32_t *bits, unsigned char *scratch,
                        VocrLineSet *out, int recovery)
{
    unsigned w = im->width, h = im->height, words = (w + 31u) / 32u;
    unsigned max_runs = (w + GAP) / (MIN_H + GAP), cap = 4u * max_runs;
    unsigned counts[4] = {0, 0, 0, 0}, y, i, j, k, nc = 0, truncated = 0, med = 0, total = 0, broad_only = 1;
    TextRun *runs = (TextRun *)(void *)scratch;
    TextComponent *components = (TextComponent *)(void *)(scratch + up4(sizeof(TextRun) * cap));
    TextCand *cand = (TextCand *)(void *)((unsigned char *)components + up4(sizeof(TextComponent) * cap));
    unsigned cand_cap = band_cap(h);
    if (w < MIN_H || h < MIN_H) return VOCR_LINES_OK;
    memset(components, 0, sizeof(TextComponent) * cap);
    for (y = 0; y <= h + MERGE; y++) {
        unsigned slot = y % 4u;
        TextRun *current = runs + (size_t)slot * max_runs;
        counts[slot] = 0;
        for (i = 0; i < cap; i++) {
            if (components[i].parent == i + 1u && y > components[i].last + MERGE + 1u) {
                textbox_emit(&components[i], cand, &nc, cand_cap, &truncated);
                components[i].parent = 0;
            }
        }
        if (y >= h) continue;
        counts[slot] = textbox_runs(bits + (size_t)y * words, w, current);
        for (i = 0; i < counts[slot]; i++) {
            TextRun *run = &current[i];
            unsigned label = 0, age;
            for (age = 1; age <= MERGE + 1u && age <= y; age++) {
                unsigned previous = (y - age) % 4u;
                TextRun *before = runs + (size_t)previous * max_runs;
                for (j = 0; j < counts[previous]; j++) {
                    unsigned root;
                    if (run->x1 > before[j].x2 || before[j].x1 > run->x2) continue;
                    root = component_root(components, before[j].label);
                    if (age > 1u && textbox_separate(bits, words, h, y, &components[root - 1u], run)) continue;
                    label = label ? component_join(components, label, root) : root;
                }
            }
            if (!label) {
                for (j = 0; j < cap && components[j].parent; j++) {}
                if (j == cap) return VOCR_LINES_OK; /* preserve GRAD on unsafe scratch topology */
                label = j + 1u;
                components[j].x1 = run->x1; components[j].top = y;
                components[j].x2 = run->x2; components[j].bot = y + 1u;
                components[j].parent = label; components[j].mass = 0;
            }
            run->label = component_root(components, label);
            {
                TextComponent *component = &components[run->label - 1u];
                if (run->x1 < component->x1) component->x1 = run->x1;
                if (run->x2 > component->x2) component->x2 = run->x2;
                component->bot = y + 1u; component->last = y;
                component->mass += run->x2 - run->x1;
            }
        }
        /* Rewrite every live reference before freeing merged labels for reuse. */
        for (i = 0; i < 4; i++)
            for (j = 0; j < counts[i]; j++) {
                TextRun *run = &runs[(size_t)i * max_runs + j];
                run->label = component_root(components, run->label);
            }
        for (i = 0; i < cap; i++)
            if (components[i].parent && components[i].parent != i + 1u) components[i].parent = 0;
    }
    for (i = 0; i < cap; i++)
        if (components[i].parent == i + 1u) textbox_emit(&components[i], cand, &nc, cand_cap, &truncated);
    if (truncated) return VOCR_LINES_OK;
    for (i = 1; i < nc; i++) {
        TextCand value = cand[i]; j = i;
        while (j && (cand[j - 1u].top > value.top ||
                     (cand[j - 1u].top == value.top && cand[j - 1u].x1 > value.x1))) {
            cand[j] = cand[j - 1u]; j--;
        }
        cand[j] = value;
    }
    /* Estimate only from broad coherent support. Empty GRAD output may also
     * use a short/narrow component; retained GRAD rows never depend on this. */
    for (i = 0; i < nc; i++) {
        unsigned ch = cand[i].bot - cand[i].top, weight = (cand[i].mass + ch / 2u) / ch;
        if (ch <= LEG_MAX_LINE_H && weight >= 2u * ch) total += weight;
    }
    if (!total && !out->count) {
        broad_only = 0;
        for (i = 0; i < nc; i++) {
            unsigned ch = cand[i].bot - cand[i].top;
            if (ch <= LEG_MAX_LINE_H) total += (cand[i].mass + ch / 2u) / ch;
        }
    }
    for (i = 0; i < nc && !med; i++) {
        unsigned ch = cand[i].bot - cand[i].top, below = 0, equal = 0;
        if (ch > LEG_MAX_LINE_H) continue;
        for (j = 0; j < nc; j++) {
            unsigned other = cand[j].bot - cand[j].top, weight;
            if (other > LEG_MAX_LINE_H) continue;
            weight = (cand[j].mass + other / 2u) / other;
            if (broad_only && weight < 2u * other) continue;
            if (other < ch) below += weight;
            if (other == ch) equal += weight;
        }
        if (below <= total / 2u && total / 2u < below + equal) med = ch;
    }
    for (i = 0; i < nc; i++) {
        unsigned x1 = cand[i].x1, x2 = cand[i].x2, top = cand[i].top, bot = cand[i].bot;
        unsigned bh = bot - top, last, x, y1, y2, weight = (cand[i].mass + bh / 2u) / bh;
        if (bh > LEG_MAX_LINE_H) { cand[i].mass = TEXT_DUP; continue; }
        /* Recover nearby punctuation which was too small to make a kept run. */
        for (last = x2 - 1u, x = x2; x < w && x - last <= bh; x++)
            for (k = top; k < bot; k++)
                if (bit(bits + (size_t)k * words, x)) { last = x; break; }
        x2 = last + 1u;
        for (last = x1, x = x1; x && last - (x - 1u) <= bh; x--)
            for (k = top; k < bot; k++)
                if (bit(bits + (size_t)k * words, x - 1u)) { last = x - 1u; break; }
        x1 = last;
        y1 = top; y2 = bot;
        while (y1 && top - y1 < PAD && row_on(bits + (size_t)(y1 - 1u) * words, x1, x2) >= 2) y1--;
        while (y2 < h && y2 - bot < PAD && row_on(bits + (size_t)y2 * words, x1, x2) >= 2) y2++;
        cand[i].x1 = x1 > PAD ? x1 - PAD : 0; cand[i].top = y1 > PAD ? y1 - PAD : 0;
        cand[i].x2 = x2 + PAD < w ? x2 + PAD : w; cand[i].bot = y2 + PAD < h ? y2 + PAD : h;
        cand[i].mass = (weight << 8) | bh | (weight >= 2u * bh ? TEXT_STRONG : 0);
    }
    /* Expansion can make previously distinct fragments the same physical row.
     * Coalesce FINAL geometry before topology decisions or recognizer calls. */
    for (i = 0; i < nc; i++) {
        if (cand[i].mass & TEXT_DUP) continue;
        for (j = i + 1u; j < nc; j++) {
            if (cand[j].mass & TEXT_DUP) continue;
            if (!text_same_box(cand[i].x1, cand[i].top, cand[i].x2 - cand[i].x1, cand[i].bot - cand[i].top,
                               cand[j].x1, cand[j].top, cand[j].x2 - cand[j].x1, cand[j].bot - cand[j].top)) continue;
            if ((cand[j].mass & 255u) > (cand[i].mass & 255u)) cand[i].mass = cand[j].mass;
            if (cand[j].x1 < cand[i].x1) cand[i].x1 = cand[j].x1;
            if (cand[j].top < cand[i].top) cand[i].top = cand[j].top;
            if (cand[j].x2 > cand[i].x2) cand[i].x2 = cand[j].x2;
            if (cand[j].bot > cand[i].bot) cand[i].bot = cand[j].bot;
            cand[j].mass |= TEXT_DUP;
        }
    }
    text_route(im, cand, nc, med, out);
    if (recovery) text_recover(im, cand, nc, bits, scratch, out);
    return VOCR_LINES_OK;
}

static int find_grad(const VocrLinesImage *im, unsigned char *base, VocrLineSet *out, int textbox)
{
    unsigned w = im->width, h = im->height, words = (w + 31u) / 32u;
    unsigned x, y, i, nb = 0, nc = 0, peak = 0, th, T, p99 = 0;
    uint32_t hist[256];
    unsigned long long n, cum;
    uint32_t *bits, *prof, *kc, *oc;
    unsigned char *crow;
    Band *bands;
    Cand *cand;
    Contrast cs;
    unsigned char *p = base;

    bits = (uint32_t *)(void *)p; p += up4((size_t)words * h * 4u);
    prof = (uint32_t *)(void *)p; p += up4((size_t)h * 4u);
    for (i = 0; i < 3; i++) { cs.mx[i] = p; p += up4(w); }
    for (i = 0; i < 3; i++) { cs.mn[i] = p; p += up4(w); }
    crow = p; p += up4(w);
    kc = (uint32_t *)(void *)p; p += up4((size_t)w * 4u);
    oc = (uint32_t *)(void *)p; p += up4((size_t)w * 4u);
    bands = (Band *)(void *)p; p += up4(sizeof(Band) * band_cap(h));
    cand = (Cand *)(void *)p;
    cs.im = im;
    cs.next = 0;

    /* Pass 1: contrast histogram -> threshold. */
    memset(hist, 0, sizeof(hist));
    contrast_begin(&cs);
    for (y = 0; y < h; y++) {
        contrast_row(&cs, y, crow);
        for (x = 0; x < w; x++) hist[crow[x]]++;
    }
    n = (unsigned long long)w * h;
    for (cum = 0, i = 0; i < 256; i++) {
        cum += hist[i];
        if (cum * 100u >= n * 99u) { p99 = i; break; }
    }
    T = 2u * p99 / 5u;
    if (T < T_MIN) T = T_MIN;

    /* Pass 2: on-mask and row profile of kept runs. */
    memset(bits, 0, (size_t)words * h * 4u);
    contrast_begin(&cs);
    for (y = 0; y < h; y++) {
        uint32_t *row = bits + (size_t)y * words;
        contrast_row(&cs, y, crow);
        for (x = 0; x < w; x++)
            row[x >> 5] |= (uint32_t)(crow[x] >= T) << (x & 31u);
        prof[y] = row_kept(row, w, NULL);
        if (prof[y] > peak) peak = prof[y];
    }
    th = peak / ROW_DIV;
    if (th < LMIN) th = LMIN;

    /* Bands of active rows (gaps of up to MERGE rows bridged), at least MIN_H tall. A
     * bridged gap holding an empty row, with runs of at least MIN_H active rows right
     * above and below it, separates two lines instead. */
    for (y = 0; y < h;) {
        unsigned s, rs, last;
        if (prof[y] < th) { y++; continue; }
        s = rs = last = y++;            /* rs: start of the current run of active rows */
        while (y < h && (prof[y] >= th || y - last <= MERGE)) {
            if (prof[y] >= th) {
                if (y - last > 1u) {    /* y ends a bridged gap */
                    unsigned k = y, g, empty = 0;
                    while (k < h && prof[k] >= th) k++;
                    for (g = last + 1u; g < y; g++)
                        if (!prof[g]) empty = 1u;
                    if (empty && last + 1u - rs >= MIN_H && k - y >= MIN_H) {
                        bands[nb].a = s;
                        bands[nb].b = last + 1u;
                        nb++;
                        s = y;
                    }
                    rs = y;
                }
                last = y;
            }
            y++;
        }
        if (last + 1 - s >= MIN_H) {
            bands[nb].a = s;
            bands[nb].b = last + 1;
            nb++;
        }
    }

    for (i = 0; i < nb; i++) {
        unsigned a = bands[i].a, b = bands[i].b, bh = b - a, d, x1 = 0, x2 = 0, last;
        unsigned long best = 0, mass = 0;
        unsigned cs0 = 0, prevc = 0, have = 0, up, dn, top, bot;
        memset(kc, 0, (size_t)w * 4u);
        memset(oc, 0, (size_t)w * 4u);
        for (y = a; y < b; y++) {
            const uint32_t *row = bits + (size_t)y * words;
            row_kept(row, w, kc);
            for (x = 0; x < w; x++) {
                if (!row[x >> 5]) { x |= 31u; continue; }
                oc[x] += (uint32_t)bit(row, x);
            }
        }
        /* column clusters of kept pixels; keep the heaviest (first on ties) */
        for (x = 0; x <= w; x++) {
            if (x < w && !kc[x]) continue;
            if (have && (x == w || x - prevc > GAPC * bh)) {
                if (mass > best) { best = mass; x1 = cs0; x2 = prevc + 1; }
                have = 0;
            }
            if (x == w) break;
            if (!have) { cs0 = x; mass = 0; have = 1; }
            mass += kc[x];
            prevc = x;
        }
        if (!best) continue;
        /* widen over nearby on-pixels */
        d = bh > 4u ? bh : 4u;
        for (last = x2 - 1, x = x2; x < w && x - last <= d; x++)
            if (oc[x]) last = x;
        x2 = last + 1;
        for (last = x1, x = x1; x > 0 && last - (x - 1) <= d; x--)
            if (oc[x - 1]) last = x - 1;
        x1 = last;
        /* grow vertically into the gaps, at most half way to the neighbour band */
        up = bh / 2u;
        if (i) { if ((a - bands[i - 1].b) / 2u < up) up = (a - bands[i - 1].b) / 2u; }
        else if (a < up) up = a;
        dn = bh / 2u;
        if (i + 1 < nb) { if ((bands[i + 1].a - b) / 2u < dn) dn = (bands[i + 1].a - b) / 2u; }
        else if (h - b < dn) dn = h - b;
        for (top = a; a - top < up && row_on(bits + (size_t)(top - 1) * words, x1, x2) >= 2; top--) {}
        for (bot = b; bot - b < dn && row_on(bits + (size_t)bot * words, x1, x2) >= 2; bot++) {}
        cand[nc].x1 = x1; cand[nc].top = top; cand[nc].x2 = x2; cand[nc].bot = bot;
        nc++;
    }

    /* drop lines under FURI_PCT of the median height (upper median) */
    out->count = 0;
    out->truncated = 0;
    if (nc) {
        unsigned med = 0, k, j;
        /* med = sorted(heights)[nc / 2] */
        for (k = 0; k < nc; k++) {
            unsigned hk = cand[k].bot - cand[k].top, below = 0, equal = 0;
            for (j = 0; j < nc; j++) {
                unsigned hj = cand[j].bot - cand[j].top;
                below += hj < hk;
                equal += hj == hk;
            }
            if (below <= nc / 2u && nc / 2u < below + equal) { med = hk; break; }
        }
        for (k = 0; k < nc; k++) {
            unsigned hk = cand[k].bot - cand[k].top, lx, ly, ex, ey;
            VocrLine *ln;
            if (100u * hk < FURI_PCT * med) continue;
            if (out->count == VOCR_LINES_MAX) { out->truncated = 1; break; }
            lx = cand[k].x1 > PAD ? cand[k].x1 - PAD : 0;
            ly = cand[k].top > PAD ? cand[k].top - PAD : 0;
            ex = cand[k].x2 + PAD < w ? cand[k].x2 + PAD : w;
            ey = cand[k].bot + PAD < h ? cand[k].bot + PAD : h;
            ln = &out->lines[out->count++];
            ln->x = lx; ln->y = ly; ln->w = ex - lx; ln->h = ey - ly;
            ln->dark = (unsigned)vocr_line_dark(im, lx, ly, ln->w, ln->h);
        }
    }
    if (textbox && !out->truncated)
        return find_textbox(im, bits, (unsigned char *)(void *)prof, out, textbox == 2);
    return VOCR_LINES_OK;
}

/* Exact port of vita-jp-overlay's vjo_ocr_find_lines (core/ocr_lines.c). */
static int find_legacy(const VocrLinesImage *im, unsigned char *base, VocrLineSet *out)
{
    unsigned w = im->width, h = im->height, y, start = 0, last = 0, active = 0;
    uint32_t *left = (uint32_t *)(void *)base, *right = left + up4((size_t)h * 4u) / 4u;
    out->count = 0;
    out->truncated = 0;
    if (w > LEG_MAX_W || h > LEG_MAX_H) return VOCR_LINES_E_REGION;
    for (y = 0; y <= h; y++) {
        unsigned count = 0;
        if (y < h) {
            const unsigned char *p = im->pixels + (size_t)y * im->stride;
            unsigned x;
            left[y] = w; right[y] = 0;
            for (x = 0; x < w; x++, p += im->bpp) {
                int bright = im->bpp == VOCR_LINES_GREY ? p[0] >= 180
                                                        : p[0] >= 180 && p[1] >= 180 && p[2] >= 180;
                if (bright) {
                    if (x < left[y]) left[y] = x;
                    right[y] = x + 1;
                    count++;
                }
            }
            if (count > w * 3u / 4u) return VOCR_LINES_E_REGION;
        }
        if (count >= 2) {
            if (!active) { start = y; active = 1; }
            last = y;
        }
        if (active && (y == h || y - last >= 3)) {
            unsigned lh = last - start + 1;
            if (lh >= 8) {
                unsigned x1 = w, x2 = 0, yy;
                if (lh > LEG_MAX_LINE_H - 8 || out->count == LEG_MAX_LINES) return VOCR_LINES_E_REGION;
                for (yy = start; yy <= last; yy++) {
                    if (left[yy] < x1) x1 = left[yy];
                    if (right[yy] > x2) x2 = right[yy];
                }
                if (x2 > x1) {
                    VocrLine *line = &out->lines[out->count++];
                    unsigned endx = x2 + 4 < w ? x2 + 4 : w, endy = last + 5 < h ? last + 5 : h;
                    line->x = x1 > 4 ? x1 - 4 : 0;
                    line->y = start > 4 ? start - 4 : 0;
                    line->w = endx - line->x;
                    line->h = endy - line->y;
                    line->dark = 0;
                }
            }
            active = 0;
        }
    }
    return VOCR_LINES_OK;
}

int vocr_find_lines(const VocrLinesImage *im, int finder, void *arena, size_t arena_bytes,
                    VocrLineSet *out)
{
    unsigned char *base;
    int rc;
    if (!out) return VOCR_LINES_E_ARG;
    out->count = 0;
    out->truncated = 0;
    if (!im || !im->pixels || !im->width || !im->height || !arena ||
        (im->bpp != VOCR_LINES_GREY && im->bpp != VOCR_LINES_RGBA) ||
        im->stride < (size_t)im->width * im->bpp ||
        (finder != VOCR_LINES_GRAD && finder != VOCR_LINES_LEGACY && finder != VOCR_LINES_TEXTBOX &&
         finder != VOCR_LINES_TEXTBOX_RECOVERY))
        return VOCR_LINES_E_ARG;
    if (arena_bytes < vocr_lines_arena_bytes(im->width, im->height)) return VOCR_LINES_E_ARENA;
    base = align4(arena);
    rc = finder == VOCR_LINES_LEGACY ? find_legacy(im, base, out) :
         find_grad(im, base, out, finder == VOCR_LINES_TEXTBOX_RECOVERY ? 2 : finder == VOCR_LINES_TEXTBOX);
    if (rc) out->count = 0; /* never report a partial result */
    return rc;
}

int vocr_line_dark(const VocrLinesImage *im, unsigned x, unsigned y, unsigned w, unsigned h)
{
    uint32_t hist[256], bh[256];
    unsigned long long n, nb_, k, cum, nd = 0, nbr = 0;
    unsigned i, j, bg = 0, lo = 0, hi = 0, d;
    if (!im || !im->pixels || (im->bpp != VOCR_LINES_GREY && im->bpp != VOCR_LINES_RGBA) ||
        !w || !h || x >= im->width || y >= im->height || w > im->width - x || h > im->height - y)
        return VOCR_LINES_E_ARG;
    memset(hist, 0, sizeof(hist));
    memset(bh, 0, sizeof(bh));
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) hist[grey_at(im, x + i, y + j)]++;
    /* border: top row, bottom row, left column, right column (corners twice) */
    for (i = 0; i < w; i++) {
        bh[grey_at(im, x + i, y)]++;
        bh[grey_at(im, x + i, y + h - 1)]++;
    }
    for (j = 0; j < h; j++) {
        bh[grey_at(im, x, y + j)]++;
        bh[grey_at(im, x + w - 1, y + j)]++;
    }
    nb_ = 2ul * w + 2ul * h;
    for (cum = 0, i = 0; i < 256; i++) {        /* sorted(border)[nb_ / 2] */
        cum += bh[i];
        if (cum > nb_ / 2u) { bg = i; break; }
    }
    n = (unsigned long)w * h;
    k = n * 2u / 100u;
    for (cum = 0, i = 0; i < 256; i++) {
        cum += hist[i];
        if (cum >= k) { lo = i; break; }
    }
    k = n * 98u / 100u;
    for (cum = 0, i = 0; i < 256; i++) {
        cum += hist[i];
        if (cum >= k) { hi = i; break; }
    }
    d = (hi - lo) / 4u;
    if (d < 16u) d = 16u;
    for (i = 0; i < 256; i++) {
        if (i + d < bg) nd += hist[i];
        if (i > bg + d) nbr += hist[i];
    }
    return nd > 8u * nbr ? 1 : 0;
}
