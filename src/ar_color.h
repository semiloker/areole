/*
 * ar_color.h -- colour notations, colour spaces, and the arithmetic between
 * them.
 *
 * Everything here is integer. There is no float in areole and there is not
 * going to be one: the supported floor is a Pentium II, where a float is not
 * absent but is slower than the integer path and, worse, is not bit-identical
 * across the x87 and SSE builds of the same source. A colour that differs by
 * one in the last place between two builds makes a corpus against a browser
 * meaningless.
 *
 * The scale is AR_CFIX = 4096, one fixed-point unit per 1/4096. That is chosen
 * against two limits at once. It must be fine enough that error accumulated
 * over a conversion chain stays inside the 1/255 a channel is published to --
 * 1/4096 is sixteen times finer, and the longest chain here is six steps. And
 * it must be coarse enough that a product of two values fits a signed 32-bit
 * int without a 64-bit intermediate, because C89 has none this project will
 * use: two operands below 8.0 multiply to under 2^30, and no value in any
 * space used here exceeds 8.0.
 */
#ifndef AR_COLOR_H
#define AR_COLOR_H

#include "../include/areole.h"

/* One fixed-point unit is 1/4096. See the header comment for why this number
   and not a rounder one. */
#define AR_CFIX      4096
#define AR_CFIX_SHIFT 12

/*
 * a * b, both in AR_CFIX scale, result in AR_CFIX scale. Both operands must be
 * under 8.0 or the product overflows -- which is the constraint the scale was
 * picked to satisfy, not a caveat on top of it.
 *
 * It rounds rather than truncating, and does the sign by hand rather than by
 * shifting. Both of those are deliberate.
 *
 * Truncating costs up to one unit per multiply, always downward, and a colour
 * crosses six of them on the way out of Oklab -- so the error does not cancel,
 * it accumulates into a bias. That is invisible in the middle of the range and
 * loud at the bottom, because below 0.0031 the sRGB curve is a straight line
 * with a slope of 12.92: one fixed-point unit of linear error there is most of
 * an 8-bit step. Rounding a round trip through Oklab took pure red's green
 * channel from 6 back to 0.
 *
 * And `>>` on a negative signed int is implementation-defined in C89, which
 * matters here because Lab and Oklab both carry signed components -- this is
 * not a hypothetical portability worry, it is half the values in the file.
 */
static AR_INLINE ar_i32 ar__cmul(ar_i32 a, ar_i32 b)
{
    ar_i32 p = a * b;

    if (p >= 0)
    {
        return (p + AR_CFIX / 2) / AR_CFIX;
    }
    return -((-p + AR_CFIX / 2) / AR_CFIX);
}

#define AR_CMUL(a, b) ar__cmul((a), (b))

/*
 * The colour spaces a value can be stated in.
 *
 * These are the spaces CSS Color 4 interpolates in, plus the two that only
 * ever appear as an input notation. `srgb` and `hsl` and `hwb` are the same
 * numbers in different clothes -- a colour stated in any of them is converted
 * to 8-bit sRGB the moment it is parsed, because the conversion is exact and
 * there is nothing later that could want the original.
 *
 * The wide-gamut four are different: they are kept as a space tag for
 * color-mix(), because mixing in oklch and mixing in srgb give visibly
 * different answers and the stylesheet chooses which.
 */
typedef enum ar_color_space
{
    AR_CS_SRGB = 0,
    AR_CS_SRGB_LINEAR,
    AR_CS_HSL,
    AR_CS_OKLAB,
    AR_CS_OKLCH,
    AR_CS_LAB,
    AR_CS_LCH,
    AR_CS_COUNT
} ar_color_space;

/*
 * How a hue is interpolated between two angles.
 *
 * Two hues 350 degrees apart are also 10 degrees apart the other way round,
 * and which one a gradient takes is a visible choice rather than a detail:
 * `shorter` sweeps through red, `longer` through the whole wheel.
 */
typedef enum ar_hue_method
{
    AR_HUE_SHORTER = 0,
    AR_HUE_LONGER,
    AR_HUE_INCREASING,
    AR_HUE_DECREASING
} ar_hue_method;

/*
 * A colour in a stated space, before it becomes eight bits per channel.
 *
 * The three components carry whatever that space means by them, in AR_CFIX
 * scale, except a hue which is in whole degrees times AR_CFIX. Alpha is always
 * 0..AR_CFIX.
 */
typedef struct ar_color_val
{
    ar_i32 c[3];
    ar_i32 alpha;
    ar_u8  space;
} ar_color_val;

/* --- the sRGB transfer function -----------------------------------------
 *
 * Both directions go through one 256-entry table. Decoding is a lookup;
 * encoding is a binary search of the same table, which makes it the exact
 * inverse of decoding by construction rather than by a second approximation
 * that has to be kept in agreement with the first.
 */
