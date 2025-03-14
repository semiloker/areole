/*
 * areole against Win32 GDI.
 * SPDX-License-Identifier: MIT
 *
 * The comparison a prospective user cares about most: what does the operating
 * system charge for the same drawing?
 *
 * Both engines write into the same DIB section. GDI is in its native best case
 * -- a 32 bpp top-down BI_RGB section selected into a memory DC, which is what
 * every Win32 application that draws off-screen uses -- and areole writes into
 * exactly the same memory. Neither is helped by where the pixels live.
 *
 * Each side uses its own idiomatic fast path. GDI gets cached brushes and a
 * cached font, because a real application caches them; recreating one per call
 * would be a strawman. areole gets ar_fill_rect and ar_draw_text, which is all
 * it has.
 *
 * Compiled as gnu89: it includes <windows.h>.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "compare.h"

#include <string.h>

typedef struct gdi_state
{
    HDC     dc;
    HBITMAP dib;
    HBITMAP prev;
    HBRUSH  solid;
    HBRUSH  blend_src;
    HFONT   font;
} gdi_state;

static gdi_state g_gdi;

/* The same 500 rectangles for both engines, at the same coordinates, from the
   same deterministic sequence. */
#define RECTS 500
static ar_i32 g_rx[RECTS], g_ry[RECTS];

static ar_rect whole(const cmp_ctx *c)
{
    return ar_rect_make(0, 0, c->w, c->h);
}

static int gdi_setup(cmp_ctx *c)
{
    BITMAPINFO bi;
    HDC        screen;
    void      *bits = 0;
    int        i;

    memset(&g_gdi, 0, sizeof g_gdi);

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)c->w;
    bi.bmiHeader.biHeight = -(LONG)c->h; /* top down, as areole uses */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    g_gdi.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!g_gdi.dib || !bits)
    {
        return 0;
    }

    g_gdi.dc = CreateCompatibleDC(NULL);
    if (!g_gdi.dc)
    {
        return 0;
    }
    g_gdi.prev = (HBITMAP)SelectObject(g_gdi.dc, g_gdi.dib);

    /* Cached, because an application caches. */
    g_gdi.solid = CreateSolidBrush(RGB(0x33, 0x66, 0x99));
    g_gdi.blend_src = CreateSolidBrush(RGB(0x33, 0x66, 0x99));
    g_gdi.font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
    SelectObject(g_gdi.dc, g_gdi.font);
    SetBkMode(g_gdi.dc, TRANSPARENT);
    SetTextColor(g_gdi.dc, RGB(0x2B, 0x2B, 0x2B));

    /* Both engines draw into these same pixels. */
    c->surface.pixels = (ar_u32 *)bits;
    c->surface.w = c->w;
    c->surface.h = c->h;
    c->surface.stride = c->w;
    c->native = &g_gdi;

    bench_srand(0x1234ABCDu);
    for (i = 0; i < RECTS; ++i)
    {
        g_rx[i] = (ar_i32)(bench_rand() % (ar_u32)(c->w - 64));
        g_ry[i] = (ar_i32)(bench_rand() % (ar_u32)(c->h - 64));
    }
    return 1;
}

static void gdi_teardown(cmp_ctx *c)
{
    (void)c;
    if (g_gdi.dc)
    {
        if (g_gdi.prev)
        {
            SelectObject(g_gdi.dc, g_gdi.prev);
        }
        DeleteDC(g_gdi.dc);
    }
    if (g_gdi.dib)
    {
        DeleteObject(g_gdi.dib);
    }
    if (g_gdi.solid)
    {
        DeleteObject(g_gdi.solid);
    }
    if (g_gdi.blend_src)
    {
        DeleteObject(g_gdi.blend_src);
    }
    if (g_gdi.font)
    {
        DeleteObject(g_gdi.font);
    }
    memset(&g_gdi, 0, sizeof g_gdi);
}

/* ------------------------------------------------------------------------
 * Clearing the whole surface
 * ------------------------------------------------------------------------ */
static void a_clear(cmp_ctx *c)
{
    ar_surface_clear(&c->surface, AR_HEX(0x336699));
}

static void g_clear(cmp_ctx *c)
{
    RECT r;
    (void)c;
    r.left = 0;
    r.top = 0;
    r.right = c->w;
    r.bottom = c->h;
    FillRect(g_gdi.dc, &r, g_gdi.solid);
}

/* ------------------------------------------------------------------------
 * Five hundred opaque rectangles
 * ------------------------------------------------------------------------ */
static void a_fill(cmp_ctx *c)
{
    ar_rect clip = whole(c);
    int     i;

    for (i = 0; i < RECTS; ++i)
    {
        ar_fill_rect(&c->surface, ar_rect_make(g_rx[i], g_ry[i], 64, 64), clip,
                     AR_RGBA(0x33, 0x66, 0x99, 0xFF));
    }
}

static void g_fill(cmp_ctx *c)
{
    RECT r;
    int  i;
    (void)c;

    for (i = 0; i < RECTS; ++i)
    {
        r.left = g_rx[i];
        r.top = g_ry[i];
        r.right = g_rx[i] + 64;
        r.bottom = g_ry[i] + 64;
        FillRect(g_gdi.dc, &r, g_gdi.solid);
    }
}

