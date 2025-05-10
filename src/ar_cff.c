/*
 * areole - CFF outlines and the Type 2 charstring interpreter.
 * SPDX-License-Identifier: MIT
 *
 * Most fonts shipped this century carry PostScript outlines in a CFF table
 * rather than quadratics in glyf, so refusing them means refusing most of the
 * corpus. The shapes are cubic, and the description is a program rather than a
 * list of points: a charstring is a stack machine with subroutines, and a
 * glyph is drawn by running it.
 *
 * That makes this the one place in the library that executes data from a file,
 * which is worth naming plainly. Three bounds hold it: every read goes through
 * the same checked accessors as the rest of the parser, subroutine recursion is
 * capped at ten deep, and the operand stack is fixed at the 48 entries the
 * specification allows and is checked on every push. A charstring that tries to
 * escape any of them stops and the glyph comes out unfinished, which is the
 * correct outcome for a malformed font.
 */
#include "ar_font_file.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * INDEX structures
 *
 * A CFF INDEX is a count, an offset size, count+1 offsets, then the data. It
 * is the container for names, dictionaries, charstrings and subroutines alike.
 * ------------------------------------------------------------------------ */
/* ar_cff_index lives in ar_font_file.h: the face holds three of them. */

static ar_u32 ar__cff_u8(const ar_face *f, ar_u32 off)
{
    return off < f->size ? f->data[off] : 0u;
}

static ar_u32 ar__cff_u16(const ar_face *f, ar_u32 off)
{
    if (off + 1 >= f->size)
    {
        return 0;
    }
    return ((ar_u32)f->data[off] << 8) | f->data[off + 1];
}

static ar_u32 ar__cff_offset(const ar_face *f, ar_u32 at, ar_u32 size)
{
    ar_u32 v = 0, i;

    if (size < 1 || size > 4)
    {
        return 0;
    }
    for (i = 0; i < size; ++i)
    {
        v = (v << 8) | ar__cff_u8(f, at + i);
    }
    return v;
}

static int ar__cff_read_index(const ar_face *f, ar_u32 at, ar_cff_index *ix)
{
    ar_u32 last;

    memset(ix, 0, sizeof *ix);
    if (at + 2 > f->size)
    {
        return 0;
    }
    ix->count = ar__cff_u16(f, at);
    if (ix->count == 0)
    {
        ix->end = at + 2;
        return 1; /* an empty INDEX is two bytes and entirely legal */
    }

    ix->off_size = ar__cff_u8(f, at + 2);
    if (ix->off_size < 1 || ix->off_size > 4)
    {
        return 0;
    }
    ix->offsets = at + 3;

    /* The offset array must fit before the data can be located at all. */
    if (ix->offsets + (ix->count + 1) * ix->off_size > f->size)
    {
        return 0;
    }
    ix->data = ix->offsets + (ix->count + 1) * ix->off_size - 1;

    last = ar__cff_offset(f, ix->offsets + ix->count * ix->off_size, ix->off_size);
    if (last == 0 || ix->data + last > f->size)
    {
        return 0;
    }
    ix->end = ix->data + last;
    return 1;
}

static int ar__cff_entry(const ar_face *f, const ar_cff_index *ix, ar_u32 i, ar_u32 *start,
                         ar_u32 *end)
{
    ar_u32 a, b;

    if (i >= ix->count)
    {
        return 0;
    }
    a = ar__cff_offset(f, ix->offsets + i * ix->off_size, ix->off_size);
    b = ar__cff_offset(f, ix->offsets + (i + 1) * ix->off_size, ix->off_size);
    if (b < a || ix->data + b > f->size)
    {
        return 0;
    }
    *start = ix->data + a;
    *end = ix->data + b;
    return 1;
}

/* ------------------------------------------------------------------------
 * DICT
 *
 * Operands then an operator, repeatedly. Only the handful of keys that decide
 * where the outlines are get read; everything else is stepped over.
 * ------------------------------------------------------------------------ */
