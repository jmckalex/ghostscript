/* Copyright (C) 2026 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.
*/

/* SVG output device ("svg")

   Writes one standalone SVG document per page:

   - paths, strokes and clipping are written as native SVG, at full
     (fixed point) precision;
   - glyphs of outline fonts are defined once per page as <path> elements
     and drawn with <use>, and an invisible, selectable Unicode text layer
     is placed over the page so text can be selected, searched and copied;
   - anything SVG cannot express (exotic shadings and patterns, raster
     operations, transparency groups rendered by the compositor, ...) is
     rendered by the graphics library into an RGBA canvas at the device
     resolution, which is emitted as an embedded PNG image at the correct
     position in the painting order.

   Multi-page output requires a %d in OutputFile.  The device resolution
   only affects the raster fallback; vector content is resolution
   independent.

   Device parameters:
     -dSVGTextLayer=false     omit the invisible text layer
     -dSVGReuseGlyphs=false   write every glyph as a plain path
     -dSVGTransparency=false  let the pdf14 compositor flatten transparency

   User documentation: doc/src/VectorDevices.rst, "SVG output".
*/

#include "math_.h"
#include "memory_.h"
#include "string_.h"
#include "stdio_.h"
#include "gx.h"
#include "gserrors.h"
#include "gsparam.h"
#include "gsmatrix.h"
#include "gscoord.h"
#include "gssprintf.h"
#include "gxdevice.h"
#include "gdevvec.h"
#include "gxpath.h"
#include "gxpaint.h"
#include "gzpath.h"
#include "gzcpath.h"
#include "gxgstate.h"
#include "gxdcolor.h"
#include "gxfont.h"
#include "gxtext.h"
#include "gxchar.h"
#include "gscpm.h"
#include "gxdevsop.h"
#include "gxdevmem.h"
#include "gxiparam.h"
#include "gsiparam.h"
#include "gsiparm3.h"
#include "gsiparm4.h"
#include "gxcspace.h"
#include "gsptype2.h"
#include "gsshade.h"
#include "gsfunc.h"
#include "gxshade.h"
#include "gstrans.h"
#include "gxcomp.h"
#include "png_.h"
#include "doc_common.h"

#ifndef X_DPI
#  define X_DPI 192
#endif
#ifndef Y_DPI
#  define Y_DPI 192
#endif

/* Decimal places used for page coordinates (in points) and for glyph
   outlines (in units of 1/1000 em). */
#define SVG_PAGE_PREC 2
#define SVG_GLYPH_PREC 1

/* ---------------- Growable output buffers ---------------- */

typedef struct svg_buf_s {
    gs_memory_t *mem;           /* non-GC memory */
    char *data;
    size_t len, cap;
    int error;                  /* sticky allocation failure */
} svg_buf;

static void
sb_init(svg_buf *b, gs_memory_t *mem)
{
    b->mem = mem;
    b->data = NULL;
    b->len = b->cap = 0;
    b->error = 0;
}

static void
sb_free(svg_buf *b)
{
    if (b->data)
        gs_free_object(b->mem, b->data, "svg_buf");
    b->data = NULL;
    b->len = b->cap = 0;
}

static int
sb_reserve(svg_buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    size_t cap;
    char *data;

    if (b->error)
        return b->error;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 4096;
    while (cap < need)
        cap *= 2;
    data = (char *)gs_alloc_bytes(b->mem, cap, "svg_buf");
    if (data == NULL)
        return b->error = gs_note_error(gs_error_VMerror);
    if (b->len)
        memcpy(data, b->data, b->len);
    if (b->data)
        gs_free_object(b->mem, b->data, "svg_buf");
    b->data = data;
    b->cap = cap;
    return 0;
}

static void
sb_write(svg_buf *b, const void *p, size_t n)
{
    if (sb_reserve(b, n) < 0)
        return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void
sb_puts(svg_buf *b, const char *s)
{
    sb_write(b, s, strlen(s));
}

static void
sb_putc(svg_buf *b, char c)
{
    sb_write(b, &c, 1);
}

static void
sb_printf(svg_buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = gs_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    sb_write(b, tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}

/* ---------------- Number formatting ---------------- */

static const double svg_p10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };

/* Round v to `prec` decimal places, as an integer number of units. */
static int64_t
svg_units(double v, int prec)
{
    double s = v * svg_p10[prec];

    if (!(s == s))              /* NaN */
        return 0;
    if (s > 9e15)
        s = 9e15;
    if (s < -9e15)
        s = -9e15;
    return (int64_t)floor(s + 0.5);
}

/* Format an integer number of 10^-prec units, trimming trailing zeros
   and the leading zero of a pure fraction ("0.5" -> ".5"). */
static void
svg_fmt_units(char *out, int64_t u, int prec)
{
    char digits[32];
    char *p = out;
    int64_t ip, fp;
    int n, i;

    if (u < 0) {
        *p++ = '-';
        u = -u;
    }
    ip = u / (int64_t)svg_p10[prec];
    fp = u % (int64_t)svg_p10[prec];
    if (ip != 0 || fp == 0) {
        n = 0;
        do {
            digits[n++] = (char)('0' + ip % 10);
            ip /= 10;
        } while (ip);
        while (n)
            *p++ = digits[--n];
    }
    if (fp) {
        *p++ = '.';
        for (i = prec - 1; i >= 0; --i) {
            digits[i] = (char)('0' + fp % 10);
            fp /= 10;
        }
        n = prec;
        while (n > 0 && digits[n - 1] == '0')
            --n;
        memcpy(p, digits, n);
        p += n;
    }
    *p = 0;
}

static void
sb_num(svg_buf *b, double v, int prec)
{
    char tmp[40];

    svg_fmt_units(tmp, svg_units(v, prec), prec);
    sb_puts(b, tmp);
}

/* Format with about `sig` significant digits (for matrix coefficients). */
static void
sb_sig(svg_buf *b, double v, int sig)
{
    double a = fabs(v);
    int prec = sig;

    if (a >= 1)
        prec = sig - 1 - (int)floor(log10(a));
    if (prec < 0)
        prec = 0;
    if (prec > 6)
        prec = 6;
    sb_num(b, v, prec);
}

static void
sb_matrix(svg_buf *b, const gs_matrix *m)
{
    sb_puts(b, "matrix(");
    sb_sig(b, m->xx, 6); sb_putc(b, ' ');
    sb_sig(b, m->xy, 6); sb_putc(b, ' ');
    sb_sig(b, m->yx, 6); sb_putc(b, ' ');
    sb_sig(b, m->yy, 6); sb_putc(b, ' ');
    sb_num(b, m->tx, 3); sb_putc(b, ' ');
    sb_num(b, m->ty, 3); sb_putc(b, ')');
}

static void
sb_color(svg_buf *b, gx_color_index c)
{
    static const char hex[] = "0123456789abcdef";
    char s[8];
    int r = (int)(c >> 16) & 0xff, g = (int)(c >> 8) & 0xff, bl = (int)c & 0xff;

    s[0] = '#';
    if ((r >> 4) == (r & 15) && (g >> 4) == (g & 15) && (bl >> 4) == (bl & 15)) {
        s[1] = hex[r & 15]; s[2] = hex[g & 15]; s[3] = hex[bl & 15]; s[4] = 0;
    } else {
        s[1] = hex[r >> 4]; s[2] = hex[r & 15];
        s[3] = hex[g >> 4]; s[4] = hex[g & 15];
        s[5] = hex[bl >> 4]; s[6] = hex[bl & 15]; s[7] = 0;
    }
    sb_puts(b, s);
}

/* ---------------- Hashing and a small hash map ---------------- */

static uint64_t
svg_hash(uint64_t h, const void *p, size_t n)
{
    const byte *s = (const byte *)p;

    while (n--) {
        h ^= *s++;
        h *= (uint64_t)0x100000001b3ULL;
    }
    return h;
}
#define SVG_HASH_INIT ((uint64_t)0xcbf29ce484222325ULL)

typedef struct svg_map_entry_s {
    uint64_t key;               /* 0 = empty */
    int id;
    double scale;               /* glyphs: outline units per character unit */
    double advance;             /* glyphs: advance width in character units */
} svg_map_entry;

typedef struct svg_map_s {
    gs_memory_t *mem;
    svg_map_entry *e;
    uint size, count;           /* size is a power of two */
} svg_map;

static void
svg_map_init(svg_map *m, gs_memory_t *mem)
{
    m->mem = mem;
    m->e = NULL;
    m->size = m->count = 0;
}

static void
svg_map_clear(svg_map *m)
{
    if (m->e)
        gs_free_object(m->mem, m->e, "svg_map");
    m->e = NULL;
    m->size = m->count = 0;
}

static svg_map_entry *
svg_map_find(svg_map *m, uint64_t key)
{
    uint i;

    if (key == 0)
        key = 1;
    if (m->size == 0)
        return NULL;
    for (i = (uint)key & (m->size - 1); m->e[i].key != 0; i = (i + 1) & (m->size - 1))
        if (m->e[i].key == key)
            return &m->e[i];
    return NULL;
}

/* Insert a new key (which must not be present). */
static svg_map_entry *
svg_map_add(svg_map *m, uint64_t key)
{
    uint i;

    if (key == 0)
        key = 1;
    if ((m->count + 1) * 2 > m->size) {
        uint nsize = m->size ? m->size * 2 : 256;
        svg_map_entry *ne = (svg_map_entry *)gs_alloc_bytes(m->mem,
                                     (size_t)nsize * sizeof(svg_map_entry), "svg_map");

        if (ne == NULL)
            return NULL;
        memset(ne, 0, (size_t)nsize * sizeof(svg_map_entry));
        for (i = 0; i < m->size; ++i)
            if (m->e[i].key) {
                uint j = (uint)m->e[i].key & (nsize - 1);

                while (ne[j].key)
                    j = (j + 1) & (nsize - 1);
                ne[j] = m->e[i];
            }
        if (m->e)
            gs_free_object(m->mem, m->e, "svg_map");
        m->e = ne;
        m->size = nsize;
    }
    for (i = (uint)key & (m->size - 1); m->e[i].key != 0; i = (i + 1) & (m->size - 1))
        ;
    m->e[i].key = key;
    m->count++;
    return &m->e[i];
}

/* ---------------- Device definition ---------------- */

/* Text layer font hints, from the font name. */
#define SVG_TEXT_SERIF 1
#define SVG_TEXT_MONO 2
#define SVG_TEXT_BOLD 4
#define SVG_TEXT_ITALIC 8

/* A run of glyphs on one baseline, for the text layer. */
typedef struct svg_text_run_s {
    bool active;
    gs_matrix m;                /* run space (1 unit = 1pt, y down) -> page */
    double size;                /* em size in points */
    bool upright;               /* m is a pure translation */
    int style;                  /* SVG_TEXT_* */
    double end;                 /* run-space x where the last glyph ends */
    bool last_space;
    svg_buf xs;                 /* x positions */
    svg_buf chars;              /* escaped UTF-8 text */
} svg_text_run;

/* A transparency group or soft mask level.  Level 0 is the page. */
#define SVG_MAX_LEVELS 64
typedef struct svg_level_s {
    svg_buf *parent_out;        /* output to restore when this level ends */
    bool is_mask;
    bool wrote_g;               /* a group that needed a <g> */
    bool alpha_mask;            /* mask: alpha rather than luminosity */
    double backdrop;            /* mask: luminosity of the backdrop */
    bool has_tr;                /* mask: transfer function (TR) */
    byte tr[256];               /* mask: TR sampled at 0, 1/255, ..., 1 */
    svg_buf content;            /* mask: its content */
    int active_mask;            /* mask for the next group begun here */
} svg_level;

typedef struct gx_device_svg_s {
    gx_device_vector_common;
    /* Parameters */
    bool TextLayer;
    bool ReuseGlyphs;
    /* Page content */
    svg_buf defs;               /* glyph outlines */
    svg_buf body;               /* painted content, in order */
    svg_buf text;               /* invisible text layer */
    int next_id;
    int pages_written;
    bool warned_multipage;
    /* Clipping: the clip group currently open in body */
    bool clip_open;
    gs_id clip_id;
    svg_map clips;              /* clip path content hash -> def id */
    svg_map glyphs;             /* (font, glyph) -> def id */
    /* Text */
    gs_text_enum_t *text_enum;  /* the enumerator whose glyphs we track */
    bool text_invisible;        /* rendering mode 3: text layer only */
    bool painting_glyph;        /* filling a glyph outline ourselves */
    bool pixelated;             /* page has unsmoothed (class "px") images */
    svg_text_run run;
    /* Transparency: nested groups and soft masks being written */
    bool Transparency;
    svg_buf *out;               /* where painting goes: body or a mask */
    svg_level levels[SVG_MAX_LEVELS];
    int depth;
    int mask_depth;             /* > 0 while constructing a soft mask */
    /* Raster fallback canvas (RGBA, device resolution) */
    byte *canvas;
    int cx0, cy0, cx1, cy1;     /* dirty box, exclusive max; empty if cx0 >= cx1 */
} gx_device_svg;

gs_public_st_suffix_add1_final(st_device_svg, gx_device_svg,
    "gx_device_svg", device_svg_enum_ptrs, device_svg_reloc_ptrs,
    gx_device_finalize, st_device_vector, text_enum);

#define svg_device_body(dname, init)\
  std_device_dci_type_body(gx_device_svg, init, dname, &st_device_svg, \
                           DEFAULT_WIDTH_10THS * X_DPI / 10, \
                           DEFAULT_HEIGHT_10THS * Y_DPI / 10, \
                           X_DPI, Y_DPI, 3, 24, 255, 255, 256, 256)

static dev_proc_initialize_device_procs(svg_initialize_device_procs);
static dev_proc_open_device(svg_open_device);
static dev_proc_output_page(svg_output_page);
static dev_proc_close_device(svg_close_device);
static dev_proc_get_params(svg_get_params);
static dev_proc_put_params(svg_put_params);
static dev_proc_fill_rectangle(svg_fill_rectangle);
static dev_proc_copy_mono(svg_copy_mono);
static dev_proc_copy_color(svg_copy_color);
static dev_proc_copy_alpha(svg_copy_alpha);
static dev_proc_fill_mask(svg_fill_mask);
static dev_proc_fill_path(svg_fill_path);
#define SVG_PAINT_FILL 0
#define SVG_PAINT_STROKE 1
#define SVG_PAINT_ELEMENT 2
static void svg_put_transparency(gx_device_svg *svg, const gs_gstate *pgs, int what);
static const char *svg_blend_name(gs_blend_mode_t bm);
static int svg_glyph_draw(gx_device_svg *svg, const gs_rect *dev_box, bool bitmap,
                          int rule, const gx_drawing_color *pdcolor,
                          const gx_clip_path *pcpath);
static dev_proc_fill_path(svg_fill_path);
static dev_proc_stroke_path(svg_stroke_path);
static dev_proc_fill_rectangle_hl_color(svg_fill_rectangle_hl_color);
static dev_proc_text_begin(svg_text_begin);
static dev_proc_fillpage(svg_fillpage);
static dev_proc_dev_spec_op(svg_dev_spec_op);
static dev_proc_begin_typed_image(svg_begin_typed_image);
static dev_proc_composite(svg_composite);

const gx_device_svg gs_svg_device = {
    svg_device_body("svg", svg_initialize_device_procs),
};

static int
svg_initialize_device(gx_device *dev)
{
    gx_device_svg *svg = (gx_device_svg *)dev;

    svg->TextLayer = true;
    svg->ReuseGlyphs = true;
    svg->Transparency = true;
    return 0;
}

static void
svg_initialize_device_procs(gx_device *dev)
{
    set_dev_proc(dev, initialize_device, svg_initialize_device);
    set_dev_proc(dev, open_device, svg_open_device);
    set_dev_proc(dev, output_page, svg_output_page);
    set_dev_proc(dev, close_device, svg_close_device);
    set_dev_proc(dev, map_rgb_color, gx_default_rgb_map_rgb_color);
    set_dev_proc(dev, map_color_rgb, gx_default_rgb_map_color_rgb);
    set_dev_proc(dev, encode_color, gx_default_rgb_map_rgb_color);
    set_dev_proc(dev, decode_color, gx_default_rgb_map_color_rgb);
    set_dev_proc(dev, fill_rectangle, svg_fill_rectangle);
    set_dev_proc(dev, copy_mono, svg_copy_mono);
    set_dev_proc(dev, copy_color, svg_copy_color);
    set_dev_proc(dev, copy_alpha, svg_copy_alpha);
    set_dev_proc(dev, fill_mask, svg_fill_mask);
    set_dev_proc(dev, get_params, svg_get_params);
    set_dev_proc(dev, put_params, svg_put_params);
    set_dev_proc(dev, get_page_device, gx_page_device_get_page_device);
    set_dev_proc(dev, fill_path, svg_fill_path);
    set_dev_proc(dev, stroke_path, svg_stroke_path);
    set_dev_proc(dev, fill_rectangle_hl_color, svg_fill_rectangle_hl_color);
    set_dev_proc(dev, text_begin, svg_text_begin);
    set_dev_proc(dev, fillpage, svg_fillpage);
    set_dev_proc(dev, dev_spec_op, svg_dev_spec_op);
    set_dev_proc(dev, begin_typed_image, svg_begin_typed_image);
    set_dev_proc(dev, composite, svg_composite);
}

/* The vector procedures are required by the vector device superclass but
   this device emits its output directly from the high-level procedures. */
static int svgv_beginpage(gx_device_vector *v) { return 0; }
static int svgv_setlinewidth(gx_device_vector *v, double w) { return 0; }
static int svgv_setlinecap(gx_device_vector *v, gs_line_cap c) { return 0; }
static int svgv_setlinejoin(gx_device_vector *v, gs_line_join j) { return 0; }
static int svgv_setmiterlimit(gx_device_vector *v, double l) { return 0; }
static int svgv_setdash(gx_device_vector *v, const float *p, uint n, double o) { return 0; }
static int svgv_setlogop(gx_device_vector *v, gs_logical_operation_t l,
                         gs_logical_operation_t d) { return 0; }
static bool svgv_can_handle_hl_color(gx_device_vector *v, const gs_gstate *pgs,
                                     const gx_drawing_color *pdc) { return false; }
static int svgv_setcolor(gx_device_vector *v, const gs_gstate *pgs,
                         const gx_drawing_color *pdc) { return 0; }
static int svgv_beginpath(gx_device_vector *v, gx_path_type_t t) { return 0; }
static int svgv_moveto(gx_device_vector *v, double x0, double y0, double x, double y,
                       gx_path_type_t t) { return 0; }
static int svgv_curveto(gx_device_vector *v, double x0, double y0, double x1, double y1,
                        double x2, double y2, double x3, double y3,
                        gx_path_type_t t) { return 0; }
static int svgv_closepath(gx_device_vector *v, double x0, double y0, double xs, double ys,
                          gx_path_type_t t) { return 0; }
static int svgv_endpath(gx_device_vector *v, gx_path_type_t t) { return 0; }

static const gx_device_vector_procs svg_vector_procs = {
    svgv_beginpage, svgv_setlinewidth, svgv_setlinecap, svgv_setlinejoin,
    svgv_setmiterlimit, svgv_setdash, gdev_vector_setflat, svgv_setlogop,
    svgv_can_handle_hl_color, svgv_setcolor, svgv_setcolor,
    gdev_vector_dopath, gdev_vector_dorect, svgv_beginpath, svgv_moveto,
    svgv_moveto, svgv_curveto, svgv_closepath, svgv_endpath
};

static gs_memory_t *
svg_mem(const gx_device_svg *svg)
{
    return svg->memory->non_gc_memory;
}

/* Scale from device pixels to points. */
static double svg_sx(const gx_device_svg *svg) { return 72.0 / svg->HWResolution[0]; }
static double svg_sy(const gx_device_svg *svg) { return 72.0 / svg->HWResolution[1]; }

/* ---------------- Path data ---------------- */

/* Writes path data with relative commands.  Points are computed from the
   path's fixed coordinates as (x, y) * m, then rounded to `prec` decimals;
   deltas are taken between rounded values so no error accumulates. */
typedef struct svg_pathw_s {
    svg_buf *b;
    int prec;
    int64_t cx, cy;             /* current point, in output units */
    int64_t sx, sy;             /* subpath start */
    char last;                  /* last command letter */
    bool need_sep;
} svg_pathw;

static void
pw_num(svg_pathw *w, int64_t u)
{
    char tmp[40];

    svg_fmt_units(tmp, u, w->prec);
    if (w->need_sep && tmp[0] != '-')
        sb_putc(w->b, ' ');
    sb_puts(w->b, tmp);
    w->need_sep = true;
}

static void
pw_cmd(svg_pathw *w, char c)
{
    if (c != w->last || c == 'm') {
        sb_putc(w->b, c);
        w->need_sep = false;
    }
    /* After an "m", implicit repeats mean "l". */
    w->last = (c == 'm' ? 'l' : c);
}

static void
pw_point(const gs_fixed_point *p, const gs_matrix *m, int prec, int64_t *x, int64_t *y)
{
    double px = fixed2float(p->x), py = fixed2float(p->y);

    *x = svg_units(px * m->xx + py * m->yx + m->tx, prec);
    *y = svg_units(px * m->xy + py * m->yy + m->ty, prec);
}

static void
pw_flush_move(svg_pathw *w, bool *pending, int64_t x, int64_t y)
{
    if (!*pending)
        return;
    *pending = false;
    pw_cmd(w, 'm');
    pw_num(w, x - w->cx);
    pw_num(w, y - w->cy);
    w->cx = w->sx = x;
    w->cy = w->sy = y;
}

/* Append the path's data.  gaps_as_moves: gapto segments become moves
   (for stroking) instead of lines (for filling). */
static void
svg_put_path(svg_buf *b, const gx_path *ppath, const gs_matrix *m, int prec,
             bool gaps_as_moves)
{
    svg_pathw w;
    gs_path_enum penum;
    gs_fixed_point pts[3];
    int64_t x[3], y[3], mx = 0, my = 0;
    int op, i;
    bool pending = false;

    w.b = b;
    w.prec = prec;
    w.cx = w.cy = w.sx = w.sy = 0;
    w.last = 0;
    w.need_sep = false;
    gx_path_enum_init(&penum, ppath);
    while ((op = gx_path_enum_next(&penum, pts)) != 0) {
        switch (op) {
        case gs_pe_gapto:
            if (!gaps_as_moves) {
                if (pending)
                    break;
                goto line;
            }
            /* fall through */
        case gs_pe_moveto:
            /* Written lazily: a move with nothing drawn from it paints
               nothing (glyph outlines often start with one). */
            pw_point(&pts[0], m, prec, &mx, &my);
            pending = true;
            break;
        case gs_pe_lineto:
        line:
            pw_flush_move(&w, &pending, mx, my);
            pw_point(&pts[0], m, prec, &x[0], &y[0]);
            if (y[0] == w.cy && x[0] != w.cx) {
                pw_cmd(&w, 'h');
                pw_num(&w, x[0] - w.cx);
            } else if (x[0] == w.cx && y[0] != w.cy) {
                pw_cmd(&w, 'v');
                pw_num(&w, y[0] - w.cy);
            } else {
                pw_cmd(&w, 'l');
                pw_num(&w, x[0] - w.cx);
                pw_num(&w, y[0] - w.cy);
            }
            w.cx = x[0];
            w.cy = y[0];
            break;
        case gs_pe_curveto:
            pw_flush_move(&w, &pending, mx, my);
            for (i = 0; i < 3; ++i)
                pw_point(&pts[i], m, prec, &x[i], &y[i]);
            pw_cmd(&w, 'c');
            for (i = 0; i < 3; ++i) {
                pw_num(&w, x[i] - w.cx);
                pw_num(&w, y[i] - w.cy);
            }
            w.cx = x[2];
            w.cy = y[2];
            break;
        case gs_pe_closepath:
            if (pending)
                break;
            sb_putc(b, 'z');
            w.last = 'z';
            w.need_sep = false;
            w.cx = w.sx;
            w.cy = w.sy;
            break;
        }
    }
}

/* Device space (fixed path coordinates) to page points. */
static void
svg_page_matrix(const gx_device_svg *svg, gs_matrix *m)
{
    gs_make_scaling(svg_sx(svg), svg_sy(svg), m);
}

/* Bounding box of the control points of ppath transformed by m. */
static bool
svg_path_bbox(const gx_path *ppath, const gs_matrix *m, gs_rect *r)
{
    gs_path_enum penum;
    gs_fixed_point pts[3];
    int op, i, n;
    bool any = false;
    gs_fixed_point move = { 0, 0 }, q[4];
    bool pending = false;

    gx_path_enum_init(&penum, ppath);
    while ((op = gx_path_enum_next(&penum, pts)) != 0) {
        if (op == gs_pe_moveto || op == gs_pe_gapto) {
            move = pts[0];
            pending = true;
            continue;
        }
        if (op == gs_pe_closepath)
            continue;
        n = 0;
        if (pending) {
            q[n++] = move;
            pending = false;
        }
        for (i = 0; i < (op == gs_pe_curveto ? 3 : 1); ++i)
            q[n++] = pts[i];
        for (i = 0; i < n; ++i) {
            double px = fixed2float(q[i].x), py = fixed2float(q[i].y);
            double x = px * m->xx + py * m->yx + m->tx;
            double y = px * m->xy + py * m->yy + m->ty;

            if (!any) {
                r->p.x = r->q.x = x;
                r->p.y = r->q.y = y;
                any = true;
            } else {
                if (x < r->p.x) r->p.x = x;
                if (x > r->q.x) r->q.x = x;
                if (y < r->p.y) r->p.y = y;
                if (y > r->q.y) r->q.y = y;
            }
        }
    }
    return any;
}

/* ---------------- Raster fallback canvas ---------------- */

static int
svg_canvas_ensure(gx_device_svg *svg)
{
    size_t n;

    if (svg->canvas)
        return 0;
    n = (size_t)svg->width * svg->height * 4;
    svg->canvas = gs_alloc_bytes(svg_mem(svg), n, "svg canvas");
    if (svg->canvas == NULL)
        return_error(gs_error_VMerror);
    memset(svg->canvas, 0, n);
    svg->cx0 = svg->cy0 = svg->cx1 = svg->cy1 = 0;
    return 0;
}

/* Clip a rectangle to the canvas and mark it dirty.  Returns 1 if there is
   something to draw, 0 if the rectangle is empty, < 0 on error. */
static int
svg_canvas_rect(gx_device_svg *svg, int *x, int *y, int *w, int *h)
{
    int code;

    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > svg->width) *w = svg->width - *x;
    if (*y + *h > svg->height) *h = svg->height - *y;
    if (*w <= 0 || *h <= 0)
        return 0;
    if ((code = svg_canvas_ensure(svg)) < 0)
        return code;
    if (svg->cx0 >= svg->cx1) {
        svg->cx0 = *x; svg->cy0 = *y;
        svg->cx1 = *x + *w; svg->cy1 = *y + *h;
    } else {
        if (*x < svg->cx0) svg->cx0 = *x;
        if (*y < svg->cy0) svg->cy0 = *y;
        if (*x + *w > svg->cx1) svg->cx1 = *x + *w;
        if (*y + *h > svg->cy1) svg->cy1 = *y + *h;
    }
    return 1;
}

