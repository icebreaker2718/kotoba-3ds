/* Post-finder refinement: see vocr_lines_refine.h and docs/finder-tighten.md.
 *
 * Per line box (x, y, w, h) of the finder's set, in set order:
 *  1. Grey g = (77 R + 150 G + 29 B) >> 8 (the finder's and the recognizer's grey).
 *  2. Statistics of the box's middle rows [h/4, h - h/4): bg = their median, lo / hi = their
 *     2nd / 98th percentiles, d = max(16, (hi - lo) / 4) (vocr_line_dark's rule on the rows
 *     the text fills, so a portrait or a scene at the box's top and bottom cannot become the
 *     background).
 *  3. Polarity of the ink mask: the line's dark flag, unless the middle rows hold over four
 *     times as many pixels deviating the other way (a box whose border sits on scenery that
 *     made the finder call dark text on a light button light, or the reverse).
 *  4. Ink mask: pixels deviating from bg by more than d in that direction.
 *  5. VOCR_REFINE_FRAMES: a row whose longest run of ink covers half the box width or more is
 *     a frame line and is cleared.
 *  6. Bands: runs of rows holding 2 or more ink pixels in 2 or more runs (a text row crosses
 *     several strokes; a lone run is a border or the edge of a shape). The main band is the
 *     heaviest (first on ties). VOCR_REFINE_SPLIT: every other band at least 80% as tall as the
 *     main band, at least 6 rows, a quarter of its mass, and with glyphs (the upper median of
 *     its column clusters' heights) at least 70% as tall as the main band's becomes a line of
 *     its own (furigana and stray marks do not); all of them only if the set stays within
 *     VOCR_LINES_MAX lines.
 *  7. VOCR_REFINE_TIGHTEN: each line's rows become its band plus a margin of 16% of the band's
 *     height, never past the box, nor past the midpoint to the next selected band.
 *  8. VOCR_REFINE_CURSOR / VOCR_REFINE_PORTRAIT: column clusters of the band's ink; a trailing
 *     isolated blob (a speck, or a solid blob at least 0.6 times as wide as tall) beyond a gap
 *     of 1.5 band heights, and an end part beyond a gap of one band height whose ink runs on
 *     vertically for 1.9 band heights or more, at least half a band height above and below the
 *     band (a portrait edge or another object crossing the line), or that is wide and solid,
 *     are cut off (half a band height of margin, never past the middle of the gap).
 * A box without a band is kept as it is. Boxes never grow, keep their dark flags and stay in
 * order; split lines take the place of their box.
 *
 * Memory: the box's ink mask (1 bit per pixel), 2 bytes per row and 4 per column from the
 * caller's arena; when the arena also holds the box's grey pixels, the box is read once,
 * else twice (the middle rows for the statistics, then every row for the mask).
 */
#include "vocr_lines_refine.h"

#include <stdint.h>
#include <string.h>

#define R_DMIN 16u
#define R_FLIP 4u          /* polarity flip: other-way deviations > R_FLIP x the line's own */
#define R_MINROW 2u        /* a band row holds at least this many ink pixels */
#define R_MINRUNS 2u       /* ... in at least this many runs */
#define R_SPLIT_H 80u      /* % of the main band's height a split band needs */
#define R_SPLIT_M 25u      /* % of the main band's mass a split band needs */
#define R_SPLIT_MINH 6u    /* rows a split band needs */
#define R_SPLIT_G 70u      /* % of the main band's glyph height a split band's glyphs need */
#define R_MARGIN 16u       /* % of the band height added above and below */
#define R_CUR_GAP 15u      /* tenths of a band height: the cursor's gap */
#define R_CUR_W 15u        /* tenths of a band height: the cursor's widest */
#define R_POR_GAP 10u      /* tenths of a band height: the portrait's gap */
#define R_POR_SPAN 19u     /* tenths of a band height: a portrait's vertical run */
#define R_POR_EXT 5u       /* tenths of a band height: ... that far above and below its band ink */
#define R_POR_W 15u        /* tenths of a band height: a wide solid part */
#define R_SOLID 6u         /* tenths: solid fill (ink / bounding box) */
#define R_XMARGIN 50u      /* % of the band height kept beside a cut */