static int ar__cff_dict_get(const ar_face *f, ar_u32 at, ar_u32 end, ar_u32 op, ar_i32 *out,
                            ar_i32 want)
{
    ar_i32 stack[48];
    ar_i32 sp = 0;

    while (at < end)
    {
        ar_u32 b0 = ar__cff_u8(f, at);

        if (b0 <= 21)
        {
            ar_u32 key = b0;
            ++at;
            if (b0 == 12)
            {
                key = 1200u + ar__cff_u8(f, at);
                ++at;
            }
            if (key == op && sp >= want)
            {
                ar_i32 i;
                for (i = 0; i < want; ++i)
                {
                    /* The last `want` operands are the ones that belong to
                       this operator. */
                    out[i] = stack[sp - want + i];
                }
                return 1;
            }
            sp = 0;
            continue;
        }

        if (sp >= 48)
        {
            return 0;
        }

        if (b0 == 28)
        {
            ar_i32 v = (ar_i32)ar__cff_u16(f, at + 1);
            stack[sp++] = v >= 32768 ? v - 65536 : v;
            at += 3;
        }
        else if (b0 == 29)
        {
            ar_u32 v = ((ar_u32)ar__cff_u8(f, at + 1) << 24) | ((ar_u32)ar__cff_u8(f, at + 2) << 16) |
                       ((ar_u32)ar__cff_u8(f, at + 3) << 8) | ar__cff_u8(f, at + 4);
            stack[sp++] = (ar_i32)v;
            at += 5;
        }
        else if (b0 == 30)
        {
            /* A real number, nibble encoded. Nothing this reads is a real
               number, so it is stepped over rather than decoded. */
            ++at;
            while (at < end)
            {
                ar_u32 b = ar__cff_u8(f, at++);
                if ((b & 0x0Fu) == 0x0Fu || (b & 0xF0u) == 0xF0u)
                {
                    break;
                }
            }
            stack[sp++] = 0;
        }
        else if (b0 >= 32 && b0 <= 246)
        {
            stack[sp++] = (ar_i32)b0 - 139;
            ++at;
        }
        else if (b0 >= 247 && b0 <= 250)
        {
            stack[sp++] = ((ar_i32)b0 - 247) * 256 + (ar_i32)ar__cff_u8(f, at + 1) + 108;
            at += 2;
        }
        else if (b0 >= 251 && b0 <= 254)
        {
            stack[sp++] = -(((ar_i32)b0 - 251) * 256) - (ar_i32)ar__cff_u8(f, at + 1) - 108;
            at += 2;
        }
        else
        {
            return 0; /* reserved: the dictionary is not one */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Type 2 charstrings
 * ------------------------------------------------------------------------ */
#define CS_MAX_STACK 48
#define CS_MAX_DEPTH 10

typedef struct cs_ctx
{
    const ar_face *f;
    ar_path       *path;
    ar_i32         st[CS_MAX_STACK];
    ar_i32         sp;
    ar_i32         x, y;   /* current point, font units */
    ar_i32         nstems; /* for hintmask's operand bytes */
    int            width_parsed;
    int            open;
    ar_i32         ppem;
    ar_i32         ox, oy;
    ar_i32         depth;
} cs_ctx;

/* Font units to 26.6 pixels, y flipped, offset applied. */
static ar_i32 cs_px(const cs_ctx *c, ar_i32 v)
{
    return ar_face_scale(c->f, v, c->ppem);
}

static void cs_moveto(cs_ctx *c)
{
    if (c->open)
    {
        ar_path_close(c->path);
    }
    ar_path_move_to(c->path, c->ox + cs_px(c, c->x), c->oy - cs_px(c, c->y));
    c->open = 1;
}

static void cs_lineto(cs_ctx *c)
{
    ar_path_line_to(c->path, c->ox + cs_px(c, c->x), c->oy - cs_px(c, c->y));
}

static void cs_curveto(cs_ctx *c, ar_i32 x1, ar_i32 y1, ar_i32 x2, ar_i32 y2)
{
    ar_path_cubic_to(c->path, c->ox + cs_px(c, x1), c->oy - cs_px(c, y1), c->ox + cs_px(c, x2),
                     c->oy - cs_px(c, y2), c->ox + cs_px(c, c->x), c->oy - cs_px(c, c->y));
}

/* The specification's bias: small subroutine arrays index from a smaller
   origin, so that the common single-byte operand range reaches them. */
static ar_i32 cs_bias(ar_u32 count)
{
    if (count < 1240u)
    {
        return 107;
    }
    if (count < 33900u)
    {
        return 1131;
    }
    return 32768;
}

static void cs_run(cs_ctx *c, ar_u32 at, ar_u32 end);

static void cs_call(cs_ctx *c, const ar_cff_index *ix, ar_i32 idx)
{
    ar_u32 s, e;

    if (c->depth >= CS_MAX_DEPTH)
    {
        return;
    }
    if (idx < 0 || !ar__cff_entry(c->f, ix, (ar_u32)idx, &s, &e))
    {
        return;
    }
    ++c->depth;
    cs_run(c, s, e);
    --c->depth;
}

static void cs_run(cs_ctx *c, ar_u32 at, ar_u32 end)
{
    const ar_face *f = c->f;

    while (at < end)
    {
        ar_u32 b0 = ar__cff_u8(f, at);
        ar_i32 i;

        /* Operands. */
        if (b0 >= 32 || b0 == 28)
        {
            ar_i32 v;
            if (b0 == 28)
            {
                ar_i32 raw = (ar_i32)ar__cff_u16(f, at + 1);
                v = raw >= 32768 ? raw - 65536 : raw;
                at += 3;
            }
            else if (b0 <= 246)
            {
                v = (ar_i32)b0 - 139;
                ++at;
            }
            else if (b0 <= 250)
            {
                v = ((ar_i32)b0 - 247) * 256 + (ar_i32)ar__cff_u8(f, at + 1) + 108;
                at += 2;
            }
            else if (b0 <= 254)
            {
                v = -(((ar_i32)b0 - 251) * 256) - (ar_i32)ar__cff_u8(f, at + 1) - 108;
                at += 2;
            }
            else
            {
                /* 16.16 fixed. Rounded to an integer font unit, which is the
                   resolution everything downstream has anyway. */
                ar_i32 hi = (ar_i32)ar__cff_u16(f, at + 1);
                if (hi >= 32768)
                {
                    hi -= 65536;
                }
                v = hi;
                at += 5;
            }
            if (c->sp < CS_MAX_STACK)
            {
                c->st[c->sp++] = v;
            }
            continue;
        }

        ++at;
        switch (b0)
        {
        case 1:  /* hstem   */
        case 3:  /* vstem   */
        case 18: /* hstemhm */
        case 23: /* vstemhm */
            c->nstems += c->sp / 2;
            c->sp = 0;
            break;

        case 19: /* hintmask */
        case 20: /* cntrmask */
            /* An implicit vstem: any operands still on the stack are stem
               hints, and the mask that follows is one bit per hint. Getting
               this wrong desynchronises the whole charstring, which is why it
               is the classic CFF bug. */
            c->nstems += c->sp / 2;
            c->sp = 0;
            at += (ar_u32)((c->nstems + 7) / 8);
            break;

        case 21: /* rmoveto */
            i = c->sp >= 2 ? c->sp - 2 : 0;
            if (c->sp > 2)
            {
                c->width_parsed = 1;
            }
            c->x += c->st[i];
            c->y += c->st[i + 1];
            cs_moveto(c);
            c->sp = 0;
            break;

        case 22: /* hmoveto */
            i = c->sp >= 1 ? c->sp - 1 : 0;
            c->x += c->st[i];
            cs_moveto(c);
            c->sp = 0;
            break;

        case 4: /* vmoveto */
            i = c->sp >= 1 ? c->sp - 1 : 0;
            c->y += c->st[i];
            cs_moveto(c);
            c->sp = 0;
            break;

        case 5: /* rlineto */
            for (i = 0; i + 1 < c->sp; i += 2)
            {
                c->x += c->st[i];
                c->y += c->st[i + 1];
                cs_lineto(c);
            }
            c->sp = 0;
            break;

        case 6: /* hlineto */
        case 7: /* vlineto */
        {
            int horizontal = (b0 == 6);
            for (i = 0; i < c->sp; ++i)
            {
                if (horizontal)
                {
                    c->x += c->st[i];
                }
                else
                {
                    c->y += c->st[i];
                }
                cs_lineto(c);
                horizontal = !horizontal;
            }
            c->sp = 0;
            break;
        }

        case 8: /* rrcurveto */
            for (i = 0; i + 5 < c->sp; i += 6)
            {
                ar_i32 x1 = c->x + c->st[i], y1 = c->y + c->st[i + 1];
                ar_i32 x2 = x1 + c->st[i + 2], y2 = y1 + c->st[i + 3];
                c->x = x2 + c->st[i + 4];
                c->y = y2 + c->st[i + 5];
                cs_curveto(c, x1, y1, x2, y2);
            }
            c->sp = 0;
            break;

        case 24: /* rcurveline */
            for (i = 0; i + 5 < c->sp - 2; i += 6)
            {
                ar_i32 x1 = c->x + c->st[i], y1 = c->y + c->st[i + 1];
                ar_i32 x2 = x1 + c->st[i + 2], y2 = y1 + c->st[i + 3];
                c->x = x2 + c->st[i + 4];
                c->y = y2 + c->st[i + 5];
                cs_curveto(c, x1, y1, x2, y2);
            }
            if (i + 1 < c->sp)
            {
                c->x += c->st[i];
                c->y += c->st[i + 1];
                cs_lineto(c);
            }
            c->sp = 0;
            break;

        case 25: /* rlinecurve */
            for (i = 0; i + 1 < c->sp - 6; i += 2)
            {
                c->x += c->st[i];
                c->y += c->st[i + 1];
                cs_lineto(c);
            }
            if (i + 5 < c->sp)
            {
                ar_i32 x1 = c->x + c->st[i], y1 = c->y + c->st[i + 1];
                ar_i32 x2 = x1 + c->st[i + 2], y2 = y1 + c->st[i + 3];
                c->x = x2 + c->st[i + 4];
                c->y = y2 + c->st[i + 5];
                cs_curveto(c, x1, y1, x2, y2);
            }
            c->sp = 0;
            break;

        case 26: /* vvcurveto */
        case 27: /* hhcurveto */
        {
            ar_i32 d = 0;
            i = 0;
            if (c->sp & 1)
            {
                d = c->st[0];
                i = 1;
            }
            for (; i + 3 < c->sp; i += 4)
            {
                ar_i32 x1, y1, x2, y2;
                if (b0 == 26)
                {
                    x1 = c->x + d;
                    y1 = c->y + c->st[i];
                    x2 = x1 + c->st[i + 1];
                    y2 = y1 + c->st[i + 2];
                    c->x = x2;
                    c->y = y2 + c->st[i + 3];
                }
                else
                {
                    x1 = c->x + c->st[i];
                    y1 = c->y + d;
                    x2 = x1 + c->st[i + 1];
                    y2 = y1 + c->st[i + 2];
                    c->x = x2 + c->st[i + 3];
                    c->y = y2;
                }
                d = 0;
                cs_curveto(c, x1, y1, x2, y2);
            }
            c->sp = 0;
            break;
        }

        case 30: /* vhcurveto */
        case 31: /* hvcurveto */
        {
            int horizontal = (b0 == 31);
            i = 0;
            while (i + 3 < c->sp)
            {
                ar_i32 x1, y1, x2, y2;
                int    last = (i + 8 > c->sp);
                if (horizontal)
                {
                    x1 = c->x + c->st[i];
                    y1 = c->y;
                    x2 = x1 + c->st[i + 1];
                    y2 = y1 + c->st[i + 2];
                    c->y = y2 + c->st[i + 3];
                    c->x = x2 + ((last && i + 4 < c->sp) ? c->st[i + 4] : 0);
                }
                else
                {
                    x1 = c->x;
                    y1 = c->y + c->st[i];
                    x2 = x1 + c->st[i + 1];
                    y2 = y1 + c->st[i + 2];
                    c->x = x2 + c->st[i + 3];
                    c->y = y2 + ((last && i + 4 < c->sp) ? c->st[i + 4] : 0);
                }
                cs_curveto(c, x1, y1, x2, y2);
                horizontal = !horizontal;
                i += 4;
            }
            c->sp = 0;
            break;
        }

        case 10: /* callsubr */
            if (c->sp > 0)
            {
                ar_i32 idx = c->st[--c->sp] + cs_bias(f->cff_subrs.count);
                cs_call(c, &f->cff_subrs, idx);
            }
            break;

        case 29: /* callgsubr */
            if (c->sp > 0)
            {
                ar_i32 idx = c->st[--c->sp] + cs_bias(f->cff_gsubrs.count);
                cs_call(c, &f->cff_gsubrs, idx);
            }
            break;

        case 11: /* return */
            return;

        case 14: /* endchar */
            if (c->open)
            {
                ar_path_close(c->path);
                c->open = 0;
            }
            return;

        case 12: /* escape: flex and arithmetic */
        {
            ar_u32 b1 = ar__cff_u8(f, at);
            ++at;
            /* The four flex operators describe a pair of curves that are
               nearly a line. Drawing them as two curves is exactly right; the
               flex depth argument only matters to a hinting engine. */
            if (b1 == 35 && c->sp >= 13) /* flex */
            {
                ar_i32 x1 = c->x + c->st[0], y1 = c->y + c->st[1];
                ar_i32 x2 = x1 + c->st[2], y2 = y1 + c->st[3];
                ar_i32 x3 = x2 + c->st[4], y3 = y2 + c->st[5];
                ar_i32 x4 = x3 + c->st[6], y4 = y3 + c->st[7];
                ar_i32 x5 = x4 + c->st[8], y5 = y4 + c->st[9];
                c->x = x5 + c->st[10];
                c->y = y5 + c->st[11];
                {
                    ar_i32 sx = c->x, sy = c->y;
                    c->x = x3;
                    c->y = y3;
                    cs_curveto(c, x1, y1, x2, y2);
                    c->x = sx;
                    c->y = sy;
                    cs_curveto(c, x4, y4, x5, y5);
                }
            }
            else if (b1 == 34 && c->sp >= 7) /* hflex */
            {
                ar_i32 y0 = c->y;
                ar_i32 x1 = c->x + c->st[0], y1 = c->y;
                ar_i32 x2 = x1 + c->st[1], y2 = y1 + c->st[2];
                ar_i32 x3 = x2 + c->st[3], y3 = y2;
                ar_i32 x4 = x3 + c->st[4], y4 = y2;
                ar_i32 x5 = x4 + c->st[5], y5 = y0;
                c->x = x5 + c->st[6];
                c->y = y0;
                {
                    ar_i32 sx = c->x;
                    c->x = x3;
                    c->y = y3;
                    cs_curveto(c, x1, y1, x2, y2);
                    c->x = sx;
                    c->y = y0;
                    cs_curveto(c, x4, y4, x5, y5);
                }
            }
            else if (b1 == 36 && c->sp >= 9) /* hflex1 */
            {
                ar_i32 y0 = c->y;
                ar_i32 x1 = c->x + c->st[0], y1 = c->y + c->st[1];
                ar_i32 x2 = x1 + c->st[2], y2 = y1 + c->st[3];
                ar_i32 x3 = x2 + c->st[4], y3 = y2;
                ar_i32 x4 = x3 + c->st[5], y4 = y2;
                ar_i32 x5 = x4 + c->st[6], y5 = y4 + c->st[7];
                c->x = x5 + c->st[8];
                c->y = y0;
                {
                    ar_i32 sx = c->x;
                    c->x = x3;
                    c->y = y3;
                    cs_curveto(c, x1, y1, x2, y2);
                    c->x = sx;
                    c->y = y0;
                    cs_curveto(c, x4, y4, x5, y5);
                }
            }
            else if (b1 == 37 && c->sp >= 11) /* flex1 */
            {
                ar_i32 sx = c->x, sy = c->y;
                ar_i32 dx = 0, dy = 0;
                ar_i32 x1, y1, x2, y2, x3, y3, x4, y4, x5, y5;
                for (i = 0; i < 10; i += 2)
                {
                    dx += c->st[i];
                    dy += c->st[i + 1];
                }
                x1 = c->x + c->st[0];
                y1 = c->y + c->st[1];
                x2 = x1 + c->st[2];
                y2 = y1 + c->st[3];
                x3 = x2 + c->st[4];
                y3 = y2 + c->st[5];
                x4 = x3 + c->st[6];
                y4 = y3 + c->st[7];
                x5 = x4 + c->st[8];
                y5 = y4 + c->st[9];
                c->x = x3;
                c->y = y3;
                cs_curveto(c, x1, y1, x2, y2);
                if (dx > dy || dx < -dy)
                {
                    c->x = x5 + c->st[10];
                    c->y = sy;
                }
                else
                {
                    c->x = sx;
                    c->y = y5 + c->st[10];
                }
                cs_curveto(c, x4, y4, x5, y5);
            }
            c->sp = 0;
            break;
        }

        default:
            /* Anything unrecognised clears the stack rather than continuing
               with operands that belong to an operator nobody understood. */
            c->sp = 0;
            break;
        }
    }
}

/* ------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------ */
int ar_cff_init(ar_face *f, ar_u32 cff)
{
    ar_cff_index names, tops, strings;
    ar_u32       at, top_start, top_end;
    ar_i32       v[2];

    f->cff = 0;
    if (!cff || cff + 4 > f->size)
    {
        return 0;
    }

    /* Header: the size field is what says where the first INDEX begins, and a
       font may legally have a larger header than the four bytes described. */
    at = cff + ar__cff_u8(f, cff + 2);

    if (!ar__cff_read_index(f, at, &names))
    {
        return 0;
    }
    if (!ar__cff_read_index(f, names.end, &tops))
    {
        return 0;
    }
    if (!ar__cff_read_index(f, tops.end, &strings))
    {
        return 0;
    }
    if (!ar__cff_read_index(f, strings.end, &f->cff_gsubrs))
    {
        return 0;
    }
    if (!ar__cff_entry(f, &tops, 0, &top_start, &top_end))
    {
        return 0;
    }

    /* CharStrings, key 17, is an offset from the start of the table. */
    if (!ar__cff_dict_get(f, top_start, top_end, 17, v, 1))
    {
        return 0;
    }
    if (!ar__cff_read_index(f, cff + (ar_u32)v[0], &f->cff_charstrings))
    {
        return 0;
    }

    /* Private DICT, key 18, is a size and an offset. Local subroutines, key
       19, are then an offset from the start of that dictionary rather than
       from the table, which is the detail this gets wrong if it is going to. */
    if (ar__cff_dict_get(f, top_start, top_end, 18, v, 2))
    {
        ar_u32 priv = cff + (ar_u32)v[1];
        ar_u32 priv_end = priv + (ar_u32)v[0];
        ar_i32 subrs[1];

        if (priv_end <= f->size && ar__cff_dict_get(f, priv, priv_end, 19, subrs, 1))
        {
            ar__cff_read_index(f, priv + (ar_u32)subrs[0], &f->cff_subrs);
        }
    }

    /* A CFF font may state its own glyph count, and it is the authority for
       its own charstrings even if maxp disagrees. */
    if (f->cff_charstrings.count > 0 && (ar_i32)f->cff_charstrings.count < f->num_glyphs)
    {
        f->num_glyphs = (ar_i32)f->cff_charstrings.count;
    }

    f->cff = cff;
    return 1;
}

int ar_cff_outline(const ar_face *f, ar_i32 glyph, ar_i32 ppem, ar_i32 ox, ar_i32 oy, ar_path *p)
{
    cs_ctx c;
    ar_u32 s, e;

    if (!f->cff || glyph < 0)
    {
        return 0;
    }
    if (!ar__cff_entry(f, &f->cff_charstrings, (ar_u32)glyph, &s, &e))
    {
        return 0;
    }

    memset(&c, 0, sizeof c);
    c.f = f;
    c.path = p;
    c.ppem = ppem;
    c.ox = ox;
    c.oy = oy;

    cs_run(&c, s, e);
    if (c.open)
    {
        ar_path_close(p);
    }
    return 1;
}