static void
svg_put_pixel(byte *p, gx_color_index c)
{
    p[0] = (byte)(c >> 16);
    p[1] = (byte)(c >> 8);
    p[2] = (byte)c;
    p[3] = 255;
}

static int
svg_fill_rectangle(gx_device *dev, int x, int y, int w, int h, gx_color_index color)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    int i, j, code;

    if (color == gx_no_color_index || svg->text_invisible)
        return 0;
    if ((code = svg_canvas_rect(svg, &x, &y, &w, &h)) <= 0)
        return code;
    for (j = 0; j < h; ++j) {
        byte *p = svg->canvas + ((size_t)(y + j) * svg->width + x) * 4;

        for (i = 0; i < w; ++i, p += 4)
            svg_put_pixel(p, color);
    }
    return 0;
}

static int
svg_copy_mono(gx_device *dev, const byte *data, int dx, int raster, gx_bitmap_id id,
              int x, int y, int w, int h, gx_color_index zero, gx_color_index one)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    int x0 = x, y0 = y, i, j, code;

    if (svg->text_invisible)
        return 0;
    if ((code = svg_canvas_rect(svg, &x, &y, &w, &h)) <= 0)
        return code;
    dx += x - x0;
    data += (y - y0) * raster;
    for (j = 0; j < h; ++j, data += raster) {
        byte *p = svg->canvas + ((size_t)(y + j) * svg->width + x) * 4;

        for (i = 0; i < w; ++i, p += 4) {
            int bit = (data[(dx + i) >> 3] >> (7 - ((dx + i) & 7))) & 1;
            gx_color_index c = bit ? one : zero;

            if (c != gx_no_color_index)
                svg_put_pixel(p, c);
        }
    }
    return 0;
}

static int
svg_copy_color(gx_device *dev, const byte *data, int data_x, int raster, gx_bitmap_id id,
               int x, int y, int w, int h)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    int x0 = x, y0 = y, i, j, code;

    if (svg->text_invisible)
        return 0;
    if ((code = svg_canvas_rect(svg, &x, &y, &w, &h)) <= 0)
        return code;
    data += (y - y0) * raster + (data_x + x - x0) * 3;
    for (j = 0; j < h; ++j, data += raster) {
        byte *p = svg->canvas + ((size_t)(y + j) * svg->width + x) * 4;
        const byte *s = data;

        for (i = 0; i < w; ++i, p += 4, s += 3) {
            p[0] = s[0]; p[1] = s[1]; p[2] = s[2]; p[3] = 255;
        }
    }
    return 0;
}

static int
svg_fill_mask(gx_device *dev, const byte *data, int data_x, int raster, gx_bitmap_id id,
              int x, int y, int w, int h, const gx_drawing_color *pdcolor, int depth,
              gs_logical_operation_t lop, const gx_clip_path *pcpath)
{
    gx_device_svg *svg = (gx_device_svg *)dev;

    if (svg->text_enum && !svg->painting_glyph && depth == 1 && w > 0 && h > 0) {
        gs_rect box;
        int code;

        box.p.x = x; box.p.y = y;
        box.q.x = x + w; box.q.y = y + h;
        code = svg_glyph_draw(svg, &box, true, gx_rule_winding_number, pdcolor, pcpath);
        if (code != 0)
            return code < 0 ? code : 0;
    }
    if (svg->text_invisible)
        return 0;
    return gx_default_fill_mask(dev, data, data_x, raster, id, x, y, w, h,
                                pdcolor, depth, lop, pcpath);
}