/* ------------------------------------------------------------------------
 * Five hundred translucent rectangles
 *
 * GDI's only per-pixel alpha is AlphaBlend from msimg32, blending a source
 * bitmap. areole blends a solid colour directly. That is not quite the same
 * operation, so the caveat is stated rather than hidden: GDI is doing strictly
 * more work by reading a source surface areole does not need.
 * ------------------------------------------------------------------------ */
static HBITMAP g_src_dib;
static HDC     g_src_dc;

static void ensure_blend_source(void)
{
    BITMAPINFO bi;
    void      *bits = 0;
    HDC        screen;
    int        i;

    if (g_src_dib)
    {
        return;
    }
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 64;
    bi.bmiHeader.biHeight = -64;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    g_src_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!g_src_dib)
    {
        return;
    }
    g_src_dc = CreateCompatibleDC(NULL);
    SelectObject(g_src_dc, g_src_dib);

    /* Premultiplied by 0x80, which is what AV_ALPHAFORMAT requires. */
    for (i = 0; i < 64 * 64; ++i)
    {
        ((ar_u32 *)bits)[i] = 0x8019804Cu;
    }
}

static void a_blend(cmp_ctx *c)
{
    ar_rect clip = whole(c);
    int     i;

    for (i = 0; i < RECTS; ++i)
    {
        ar_fill_rect(&c->surface, ar_rect_make(g_rx[i], g_ry[i], 64, 64), clip,
                     AR_RGBA(0x33, 0x66, 0x99, 0x80));
    }
}

static void g_blend(cmp_ctx *c)
{
    BLENDFUNCTION bf;
    int           i;
    (void)c;

    ensure_blend_source();
    if (!g_src_dc)
    {
        return;
    }

    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    for (i = 0; i < RECTS; ++i)
    {
        AlphaBlend(g_gdi.dc, g_rx[i], g_ry[i], 64, 64, g_src_dc, 0, 0, 64, 64, bf);
    }
}

/* ------------------------------------------------------------------------
 * Text
 *
 * The least fair case in the set, and the caveat says so plainly. GDI renders
 * a hinted, kerned, antialiased outline face through the system font stack;
 * areole blits an 8x8 bitmap. GDI is doing enormously more work and produces
 * enormously better output. The number is worth having anyway, because it says
 * what the ceiling costs -- and because 0.2.0 replaces areole's side with an
 * outline rasterizer and this becomes a fair fight.
 * ------------------------------------------------------------------------ */
static const char *const LINE =
    "The quick brown fox jumps over the lazy dog 0123456789 -- and again.";

#define LINES 24

static void a_text(cmp_ctx *c)
{
    ar_rect clip = whole(c);
    int     i;

    for (i = 0; i < LINES; ++i)
    {
        ar_draw_text(&c->surface, clip, 8, 8 + i * 12, LINE, 1, AR_HEX(0x2B2B2B));
    }
}

static void g_text(cmp_ctx *c)
{
    int i;
    int len = (int)strlen(LINE);
    (void)c;

    for (i = 0; i < LINES; ++i)
    {
        TextOutA(g_gdi.dc, 8, 8 + i * 12, LINE, len);
    }
}

/* ------------------------------------------------------------------------
 * Hairlines
 * ------------------------------------------------------------------------ */
static void a_hair(cmp_ctx *c)
{
    ar_rect clip = whole(c);
    ar_i32  y;

    for (y = 0; y < c->h; y += 4)
    {
        ar_fill_rect(&c->surface, ar_rect_make(0, y, c->w, 1), clip, AR_HEX(0xE8DFCC));
    }
}

static void g_hair(cmp_ctx *c)
{
    RECT   r;
    ar_i32 y;
    (void)c;

    for (y = 0; y < c->h; y += 4)
    {
        r.left = 0;
        r.top = y;
        r.right = c->w;
        r.bottom = y + 1;
        FillRect(g_gdi.dc, &r, g_gdi.solid);
    }
}

/* ------------------------------------------------------------------------ */
static const cmp_case CASES[] = {
    {"clear", "one opaque fill of the whole surface", 0, a_clear, g_clear, 0, 0},
    {"fill_500", "500 opaque 64x64 rectangles", 0, a_fill, g_fill, 0, 0},
    {"blend_500", "500 translucent 64x64 rectangles",
     "GDI has no solid-colour alpha fill, so AlphaBlend reads a source surface areole does not "
     "need. GDI is doing strictly more work here.",
     a_blend, g_blend, 0, 0},
    {"text_24_lines", "24 lines of latin text",
     "Not a fair fight and not meant to be. GDI renders a hinted, kerned, antialiased outline "
     "face; areole blits an 8x8 bitmap. This measures the ceiling, and becomes a real comparison "
     "when 0.2.0 gives areole outlines.",
     a_text, g_text, 0, 0},
    {"hairlines", "150 one-pixel-high fills", 0, a_hair, g_hair, 0, 0}};

static const cmp_case *gdi_cases(int *count)
{
    *count = (int)(sizeof CASES / sizeof CASES[0]);
    return CASES;
}

static const cmp_engine GDI = {"Win32 GDI", "the system one, whatever this Windows ships",
                               gdi_setup, gdi_teardown, gdi_cases};

const cmp_engine *cmp_engine_gdi(void)
{
    return &GDI;
}