typedef struct {
    const VocrLinesImage *im;  /* in-memory image, or */
    const VocrLinesRows *rows; /* a row source */
} Src;

static size_t up4(size_t n)
{
    return (n + 3u) & ~(size_t)3u;
}

static unsigned char *align4(void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (unsigned char *)p + ((4u - (v & 3u)) & 3u);
}

/* Arena bytes of the two-pass form for a box w x h (after base alignment). */
static size_t mask_bytes(unsigned w, unsigned h)
{
    size_t words = ((size_t)w + 31u) / 32u;
    return up4(words * h * 4u) + up4((size_t)h * 2u) + up4((size_t)w * 4u) + up4(((size_t)h + 1u) * 4u);
}

size_t vocr_refine_arena_bytes(unsigned max_width, unsigned max_height)
{
    if (!max_width || !max_height) return 0;
    return 3u + mask_bytes(max_width, max_height);
}

static unsigned src_width(const Src *s)
{
    return s->im ? s->im->width : s->rows->width;
}

static unsigned src_height(const Src *s)
{
    return s->im ? s->im->height : s->rows->height;
}

static unsigned src_bpp(const Src *s)
{
    return s->im ? s->im->bpp : s->rows->bpp;
}

static const unsigned char *src_row(const Src *s, unsigned y)
{
    if (s->im) return s->im->pixels + (size_t)y * s->im->stride;
    return s->rows->row(s->rows->user, y);
}