static int
svg_copy_alpha(gx_device *dev, const byte *data, int data_x, int raster, gx_bitmap_id id,
               int x, int y, int w, int h, gx_color_index color, int depth)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    int x0 = x, y0 = y, i, j, code;
    int maxv = (1 << depth) - 1;
    int cr = (int)(color >> 16) & 0xff, cg = (int)(color >> 8) & 0xff, cb = (int)color & 0xff;

    if (color == gx_no_color_index || svg->text_invisible)
        return 0;
    if ((code = svg_canvas_rect(svg, &x, &y, &w, &h)) <= 0)
        return code;
    data_x += x - x0;
    data += (y - y0) * raster;
    for (j = 0; j < h; ++j, data += raster) {
        byte *p = svg->canvas + ((size_t)(y + j) * svg->width + x) * 4;

        for (i = 0; i < w; ++i, p += 4) {
            int bit = (data_x + i) * depth;
            int v = (data[bit >> 3] >> (8 - depth - (bit & 7))) & maxv;
            int a = v * 255 / maxv, da, oa;

            if (a == 0)
                continue;
            /* Composite "over" the canvas pixel (straight alpha). */
            da = p[3] * (255 - a) / 255;
            oa = a + da;
            p[0] = (byte)((cr * a + p[0] * da) / oa);
            p[1] = (byte)((cg * a + p[1] * da) / oa);
            p[2] = (byte)((cb * a + p[2] * da) / oa);
            p[3] = (byte)oa;
        }
    }
    return 0;
}

/* ---------------- PNG / base64 ---------------- */

static void
svg_png_write(png_structp png, png_bytep data, png_size_t len)
{
    sb_write((svg_buf *)png_get_io_ptr(png), data, len);
}

static void
svg_png_flush(png_structp png)
{
}

/* Encode w x h pixels (RGBA, or RGB if !alpha; rows `stride` bytes apart,
   `bpp` bytes per source pixel) as PNG appended to out. */
static int
svg_encode_png(svg_buf *out, const byte *pix, int w, int h, int stride, int bpp, bool alpha)
{
    png_structp png;
    png_infop info;
    byte *row = NULL;
    int y, x, n = alpha ? 4 : 3;

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL)
        return_error(gs_error_VMerror);
    info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_write_struct(&png, NULL);
        return_error(gs_error_VMerror);
    }
    row = gs_alloc_bytes(out->mem, (size_t)w * n, "svg png row");
    if (row == NULL || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        if (row)
            gs_free_object(out->mem, row, "svg png row");
        return_error(gs_error_ioerror);
    }
    png_set_write_fn(png, out, svg_png_write, svg_png_flush);
    png_set_IHDR(png, info, w, h, 8, alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (y = 0; y < h; ++y) {
        const byte *s = pix + (size_t)y * stride;

        if (bpp == n)
            png_write_row(png, (png_bytep)s);
        else {
            for (x = 0; x < w; ++x)
                memcpy(row + x * n, s + x * bpp, n);
            png_write_row(png, row);
        }
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    gs_free_object(out->mem, row, "svg png row");
    return out->error;
}

static void
sb_base64(svg_buf *b, const byte *p, size_t n)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char q[4];

    if (sb_reserve(b, (n + 2) / 3 * 4) < 0)
        return;
    for (; n >= 3; n -= 3, p += 3) {
        q[0] = tab[p[0] >> 2];
        q[1] = tab[((p[0] & 3) << 4) | (p[1] >> 4)];
        q[2] = tab[((p[1] & 15) << 2) | (p[2] >> 6)];
        q[3] = tab[p[2] & 63];
        sb_write(b, q, 4);
    }
    if (n) {
        q[0] = tab[p[0] >> 2];
        q[1] = tab[((p[0] & 3) << 4) | (n > 1 ? p[1] >> 4 : 0)];
        q[2] = n > 1 ? tab[(p[1] & 15) << 2] : '=';
        q[3] = '=';
        sb_write(b, q, 4);
    }
}

/* Append an <image> for the given pixels, placed by `m` (image space of
   the unit square, as in PostScript, to page space: here simply the
   pixel rectangle). */
static int
svg_put_png_image(gx_device_svg *svg, svg_buf *b, const byte *pix, int w, int h,
                  int stride, int bpp, bool alpha, double x, double y,
                  double width, double height, const gs_gstate *pgs)
{
    svg_buf png;
    int code;

    sb_init(&png, svg_mem(svg));
    code = svg_encode_png(&png, pix, w, h, stride, bpp, alpha);
    if (code >= 0) {
        sb_puts(b, "<image x=\"");
        sb_num(b, x, 3);
        sb_puts(b, "\" y=\"");
        sb_num(b, y, 3);
        sb_puts(b, "\" width=\"");
        sb_num(b, width, 3);
        sb_puts(b, "\" height=\"");
        sb_num(b, height, 3);
        sb_puts(b, "\" preserveAspectRatio=\"none\"");
        if (pgs)
            svg_put_transparency(svg, pgs, SVG_PAINT_ELEMENT);
        sb_puts(b, " xlink:href=\"data:image/png;base64,");
        sb_base64(b, (const byte *)png.data, png.len);
        sb_puts(b, "\"/>\n");
    }
    sb_free(&png);
    return code;
}

/* ---------------- Clipping ---------------- */

static void
svg_close_clip(gx_device_svg *svg)
{
    if (svg->clip_open) {
        sb_puts(svg->out, "</g>\n");
        svg->clip_open = false;
    }
}

/* Find or write a <clipPath> for one path, intersected with clip `prev`
   (0 = none).  Returns the def id, or < 0 on error. */
static int
svg_clip_def(gx_device_svg *svg, const gx_path *ppath, const gx_clip_rect *rects,
             int rule, int prev)
{
    svg_buf d;
    gs_matrix m;
    uint64_t h;
    svg_map_entry *e;
    int id;

    sb_init(&d, svg_mem(svg));
    svg_page_matrix(svg, &m);
    if (ppath)
        svg_put_path(&d, ppath, &m, SVG_PAGE_PREC, false);
    else {
        for (; rects; rects = rects->next) {
            if (rects->xmax <= rects->xmin || rects->ymax <= rects->ymin)
                continue;
            sb_putc(&d, 'M');
            sb_num(&d, rects->xmin * m.xx, SVG_PAGE_PREC); sb_putc(&d, ' ');
            sb_num(&d, rects->ymin * m.yy, SVG_PAGE_PREC);
            sb_putc(&d, 'H');
            sb_num(&d, rects->xmax * m.xx, SVG_PAGE_PREC);
            sb_putc(&d, 'V');
            sb_num(&d, rects->ymax * m.yy, SVG_PAGE_PREC);
            sb_putc(&d, 'H');
            sb_num(&d, rects->xmin * m.xx, SVG_PAGE_PREC);
            sb_putc(&d, 'Z');
        }
    }
    if (d.error) {
        id = d.error;
        goto done;
    }
    h = svg_hash(SVG_HASH_INIT, d.data ? d.data : "", d.len);
    h = svg_hash(h, &rule, sizeof(rule));
    h = svg_hash(h, &prev, sizeof(prev));
    e = svg_map_find(&svg->clips, h);
    if (e) {
        id = e->id;
        goto done;
    }
    id = ++svg->next_id;
    e = svg_map_add(&svg->clips, h);
    if (e == NULL) {
        id = gs_note_error(gs_error_VMerror);
        goto done;
    }
    e->id = id;
    sb_printf(&svg->defs, "<clipPath id=\"c%d\"", id);
    if (prev)
        sb_printf(&svg->defs, " clip-path=\"url(#c%d)\"", prev);
    sb_puts(&svg->defs, "><path d=\"");
    if (d.data)
        sb_puts(&svg->defs, d.data);
    sb_puts(&svg->defs, rule > 0 ? "\" clip-rule=\"evenodd\"/></clipPath>\n" : "\"/></clipPath>\n");
done:
    sb_free(&d);
    return id;
}

static int
svg_set_clip(gx_device_svg *svg, const gx_clip_path *pcpath)
{
    int id = 0;

    if (pcpath && gx_cpath_includes_rectangle(pcpath, fixed_0, fixed_0,
                                              int2fixed(svg->width), int2fixed(svg->height)))
        pcpath = NULL;
    if (pcpath == NULL) {
        svg_close_clip(svg);
        return 0;
    }
    if (svg->clip_open && svg->clip_id == pcpath->id)
        return 0;
    svg_close_clip(svg);
    if (pcpath->path_valid)
        id = svg_clip_def(svg, &pcpath->path, NULL, pcpath->rule, 0);
    else if (pcpath->path_list) {
        const gx_cpath_path_list *e;

        for (e = pcpath->path_list; e && id >= 0; e = e->next)
            id = svg_clip_def(svg, &e->path, NULL, e->rule, id);
    } else {
        const gx_clip_list *list = gx_cpath_list(pcpath);
        const gx_clip_rect *r = list->head ? list->head : &list->single;

        id = svg_clip_def(svg, NULL, r, -1, 0);
    }
    if (id < 0)
        return id;
    sb_printf(svg->out, "<g clip-path=\"url(#c%d)\">\n", id);
    svg->clip_open = true;
    svg->clip_id = pcpath->id;
    return 0;
}

/* Emit any pending raster fallback content (unclipped: it was clipped
   when it was rendered). */
static int
svg_flush_canvas_painted(gx_device_svg *svg, const gs_gstate *pgs)
{
    int x0 = svg->cx0, y0 = svg->cy0, w = svg->cx1 - svg->cx0, h = svg->cy1 - svg->cy0;
    int x, y, code;
    bool alpha = false;
    byte *base;

    if (svg->canvas == NULL || w <= 0 || h <= 0)
        return 0;
    base = svg->canvas + ((size_t)y0 * svg->width + x0) * 4;
    for (y = 0; y < h && !alpha; ++y) {
        const byte *p = base + (size_t)y * svg->width * 4;

        for (x = 0; x < w; ++x)
            if (p[x * 4 + 3] != 255) {
                alpha = true;
                break;
            }
    }
    svg_close_clip(svg);
    code = svg_put_png_image(svg, svg->out, base, w, h, svg->width * 4, 4, alpha,
                             x0 * svg_sx(svg), y0 * svg_sy(svg),
                             w * svg_sx(svg), h * svg_sy(svg), pgs);
    for (y = 0; y < h; ++y)
        memset(base + (size_t)y * svg->width * 4, 0, (size_t)w * 4);
    svg->cx0 = svg->cy0 = svg->cx1 = svg->cy1 = 0;
    return code;
}

static int
svg_flush_canvas(gx_device_svg *svg)
{
    return svg_flush_canvas_painted(svg, NULL);
}

/* Prepare to append a painted element to the body. */
static int
svg_begin_element(gx_device_svg *svg, const gx_clip_path *pcpath)
{
    int code = svg_flush_canvas(svg);

    if (code < 0)
        return code;
    return svg_set_clip(svg, pcpath);
}

/* ---------------- Text layer ---------------- */

static void
svg_flush_run(gx_device_svg *svg)
{
    svg_text_run *r = &svg->run;

    if (!r->active)
        return;
    r->active = false;
    if (r->chars.len == 0)
        return;
    if (r->upright) {
        sb_puts(&svg->text, "<text x=\"");
        if (r->xs.data)
            sb_puts(&svg->text, r->xs.data);
        sb_puts(&svg->text, "\" y=\"");
        sb_num(&svg->text, r->m.ty, 3);
        sb_puts(&svg->text, "\" font-size=\"");
    } else {
        sb_puts(&svg->text, "<text transform=\"");
        sb_matrix(&svg->text, &r->m);
        sb_puts(&svg->text, "\" x=\"");
        if (r->xs.data)
            sb_puts(&svg->text, r->xs.data);
        sb_puts(&svg->text, "\" font-size=\"");
    }
    sb_num(&svg->text, r->size, 3);
    sb_putc(&svg->text, '"');
    if (r->style & SVG_TEXT_SERIF)
        sb_puts(&svg->text, " font-family=\"serif\"");
    if (r->style & SVG_TEXT_MONO)
        sb_puts(&svg->text, " font-family=\"monospace\"");
    if (r->style & SVG_TEXT_BOLD)
        sb_puts(&svg->text, " font-weight=\"bold\"");
    if (r->style & SVG_TEXT_ITALIC)
        sb_puts(&svg->text, " font-style=\"italic\"");
    sb_putc(&svg->text, '>');
    if (r->chars.data)
        sb_puts(&svg->text, r->chars.data);
    sb_puts(&svg->text, "</text>\n");
}