ar_i32 ar_srgb_to_linear(ar_i32 c8);   /* 0..255 -> 0..AR_CFIX */
ar_i32 ar_linear_to_srgb(ar_i32 lin);  /* 0..AR_CFIX -> 0..255 */

/* Integer cube root of a value in AR_CFIX scale, result in AR_CFIX scale.
   Lab and Oklab both need one and neither needs anything else transcendental,
   which is why this is here and not a general pow(). */
ar_i32 ar_cbrt_fix(ar_i32 x);

/* --- named colours -------------------------------------------------------
 *
 * Returns 1 and writes 0xAARRGGBB on a hit. The table is the 148 CSS named
 * colours including `rebeccapurple`, sorted, and searched by bisection.
 */
int ar_color_named(const char *name, ar_u32 len, ar_u32 *out);

/* --- conversions ---------------------------------------------------------
 *
 * Every one of these takes and returns AR_CFIX scale. They are separate rather
 * than one switch because each is exercised directly by ar_test: a conversion
 * that is only ever reached through a parser is a conversion whose failures
 * look like parse failures.
 */
void ar_hsl_to_rgb(ar_i32 h, ar_i32 s, ar_i32 l, ar_i32 *rgb);
void ar_hwb_to_rgb(ar_i32 h, ar_i32 w, ar_i32 b, ar_i32 *rgb);
void ar_oklab_to_linear(const ar_i32 *lab, ar_i32 *lin);
void ar_linear_to_oklab(const ar_i32 *lin, ar_i32 *lab);
void ar_lab_to_xyz(const ar_i32 *lab, ar_i32 *xyz);
void ar_xyz_to_linear(const ar_i32 *xyz, ar_i32 *lin);
void ar_linear_to_xyz(const ar_i32 *lin, ar_i32 *xyz);
void ar_xyz_to_lab(const ar_i32 *xyz, ar_i32 *lab);
void ar_polar_to_rect(ar_i32 l, ar_i32 c, ar_i32 h, ar_i32 *lab);
void ar_rect_to_polar(const ar_i32 *lab, ar_i32 *lch);

/* Pack a value in any space down to 0xAARRGGBB, clamping out-of-gamut
   components into sRGB rather than reporting them. Out of gamut is not an
   error here: `oklch(0.9 0.4 140)` is a legal colour that no sRGB display can
   show, and the honest answer for a screen is the nearest one it can. */
ar_u32 ar_color_pack(const ar_color_val *v);

/* Unpack 0xAARRGGBB into a value in the given space, which is what color-mix()
   needs before it can interpolate. */
void ar_color_unpack(ar_u32 rgba, ar_u8 space, ar_color_val *out);

/* color-mix(in <space>, a p1%, b p2%). `w` is a's share in AR_CFIX scale,
   already normalised by the caller -- CSS's two-percentage form has rules
   about what happens when they do not sum to 100 and those belong with the
   parser, not here. */
ar_u32 ar_color_mix(ar_u32 a, ar_u32 b, ar_i32 w, ar_u8 space, ar_u8 hue_method);

/* --- system colours ------------------------------------------------------
 *
 * The CSS system colours, which are the ones that make a form control look
 * native on Windows 98 and on Windows 11 from one stylesheet. They are an
 * enumeration here and a lookup at frame end, never a literal: the theme they
 * name can change while the program runs.
 */
typedef enum ar_sys_color
{
    AR_SYS_CANVAS = 0,
    AR_SYS_CANVASTEXT,
    AR_SYS_LINKTEXT,
    AR_SYS_VISITEDTEXT,
    AR_SYS_ACTIVETEXT,
    AR_SYS_BUTTONFACE,
    AR_SYS_BUTTONTEXT,
    AR_SYS_BUTTONBORDER,
    AR_SYS_FIELD,
    AR_SYS_FIELDTEXT,
    AR_SYS_HIGHLIGHT,
    AR_SYS_HIGHLIGHTTEXT,
    AR_SYS_SELECTEDITEM,
    AR_SYS_SELECTEDITEMTEXT,
    AR_SYS_MARK,
    AR_SYS_MARKTEXT,
    AR_SYS_GRAYTEXT,
    AR_SYS_ACCENTCOLOR,
    AR_SYS_ACCENTCOLORTEXT,
    AR_SYS_COUNT
} ar_sys_color;

/* Name to index. Returns -1 for anything that is not one. */
int ar_sys_color_by_name(const char *name, ar_u32 len);

/* The built-in light and dark themes, which are what a backend that reports
   nothing falls back to. `dark` selects the second set. */
ar_u32 ar_sys_color_default(int which, int dark);

#endif /* AR_COLOR_H */