static unsigned grey_px(const unsigned char *row, unsigned x, unsigned bpp)
{
    const unsigned char *p = row + (size_t)x * bpp;
    if (bpp == VOCR_LINES_GREY) return p[0];
    return (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
}

static int bit(const uint32_t *row, unsigned x)
{
    return (int)((row[x >> 5] >> (x & 31u)) & 1u);
}

static unsigned pct(const uint32_t *hist, unsigned long long k)
{
    unsigned long long cum = 0;
    unsigned v;
    for (v = 0; v < 256u; v++) {
        cum += hist[v];
        if (cum >= k) return v;
    }
    return 255u;
}

/* Box statistics: the threshold and the mask's polarity from the middle rows' histogram. */
typedef struct {
    unsigned bg, d, mdark;
} Stats;

static void stats_of(const uint32_t *hist, unsigned long long n, unsigned dark, Stats *st)
{
    unsigned long long nd = 0, nb = 0, mine, other;
    unsigned v, lo, hi;
    st->bg = pct(hist, n / 2u + 1u);
    lo = pct(hist, n * 2u / 100u);
    hi = pct(hist, n * 98u / 100u);
    st->d = (hi - lo) / 4u;
    if (st->d < R_DMIN) st->d = R_DMIN;
    for (v = 0; v < 256u; v++) {
        if (v + st->d < st->bg) nd += hist[v];
        if (v > st->bg + st->d) nb += hist[v];
    }
    mine = dark ? nd : nb;
    other = dark ? nb : nd;
    st->mdark = other > R_FLIP * mine ? !dark : dark;
}

static int ink(unsigned g, const Stats *st)
{
    return st->mdark ? g + st->d < st->bg : g > st->bg + st->d;
}

/* The box's mask (bits), one row at a time; frame rows cleared when frames is set. Row
 * counts of the cleared mask into cnt. */
static void mask_row(uint32_t *row, unsigned words, unsigned w, const unsigned char *grey, const Stats *st,
                     int frames, uint16_t *cnt)
{
    unsigned x, n = 0, run = 0, best = 0, runs = 0;
    memset(row, 0, (size_t)words * 4u);
    for (x = 0; x < w; x++) {
        if (ink(grey[x], st)) {
            row[x >> 5] |= 1u << (x & 31u);
            n++;
            if (!run) runs++;
            if (++run > best) best = run;
        } else {
            run = 0;
        }
    }
    if (frames && n && 2u * best >= w) {
        memset(row, 0, (size_t)words * 4u);
        n = 0;
    }
    *cnt = (uint16_t)(runs >= R_MINRUNS ? n : 0u);   /* a band row crosses several strokes */
}

/* Next band (run of rows with cnt >= R_MINROW) at or after row *y: returns 0 if none. */
static int next_band(const uint16_t *cnt, unsigned h, unsigned *y, unsigned *a, unsigned *b, unsigned long *mass)
{
    unsigned r = *y;
    while (r < h && cnt[r] < R_MINROW) r++;
    if (r >= h) {
        *y = h;
        return 0;
    }
    *a = r;
    *mass = 0;
    while (r < h && cnt[r] >= R_MINROW) *mass += cnt[r++];
    *b = r;
    *y = r;
    return 1;
}

/* Column clusters of the band [ya, yb): fills col (ink per column) and returns the count. */
static void band_columns(const uint32_t *bits, unsigned words, unsigned w, unsigned ya, unsigned yb, uint32_t *col)
{
    unsigned x, y;
    memset(col, 0, (size_t)w * 4u);
    for (y = ya; y < yb; y++) {
        const uint32_t *row = bits + (size_t)y * words;
        for (x = 0; x < w; x++) {
            if (!row[x >> 5]) { x |= 31u; continue; }
            col[x] += (uint32_t)bit(row, x);
        }
    }
}

/* Next cluster of columns with ink at or after column *x within [x, end): 0 if none. */
static int next_cluster(const uint32_t *col, unsigned end, unsigned *x, unsigned *s, unsigned *e)
{
    unsigned c = *x;
    while (c < end && !col[c]) c++;
    if (c >= end) {
        *x = end;
        return 0;
    }
    *s = c;
    while (c < end && col[c]) c++;
    *e = c;
    *x = c;
    return 1;
}

/* Glyph height of the band [ya, yb): the upper median of its column clusters' ink heights
 * (heights counted in hist, hb + 1 entries). */
static unsigned glyph_height(const uint32_t *bits, unsigned words, unsigned w, unsigned ya, unsigned yb,
                             uint32_t *col, uint32_t *hist)
{
    unsigned c, s, e, i, hb = yb - ya;
    unsigned long n = 0, k = 0;
    band_columns(bits, words, w, ya, yb, col);
    memset(hist, 0, ((size_t)hb + 1u) * 4u);
    for (c = 0; next_cluster(col, w, &c, &s, &e);) {
        unsigned y, t = yb, b = ya;
        for (y = ya; y < yb; y++) {
            const uint32_t *row = bits + (size_t)y * words;
            unsigned x;
            for (x = s; x < e; x++)
                if (bit(row, x)) break;
            if (x < e) {
                if (t == yb) t = y;
                b = y + 1u;
            }
        }
        hist[b - t]++;
        n++;
    }
    for (i = 0; i <= hb; i++) {           /* sorted(heights)[n / 2] */
        k += hist[i];
        if (k > n / 2u) return i;
    }
    return 0;
}

/* Rows of the band [ya, yb) with ink in columns [s, e): their extent [t, b), and how far the run
 * of rows with ink in those columns that holds it continues above (*up) and below (*dn). */
static int part_rows(const uint32_t *bits, unsigned words, unsigned h, unsigned ya, unsigned yb, unsigned s,
                     unsigned e, unsigned *t, unsigned *b, unsigned *up, unsigned *dn)
{
    unsigned y, top = h, bot = 0;
    for (y = ya; y < yb; y++) {
        const uint32_t *row = bits + (size_t)y * words;
        unsigned x;
        for (x = s; x < e; x++)
            if (bit(row, x)) break;
        if (x < e) {
            if (top == h) top = y;
            bot = y + 1u;
        }
    }
    if (top == h) return 0;
    *t = top;
    *b = bot;
    while (top > 0) {
        const uint32_t *row = bits + (size_t)(top - 1u) * words;
        unsigned x;
        for (x = s; x < e; x++)
            if (bit(row, x)) break;
        if (x == e) break;
        top--;
    }
    while (bot < h) {
        const uint32_t *row = bits + (size_t)bot * words;
        unsigned x;
        for (x = s; x < e; x++)
            if (bit(row, x)) break;
        if (x == e) break;
        bot++;
    }
    *up = *t - top;
    *dn = bot - *b;
    return 1;
}

static unsigned long col_mass(const uint32_t *col, unsigned s, unsigned e)
{
    unsigned long m = 0;
    unsigned x;
    for (x = s; x < e; x++) m += col[x];
    return m;
}

/* A portrait-like part [s, e) of the band [ya, yb): its ink runs on vertically for R_POR_SPAN
 * tenths of a band height, at least R_POR_EXT tenths both above and below its ink in the band
 * (an object crossing the line, not scenery above or below text), or it is wider than R_POR_W
 * tenths and solidly filled. */
static int portrait_part(const uint32_t *bits, unsigned words, unsigned h, unsigned ya, unsigned yb,
                         const uint32_t *col, unsigned s, unsigned e)
{
    unsigned hb = yb - ya, t, b, up, dn, pw = e - s;
    unsigned long mass = col_mass(col, s, e);
    if (!part_rows(bits, words, h, ya, yb, s, e, &t, &b, &up, &dn)) return 0;
    if (10u * (up + (b - t) + dn) >= R_POR_SPAN * hb && 10u * up >= R_POR_EXT * hb && 10u * dn >= R_POR_EXT * hb)
        return 1;
    return 10u * pw > R_POR_W * hb && 10u * mass >= (unsigned long)R_SOLID * pw * (b - t);
}

/* Columns [*x0, *x1) of the line whose band is [ya, yb), after the cursor and portrait rules.
 * Clusters are runs of columns with ink in the band rows. */
static void trim_columns(const uint32_t *bits, unsigned words, unsigned w, unsigned h, unsigned ya, unsigned yb,
                         unsigned flags, uint32_t *col, unsigned *x0, unsigned *x1)
{
    unsigned hb = yb - ya, mx = (R_XMARGIN * hb + 50u) / 100u, c, s, e, n = 0;
    unsigned ls = 0, le = 0, pe = 0;
    band_columns(bits, words, w, ya, yb, col);
    for (c = 0; next_cluster(col, w, &c, &s, &e);) {
        pe = le;
        ls = s;
        le = e;
        n++;
    }
    if (n < 2) return;
    if (flags & VOCR_REFINE_CURSOR) {
        unsigned gap = ls - pe, cw = le - ls, t, b, up, dn;
        if (10u * gap > R_CUR_GAP * hb && 10u * cw <= R_CUR_W * hb &&
            part_rows(bits, words, h, ya, yb, ls, le, &t, &b, &up, &dn)) {
            unsigned ph = b - t;
            unsigned long mass = col_mass(col, ls, le);
            if (4u * ph <= hb || (10u * mass >= (unsigned long)R_SOLID * cw * ph && 10u * cw >= 6u * ph)) {
                unsigned cut = pe + mx, mid = (pe + ls) / 2u;
                *x1 = cut < mid ? cut : mid;
            }
        }
    }
    if (flags & VOCR_REFINE_PORTRAIT) {
        unsigned gs, ge, have, lim = *x1;
        /* left end: the clusters up to the first gap over R_POR_GAP tenths of a band height */
        c = 0;
        if (next_cluster(col, lim, &c, &gs, &ge)) {
            while (next_cluster(col, lim, &c, &s, &e)) {
                if (10u * (s - ge) > R_POR_GAP * hb) {
                    if (portrait_part(bits, words, h, ya, yb, col, gs, ge)) {
                        unsigned cut = s > mx ? s - mx : 0, mid = (ge + s + 1u) / 2u;
                        *x0 = cut > mid ? cut : mid;
                    }
                    break;
                }
                ge = e;
            }
        }
        /* right end: the clusters after the last such gap, within the columns kept so far */
        {
            unsigned prev_e = 0, part_s = 0, gap_s = 0;
            have = 0;
            for (c = *x0; next_cluster(col, lim, &c, &s, &e);) {
                if (have && 10u * (s - prev_e) > R_POR_GAP * hb) {
                    gap_s = prev_e;
                    part_s = s;
                }
                prev_e = e;
                have = 1;
            }
            if (part_s && portrait_part(bits, words, h, ya, yb, col, part_s, prev_e)) {
                unsigned cut = gap_s + mx, mid = (gap_s + part_s) / 2u;
                *x1 = cut < mid ? cut : mid;
            }
        }
    }
}

/* A band comparable in height and mass to the main band [ka, kb) of mass km (its glyphs are
 * compared separately). */
static int comparable(unsigned a, unsigned b, unsigned long mass, unsigned ka, unsigned kb, unsigned long km)
{
    return 100u * (b - a) >= R_SPLIT_H * (kb - ka) && 100u * mass >= R_SPLIT_M * km && b - a >= R_SPLIT_MINH;
}

/* Refines one box into out (appending), reading its rows from src; room is how many lines it
 * may become. Returns 0, or VOCR_LINES_E_SOURCE. */
static int refine_box(const Src *src, const VocrLine *ln, unsigned flags, unsigned char *base, size_t avail,
                      unsigned room, VocrLineSet *out, unsigned long *rows_read)
{
    unsigned w = ln->w, h = ln->h, bpp = src_bpp(src), words = (w + 31u) / 32u;
    unsigned q = h / 4u, y, x, ka = 0, kb = 0, nsel = 0, r, a, b, i;
    unsigned sel_a[VOCR_LINES_MAX], sel_b[VOCR_LINES_MAX];
    unsigned long km = 0, mass;
    size_t need = mask_bytes(w, h);
    uint32_t hist[256], *bits, *col;
    uint16_t *cnt;
    uint32_t *gh;
    unsigned char *grey = NULL, *p = base;
    unsigned long long n = (unsigned long long)w * (h - 2u * q);
    Stats st;
    int frames = (flags & VOCR_REFINE_FRAMES) != 0;
    bits = (uint32_t *)(void *)p; p += up4((size_t)words * h * 4u);
    cnt = (uint16_t *)(void *)p; p += up4((size_t)h * 2u);
    col = (uint32_t *)(void *)p; p += up4((size_t)w * 4u);
    gh = (uint32_t *)(void *)p; p += up4(((size_t)h + 1u) * 4u);
    if (need + (size_t)w * h <= avail) grey = p;    /* one pass: keep the box's grey pixels */
    memset(hist, 0, sizeof(hist));
    if (grey) {
        for (y = 0; y < h; y++) {
            const unsigned char *row = src_row(src, ln->y + y);
            unsigned char *g = grey + (size_t)y * w;
            if (!row) return VOCR_LINES_E_SOURCE;
            for (x = 0; x < w; x++) g[x] = (unsigned char)grey_px(row, ln->x + x, bpp);
            if (y >= q && y < h - q)
                for (x = 0; x < w; x++) hist[g[x]]++;
        }
        *rows_read += h;
        stats_of(hist, n, ln->dark, &st);
        for (y = 0; y < h; y++)
            mask_row(bits + (size_t)y * words, words, w, grey + (size_t)y * w, &st, frames, &cnt[y]);
    } else {
        unsigned char *g = (unsigned char *)(void *)col; /* one grey row, in the column counts */
        for (y = q; y < h - q; y++) {
            const unsigned char *row = src_row(src, ln->y + y);
            if (!row) return VOCR_LINES_E_SOURCE;
            for (x = 0; x < w; x++) hist[grey_px(row, ln->x + x, bpp)]++;
        }
        stats_of(hist, n, ln->dark, &st);
        for (y = 0; y < h; y++) {
            const unsigned char *row = src_row(src, ln->y + y);
            if (!row) return VOCR_LINES_E_SOURCE;
            for (x = 0; x < w; x++) g[x] = (unsigned char)grey_px(row, ln->x + x, bpp);
            mask_row(bits + (size_t)y * words, words, w, g, &st, frames, &cnt[y]);
        }
        *rows_read += (unsigned long)(h - 2u * q) + h;
    }
    /* the main band: the heaviest, the first on ties */
    for (r = 0; next_band(cnt, h, &r, &a, &b, &mass);)
        if (mass > km) { km = mass; ka = a; kb = b; }
    if (!km) {
        out->lines[out->count++] = *ln;         /* no band: the box as it is */
        return VOCR_LINES_OK;
    }
    if (flags & VOCR_REFINE_SPLIT) {
        unsigned kg = glyph_height(bits, words, w, ka, kb, col, gh);
        for (r = 0; next_band(cnt, h, &r, &a, &b, &mass);)
            if (a == ka || (comparable(a, b, mass, ka, kb, km) &&
                            100u * glyph_height(bits, words, w, a, b, col, gh) >= R_SPLIT_G * kg)) {
                if (nsel < VOCR_LINES_MAX) { sel_a[nsel] = a; sel_b[nsel] = b; }
                nsel++;
            }
    }
    if (nsel < 2 || nsel > room) {
        nsel = 1;
        sel_a[0] = ka;
        sel_b[0] = kb;
    }
    for (i = 0; i < nsel; i++) {
        unsigned ny0 = i ? (sel_b[i - 1] + sel_a[i] + 1u) / 2u : 0;
        unsigned ny1 = i + 1 < nsel ? (sel_b[i] + sel_a[i + 1]) / 2u : h;
        unsigned nx0 = 0, nx1 = w;
        VocrLine *o;
        if (flags & VOCR_REFINE_TIGHTEN) {
            unsigned hb = sel_b[i] - sel_a[i], mg = (R_MARGIN * hb + 50u) / 100u;
            unsigned t0 = sel_a[i] > mg ? sel_a[i] - mg : 0, t1 = sel_b[i] + mg;
            if (t0 > ny0) ny0 = t0;
            if (t1 < ny1) ny1 = t1;
        }
        if (flags & (VOCR_REFINE_CURSOR | VOCR_REFINE_PORTRAIT))
            trim_columns(bits, words, w, h, sel_a[i], sel_b[i], flags, col, &nx0, &nx1);
        o = &out->lines[out->count++];
        o->x = ln->x + nx0;
        o->y = ln->y + ny0;
        o->w = nx1 - nx0;
        o->h = ny1 - ny0;
        o->dark = ln->dark;
    }
    return VOCR_LINES_OK;
}

static int refine(const Src *src, unsigned flags, void *arena, size_t arena_bytes, VocrLineSet *set,
                  unsigned long *rows_read)
{
    unsigned i, W, H, bpp;
    unsigned long dummy = 0;
    unsigned char *base;
    size_t avail;
    VocrLineSet out;
    if (!rows_read) rows_read = &dummy;
    *rows_read = 0;
    if (!set || (flags & ~VOCR_REFINE_ALL)) return VOCR_LINES_E_ARG;
    if (!flags) return VOCR_LINES_OK;           /* bit-identical when off */
    W = src_width(src);
    H = src_height(src);
    bpp = src_bpp(src);
    if (!W || !H || (bpp != VOCR_LINES_GREY && bpp != VOCR_LINES_RGBA) || set->count > VOCR_LINES_MAX)
        return VOCR_LINES_E_ARG;
    for (i = 0; i < set->count; i++) {
        const VocrLine *ln = &set->lines[i];
        if (!ln->w || !ln->h || ln->x >= W || ln->y >= H || ln->w > W - ln->x || ln->h > H - ln->y)
            return VOCR_LINES_E_ARG;
    }
    if (!arena || arena_bytes < vocr_refine_arena_bytes(W, H)) return VOCR_LINES_E_ARENA;
    base = align4(arena);
    avail = arena_bytes - (size_t)(base - (unsigned char *)arena);
    out.count = 0;
    out.truncated = set->truncated;
    for (i = 0; i < set->count; i++) {
        int rc = refine_box(src, &set->lines[i], flags, base, avail,
                            VOCR_LINES_MAX - out.count - (set->count - i - 1u), &out, rows_read);
        if (rc) return rc;                      /* the set is untouched until here */
    }
    *set = out;
    return VOCR_LINES_OK;
}

int vocr_refine_lines(const VocrLinesImage *im, unsigned flags, void *arena, size_t arena_bytes,
                      VocrLineSet *set, unsigned long *rows_read)
{
    Src s;
    if (!im || !im->pixels || (im->bpp != VOCR_LINES_GREY && im->bpp != VOCR_LINES_RGBA) ||
        im->stride < (size_t)im->width * im->bpp)
        return VOCR_LINES_E_ARG;
    s.im = im;
    s.rows = NULL;
    return refine(&s, flags, arena, arena_bytes, set, rows_read);
}

int vocr_refine_lines_rows(const VocrLinesRows *src, unsigned flags, void *arena, size_t arena_bytes,
                           VocrLineSet *set, unsigned long *rows_read)
{
    Src s;
    if (!src || !src->row) return VOCR_LINES_E_ARG;
    s.im = NULL;
    s.rows = src;
    return refine(&s, flags, arena, arena_bytes, set, rows_read);
}