static void
svg_put_utf8(svg_buf *b, uint c)
{
    char s[8];
    int n = 0;

    switch (c) {
    case '&': sb_puts(b, "&amp;"); return;
    case '<': sb_puts(b, "&lt;"); return;
    case '>': sb_puts(b, "&gt;"); return;
    }
    if (c < 0x20 || (c >= 0xd800 && c < 0xe000) || c == 0xfffe || c == 0xffff || c > 0x10ffff)
        return;
    if (c < 0x80)
        s[n++] = (char)c;
    else if (c < 0x800) {
        s[n++] = (char)(0xc0 | (c >> 6));
        s[n++] = (char)(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        s[n++] = (char)(0xe0 | (c >> 12));
        s[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
        s[n++] = (char)(0x80 | (c & 0x3f));
    } else {
        s[n++] = (char)(0xf0 | (c >> 18));
        s[n++] = (char)(0x80 | ((c >> 12) & 0x3f));
        s[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
        s[n++] = (char)(0x80 | (c & 0x3f));
    }
    sb_write(b, s, n);
}

/* Add one character at run-space x to the current run. */
static void
svg_run_char(svg_text_run *r, uint c, double x)
{
    if (r->xs.len)
        sb_putc(&r->xs, ' ');
    sb_num(&r->xs, r->upright ? x + r->m.tx : x, 2);
    svg_put_utf8(&r->chars, c);
    r->last_space = (c == ' ');
}

/* Record a glyph for the text layer.  em: em space (1 unit = 1 em,
   y up) to page space; advance: in ems. */
/* Guess a CSS family, weight and style from a PostScript font name, so
   the browser's selection boxes roughly fit the real glyphs. */
static bool
svg_name_has(const char *name, const char *const *words)
{
    for (; *words; ++words) {
        const char *p;
        size_t n = strlen(*words);

        for (p = name; *p; ++p)
            if (!strncmp(p, *words, n))
                return true;
    }
    return false;
}

static int
svg_font_style(const gs_font *font)
{
    static const char *const mono[] = { "Courier", "Mono", "Consol", "Menlo", "Typewriter",
                                        "Code", "LetterGothic", NULL };
    static const char *const serif[] = { "Times", "Roman", "Serif", "Palatino", "P052",
                                         "Georgia", "Garamond", "Bookman", "Book", "Schoolbook",
                                         "Century", "C059", "Cambria", "Minion", "Caslon",
                                         "Baskerville", "Didot", "Bodoni", "Charter", "Utopia",
                                         "CMR", "CMBX", "CMTI", "LMRoman", "Computer Modern", NULL };
    static const char *const sans[] = { "Sans", "Gothic", "Grotesk", NULL };
    static const char *const bold[] = { "Bold", "Black", "Heavy", "Demi", "Semibold", "CMBX", NULL };
    static const char *const italic[] = { "Italic", "Oblique", "CMTI", "CMSL", NULL };
    char name[gs_font_name_max + 1];
    const gs_font_name *fn = font->key_name.size ? &font->key_name : &font->font_name;
    int style = 0;

    memcpy(name, fn->chars, min(fn->size, gs_font_name_max));
    name[min(fn->size, gs_font_name_max)] = 0;
    if (svg_name_has(name, mono))
        style |= SVG_TEXT_MONO;
    else if (svg_name_has(name, serif) && !svg_name_has(name, sans))
        style |= SVG_TEXT_SERIF;
    if (svg_name_has(name, bold))
        style |= SVG_TEXT_BOLD;
    if (svg_name_has(name, italic))
        style |= SVG_TEXT_ITALIC;
    return style;
}

static void
svg_text_glyph(gx_device_svg *svg, const gs_matrix *em, double advance,
               const ushort *uni, int nuni, int style)
{
    svg_text_run *r = &svg->run;
    gs_matrix lm;               /* run space -> page, unnormalized */
    double s, det, u, v, w;
    uint chars[4];
    int i, n = 0;

    /* Decode UTF-16 to code points. */
    for (i = 0; i < nuni && n < 4; ++i) {
        uint c = uni[i];

        if (c >= 0xd800 && c < 0xdc00 && i + 1 < nuni &&
            uni[i + 1] >= 0xdc00 && uni[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (uni[i + 1] - 0xdc00);
            ++i;
        }
        if (c != 0)
            chars[n++] = c;
    }
    if (n == 0)
        return;

    /* Text space with y down, so <text> glyphs are upright. */
    lm.xx = em->xx; lm.xy = em->xy;
    lm.yx = -em->yx; lm.yy = -em->yy;
    lm.tx = em->tx; lm.ty = em->ty;
    det = lm.xx * lm.yy - lm.xy * lm.yx;
    if (det <= 0)               /* mirrored text: still selectable, but skip */
        return;
    s = sqrt(det);

    if (r->active && style == r->style && fabs(s - r->size) <= 0.01 * s &&
        fabs(lm.xx / s - r->m.xx) < 1e-3 && fabs(lm.xy / s - r->m.xy) < 1e-3 &&
        fabs(lm.yx / s - r->m.yx) < 1e-3 && fabs(lm.yy / s - r->m.yy) < 1e-3) {
        /* Position relative to the run (the run matrix is orthonormal-ish). */
        double dx = lm.tx - r->m.tx, dy = lm.ty - r->m.ty;
        double rdet = r->m.xx * r->m.yy - r->m.xy * r->m.yx;

        u = (dx * r->m.yy - dy * r->m.yx) / rdet;
        v = (dy * r->m.xx - dx * r->m.xy) / rdet;
        if (fabs(v) > 0.2 * s || u < r->end - 0.5 * s || u > r->end + 4 * s)
            svg_flush_run(svg);
    } else
        svg_flush_run(svg);

    if (!r->active) {
        r->active = true;
        r->m.xx = lm.xx / s; r->m.xy = lm.xy / s;
        r->m.yx = lm.yx / s; r->m.yy = lm.yy / s;
        r->m.tx = lm.tx; r->m.ty = lm.ty;
        r->size = s;
        r->style = style;
        r->upright = fabs(r->m.xx - 1) < 1e-4 && fabs(r->m.yy - 1) < 1e-4 &&
            fabs(r->m.xy) < 1e-4 && fabs(r->m.yx) < 1e-4;
        r->end = 0;
        r->last_space = true;
        r->xs.len = 0;
        r->chars.len = 0;
        if (r->xs.data) r->xs.data[0] = 0;
        if (r->chars.data) r->chars.data[0] = 0;
        u = 0;
    }
    /* A visible gap with no space glyph: insert one so copied text keeps
       its word breaks. */
    if (u - r->end > 0.15 * s && !r->last_space && chars[0] != ' ')
        svg_run_char(r, ' ', r->end);
    w = advance * s;
    for (i = 0; i < n; ++i)
        svg_run_char(r, chars[i], u + w * i / n);
    if (u + w > r->end)
        r->end = u + w;
}

/* ---------------- Glyphs ---------------- */

static bool
svg_is_outline_font(const gs_font *font)
{
    switch (font->FontType) {
    case ft_encrypted:
    case ft_encrypted2:
    case ft_CID_encrypted:
    case ft_CID_TrueType:
    case ft_TrueType:
        return font->PaintType == 0;
    default:
        return false;
    }
}

/* Does the path transformed by m match the device path's bounding box? */
static bool
svg_bbox_match(const gs_rect *dev_box, const gx_path *gpath, const gs_matrix *m, double tol)
{
    gs_rect r;

    if (!svg_path_bbox(gpath, m, &r))
        return false;
    return fabs(r.p.x - dev_box->p.x) <= tol && fabs(r.p.y - dev_box->p.y) <= tol &&
        fabs(r.q.x - dev_box->q.x) <= tol && fabs(r.q.y - dev_box->q.y) <= tol;
}

/* Append a glyph outline for matrix scale(k) to an empty path. */
static int
svg_outline_at(gs_font *font, int wmode, gs_glyph glyph, double k, gx_path *gpath)
{
    gs_matrix m;
    double sbw[4];
    int code;

    /* TrueType outlines are placed relative to the current point. */
    code = gx_path_add_point(gpath, fixed_0, fixed_0);
    if (code < 0)
        return code;
    gs_make_scaling(k, k, &m);
    return font->procs.glyph_outline(font, wmode, glyph, &m, gpath, sbw);
}

/* glyph_outline should scale the outline by its matrix, but for TrueType
   (Type 42) fonts the matrix is applied twice.  Find out, once per font,
   whether the outline grows as k or k^2. */
static int
svg_outline_exponent(gx_device_svg *svg, gs_font *font, int wmode, gs_glyph glyph)
{
    gs_font *base = font->base ? font->base : font;
    uint64_t key;
    svg_map_entry *e;
    gx_path p1, p2;
    gs_fixed_rect b1, b2;
    int exponent = 0;

    if (font->FontType != ft_TrueType && font->FontType != ft_CID_TrueType)
        return 1;
    key = svg_hash(SVG_HASH_INIT, "exponent", 8);
    key = svg_hash(key, &base->id, sizeof(base->id));
    e = svg_map_find(&svg->glyphs, key);
    if (e)
        return e->id;
    gx_path_init_local(&p1, font->memory);
    gx_path_init_local(&p2, font->memory);
    if (svg_outline_at(font, wmode, glyph, 10, &p1) >= 0 &&
        svg_outline_at(font, wmode, glyph, 1000, &p2) >= 0 &&
        gx_path_bbox(&p1, &b1) >= 0 && gx_path_bbox(&p2, &b2) >= 0) {
        double e1 = fixed2float(b1.q.y - b1.p.y) + fixed2float(b1.q.x - b1.p.x);
        double e2 = fixed2float(b2.q.y - b2.p.y) + fixed2float(b2.q.x - b2.p.x);

        if (e1 > 1 && e2 > 0)   /* ratio is 100 (linear) or 10000 (quadratic) */
            exponent = e2 / e1 > 1000 ? 2 : 1;
    }
    gx_path_free(&p1, "svg_outline_exponent");
    gx_path_free(&p2, "svg_outline_exponent");
    if (exponent) {
        /* Remember the answer only once a glyph with some extent told us. */
        e = svg_map_add(&svg->glyphs, key);
        if (e)
            e->id = exponent;
    }
    return exponent ? exponent : 2;
}

/* The glyph outline for (font, glyph), in character space scaled by the
   entry's scale (chosen to give about 1000 units per em).  Creates the
   map entry (not yet written) if needed. */
static int
svg_glyph_outline(gx_device_svg *svg, gs_font *font, gs_glyph glyph, gx_path *gpath,
                  svg_map_entry **pe)
{
    gs_font *base = font->base ? font->base : font;
    int wmode = font->WMode;
    uint64_t key;
    svg_map_entry *e;
    double scale, k;
    int code, exponent;

    key = svg_hash(SVG_HASH_INIT, &base->id, sizeof(base->id));
    key = svg_hash(key, &glyph, sizeof(glyph));
    key = svg_hash(key, &wmode, sizeof(wmode));
    e = svg_map_find(&svg->glyphs, key);
    if (e)
        scale = e->scale;
    else {
        scale = 1000 * hypot(base->FontMatrix.xx, base->FontMatrix.xy);
        if (!(scale >= 1e-3 && scale <= 1e5))
            scale = 1;
    }
    exponent = svg_outline_exponent(svg, font, wmode, glyph);
    k = exponent == 2 ? sqrt(scale) : scale;
    code = svg_outline_at(font, wmode, glyph, k, gpath);
    if (code < 0)
        return code;
    if (e == NULL) {
        gs_glyph_info_t info;

        e = svg_map_add(&svg->glyphs, key);
        if (e == NULL)
            return_error(gs_error_VMerror);
        e->id = 0;              /* not written yet */
        e->scale = scale;
        e->advance = 0;
        if (font->procs.glyph_info(font, glyph, NULL, GLYPH_INFO_WIDTH0 << wmode, &info) >= 0)
            e->advance = wmode ? info.width[1].y : info.width[0].x;
    }
    *pe = e;
    return 0;
}

/* Append ppath transformed by m (applied to its fixed coordinates). */
static int
svg_path_transform(const gx_path *ppath, const gs_matrix *m, gx_path *out)
{
    gs_path_enum penum;
    gs_fixed_point pts[3], q[3];
    int op, i, code = 0;

    gx_path_enum_init(&penum, ppath);
    while (code >= 0 && (op = gx_path_enum_next(&penum, pts)) != 0) {
        int n = op == gs_pe_curveto ? 3 : op == gs_pe_closepath ? 0 : 1;

        for (i = 0; i < n; ++i) {
            double x = fixed2float(pts[i].x), y = fixed2float(pts[i].y);

            q[i].x = float2fixed(x * m->xx + y * m->yx + m->tx);
            q[i].y = float2fixed(x * m->xy + y * m->yy + m->ty);
        }
        switch (op) {
        case gs_pe_moveto: code = gx_path_add_point(out, q[0].x, q[0].y); break;
        case gs_pe_gapto:
        case gs_pe_lineto: code = gx_path_add_line(out, q[0].x, q[0].y); break;
        case gs_pe_curveto:
            code = gx_path_add_curve(out, q[0].x, q[0].y, q[1].x, q[1].y, q[2].x, q[2].y);
            break;
        case gs_pe_closepath: code = gx_path_close_subpath(out); break;
        }
    }
    return code;
}

/*
 * Called when something is painted while a text enumerator we track is
 * active: a glyph outline fill, or a glyph bitmap from the font rasterizer.
 * dev_box is the painted area in device space.  If it is the current glyph,
 * draw it as <use> of a shared definition (and add it to the text layer)
 * and return 1; return 0 to let the caller paint it normally.
 */
static int
svg_glyph_draw(gx_device_svg *svg, const gs_rect *dev_box, bool bitmap, int rule,
               const gx_drawing_color *pdcolor, const gx_clip_path *pcpath)
{
    gs_text_enum_t *pte = svg->text_enum;
    gs_gstate *pgs = pte->pgs;
    gs_font *font = pte->current_font;
    gs_glyph glyph = pte->returned.current_glyph;
    gs_font *base;
    svg_map_entry *e = NULL;
    gx_path gpath;
    gs_matrix cm, m, mp, page, em, inv_s;
    double size, tol;
    int code;
    bool empty;

    if (font == NULL || glyph == GS_NO_GLYPH || pgs == NULL || !svg_is_outline_font(font))
        return 0;
    base = font->base ? font->base : font;
    /* Outline paths must use the font's allocator: the Type 1 hinter grows
       some arrays with the font's memory but frees them with the path's. */
    gx_path_init_local(&gpath, font->memory);
    code = svg_glyph_outline(svg, font, glyph, &gpath, &e);
    if (code < 0)
        goto raw;

    /* While a glyph is drawn the CTM is the character matrix, translated
       to the glyph origin rounded to a pixel; the exact origin is kept in
       the enumerator. */
    gs_currentmatrix(pgs, &cm);
    {
        const gs_matrix *fm = &font->FontMatrix;
        double dx = 0, dy = 0;

        if (fm->tx != 0 || fm->ty != 0) {
            /* char_tm = FontMatrix x CTM, so its translation includes the
               FontMatrix translation mapped through the CTM. */
            gs_matrix fl, fli, ul;

            fl = *fm;
            fl.tx = fl.ty = 0;
            if (gs_matrix_invert(&fl, &fli) >= 0) {
                cm.tx = cm.ty = 0;
                gs_matrix_multiply(&fli, &cm, &ul);
                dx = fm->tx * ul.xx + fm->ty * ul.yx;
                dy = fm->tx * ul.xy + fm->ty * ul.yy;
            }
        }
        cm.tx = fixed2float(pte->origin.x) + dx;
        cm.ty = fixed2float(pte->origin.y) + dy;
    }
    gs_make_scaling(1 / e->scale, 1 / e->scale, &inv_s);
    gs_matrix_multiply(&inv_s, &cm, &m);        /* outline units -> device */

    /* Make sure this really is the glyph: its outline must cover the
       painted area.  A glyph with no outline (a space) has nothing to
       paint, and its rasterizer's bitmap is at most a blank pixel. */
    size = max(dev_box->q.x - dev_box->p.x, dev_box->q.y - dev_box->p.y);
    tol = bitmap ? 2.5 + 0.1 * size : 1.5 + 0.03 * size;
    {
        gs_rect r;

        empty = !svg_path_bbox(&gpath, &m, &r);
    }
    if (empty ? !bitmap || size > 1 : !svg_bbox_match(dev_box, &gpath, &m, tol))
        goto raw;

    svg_page_matrix(svg, &page);
    gs_matrix_multiply(&m, &page, &mp);         /* outline units -> page */

    if (svg->TextLayer && svg->mask_depth == 0) {
        gs_font *ufont = pte->orig_font ? pte->orig_font : font;
        ushort uni[4];
        int nuni;
        gs_matrix fm_inv, s_m, t;

        nuni = txt_get_unicode((gx_device *)svg, ufont, glyph,
                               pte->returned.current_char, uni);
        /* em space -> character space -> outline units -> page */
        if (gs_matrix_invert(&base->FontMatrix, &fm_inv) >= 0) {
            gs_make_scaling(e->scale, e->scale, &s_m);
            gs_matrix_multiply(&fm_inv, &s_m, &t);
            gs_matrix_multiply(&t, &mp, &em);
            svg_text_glyph(svg, &em, e->advance * hypot(base->FontMatrix.xx, base->FontMatrix.xy),
                           uni, nuni, svg_font_style(font->key_name.size || font->font_name.size ?
                                                     font : ufont));
        }
    }

    code = 1;
    if (svg->text_invisible || empty)
        goto out;
    if (!gx_dc_is_pure(pdcolor)) {
        /* Patterns and shadings: fill the true outline like any path. */
        gx_path dpath;
        gx_fill_params fp;

        gx_path_init_local(&dpath, svg->memory);
        code = svg_path_transform(&gpath, &m, &dpath);
        if (code >= 0) {
            fp.rule = rule;
            fp.adjust.x = fp.adjust.y = 0;
            fp.flatness = 0.25;
            svg->painting_glyph = true;
            code = svg_fill_path((gx_device *)svg, pgs, &dpath, &fp, pdcolor, pcpath);
            svg->painting_glyph = false;
        }
        gx_path_free(&dpath, "svg glyph");
        if (code >= 0)
            code = 1;
        goto out;
    }
    if (!svg->ReuseGlyphs) {
        if (bitmap) {
            /* Draw the outline as a plain path rather than the bitmap. */
            code = svg_begin_element(svg, pcpath);
            if (code < 0)
                goto out;
            sb_puts(svg->out, "<path d=\"");
            svg_put_path(svg->out, &gpath, &mp, SVG_PAGE_PREC, false);
            sb_putc(svg->out, '"');
            if (gx_dc_pure_color(pdcolor) != 0) {
                sb_puts(svg->out, " fill=\"");
                sb_color(svg->out, gx_dc_pure_color(pdcolor));
                sb_putc(svg->out, '"');
            }
            svg_put_transparency(svg, pgs, SVG_PAINT_FILL);
            sb_puts(svg->out, "/>\n");
            code = 1;
        } else
            code = 0;           /* paint what we were given */
        goto out;
    }
    if (e->id == 0) {
        gs_matrix ident;

        e->id = ++svg->next_id;
        sb_printf(&svg->defs, "<path id=\"g%d\" d=\"", e->id);
        gs_make_identity(&ident);
        svg_put_path(&svg->defs, &gpath, &ident, SVG_GLYPH_PREC, false);
        sb_puts(&svg->defs, "\"/>\n");
    }
    code = svg_begin_element(svg, pcpath);
    if (code < 0)
        goto out;
    sb_printf(svg->out, "<use xlink:href=\"#g%d\" transform=\"", e->id);
    sb_matrix(svg->out, &mp);
    sb_putc(svg->out, '"');
    if (gx_dc_pure_color(pdcolor) != 0) {
        sb_puts(svg->out, " fill=\"");
        sb_color(svg->out, gx_dc_pure_color(pdcolor));
        sb_putc(svg->out, '"');
    }
    if (rule > 0)
        sb_puts(svg->out, " fill-rule=\"evenodd\"");
    svg_put_transparency(svg, pgs, SVG_PAINT_FILL);
    sb_puts(svg->out, "/>\n");
    code = 1;
    goto out;
raw:
    code = 0;
out:
    gx_path_free(&gpath, "svg glyph");
    return code;
}

/* ---------------- Shadings ---------------- */

/*
 * Axial and radial shadings (ShadingType 2 and 3, from shfill or used as
 * a pattern) become SVG gradients.  The shading function is sampled over
 * its Domain and converted to device RGB by the graphics library, then
 * stops that linear interpolation reproduces are dropped.  SVG gradients
 * always extend at both ends, so Extend false is expressed by clipping to
 * the region the shading covers.  Anything else returns 0, and the caller
 * rasterizes the fill.
 */

#define SVG_MAX_STOPS 257

/* Write a definition <tag id="{prefix}N"{content} unless an identical one
   was already written on this page; returns its id N.  content holds
   everything after the id attribute, including the closing tag. */
static int
svg_put_def(gx_device_svg *svg, const char *tag, char prefix, svg_buf *content)
{
    uint64_t h = svg_hash(SVG_HASH_INIT, tag, strlen(tag));
    svg_map_entry *e;
    int id;

    if (content->error)
        return content->error;
    h = svg_hash(h, content->data ? content->data : "", content->len);
    e = svg_map_find(&svg->clips, h);
    if (e)
        return e->id;
    e = svg_map_add(&svg->clips, h);
    if (e == NULL)
        return_error(gs_error_VMerror);
    id = e->id = ++svg->next_id;
    sb_printf(&svg->defs, "<%s id=\"%c%d\"", tag, prefix, id);
    if (content->data)
        sb_puts(&svg->defs, content->data);
    sb_putc(&svg->defs, '\n');
    return id;
}

typedef struct svg_stop_s {
    double t;
    int c[3];
} svg_stop;

static int
svg_shading_stops(gx_device_svg *svg, const gs_gstate *pgs, const gs_shading_t *psh,
                  gs_function_t *fn, const float *domain, svg_stop *stops)
{
    const gs_color_space *pcs = psh->params.ColorSpace;
    svg_stop all[SVG_MAX_STOPS];
    int n = SVG_MAX_STOPS - 1, i, j, a, kept;

    if (fn == NULL || pcs == NULL || gs_color_space_num_components(pcs) > GS_CLIENT_COLOR_MAX_COMPONENTS)
        return -1;
    for (i = 0; i <= n; ++i) {
        float t = domain[0] + (domain[1] - domain[0]) * i / n;
        float out[GS_CLIENT_COLOR_MAX_COMPONENTS];
        gs_client_color cc;
        gx_device_color dc;
        gx_color_index c;
        int k, ncomp = gs_color_space_num_components(pcs);

        if (gs_function_evaluate(fn, &t, out) < 0)
            return -1;
        memset(&cc, 0, sizeof(cc));
        for (k = 0; k < ncomp; ++k)
            cc.paint.values[k] = out[k];
        if (pcs->type->remap_color(&cc, pcs, &dc, pgs, (gx_device *)svg,
                                   gs_color_select_texture) < 0 || !gx_dc_is_pure(&dc))
            return -1;
        c = gx_dc_pure_color(&dc);
        all[i].t = (double)i / n;
        all[i].c[0] = (int)(c >> 16) & 0xff;
        all[i].c[1] = (int)(c >> 8) & 0xff;
        all[i].c[2] = (int)c & 0xff;
    }
    /* Greedily keep only the stops needed to stay within one level. */
    stops[0] = all[0];
    kept = 1;
    a = 0;
    for (i = 2; i <= n; ++i) {
        bool ok = true;

        for (j = a + 1; j < i && ok; ++j) {
            int k;

            for (k = 0; k < 3; ++k) {
                double v = all[a].c[k] + (all[i].c[k] - all[a].c[k]) * (double)(j - a) / (i - a);

                if (fabs(v - all[j].c[k]) > 1.0) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            stops[kept++] = all[i - 1];
            a = i - 1;
        }
    }
    stops[kept++] = all[n];
    return kept;
}

/* Stops at offsets o0 + t * (1 - o0) (o0 > 0 maps a concentric start
   circle onto the gradient, padding inside it with the first color). */
static void
svg_put_stops(svg_buf *b, const svg_stop *stops, int n, bool reverse, double o0)
{
    int i;

    for (i = 0; i < n; ++i) {
        const svg_stop *st = &stops[reverse ? n - 1 - i : i];

        sb_puts(b, "<stop offset=\"");
        sb_num(b, o0 + (reverse ? 1 - st->t : st->t) * (1 - o0), 4);
        sb_puts(b, "\" stop-color=\"");
        sb_color(b, ((gx_color_index)st->c[0] << 16) | (st->c[1] << 8) | st->c[2]);
        sb_puts(b, "\"/>");
    }
}

/* Write a clipPath whose single path is given in pattern space. */
static int
svg_pattern_clip(gx_device_svg *svg, const char *d, bool evenodd, const gs_matrix *pm)
{
    svg_buf c;
    int id;

    sb_init(&c, svg_mem(svg));
    sb_puts(&c, "><path transform=\"");
    sb_matrix(&c, pm);
    sb_puts(&c, "\" d=\"");
    sb_puts(&c, d);
    sb_puts(&c, evenodd ? "\" clip-rule=\"evenodd\"/></clipPath>" : "\"/></clipPath>");
    id = svg_put_def(svg, "clipPath", 'c', &c);
    sb_free(&c);
    return id;
}

static void
svg_circle_d(svg_buf *b, double cx, double cy, double r)
{
    /* Two arcs make a full circle. */
    sb_putc(b, 'M'); sb_sig(b, cx - r, 7); sb_putc(b, ' '); sb_sig(b, cy, 7);
    sb_putc(b, 'a'); sb_sig(b, r, 7); sb_putc(b, ' '); sb_sig(b, r, 7);
    sb_puts(b, " 0 1 0 "); sb_sig(b, 2 * r, 7); sb_puts(b, " 0a");
    sb_sig(b, r, 7); sb_putc(b, ' '); sb_sig(b, r, 7);
    sb_puts(b, " 0 1 0 "); sb_sig(b, -2 * r, 7); sb_puts(b, " 0z");
}

static int
svg_shading_fill(gx_device_svg *svg, const gs_gstate *pgs, gx_path *ppath,
                 const gx_fill_params *params, const gx_drawing_color *pdcolor,
                 const gx_clip_path *pcpath)
{
    const gs_pattern2_instance_t *pinst = (const gs_pattern2_instance_t *)pdcolor->ccolor.pattern;
    const gs_shading_t *psh;
    svg_stop stops[SVG_MAX_STOPS];
    gs_matrix pm, page, inv;
    gs_fixed_rect fb;
    svg_buf d, g;
    double big;
    int type, nstops, gid, clips[3], nclips = 0, i, code;
    bool reverse = false;

    if (pgs == NULL || pinst == NULL || (psh = pinst->templat.Shading) == NULL)
        return 0;
    type = ShadingType(psh);
    if (type != 2 && type != 3)
        return 0;
    if (psh->params.Background && !pinst->shfill)
        return 0;
    /* pattern space -> page */
    gs_currentmatrix(pinst->saved, &pm);
    svg_page_matrix(svg, &page);
    gs_matrix_multiply(&pm, &page, &pm);
    if (gs_matrix_invert(&pm, &inv) < 0)
        return 0;
    /* A length that reaches past the fill area, in pattern space. */
    if (gx_path_bbox(ppath, &fb) < 0)
        return 0;
    {
        gs_point c[4];
        double sx = svg_sx(svg), sy = svg_sy(svg);

        gs_point_transform(fixed2float(fb.p.x) * sx, fixed2float(fb.p.y) * sy, &inv, &c[0]);
        gs_point_transform(fixed2float(fb.q.x) * sx, fixed2float(fb.p.y) * sy, &inv, &c[1]);
        gs_point_transform(fixed2float(fb.p.x) * sx, fixed2float(fb.q.y) * sy, &inv, &c[2]);
        gs_point_transform(fixed2float(fb.q.x) * sx, fixed2float(fb.q.y) * sy, &inv, &c[3]);
        big = 0;
        for (i = 0; i < 4; ++i)
            big = max(big, fabs(c[i].x) + fabs(c[i].y));
        big = 4 * big + 1;
    }

    sb_init(&d, svg_mem(svg));
    sb_init(&g, svg_mem(svg));
    gid = 0;
    if (type == 2) {
        const gs_shading_A_params_t *a = &((const gs_shading_A_t *)psh)->params;
        double x0 = a->Coords[0], y0 = a->Coords[1], x1 = a->Coords[2], y1 = a->Coords[3];
        double dx = x1 - x0, dy = y1 - y0, len = hypot(dx, dy);

        if (len == 0)
            goto none;
        nstops = svg_shading_stops(svg, pgs, psh, a->Function, a->Domain, stops);
        if (nstops < 0)
            goto none;
        sb_puts(&g, " gradientUnits=\"userSpaceOnUse\" x1=\"");
        sb_sig(&g, x0, 7); sb_puts(&g, "\" y1=\"");
        sb_sig(&g, y0, 7); sb_puts(&g, "\" x2=\"");
        sb_sig(&g, x1, 7); sb_puts(&g, "\" y2=\"");
        sb_sig(&g, y1, 7); sb_puts(&g, "\" gradientTransform=\"");
        sb_matrix(&g, &pm);
        sb_puts(&g, "\">");
        svg_put_stops(&g, stops, nstops, false, 0);
        sb_puts(&g, "</linearGradient>");
        gid = svg_put_def(svg, "linearGradient", 's', &g);
        if (!a->Extend[0] || !a->Extend[1]) {
            /* The slab between the ends that don't extend. */
            double ta = a->Extend[0] ? -big / len : 0, tb = a->Extend[1] ? 1 + big / len : 1;
            double nx = -dy / len * big, ny = dx / len * big;
            double px[4], py[4];

            px[0] = x0 + ta * dx + nx; py[0] = y0 + ta * dy + ny;
            px[1] = x0 + tb * dx + nx; py[1] = y0 + tb * dy + ny;
            px[2] = x0 + tb * dx - nx; py[2] = y0 + tb * dy - ny;
            px[3] = x0 + ta * dx - nx; py[3] = y0 + ta * dy - ny;
            for (i = 0; i < 4; ++i) {
                sb_putc(&d, i ? 'L' : 'M');
                sb_sig(&d, px[i], 7); sb_putc(&d, ' '); sb_sig(&d, py[i], 7);
            }
            sb_putc(&d, 'Z');
            clips[nclips++] = svg_pattern_clip(svg, d.data, false, &pm);
        }
    } else {
        const gs_shading_R_params_t *r = &((const gs_shading_R_t *)psh)->params;
        double x0 = r->Coords[0], y0 = r->Coords[1], r0 = r->Coords[2];
        double x1 = r->Coords[3], y1 = r->Coords[4], r1 = r->Coords[5];
        bool e0 = r->Extend[0], e1 = r->Extend[1], concentric;

        if (r0 > r1) {
            /* SVG wants the focal circle to be the smaller one. */
            double t;
            bool te;

            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
            t = r0; r0 = r1; r1 = t;
            te = e0; e0 = e1; e1 = te;
            reverse = true;
        }
        /* Only nested circles (start inside end) are expressed natively. */
        if (r1 <= 0 || hypot(x1 - x0, y1 - y0) + r0 > r1 * (1 - 1e-6))
            goto none;
        nstops = svg_shading_stops(svg, pgs, psh, r->Function, r->Domain, stops);
        if (nstops < 0)
            goto none;
        sb_puts(&g, " gradientUnits=\"userSpaceOnUse\" cx=\"");
        sb_sig(&g, x1, 7); sb_puts(&g, "\" cy=\"");
        sb_sig(&g, y1, 7); sb_puts(&g, "\" r=\"");
        sb_sig(&g, r1, 7); sb_puts(&g, "\" fx=\"");
        sb_sig(&g, x0, 7); sb_puts(&g, "\" fy=\"");
        sb_sig(&g, y0, 7);
        /* A start circle concentric with the end circle is expressed by
           moving the stops, which every renderer supports; otherwise use
           SVG 2's focal radius. */
        concentric = x0 == x1 && y0 == y1;
        if (r0 > 0 && !concentric) {
            sb_puts(&g, "\" fr=\"");
            sb_sig(&g, r0, 7);
        }
        sb_puts(&g, "\" gradientTransform=\"");
        sb_matrix(&g, &pm);
        sb_puts(&g, "\">");
        svg_put_stops(&g, stops, nstops, reverse, concentric ? r0 / r1 : 0);
        sb_puts(&g, "</radialGradient>");
        gid = svg_put_def(svg, "radialGradient", 's', &g);
        if (!e1 || (!e0 && r0 > 0)) {
            /* Outside the end circle, or inside the start circle, is
               unpainted unless extended. */
            if (!e1)
                svg_circle_d(&d, x1, y1, r1);
            else {
                sb_putc(&d, 'M'); sb_sig(&d, -big, 7); sb_putc(&d, ' '); sb_sig(&d, -big, 7);
                sb_putc(&d, 'H'); sb_sig(&d, big, 7); sb_putc(&d, 'V'); sb_sig(&d, big, 7);
                sb_putc(&d, 'H'); sb_sig(&d, -big, 7); sb_putc(&d, 'Z');
            }
            if (!e0 && r0 > 0)
                svg_circle_d(&d, x0, y0, r0);
            clips[nclips++] = svg_pattern_clip(svg, d.data, !e0 && r0 > 0, &pm);
        }
    }
    if (psh->params.have_BBox) {
        const gs_rect *bb = &psh->params.BBox;

        d.len = 0;
        sb_putc(&d, 'M'); sb_sig(&d, bb->p.x, 7); sb_putc(&d, ' '); sb_sig(&d, bb->p.y, 7);
        sb_putc(&d, 'H'); sb_sig(&d, bb->q.x, 7); sb_putc(&d, 'V'); sb_sig(&d, bb->q.y, 7);
        sb_putc(&d, 'H'); sb_sig(&d, bb->p.x, 7); sb_putc(&d, 'Z');
        clips[nclips++] = svg_pattern_clip(svg, d.data, false, &pm);
    }
    sb_free(&d);
    sb_free(&g);
    if (gid < 0)
        return gid;
    for (i = 0; i < nclips; ++i)
        if (clips[i] < 0)
            return clips[i];

    code = svg_begin_element(svg, pcpath);
    if (code < 0)
        return code;
    for (i = 0; i < nclips; ++i)
        sb_printf(svg->out, "<g clip-path=\"url(#c%d)\">", clips[i]);
    {
        gs_matrix m;

        svg_page_matrix(svg, &m);
        sb_puts(svg->out, "<path d=\"");
        svg_put_path(svg->out, ppath, &m, SVG_PAGE_PREC, false);
        sb_printf(svg->out, "\" fill=\"url(#s%d)\"", gid);
        if (params->rule > 0)
            sb_puts(svg->out, " fill-rule=\"evenodd\"");
        svg_put_transparency(svg, pgs, SVG_PAINT_FILL);
        sb_puts(svg->out, "/>");
    }
    for (i = 0; i < nclips; ++i)
        sb_puts(svg->out, "</g>");
    sb_putc(svg->out, '\n');
    return svg->out->error ? svg->out->error : 1;
none:
    sb_free(&d);
    sb_free(&g);
    return 0;
}

/* ---------------- Text enumerator hook ---------------- */

typedef struct svg_text_procs_s {
    gs_text_enum_procs_t procs;         /* must be first */
    const gs_text_enum_procs_t *orig;
    gs_memory_t *mem;
} svg_text_procs_t;

static void
svg_text_release(gs_text_enum_t *pte, client_name_t cname)
{
    svg_text_procs_t *p = (svg_text_procs_t *)pte->procs;
    gx_device_svg *svg = (gx_device_svg *)pte->dev;

    if (svg->text_enum == pte) {
        svg->text_enum = NULL;
        svg->text_invisible = false;
    }
    pte->procs = p->orig;
    p->orig->release(pte, cname);
    gs_free_object(p->mem, p, "svg_text_release");
}

static int
svg_text_begin(gx_device *dev, gs_gstate *pgs, const gs_text_params_t *text,
               gs_font *font, const gx_clip_path *pcpath, gs_text_enum_t **ppte)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    gs_text_params_t t = *text;
    bool invisible = false;
    svg_text_procs_t *p;
    gs_show_enum *penum;
    int code;

    /* Invisible text (rendering mode 3, e.g. OCR layers) is drawn with
       painting suppressed, so its glyphs reach the text layer. */
    if (svg->TextLayer && (t.operation & TEXT_RENDER_MODE_3) &&
        (t.operation & TEXT_DO_NONE) && svg->text_enum == NULL) {
        t.operation = (t.operation & ~TEXT_DO_NONE) | TEXT_DO_DRAW;
        invisible = true;
    }
    code = gx_default_text_begin(dev, pgs, &t, font, pcpath, ppte);
    if (code < 0 || !(t.operation & TEXT_DO_DRAW) || svg->text_enum != NULL ||
        (!svg->TextLayer && !svg->ReuseGlyphs))
        return code;

    p = (svg_text_procs_t *)gs_alloc_bytes(svg_mem(svg), sizeof(*p), "svg_text_begin");
    if (p == NULL)
        return code;            /* just don't track this text */
    penum = (gs_show_enum *)*ppte;
    p->procs = *penum->procs;
    p->procs.release = svg_text_release;
    p->orig = penum->procs;
    p->mem = svg_mem(svg);
    penum->procs = &p->procs;
    /* Draw glyphs as outlines through fill_path, never as cached bitmaps,
       and without hinting. */
    penum->can_cache = -1;
    penum->device_disabled_grid_fitting = true;
    svg->text_enum = *ppte;
    svg->text_invisible = invisible;
    return code;
}

/* ---------------- Painting ---------------- */

static int
svg_fill_path(gx_device *dev, const gs_gstate *pgs, gx_path *ppath,
              const gx_fill_params *params, const gx_drawing_color *pdcolor,
              const gx_clip_path *pcpath)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    gs_matrix m;
    int code;

    if (gx_path_is_void(ppath))
        return 0;
    if (svg->text_enum && !svg->painting_glyph) {
        gs_fixed_rect fb;
        gs_rect box;

        if (gx_path_bbox(ppath, &fb) >= 0) {
            box.p.x = fixed2float(fb.p.x); box.p.y = fixed2float(fb.p.y);
            box.q.x = fixed2float(fb.q.x); box.q.y = fixed2float(fb.q.y);
            code = svg_glyph_draw(svg, &box, false, params->rule, pdcolor, pcpath);
            if (code != 0)
                return code < 0 ? code : 0;
        }
    }
    if (svg->text_invisible)
        return 0;
    if (!gx_dc_is_pure(pdcolor)) {
        if (pgs == NULL)
            return_error(gs_error_rangecheck);
        if (gx_dc_is_pattern2_color(pdcolor)) {
            code = svg_shading_fill(svg, pgs, ppath, params, pdcolor, pcpath);
            if (code != 0)
                return code < 0 ? code : 0;
        }
        /* Rasterize into the canvas.  With transparency in effect, emit
           it right away so it gets this fill's opacity and blend mode. */
        code = svg_flush_canvas(svg);
        if (code < 0)
            return code;
        code = gx_default_fill_path(dev, pgs, ppath, params, pdcolor, pcpath);
        if (code >= 0 && svg->Transparency &&
            (pgs->fillconstantalpha < 1 || svg_blend_name(pgs->blend_mode)))
            code = svg_flush_canvas_painted(svg, pgs);
        return code;
    }
    code = svg_begin_element(svg, pcpath);
    if (code < 0)
        return code;
    svg_page_matrix(svg, &m);
    sb_puts(svg->out, "<path d=\"");
    svg_put_path(svg->out, ppath, &m, SVG_PAGE_PREC, false);
    sb_putc(svg->out, '"');
    if (gx_dc_pure_color(pdcolor) != 0) {
        sb_puts(svg->out, " fill=\"");
        sb_color(svg->out, gx_dc_pure_color(pdcolor));
        sb_putc(svg->out, '"');
    }
    if (params->rule > 0)
        sb_puts(svg->out, " fill-rule=\"evenodd\"");
    svg_put_transparency(svg, pgs, SVG_PAINT_FILL);
    sb_puts(svg->out, "/>\n");
    return svg->out->error;
}

static int
svg_stroke_path(gx_device *dev, const gs_gstate *pgs, gx_path *ppath,
                const gx_stroke_params *params, const gx_drawing_color *pdcolor,
                const gx_clip_path *pcpath)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    const gx_line_params *lp = &pgs->line_params;
    gs_matrix ctm, page, m, inv, pm;
    double k, det, width;
    bool transform;
    int code, i;

    if (gx_path_is_void(ppath) || svg->text_invisible)
        return 0;
    if (!gx_dc_is_pure(pdcolor))
        return gx_default_stroke_path(dev, pgs, ppath, params, pdcolor, pcpath);

    /* The pen is defined in user space.  If the CTM is a similarity
       (uniform scale, rotation, reflection), stroke in page space with a
       scaled width; otherwise write the path in (rescaled) user space
       with the CTM as a transform, which SVG applies to the pen too. */
    gs_currentmatrix(pgs, &ctm);
    svg_page_matrix(svg, &page);
    gs_matrix_multiply(&ctm, &page, &m);        /* user -> page */
    det = m.xx * m.yy - m.xy * m.yx;
    k = sqrt(fabs(det));
    transform = !(fabs(m.xx - m.yy) <= 1e-6 * k && fabs(m.xy + m.yx) <= 1e-6 * k) &&
                !(fabs(m.xx + m.yy) <= 1e-6 * k && fabs(m.xy - m.yx) <= 1e-6 * k);
    if (k == 0)
        return 0;

    code = svg_begin_element(svg, pcpath);
    if (code < 0)
        return code;
    sb_puts(svg->out, "<path d=\"");
    if (transform) {
        /* Write the path in k * user space, and transform it to the page
           with mk = (m's linear part / k, m's translation). */
        gs_matrix mk;

        mk = m;
        mk.xx /= k; mk.xy /= k; mk.yx /= k; mk.yy /= k;
        if (gs_matrix_invert(&mk, &inv) < 0)
            transform = false;
        else {
            gs_matrix_multiply(&page, &inv, &pm);       /* device -> k user */
            svg_put_path(svg->out, ppath, &pm, SVG_PAGE_PREC, true);
            sb_puts(svg->out, "\" transform=\"");
            sb_matrix(svg->out, &mk);
        }
    }
    if (!transform)
        svg_put_path(svg->out, ppath, &page, SVG_PAGE_PREC, true);
    sb_puts(svg->out, "\" fill=\"none\" stroke=\"");
    sb_color(svg->out, gx_dc_pure_color(pdcolor));
    sb_putc(svg->out, '"');

    width = lp->half_width * 2 * k;
    if (width <= 0)
        /* Zero width means the thinnest line the device can draw. */
        sb_puts(svg->out, " stroke-width=\"1\" vector-effect=\"non-scaling-stroke\"");
    else if (fabs(width - 1) > 1e-4) {
        sb_puts(svg->out, " stroke-width=\"");
        sb_num(svg->out, width, 3);
        sb_putc(svg->out, '"');
    }
    switch (lp->start_cap) {
    case gs_cap_round: sb_puts(svg->out, " stroke-linecap=\"round\""); break;
    case gs_cap_square: sb_puts(svg->out, " stroke-linecap=\"square\""); break;
    default: break;
    }
    switch (lp->join) {
    case gs_join_round: sb_puts(svg->out, " stroke-linejoin=\"round\""); break;
    case gs_join_bevel:
    case gs_join_none: sb_puts(svg->out, " stroke-linejoin=\"bevel\""); break;
    default:
        if (fabs(lp->miter_limit - 4) > 1e-4) {
            sb_puts(svg->out, " stroke-miterlimit=\"");
            sb_num(svg->out, lp->miter_limit < 1 ? 1 : lp->miter_limit, 3);
            sb_putc(svg->out, '"');
        }
        break;
    }
    if (lp->dash.pattern_size > 0) {
        double sum = 0;

        for (i = 0; i < (int)lp->dash.pattern_size; ++i)
            sum += lp->dash.pattern[i];
        if (sum > 0) {
            sb_puts(svg->out, " stroke-dasharray=\"");
            for (i = 0; i < (int)lp->dash.pattern_size; ++i) {
                if (i)
                    sb_putc(svg->out, ' ');
                sb_num(svg->out, lp->dash.pattern[i] * k, 3);
            }
            sb_putc(svg->out, '"');
            if (lp->dash.offset != 0) {
                sb_puts(svg->out, " stroke-dashoffset=\"");
                sb_num(svg->out, lp->dash.offset * k, 3);
                sb_putc(svg->out, '"');
            }
        }
    }
    svg_put_transparency(svg, pgs, SVG_PAINT_STROKE);
    sb_puts(svg->out, "/>\n");
    return svg->out->error;
}

static int
svg_fill_rectangle_hl_color(gx_device *dev, const gs_fixed_rect *rect,
                            const gs_gstate *pgs, const gx_drawing_color *pdcolor,
                            const gx_clip_path *pcpath)
{
    gx_path path;
    gx_fill_params params;
    int code;

    gx_path_init_local(&path, dev->memory);
    code = gx_path_add_rectangle(&path, rect->p.x, rect->p.y, rect->q.x, rect->q.y);
    if (code >= 0) {
        params.rule = gx_rule_winding_number;
        params.adjust.x = params.adjust.y = 0;
        params.flatness = 0.25;
        code = svg_fill_path(dev, pgs, &path, &params, pdcolor, pcpath);
    }
    gx_path_free(&path, "svg_fill_rectangle_hl_color");
    return code;
}

/* ---------------- Images ---------------- */

/*
 * Images are rendered by the graphics library into an offscreen memory
 * device whose pixel grid is the image's own sample grid (reduced if the
 * image has much more resolution than the device), so every color space,
 * Decode array and mask type is handled exactly as for raster output.  The
 * result is embedded as a PNG transformed onto the page.
 *
 * Stencil masks are rendered black on white, giving the alpha of a fill in
 * the current color.  Color-key masked images (ImageType 4) are rendered on
 * white and on black: pixels that differ were not painted.
 */
typedef struct svg_image_enum_s {
    gx_image_enum_common;
    gx_image_enum_common_t *inner[2];
    gx_device_memory *mdev[2];
    int passes;
    int gw, gh;                 /* grid size */
    gs_matrix gm;               /* grid -> page */
    bool stencil;               /* ImageMask: alpha from pass 0 */
    gx_color_index mask_color;
    bool pixelated;
    float opacity;              /* fill alpha when the image began */
    gs_blend_mode_t blend_mode;
} svg_image_enum_t;

gs_private_st_suffix_add4(st_svg_image_enum, svg_image_enum_t, "svg_image_enum_t",
    svg_image_enum_enum_ptrs, svg_image_enum_reloc_ptrs, st_gx_image_enum_common,
    inner[0], inner[1], mdev[0], mdev[1]);

static int
svg_image_plane_data(gx_image_enum_common_t *info, const gx_image_plane_t *planes,
                     int height, int *rows_used)
{
    svg_image_enum_t *pie = (svg_image_enum_t *)info;
    int code = gx_image_plane_data_rows(pie->inner[0], planes, height, rows_used);

    if (code >= 0 && pie->passes > 1) {
        int used;
        int code1 = gx_image_plane_data_rows(pie->inner[1], planes, height, &used);

        if (code1 < 0)
            code = code1;
    }
    return code;
}

static bool
svg_image_planes_wanted(const gx_image_enum_common_t *info, byte *wanted)
{
    const svg_image_enum_t *pie = (const svg_image_enum_t *)info;

    return gx_image_planes_wanted(pie->inner[0], wanted);
}

static void
svg_image_free(svg_image_enum_t *pie)
{
    int i;

    for (i = 0; i < 2; ++i)
        if (pie->mdev[i]) {
            gx_device *d = (gx_device *)pie->mdev[i];

            dev_proc(d, close_device)(d);
            gx_device_set_target((gx_device_forward *)d, NULL);
            gs_free_object(pie->memory, d, "svg image mdev");
            pie->mdev[i] = NULL;
        }
    gx_image_free_enum((gx_image_enum_common_t **)&pie);
}

static int
svg_image_end_image(gx_image_enum_common_t *info, bool draw_last)
{
    svg_image_enum_t *pie = (svg_image_enum_t *)info;
    gx_device_svg *svg = (gx_device_svg *)pie->dev;
    int code = 0, i, x, y, n = pie->gw * pie->gh;
    byte *pix = NULL;
    bool alpha = pie->stencil || pie->passes > 1;

    for (i = 0; i < pie->passes; ++i) {
        int code1 = gx_image_end(pie->inner[i], draw_last);

        if (code1 < 0 && code >= 0)
            code = code1;
        pie->inner[i] = NULL;
    }
    if (code < 0 || !draw_last)
        goto done;
    pix = gs_alloc_bytes(svg_mem(svg), (size_t)n * (alpha ? 4 : 3), "svg image pixels");
    if (pix == NULL) {
        code = gs_note_error(gs_error_VMerror);
        goto done;
    }
    for (y = 0; y < pie->gh; ++y) {
        const byte *a = pie->mdev[0]->line_ptrs[y];
        const byte *b = pie->passes > 1 ? pie->mdev[1]->line_ptrs[y] : NULL;
        byte *q = pix + (size_t)y * pie->gw * (alpha ? 4 : 3);

        for (x = 0; x < pie->gw; ++x, a += 3) {
            if (pie->stencil) {
                q[0] = (byte)(pie->mask_color >> 16);
                q[1] = (byte)(pie->mask_color >> 8);
                q[2] = (byte)pie->mask_color;
                q[3] = (byte)(255 - a[0]);
                q += 4;
            } else if (b) {
                bool painted = a[0] == b[0] && a[1] == b[1] && a[2] == b[2];

                q[0] = a[0]; q[1] = a[1]; q[2] = a[2];
                q[3] = painted ? 255 : 0;
                q += 4;
                b += 3;
            } else {
                q[0] = a[0]; q[1] = a[1]; q[2] = a[2];
                q += 3;
            }
        }
    }
    {
        svg_buf png;
        svg_buf *body = svg->out;

        sb_init(&png, svg_mem(svg));
        code = svg_encode_png(&png, pix, pie->gw, pie->gh, pie->gw * (alpha ? 4 : 3),
                              alpha ? 4 : 3, alpha);
        if (code >= 0) {
            /* Put the scaling in width/height and only rotation, skew
               and translation in the transform, so renderers resample the
               image at its final size. */
            gs_matrix t = pie->gm;
            double ax = hypot(t.xx, t.xy), ay = hypot(t.yx, t.yy);

            t.xx /= ax; t.xy /= ax;
            t.yx /= ay; t.yy /= ay;
            sb_puts(body, "<image");
            if (fabs(t.xx - 1) < 1e-6 && fabs(t.yy - 1) < 1e-6 &&
                fabs(t.xy) < 1e-6 && fabs(t.yx) < 1e-6) {
                sb_puts(body, " x=\"");
                sb_num(body, t.tx, 3);
                sb_puts(body, "\" y=\"");
                sb_num(body, t.ty, 3);
                sb_putc(body, '"');
            } else {
                t.tx = pie->gm.tx; t.ty = pie->gm.ty;
                sb_puts(body, " transform=\"");
                sb_matrix(body, &t);
                sb_putc(body, '"');
            }
            sb_puts(body, " width=\"");
            sb_num(body, pie->gw * ax, 3);
            sb_puts(body, "\" height=\"");
            sb_num(body, pie->gh * ay, 3);
            sb_puts(body, "\" preserveAspectRatio=\"none\"");
            if (pie->pixelated)
            {
                /* The attribute is understood by SVG renderers, the class
                   (styled in svg_write_page) by browsers. */
                sb_puts(body, " class=\"px\" image-rendering=\"optimizeSpeed\"");
                svg->pixelated = true;
            }
            if (svg->Transparency) {
                const char *blend = svg_blend_name(pie->blend_mode);

                if (pie->opacity < 1) {
                    sb_puts(body, " opacity=\"");
                    sb_num(body, pie->opacity < 0 ? 0 : pie->opacity, 4);
                    sb_putc(body, '"');
                }
                if (blend)
                    sb_printf(body, " style=\"mix-blend-mode:%s\"", blend);
            }
            sb_puts(body, " xlink:href=\"data:image/png;base64,");
            sb_base64(body, (const byte *)png.data, png.len);
            sb_puts(body, "\"/>\n");
            code = body->error;
        }
        sb_free(&png);
    }
done:
    if (pix)
        gs_free_object(svg_mem(svg), pix, "svg image pixels");
    svg_image_free(pie);
    return code;
}

/* An enumerator that consumes and discards image data. */
typedef struct svg_null_image_enum_s {
    gx_image_enum_common;
    int rows, height;
} svg_null_image_enum_t;

gs_private_st_suffix_add0(st_svg_null_image_enum, svg_null_image_enum_t,
    "svg_null_image_enum_t", svg_null_image_enum_enum_ptrs,
    svg_null_image_enum_reloc_ptrs, st_gx_image_enum_common);

static int
svg_null_plane_data(gx_image_enum_common_t *info, const gx_image_plane_t *planes,
                    int height, int *rows_used)
{
    svg_null_image_enum_t *pie = (svg_null_image_enum_t *)info;

    *rows_used = height;
    pie->rows += height;
    return pie->rows >= pie->height;
}

static int
svg_null_end_image(gx_image_enum_common_t *info, bool draw_last)
{
    gx_image_free_enum(&info);
    return 0;
}

static const gx_image_enum_procs_t svg_null_image_enum_procs = {
    svg_null_plane_data, svg_null_end_image
};

static const gx_image_enum_procs_t svg_image_enum_procs = {
    svg_image_plane_data, svg_image_end_image, NULL, svg_image_planes_wanted
};

static int
svg_begin_typed_image(gx_device *dev, const gs_gstate *pgs, const gs_matrix *pmat,
                      const gs_image_common_t *pic, const gs_int_rect *prect,
                      const gx_drawing_color *pdcolor, const gx_clip_path *pcpath,
                      gs_memory_t *mem, gx_image_enum_common_t **pinfo)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    const gs_pixel_image_t *pim = (const gs_pixel_image_t *)pic;
    svg_image_enum_t *pie;
    gs_matrix ctm, inv, m, page, g;
    gx_drawing_color black;
    double sx, sy, fx, fy;
    int type = pic->type->index, code, i, W, H;
    bool stencil = false;

    /* ImageType 3 (explicit masks) builds its own mask clipping devices
       and is left to the raster fallback. */
    if (svg->text_invisible || pgs == NULL ||
        (type != 1 && type != 4) || pim->Width <= 0 || pim->Height <= 0)
        goto use_default;
    W = pim->Width;
    H = pim->Height;
    if (prect && (prect->p.x != 0 || prect->p.y != 0 || prect->q.x != W || prect->q.y != H))
        goto use_default;
    if (type == 1 && ((const gs_image1_t *)pic)->ImageMask)
        stencil = true;
    ctm = pmat ? *pmat : ctm_only(pgs);
    if (gs_matrix_invert(&pim->ImageMatrix, &inv) < 0)
        goto use_default;
    gs_matrix_multiply(&inv, &ctm, &m);         /* image space -> device */

    if (stencil && svg->text_enum && !svg->painting_glyph) {
        /* A glyph bitmap from the font rasterizer (drawn as an image mask
           when the color is a pattern): draw the outline instead. */
        gs_rect box;
        gs_point c;
        int k;

        for (k = 0; k < 4; ++k) {
            gs_point_transform((k & 1) ? W : 0, (k & 2) ? H : 0, &m, &c);
            if (k == 0)
                box.p = box.q = c;
            box.p.x = min(box.p.x, c.x); box.p.y = min(box.p.y, c.y);
            box.q.x = max(box.q.x, c.x); box.q.y = max(box.q.y, c.y);
        }
        code = svg_glyph_draw(svg, &box, true, gx_rule_winding_number, pdcolor, pcpath);
        if (code < 0)
            return code;
        if (code > 0) {
            svg_null_image_enum_t *pne = gs_alloc_struct(mem, svg_null_image_enum_t,
                                         &st_svg_null_image_enum, "svg glyph image");

            if (pne == NULL)
                return_error(gs_error_VMerror);
            code = gx_image_enum_common_init((gx_image_enum_common_t *)pne,
                                             (const gs_data_image_t *)pic,
                                             &svg_null_image_enum_procs, dev, 1,
                                             gs_image_format_chunky);
            if (code < 0)
                return code;
            pne->memory = mem;
            pne->rows = 0;
            pne->height = H;
            *pinfo = (gx_image_enum_common_t *)pne;
            return 0;
        }
    }

    if (stencil && !gx_dc_is_pure(pdcolor))
        goto use_default;

    /* Device pixels per image sample along each image axis; keep at most
       two samples per device pixel. */
    sx = hypot(m.xx, m.xy);
    sy = hypot(m.yx, m.yy);
    if (sx <= 0 || sy <= 0)
        goto use_default;
    fx = min(1.0, 2 * sx);
    fy = min(1.0, 2 * sy);
    {
        double gw = ceil(W * fx), gh = ceil(H * fy);

        while (gw * gh > 16e6) {
            gw = ceil(gw / 2);
            gh = ceil(gh / 2);
        }
        fx = gw / W;
        fy = gh / H;
    }

    pie = gs_alloc_struct(mem, svg_image_enum_t, &st_svg_image_enum, "svg_begin_typed_image");
    if (pie == NULL)
        return_error(gs_error_VMerror);
    memset(((byte *)pie) + sizeof(gx_image_enum_common_t), 0,
           sizeof(*pie) - sizeof(gx_image_enum_common_t));
    pie->memory = mem;
    pie->gw = (int)(W * fx + 0.5);
    pie->gh = (int)(H * fy + 0.5);
    pie->stencil = stencil;
    pie->passes = type == 4 ? 2 : 1;
    if (stencil)
        pie->mask_color = gx_dc_pure_color(pdcolor);

    /* grid -> page = scale(1/f) x image -> device x device -> page */
    svg_page_matrix(svg, &page);
    gs_make_scaling(1 / fx, 1 / fy, &g);
    gs_matrix_multiply(&g, &m, &g);
    gs_matrix_multiply(&g, &page, &pie->gm);
    /* Unsmoothed, visibly large samples (as the graphics library would
       draw them): keep them crisp. */
    pie->opacity = pgs->fillconstantalpha;
    pie->blend_mode = pgs->blend_mode;
    pie->pixelated = !pim->Interpolate && hypot(pie->gm.xx, pie->gm.xy) >= 2 &&
        hypot(pie->gm.yx, pie->gm.yy) >= 2;

    /* user space -> grid = ImageMatrix x scale(f) */
    gs_make_scaling(fx, fy, &g);
    gs_matrix_multiply(&pim->ImageMatrix, &g, &g);
    if (stencil)
        set_nonclient_dev_color(&black, 0);
    for (i = 0; i < pie->passes; ++i) {
        gx_device_memory *md = gs_alloc_struct(mem, gx_device_memory, &st_device_memory,
                                               "svg image mdev");

        if (md == NULL) {
            code = gs_note_error(gs_error_VMerror);
            goto fail;
        }
        gs_make_mem_device(md, gdev_mem_device_for_bits(24), mem, -1, dev);
        md->width = pie->gw;
        md->height = pie->gh;
        md->HWResolution[0] = dev->HWResolution[0];
        md->HWResolution[1] = dev->HWResolution[1];
        md->color_info = dev->color_info;
        md->bitmap_memory = mem;
        pie->mdev[i] = md;
        code = dev_proc(md, open_device)((gx_device *)md);
        if (code < 0)
            goto fail;
        code = dev_proc(md, fill_rectangle)((gx_device *)md, 0, 0, pie->gw, pie->gh,
                                            i == 0 ? 0xffffff : 0);
        if (code < 0)
            goto fail;
        code = gx_default_begin_typed_image((gx_device *)md, pgs, &g, pic, NULL,
                                            stencil ? &black : pdcolor, NULL, mem,
                                            &pie->inner[i]);
        if (code < 0)
            goto fail;
    }
    code = gx_image_enum_common_init((gx_image_enum_common_t *)pie,
                                     (const gs_data_image_t *)pic, &svg_image_enum_procs,
                                     dev, 0, gs_image_format_chunky);
    if (code < 0)
        goto fail;
    pie->pgs = pgs;
    pie->pgs_level = pgs->level;
    pie->num_planes = pie->inner[0]->num_planes;
    memcpy(pie->plane_depths, pie->inner[0]->plane_depths, sizeof(pie->plane_depths));
    memcpy(pie->plane_widths, pie->inner[0]->plane_widths, sizeof(pie->plane_widths));
    code = svg_begin_element(svg, pcpath);
    if (code < 0)
        goto fail;
    *pinfo = (gx_image_enum_common_t *)pie;
    return 0;
fail:
    for (i = 0; i < 2; ++i)
        if (pie->inner[i]) {
            gx_image_end(pie->inner[i], false);
            pie->inner[i] = NULL;
        }
    svg_image_free(pie);
    return code;
use_default:
    return gx_default_begin_typed_image(dev, pgs, pmat, pic, prect, pdcolor, pcpath, mem, pinfo);
}

/* ---------------- Transparency ---------------- */

/*
 * The device handles PDF transparency itself, as pdfwrite does, instead of
 * letting the graphics library push the pdf14 compositor (which would
 * flatten everything to a raster image):
 *
 * - constant alpha and blend modes of individual objects become
 *   fill-opacity / stroke-opacity / opacity and mix-blend-mode;
 * - transparency groups become <g> elements with opacity, blend mode and
 *   isolation (groups that need none of these aren't written);
 * - soft masks become <mask> definitions, luminosity or alpha.  As in the
 *   pdf14 compositor, a mask applies to the next group begun at the same
 *   level, and stays active for later groups there until it is replaced.
 *
 * Soft mask transfer functions become a filter on the mask's content.
 * Knockout groups are drawn as ordinary groups.
 */

static const char *
svg_blend_name(gs_blend_mode_t bm)
{
    switch (bm) {
    case BLEND_MODE_Multiply: return "multiply";
    case BLEND_MODE_Screen: return "screen";
    case BLEND_MODE_Overlay: return "overlay";
    case BLEND_MODE_Darken: return "darken";
    case BLEND_MODE_Lighten: return "lighten";
    case BLEND_MODE_ColorDodge: return "color-dodge";
    case BLEND_MODE_ColorBurn: return "color-burn";
    case BLEND_MODE_HardLight: return "hard-light";
    case BLEND_MODE_SoftLight: return "soft-light";
    case BLEND_MODE_Difference: return "difference";
    case BLEND_MODE_Exclusion: return "exclusion";
    case BLEND_MODE_Hue: return "hue";
    case BLEND_MODE_Saturation: return "saturation";
    case BLEND_MODE_Color: return "color";
    case BLEND_MODE_Luminosity: return "luminosity";
    default: return NULL;       /* Normal, Compatible, CompatibleOverprint */
    }
}

/* Write opacity and blend mode attributes for an element painted with
   pgs's transparency state.  what: SVG_PAINT_FILL, _STROKE or _ELEMENT
   (the opacity of the element as a whole, e.g. an image). */

static void
svg_put_transparency(gx_device_svg *svg, const gs_gstate *pgs, int what)
{
    const char *blend;
    double a;

    if (pgs == NULL || !svg->Transparency)
        return;
    a = what == SVG_PAINT_STROKE ? pgs->strokeconstantalpha : pgs->fillconstantalpha;
    if (a < 1) {
        sb_puts(svg->out, what == SVG_PAINT_FILL ? " fill-opacity=\"" :
                what == SVG_PAINT_STROKE ? " stroke-opacity=\"" : " opacity=\"");
        sb_num(svg->out, a < 0 ? 0 : a, 4);
        sb_putc(svg->out, '"');
    }
    blend = svg_blend_name(pgs->blend_mode);
    if (blend)
        sb_printf(svg->out, " style=\"mix-blend-mode:%s\"", blend);
}

static svg_level *
svg_push_level(gx_device_svg *svg)
{
    svg_level *lv;

    if (svg->depth + 1 >= SVG_MAX_LEVELS)
        return NULL;
    lv = &svg->levels[++svg->depth];
    memset(lv, 0, sizeof(*lv));
    lv->parent_out = svg->out;
    return lv;
}

static int
svg_begin_group(gx_device_svg *svg, const gs_pdf14trans_params_t *params, bool page)
{
    int mask = svg->levels[svg->depth].active_mask;
    double opacity = params->opacity * params->shape;
    const char *blend = page ? NULL : svg_blend_name(params->blend_mode);
    bool isolate = params->Isolated && !page;
    svg_level *lv;
    int code = svg_flush_canvas(svg);

    if (code < 0)
        return code;
    svg_close_clip(svg);
    lv = svg_push_level(svg);
    if (lv == NULL)
        return_error(gs_error_limitcheck);
    if (opacity < 1 || mask || blend || isolate) {
        sb_puts(svg->out, "<g");
        if (opacity < 1) {
            sb_puts(svg->out, " opacity=\"");
            sb_num(svg->out, opacity < 0 ? 0 : opacity, 4);
            sb_putc(svg->out, '"');
        }
        if (mask)
            sb_printf(svg->out, " mask=\"url(#m%d)\"", mask);
        if (blend || isolate) {
            sb_puts(svg->out, " style=\"");
            if (blend)
                sb_printf(svg->out, "mix-blend-mode:%s%s", blend, isolate ? ";" : "");
            if (isolate)
                sb_puts(svg->out, "isolation:isolate");
            sb_putc(svg->out, '"');
        }
        sb_puts(svg->out, ">\n");
        lv->wrote_g = true;
    }
    return svg->out->error;
}

static int
svg_end_group(gx_device_svg *svg)
{
    svg_level *lv = &svg->levels[svg->depth];
    int code;

    if (svg->depth == 0 || lv->is_mask)
        return 0;               /* unbalanced: ignore */
    code = svg_flush_canvas(svg);
    if (code < 0)
        return code;
    svg_close_clip(svg);
    if (lv->wrote_g)
        sb_puts(svg->out, "</g>\n");
    svg->depth--;
    return svg->out->error;
}

static int
svg_begin_mask(gx_device_svg *svg, const gs_pdf14trans_params_t *params)
{
    svg_level *lv;
    int code;

    if (params->subtype == TRANSPARENCY_MASK_None) {
        svg->levels[svg->depth].active_mask = 0;
        return 0;
    }
    code = svg_flush_canvas(svg);
    if (code < 0)
        return code;
    svg_close_clip(svg);
    lv = svg_push_level(svg);
    if (lv == NULL)
        return_error(gs_error_limitcheck);
    lv->is_mask = true;
    lv->alpha_mask = params->subtype == TRANSPARENCY_MASK_Alpha;
    lv->backdrop = params->GrayBackground;
    if (!params->function_is_identity) {
        lv->has_tr = true;
        memcpy(lv->tr, params->transfer_fn, sizeof(lv->tr));
    }
    sb_init(&lv->content, svg_mem(svg));
    svg->out = &lv->content;
    svg->mask_depth++;
    return 0;
}

static int
svg_end_mask(gx_device_svg *svg)
{
    svg_level *lv = &svg->levels[svg->depth];
    svg_buf d;
    int code, id, filter = 0, i;

    if (svg->depth == 0 || !lv->is_mask)
        return 0;               /* unbalanced: ignore */
    code = svg_flush_canvas(svg);
    if (code < 0)
        return code;
    svg_close_clip(svg);
    sb_init(&d, svg_mem(svg));
    if (lv->has_tr) {
        /* The transfer function maps the mask's luminosity (or alpha) to
           its value: make the luminosity the alpha, look it up in a table,
           and use the result as an alpha mask. */
        sb_puts(&d, " filterUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"");
        sb_num(&d, svg->width * svg_sx(svg), 3);
        sb_puts(&d, "\" height=\"");
        sb_num(&d, svg->height * svg_sy(svg), 3);
        sb_puts(&d, "\" color-interpolation-filters=\"sRGB\">");
        if (!lv->alpha_mask)
            sb_puts(&d, "<feColorMatrix type=\"luminanceToAlpha\"/>");
        sb_puts(&d, "<feComponentTransfer><feFuncA type=\"table\" tableValues=\"");
        for (i = 0; i <= 32; ++i) {
            if (i)
                sb_putc(&d, ' ');
            sb_num(&d, lv->tr[min(i * 8, 255)] / 255.0, 3);
        }
        sb_puts(&d, "\"/></feComponentTransfer></filter>");
        filter = svg_put_def(svg, "filter", 'f', &d);
        d.len = 0;
        if (filter < 0) {
            sb_free(&d);
            return filter;
        }
    }
    sb_puts(&d, " maskUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"");
    sb_num(&d, svg->width * svg_sx(svg), 3);
    sb_puts(&d, "\" height=\"");
    sb_num(&d, svg->height * svg_sy(svg), 3);
    sb_putc(&d, '"');
    if (lv->alpha_mask || filter)
        sb_puts(&d, " mask-type=\"alpha\"");
    sb_putc(&d, '>');
    if (filter)
        sb_printf(&d, "<g filter=\"url(#f%d)\">", filter);
    if (!lv->alpha_mask && (lv->backdrop > 0 || filter)) {
        /* Where the mask's group paints nothing, its luminosity is the
           backdrop's. */
        int g = (int)(lv->backdrop * 255 + 0.5);

        g = g < 0 ? 0 : g > 255 ? 255 : g;
        sb_puts(&d, "<rect width=\"100%\" height=\"100%\" fill=\"");
        sb_color(&d, ((gx_color_index)g << 16) | (g << 8) | g);
        sb_puts(&d, "\"/>");
    }
    if (lv->content.data)
        sb_write(&d, lv->content.data, lv->content.len);
    if (filter)
        sb_puts(&d, "</g>");
    sb_puts(&d, "</mask>");
    id = svg_put_def(svg, "mask", 'm', &d);
    sb_free(&d);
    sb_free(&lv->content);
    svg->out = lv->parent_out;
    svg->depth--;
    svg->mask_depth--;
    if (id < 0)
        return id;
    svg->levels[svg->depth].active_mask = id;
    return 0;
}

static int
svg_composite(gx_device *dev, gx_device **pcdev, const gs_composite_t *pct,
              gs_gstate *pgs, gs_memory_t *memory, gx_device *cdev)
{
    gx_device_svg *svg = (gx_device_svg *)dev;

    if (svg->Transparency && pct->type->comp_id == GX_COMPOSITOR_PDF14_TRANS) {
        const gs_pdf14trans_params_t *params = &((const gs_pdf14trans_t *)pct)->params;

        /* Handle transparency ourselves: the compositor device is us. */
        *pcdev = dev;
        switch (params->pdf14_op) {
        case PDF14_BEGIN_TRANS_PAGE_GROUP:
            return svg_begin_group(svg, params, true);
        case PDF14_BEGIN_TRANS_GROUP:
            return svg_begin_group(svg, params, false);
        case PDF14_END_TRANS_GROUP:
            return svg_end_group(svg);
        case PDF14_BEGIN_TRANS_MASK:
            return svg_begin_mask(svg, params);
        case PDF14_END_TRANS_MASK:
            return svg_end_mask(svg);
        default:
            /* Device push/pop, text groups, blend parameters (read from the
               graphics state when painting), state push/pop. */
            return 0;
        }
    }
    return gx_default_composite(dev, pcdev, pct, pgs, memory, cdev);
}

/* ---------------- Pages ---------------- */

static void
svg_page_reset(gx_device_svg *svg)
{
    while (svg->depth > 0) {
        if (svg->levels[svg->depth].is_mask)
            sb_free(&svg->levels[svg->depth].content);
        svg->depth--;
    }
    memset(&svg->levels[0], 0, sizeof(svg->levels[0]));
    svg->out = &svg->body;
    svg->mask_depth = 0;
    svg->defs.len = svg->body.len = svg->text.len = 0;
    svg->defs.error = svg->body.error = svg->text.error = 0;
    svg->clip_open = false;
    svg_map_clear(&svg->clips);
    svg_map_clear(&svg->glyphs);
    svg->run.active = false;
    svg->pixelated = false;
    if (svg->canvas && svg->cx1 > svg->cx0) {
        int y;

        for (y = svg->cy0; y < svg->cy1; ++y)
            memset(svg->canvas + ((size_t)y * svg->width + svg->cx0) * 4, 0,
                   (size_t)(svg->cx1 - svg->cx0) * 4);
    }
    svg->cx0 = svg->cy0 = svg->cx1 = svg->cy1 = 0;
    svg->next_id = 0;
}

static int
svg_fillpage(gx_device *dev, gs_gstate *pgs, gx_device_color *pdevc)
{
    gx_device_svg *svg = (gx_device_svg *)dev;

    /* Erasing the page discards everything drawn on it so far. */
    svg_page_reset(svg);
    if (gx_dc_is_pure(pdevc) && gx_dc_pure_color(pdevc) != 0xffffff) {
        sb_puts(svg->out, "<rect width=\"100%\" height=\"100%\" fill=\"");
        sb_color(svg->out, gx_dc_pure_color(pdevc));
        sb_puts(svg->out, "\"/>\n");
    }
    return 0;
}

static int
svg_write_page(gx_device_svg *svg)
{
    gx_device_vector *vdev = (gx_device_vector *)svg;
    double w = svg->width * svg_sx(svg), h = svg->height * svg_sy(svg);
    svg_buf hdr;
    gp_file *f;
    int code;

    code = svg_flush_canvas(svg);
    if (code < 0)
        return code;
    svg_close_clip(svg);
    svg_flush_run(svg);
    /* Close any groups the job left open. */
    while (svg->depth > 0) {
        svg_level *lv = &svg->levels[svg->depth];

        if (lv->is_mask) {
            sb_free(&lv->content);
            svg->mask_depth--;
        } else if (lv->wrote_g)
            sb_puts(lv->parent_out, "</g>\n");
        svg->out = lv->parent_out;
        svg->depth--;
    }
    if (svg->defs.error || svg->body.error || svg->text.error)
        return_error(gs_error_VMerror);

    if (svg->pages_written > 0 && !gx_outputfile_is_separate_pages(vdev->fname, svg->memory)) {
        if (!svg->warned_multipage) {
            emprintf(svg->memory,
                     "   **** Warning: the svg device writes one page per file; use %%d in the\n"
                     "   **** output file name (e.g. -o page-%%d.svg). Further pages are not written.\n");
            svg->warned_multipage = true;
        }
        return 0;
    }

    sb_init(&hdr, svg_mem(svg));
    sb_puts(&hdr, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" "
            "xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" width=\"");
    sb_num(&hdr, w, 3);
    sb_puts(&hdr, "pt\" height=\"");
    sb_num(&hdr, h, 3);
    sb_puts(&hdr, "pt\" viewBox=\"0 0 ");
    sb_num(&hdr, w, 3);
    sb_putc(&hdr, ' ');
    sb_num(&hdr, h, 3);
    sb_puts(&hdr, "\">\n");
    if (svg->pixelated)
        sb_puts(&hdr, "<style>@supports (image-rendering:pixelated){.px{image-rendering:pixelated}}</style>\n");
    if (svg->defs.len)
        sb_puts(&hdr, "<defs>\n");
    if (hdr.error) {
        sb_free(&hdr);
        return hdr.error;
    }

    code = gx_device_open_output_file((gx_device *)svg, vdev->fname, true, false, &f);
    if (code < 0) {
        sb_free(&hdr);
        return code;
    }
    gp_fwrite(hdr.data, 1, hdr.len, f);
    sb_free(&hdr);
    if (svg->defs.len) {
        gp_fwrite(svg->defs.data, 1, svg->defs.len, f);
        gp_fputs("</defs>\n", f);
    }
    if (svg->body.len)
        gp_fwrite(svg->body.data, 1, svg->body.len, f);
    if (svg->text.len) {
        gp_fputs("<g fill-opacity=\"0\" font-family=\"sans-serif\" xml:space=\"preserve\">\n", f);
        gp_fwrite(svg->text.data, 1, svg->text.len, f);
        gp_fputs("</g>\n", f);
    }
    gp_fputs("</svg>\n", f);
    code = gx_device_close_output_file((gx_device *)svg, vdev->fname, f);
    svg->pages_written++;
    return code;
}

static int
svg_output_page(gx_device *dev, int num_copies, int flush)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    int code = svg_write_page(svg);

    svg_page_reset(svg);
    if (code < 0)
        return code;
    return gx_finish_output_page(dev, num_copies, flush);
}

static int
svg_open_device(gx_device *dev)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    gx_device_vector *vdev = (gx_device_vector *)dev;
    gs_memory_t *mem = svg_mem(svg);

    vdev->v_memory = dev->memory;
    vdev->vec_procs = &svg_vector_procs;
    gdev_vector_init(vdev);
    sb_init(&svg->defs, mem);
    sb_init(&svg->body, mem);
    svg->out = &svg->body;
    sb_init(&svg->text, mem);
    sb_init(&svg->run.xs, mem);
    sb_init(&svg->run.chars, mem);
    svg_map_init(&svg->clips, mem);
    svg_map_init(&svg->glyphs, mem);
    svg->canvas = NULL;
    svg->text_enum = NULL;
    svg->text_invisible = false;
    svg->pages_written = 0;
    svg->warned_multipage = false;
    svg->depth = 0;
    svg_page_reset(svg);
    return 0;
}

static int
svg_close_device(gx_device *dev)
{
    gx_device_svg *svg = (gx_device_svg *)dev;

    svg_page_reset(svg);
    sb_free(&svg->defs);
    sb_free(&svg->body);
    sb_free(&svg->text);
    sb_free(&svg->run.xs);
    sb_free(&svg->run.chars);
    svg_map_clear(&svg->clips);
    svg_map_clear(&svg->glyphs);
    if (svg->canvas)
        gs_free_object(svg_mem(svg), svg->canvas, "svg canvas");
    svg->canvas = NULL;
    return 0;
}

/* ---------------- Parameters ---------------- */

static int
svg_get_params(gx_device *dev, gs_param_list *plist)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    int code = gdev_vector_get_params(dev, plist);

    if (code < 0)
        return code;
    if ((code = param_write_bool(plist, "SVGTextLayer", &svg->TextLayer)) < 0)
        return code;
    if ((code = param_write_bool(plist, "SVGTransparency", &svg->Transparency)) < 0)
        return code;
    return param_write_bool(plist, "SVGReuseGlyphs", &svg->ReuseGlyphs);
}

static int
svg_put_params(gx_device *dev, gs_param_list *plist)
{
    gx_device_svg *svg = (gx_device_svg *)dev;
    gx_device_vector *vdev = (gx_device_vector *)dev;
    bool text_layer = svg->TextLayer, reuse = svg->ReuseGlyphs, transparency = svg->Transparency;
    int code, ecode = 0;

    if ((code = param_read_bool(plist, "SVGTextLayer", &text_layer)) < 0)
        param_signal_error(plist, "SVGTextLayer", ecode = code);
    if ((code = param_read_bool(plist, "SVGReuseGlyphs", &reuse)) < 0)
        param_signal_error(plist, "SVGReuseGlyphs", ecode = code);
    if ((code = param_read_bool(plist, "SVGTransparency", &transparency)) < 0)
        param_signal_error(plist, "SVGTransparency", ecode = code);
    if (ecode < 0)
        return ecode;
    code = gdev_vector_put_params(dev, plist);
    if (code < 0)
        return code;
    /* We open one file per page ourselves; don't keep one the superclass
       may have opened after an OutputFile change. */
    if (vdev->strm != NULL || vdev->file != NULL)
        gdev_vector_close_file(vdev);
    svg->TextLayer = text_layer;
    svg->ReuseGlyphs = reuse;
    svg->Transparency = transparency;
    return 0;
}

static int
svg_dev_spec_op(gx_device *dev, int op, void *data, int size)
{
    gx_device_svg *svg = (gx_device_svg *)dev;

    switch (op) {
    case gxdso_supports_hlcolor:
        /* rectfill sends unrounded rectangles to fill_rectangle_hl_color */
        return 1;
    case gxdso_pattern_can_accum:
        /* Asked with no pattern while we track text, this comes from the
           font rasterizer drawing a glyph in a shading color: say yes, so
           the glyph mask comes to fill_mask (and becomes an outline filled
           with a gradient) instead of to a clip path accumulator.  We don't
           accumulate real patterns. */
        if (data == NULL && svg->text_enum && !svg->painting_glyph)
            return 1;
        return 0;
    case gxdso_get_dev_param:
        {
            dev_param_req_t *request = (dev_param_req_t *)data;

            if (strcmp(request->Param, "SVGTextLayer") == 0)
                return param_write_bool(request->list, "SVGTextLayer", &svg->TextLayer);
            if (strcmp(request->Param, "SVGReuseGlyphs") == 0)
                return param_write_bool(request->list, "SVGReuseGlyphs", &svg->ReuseGlyphs);
            if (strcmp(request->Param, "SVGTransparency") == 0)
                return param_write_bool(request->list, "SVGTransparency", &svg->Transparency);
        }
        break;
    }
    return gdev_vector_dev_spec_op(dev, op, data, size);
}
