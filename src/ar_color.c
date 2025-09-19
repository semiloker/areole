/*
 * ar_color.c -- colour notations, colour spaces, and the arithmetic between
 * them. See ar_color.h for why every number here is an integer.
 *
 * The file is arranged in the order a colour travels: the transfer function
 * first, because everything crosses it; then the cube root, which is the only
 * transcendental Lab and Oklab need; then the spaces, innermost first; then
 * the named table; then packing and mixing, which are what the parser calls.
 */

#include "ar_color.h"

/* --- string comparison ---------------------------------------------------
 *
 * Local rather than shared with ar_css.c: this file is compiled into the same
 * library but is meant to be readable without it, and a four-line fold is not
 * worth a header dependency in the other direction.
 */
static int ar__cfold(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ar__cstrcmp(const char *name, ar_u32 len, const char *lit)
{
    ar_u32 i;

    for (i = 0; i < len; ++i)
    {
        int a = ar__cfold(name[i]);
        int b = (unsigned char)lit[i];

        if (lit[i] == '\0')
        {
            return 1;
        }
        if (a != b)
        {
            return a < b ? -1 : 1;
        }
    }
    return lit[len] == '\0' ? 0 : -1;
}

/* --- tables --------------------------------------------------------------
 * Generated. ar_test asserts their shape rather than trusting a comment.
 */

#include "ar_color_tables.h"

/* --- the sRGB transfer function ----------------------------------------- */

ar_i32 ar_srgb_to_linear(ar_i32 c8)
{
    if (c8 < 0)
    {
        c8 = 0;
    }
    if (c8 > 255)
    {
        c8 = 255;
    }
    return (ar_i32)AR_SRGB_LIN[c8];
}

/*
 * Encoding is a bisection of the decode table, then a choice between the two
 * neighbours it lands between.
 *
 * Doing it this way rather than with a second approximation of the inverse
 * curve is the whole point: round-tripping any of the 256 8-bit values through
 * decode and back returns the value it started at, exactly, for every one of
 * them. ar_test checks all 256 rather than a sample.
 */
/*
 * Division that rounds to nearest and does its own sign, for the same two
 * reasons ar__cmul does: C89 leaves the sign of an integer division's
 * remainder implementation-defined, and truncation here is a bias rather than
 * a wobble.
 */
static ar_i32 ar__div_round(ar_i32 num, ar_i32 den)
{
    if (den == 0)
    {
        return 0;
    }
    if ((num < 0) != (den < 0))
    {
        ar_i32 n = num < 0 ? -num : num;
        ar_i32 d = den < 0 ? -den : den;

        return -((n + d / 2) / d);
    }
    {
        ar_i32 n = num < 0 ? -num : num;
        ar_i32 d = den < 0 ? -den : den;

        return (n + d / 2) / d;
    }
}

/*
 * Eight times AR_CFIX, and the last three bits that decide a channel.
 *
 * The matrices out of Oklab and Lab subtract terms of similar size to get a
 * small one: linear red is 4.08*l - 3.31*m + 0.23*s, which for a colour whose
 * red is near zero is two numbers around 0.4 cancelling down to 0.0015. One
 * fixed-point unit of error in either survives the cancellation whole.
 *
 * And below 0.0031 the sRGB curve is a straight line of slope 12.92, so one
 * unit of linear error there is four fifths of an 8-bit step. That is the
 * whole of the worst case: oklab(0.556 -0.104 0.011) came out with red 9
 * against a reference 5, while the mean error over three hundred colours was
 * 0.17 of a step.
 *
 * So the last leg keeps three more bits and the bisection compares at the
 * finer scale. Everything above it stays at AR_CFIX, because nothing above it
 * cancels.
 */
#define AR_CFIX8 (AR_CFIX * 8)

static ar_i32 ar__lin8_to_srgb(ar_i32 lin8)
{
    ar_i32 lo = 0, hi = 255;

    if (lin8 <= 0)
    {
        return 0;
    }
    if (lin8 >= AR_CFIX8)
    {
        return 255;
    }

    while (lo < hi)
    {
        ar_i32 mid = (lo + hi) / 2;

        if ((ar_i32)AR_SRGB_LIN[mid] * 8 < lin8)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }

    if (lo > 0)
    {
        ar_i32 above = (ar_i32)AR_SRGB_LIN[lo] * 8 - lin8;
        ar_i32 below = lin8 - (ar_i32)AR_SRGB_LIN[lo - 1] * 8;

        if (below < above)
        {
            return lo - 1;
        }
    }
    return lo;
}

ar_i32 ar_linear_to_srgb(ar_i32 lin)
{
    ar_i32 lo = 0, hi = 255;

    if (lin <= 0)
    {
        return 0;
    }
    if (lin >= AR_CFIX)
    {
        return 255;
    }

    while (lo < hi)
    {
        ar_i32 mid = (lo + hi) / 2;

        if ((ar_i32)AR_SRGB_LIN[mid] < lin)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }

    /* `lo` is the first entry at or above the target. The nearer of it and the
       one below is the answer -- without this the encode is biased upward by
       half a step over the whole range, which is a visible one-per-channel
       drift against a browser on mid greys. */
    if (lo > 0)
    {
        ar_i32 above = (ar_i32)AR_SRGB_LIN[lo] - lin;
        ar_i32 below = lin - (ar_i32)AR_SRGB_LIN[lo - 1];

        if (below < above)
        {
            return lo - 1;
        }
    }
    return lo;
}

/* --- cube root ----------------------------------------------------------- */

/*
 * Newton's method on integers, seeded by halving the bit length.
 *
 * Three iterations are enough over the range Lab and Oklab use, but the loop
 * runs to convergence rather than a fixed count because the seed is crude near
 * zero and a fixed count there is wrong by more than the 1/255 this has to
 * hold. It converges in at most eight passes for every input in range, which
 * ar_test checks by walking the whole range rather than by argument.
 */
ar_i32 ar_cbrt_fix(ar_i32 x)
{
    ar_i32 r, i;
    int    neg = 0;

    if (x < 0)
    {
        neg = 1;
        x = -x;
    }
    if (x == 0)
    {
        return 0;
    }

    /* Seed: cbrt(x) in this scale is near x^(1/3) * AR_CFIX^(2/3). Start from
       something of the right magnitude and let Newton do the rest. */
    r = AR_CFIX;
    while (r > 1 && AR_CMUL(AR_CMUL(r, r), r) > x)
    {
        r = r / 2;
    }
    if (r < 1)
    {
        r = 1;
    }
    while (AR_CMUL(AR_CMUL(r, r), r) < x)
    {
        r = r * 2;
        if (r > (AR_CFIX * 8))
        {
            break;
        }
    }

    for (i = 0; i < 16; ++i)
    {
        ar_i32 rr = AR_CMUL(r, r);
        ar_i32 next;

        if (rr == 0)
        {
            break;
        }
        /* r' = (2r + x*F/r^2) / 3, which is Newton's step rearranged so that
           every intermediate stays inside a signed 32-bit int: x is at most
           8.0 here, so x << 12 is 134 million against a 2.1 billion ceiling. */
        next = (2 * r + ((x << AR_CFIX_SHIFT) / rr)) / 3;
        if (next == r)
        {
            break;
        }
        r = next;
    }

    return neg ? -r : r;
}

/* --- sine and the angle back out ---------------------------------------- */

/* h is degrees in AR_CFIX scale. */
static ar_i32 ar__sin_fix(ar_i32 h)
{
    ar_i32 deg, frac, a, b;
    int    neg = 0;

    /* Fold into 0..360. */
    h = h % (360 * AR_CFIX);
    if (h < 0)
    {
        h += 360 * AR_CFIX;
    }
    if (h >= 180 * AR_CFIX)
    {
        h -= 180 * AR_CFIX;
        neg = 1;
    }
    if (h > 90 * AR_CFIX)
    {
        h = 180 * AR_CFIX - h;
    }

    deg = h / AR_CFIX;
    frac = h - deg * AR_CFIX;
    if (deg >= 90)
    {
        return neg ? -AR_CFIX : AR_CFIX;
    }

    a = (ar_i32)AR_SIN_DEG[deg];
    b = (ar_i32)AR_SIN_DEG[deg + 1];
    a = a + (((b - a) * frac) >> AR_CFIX_SHIFT);

    return neg ? -a : a;
}

static ar_i32 ar__cos_fix(ar_i32 h)
{
    return ar__sin_fix(h + 90 * AR_CFIX);
}

/*
 * The angle of (x, y), in degrees times AR_CFIX.
 *
 * Bisected against the sine table rather than computed, because the table is
 * already here and an arctangent would be a second approximation to keep in
 * agreement with the first. The quadrant is settled by the signs first, so the
 * search only ever runs over ninety degrees where the function is monotone.
 */
static ar_i32 ar__atan2_fix(ar_i32 y, ar_i32 x)
{
    ar_i32 ax = x < 0 ? -x : x;
    ar_i32 ay = y < 0 ? -y : y;
    ar_i32 lo = 0, hi = 90 * AR_CFIX, a;

    if (ax == 0 && ay == 0)
    {
        return 0;
    }

    /* Find a in [0,90] with ay*cos(a) == ax*sin(a). The left side falls and
       the right side rises, so the difference is monotone and bisects. */
    while (hi - lo > 1)
    {
        ar_i32 mid = lo + (hi - lo) / 2;
        ar_i32 d = AR_CMUL(ay, ar__cos_fix(mid)) - AR_CMUL(ax, ar__sin_fix(mid));

        if (d > 0)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    a = lo;

    if (x >= 0 && y >= 0)
    {
        return a;
    }
    if (x < 0 && y >= 0)
    {
        return 180 * AR_CFIX - a;
    }
    if (x < 0 && y < 0)
    {
        return 180 * AR_CFIX + a;
    }
    return 360 * AR_CFIX - a;
}

/* --- hsl and hwb --------------------------------------------------------- */

/*
 * Both are sRGB in different clothes, and both are exact: a colour stated in
 * either converts to eight bits per channel with nothing left over, which is
 * why neither survives parsing as a space of its own.
 */
void ar_hsl_to_rgb(ar_i32 h, ar_i32 s, ar_i32 l, ar_i32 *rgb)
{
    ar_i32 c, x, m, hp, seg;
    ar_i32 r = 0, g = 0, b = 0;

    h = h % (360 * AR_CFIX);
    if (h < 0)
    {
        h += 360 * AR_CFIX;
    }
    if (s < 0)
    {
        s = 0;
    }
    if (s > AR_CFIX)
    {
        s = AR_CFIX;
    }
    if (l < 0)
    {
        l = 0;
    }
    if (l > AR_CFIX)
    {
        l = AR_CFIX;
    }

    /* c = (1 - |2l - 1|) * s */
    c = 2 * l - AR_CFIX;
    if (c < 0)
    {
        c = -c;
    }
    c = AR_CMUL(AR_CFIX - c, s);

    hp = h / 60;            /* H' = h/60, in AR_CFIX scale, 0..6 */
    seg = hp / AR_CFIX;

    /*
     * x = c * (1 - |(H' mod 2) - 1|), and the modulus is over *two* sixths,
     * not one.
     *
     * Taking it over one -- the position inside the current sixth -- looks
     * equivalent and is not: it makes x fall to zero at the start of every
     * sixth, where the real curve is only at zero on the even ones. Yellow is
     * the first hue that shows it. At 60 degrees the answer is (255,255,0) and
     * the one-sixth version gives (0,255,0), a green that is off by a primary
     * rather than by a rounding.
     */
    x = hp % (2 * AR_CFIX);
    x = x - AR_CFIX;
    if (x < 0)
    {
        x = -x;
    }
    x = AR_CMUL(c, AR_CFIX - x);

    m = l - c / 2;

    switch (seg)
    {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }

    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

void ar_hwb_to_rgb(ar_i32 h, ar_i32 w, ar_i32 b, ar_i32 *rgb)
{
    ar_i32 i;

    if (w < 0)
    {
        w = 0;
    }
    if (b < 0)
    {
        b = 0;
    }

    /* White and black summing past one is legal and means grey, at the ratio
       between them. The specification says so explicitly, and without this the
       expression below goes negative and the colour comes out inverted. */
    if (w + b >= AR_CFIX)
    {
        ar_i32 g = (w * AR_CFIX) / (w + b);

        rgb[0] = rgb[1] = rgb[2] = g;
        return;
    }

    ar_hsl_to_rgb(h, AR_CFIX, AR_CFIX / 2, rgb);
    for (i = 0; i < 3; ++i)
    {
        rgb[i] = AR_CMUL(rgb[i], AR_CFIX - w - b) + w;
    }
}

/* --- Oklab ---------------------------------------------------------------
 *
 * Björn Ottosson's matrices, scaled to AR_CFIX. Oklab reaches linear sRGB
 * directly rather than through XYZ, which is both fewer steps and less
 * accumulated error than routing it the long way round.
 */

/* Coefficients in AR_CFIX scale, rounded once here rather than at each use. */
#define K(x) ((ar_i32)((x) * AR_CFIX + ((x) < 0 ? -0.5 : 0.5)))

/* v^3, taken from AR_CFIX scale out to AR_CFIX8 scale. The intermediate is
   v^2/F times v, which peaks around 37 million here -- comfortably inside a
   signed 32-bit int, and the reason the cube is written out rather than
   reached by calling AR_CMUL twice. */
static ar_i32 ar__cube8(ar_i32 v)
{
    return ar__div_round(AR_CMUL(v, v) * v, AR_CFIX / 8);
}

/* c1*v1 + c2*v2 + c3*v3, coefficients at AR_CFIX and values at AR_CFIX8,
   result at AR_CFIX8. Each term is divided before it is added: the three
   products together would reach 3.6 billion and overflow, and one rounding per
   term costs 1.5e-5, which is a seventh of an 8-bit step at the darkest point
   where it could matter. */
static ar_i32 ar__mx8(ar_i32 c1, ar_i32 v1, ar_i32 c2, ar_i32 v2, ar_i32 c3, ar_i32 v3)
{
    return ar__div_round(c1 * v1, AR_CFIX) + ar__div_round(c2 * v2, AR_CFIX) +
           ar__div_round(c3 * v3, AR_CFIX);
}

static void ar__oklab_to_lin8(const ar_i32 *lab, ar_i32 *lin8)
{
    ar_i32 l_ = lab[0] + AR_CMUL(K(0.3963377774), lab[1]) + AR_CMUL(K(0.2158037573), lab[2]);
    ar_i32 m_ = lab[0] - AR_CMUL(K(0.1055613458), lab[1]) - AR_CMUL(K(0.0638541728), lab[2]);
    ar_i32 s_ = lab[0] - AR_CMUL(K(0.0894841775), lab[1]) - AR_CMUL(K(1.2914855480), lab[2]);

    ar_i32 l = ar__cube8(l_);
    ar_i32 m = ar__cube8(m_);
    ar_i32 s = ar__cube8(s_);

    lin8[0] = ar__mx8(K(4.0767416621), l, -K(3.3077115913), m, K(0.2309699292), s);
    lin8[1] = ar__mx8(-K(1.2684380046), l, K(2.6097574011), m, -K(0.3413193965), s);
    lin8[2] = ar__mx8(-K(0.0041960863), l, -K(0.7034186147), m, K(1.7076147010), s);
}

/* The AR_CFIX-scale form, for callers that do not go straight to eight bits.
   It delegates rather than repeating the matrix, so there is one set of
   coefficients in this file and not two to keep in agreement. */
void ar_oklab_to_linear(const ar_i32 *lab, ar_i32 *lin)
{
    ar_i32 t[3];
    int    i;

    ar__oklab_to_lin8(lab, t);
    for (i = 0; i < 3; ++i)
    {
        lin[i] = ar__div_round(t[i], 8);
    }
}

void ar_linear_to_oklab(const ar_i32 *lin, ar_i32 *lab)
{
    ar_i32 l = AR_CMUL(K(0.4122214708), lin[0]) + AR_CMUL(K(0.5363325363), lin[1]) +
               AR_CMUL(K(0.0514459929), lin[2]);
    ar_i32 m = AR_CMUL(K(0.2119034982), lin[0]) + AR_CMUL(K(0.6806995451), lin[1]) +
               AR_CMUL(K(0.1073969566), lin[2]);
    ar_i32 s = AR_CMUL(K(0.0883024619), lin[0]) + AR_CMUL(K(0.2817188376), lin[1]) +
               AR_CMUL(K(0.6299787005), lin[2]);

    ar_i32 l_ = ar_cbrt_fix(l);
    ar_i32 m_ = ar_cbrt_fix(m);
    ar_i32 s_ = ar_cbrt_fix(s);

    lab[0] = AR_CMUL(K(0.2104542553), l_) + AR_CMUL(K(0.7936177850), m_) - AR_CMUL(K(0.0040720468), s_);
    lab[1] = AR_CMUL(K(1.9779984951), l_) - AR_CMUL(K(2.4285922050), m_) + AR_CMUL(K(0.4505937099), s_);
    lab[2] = AR_CMUL(K(0.0259040371), l_) + AR_CMUL(K(0.7827717662), m_) - AR_CMUL(K(0.8086757660), s_);
}

/* --- CIE Lab, through XYZ ------------------------------------------------
 *
 * D50, because that is what CSS Color 4 specifies for `lab()` and `lch()` --
 * not D65, which is what sRGB is defined against. The Bradford adaptation
 * between the two is folded into the matrices rather than applied as a
 * separate step, which is one multiply instead of two and, more to the point,
 * one place for the white point to be wrong instead of two.
 */
/*
 * The scale `lab()` and `lch()` components are carried in, stated once here
 * because getting it wrong is invisible: every formula below still runs and
 * still produces a colour, just the wrong one.
 *
 *   L  is 0..100 in CSS and is stored as L/100 * AR_CFIX, so L=100 is 1.0.
 *   a and b run to roughly +/-125 and are stored the same way, /100 * AR_CFIX.
 *
 * Oklab needs none of this: its L is already 0..1 and its a and b are already
 * small, so those are carried at face value. Two conventions in one file is
 * one more than ideal, and the alternative -- rescaling Oklab to match -- would
 * throw away three bits of its precision to buy tidiness.
 *
 * epsilon = 216/24389 and kappa = 24389/27, the CIE constants, appear as 36
 * (which is epsilon in this scale) and 903.
 */
void ar_lab_to_xyz(const ar_i32 *lab, ar_i32 *xyz)
{
    /* The reference white, D50, in AR_CFIX. */
    static const ar_i32 wp[3] = {K(0.9642956764), AR_CFIX, K(0.8251046025)};

    /* fy = (L + 16) / 116, with L back in its 0..100 units. */
    ar_i32 fy = (lab[0] * 100 / 116) + K(16.0 / 116.0);
    ar_i32 f[3];
    int    i;

    f[0] = fy + lab[1] / 5; /* + a/500, once a is rescaled */
    f[1] = fy;
    f[2] = fy - lab[2] / 2; /* - b/200, likewise */

    for (i = 0; i < 3; ++i)
    {
        ar_i32 c = f[i];
        ar_i32 c3 = AR_CMUL(AR_CMUL(c, c), c);
        ar_i32 r;

        if (i == 1)
        {
            /* Y has its own branch in the specification, taken on L rather
               than on f: below L=8 the cube is not the inverse of the curve
               that produced it, and using f here crushes near-blacks. */
            r = (lab[0] > K(0.08)) ? c3 : (lab[0] * 100 / 903);
        }
        else if (c3 > 36)
        {
            r = c3;
        }
        else
        {
            r = (116 * c - 16 * AR_CFIX) / 903;
        }

        xyz[i] = AR_CMUL(r, wp[i]);
    }
}

void ar_xyz_to_lab(const ar_i32 *xyz, ar_i32 *lab)
{
    static const ar_i32 wp[3] = {K(0.9642956764), AR_CFIX, K(0.8251046025)};
    ar_i32              f[3];
    int                 i;

    for (i = 0; i < 3; ++i)
    {
        ar_i32 r = wp[i] ? (xyz[i] << AR_CFIX_SHIFT) / wp[i] : 0;

        if (r > 36)
        {
            f[i] = ar_cbrt_fix(r);
        }
        else
        {
            f[i] = (903 * r + 16 * AR_CFIX) / 116;
        }
    }

    /* Back out to the stored scale: L = 116 fy - 16, over 100. */
    lab[0] = (116 * f[1] - 16 * AR_CFIX) / 100;
    lab[1] = 5 * (f[0] - f[1]);  /* 500 (fx - fy), over 100 */
    lab[2] = 2 * (f[1] - f[2]);  /* 200 (fy - fz), over 100 */
}

/* D50 XYZ to linear sRGB, Bradford-adapted. Same cancellation as the Oklab
   matrix -- 3.13 X minus 1.62 Y minus 0.49 Z, for a red that may be near zero
   -- so it lands at AR_CFIX8 for the same reason and by the same route. */
static void ar__xyz_to_lin8(const ar_i32 *xyz, ar_i32 *lin8)
{
    ar_i32 x = xyz[0] * 8, y = xyz[1] * 8, z = xyz[2] * 8;

    lin8[0] = ar__mx8(K(3.1341359569), x, -K(1.6173001849), y, -K(0.4906145187), z);
    lin8[1] = ar__mx8(-K(0.9787684456), x, K(1.9161415203), y, K(0.0334540477), z);
    lin8[2] = ar__mx8(K(0.0719453599), x, -K(0.2289913285), y, K(1.4052709088), z);
}

void ar_xyz_to_linear(const ar_i32 *xyz, ar_i32 *lin)
{
    ar_i32 t[3];
    int    i;

    ar__xyz_to_lin8(xyz, t);
    for (i = 0; i < 3; ++i)
    {
        lin[i] = ar__div_round(t[i], 8);
    }
}

void ar_linear_to_xyz(const ar_i32 *lin, ar_i32 *xyz)
{
    xyz[0] = AR_CMUL(K(0.4360747066), lin[0]) + AR_CMUL(K(0.3850649150), lin[1]) +
             AR_CMUL(K(0.1430804201), lin[2]);
    xyz[1] = AR_CMUL(K(0.2225045215), lin[0]) + AR_CMUL(K(0.7168786075), lin[1]) +
             AR_CMUL(K(0.0606169710), lin[2]);
    xyz[2] = AR_CMUL(K(0.0139321740), lin[0]) + AR_CMUL(K(0.0971045147), lin[1]) +
             AR_CMUL(K(0.7141733035), lin[2]);
}

/* --- polar and back ------------------------------------------------------ */

void ar_polar_to_rect(ar_i32 l, ar_i32 c, ar_i32 h, ar_i32 *lab)
{
    lab[0] = l;
    lab[1] = AR_CMUL(c, ar__cos_fix(h));
    lab[2] = AR_CMUL(c, ar__sin_fix(h));
}

void ar_rect_to_polar(const ar_i32 *lab, ar_i32 *lch)
{
    ar_i32 a = lab[1], b = lab[2];
    ar_i32 c2;

    lch[0] = lab[0];

    /* Chroma is the hypotenuse. Computed by bisection on the square rather
       than by a square root, for the same reason the angle is: there is no
       sqrt here either, and adding one would be a third approximation. */
    {
        ar_i32 lo = 0, hi = 0;
        ar_i32 aa = a < 0 ? -a : a;
        ar_i32 bb = b < 0 ? -b : b;

        hi = aa + bb; /* the hypotenuse is never longer than this */
        c2 = AR_CMUL(a, a) + AR_CMUL(b, b);
        while (hi - lo > 1)
        {
            ar_i32 mid = lo + (hi - lo) / 2;

            if (AR_CMUL(mid, mid) < c2)
            {
                lo = mid;
            }
            else
            {
                hi = mid;
            }
        }
        lch[1] = hi;
    }

    lch[2] = ar__atan2_fix(b, a);
}

/* --- named colours ------------------------------------------------------- */

typedef struct ar__named
{
    const char *name;
    ar_u32      rgb;
} ar__named;

#include "ar_color_names.h"

int ar_color_named(const char *name, ar_u32 len, ar_u32 *out)
{
    ar_u32 lo = 0, hi = AR_NAMED_COUNT;

    while (lo < hi)
    {
        ar_u32 mid = (lo + hi) / 2;
        int    cmp = ar__cstrcmp(name, len, AR_NAMED[mid].name);

        if (cmp == 0)
        {
            *out = 0xFF000000ul | AR_NAMED[mid].rgb;
            return 1;
        }
        if (cmp < 0)
        {
            hi = mid;
        }
        else
        {
            lo = mid + 1;
        }
    }
    return 0;
}

/* --- packing ------------------------------------------------------------- */

static ar_i32 ar__clamp_unit(ar_i32 v)
{
    if (v < 0)
    {
        return 0;
    }
    if (v > AR_CFIX)
    {
        return AR_CFIX;
    }
    return v;
}

ar_u32 ar_color_pack(const ar_color_val *v)
{
    ar_i32 lin[3], xyz[3], lab[3];
    ar_i32 rgb8[3];
    ar_i32 a;
    int    i;

    switch (v->space)
    {
    case AR_CS_SRGB:
        for (i = 0; i < 3; ++i)
        {
            rgb8[i] = (ar_i32)((ar__clamp_unit(v->c[i]) * 255 + AR_CFIX / 2) / AR_CFIX);
        }
        break;

    case AR_CS_SRGB_LINEAR:
        for (i = 0; i < 3; ++i)
        {
            rgb8[i] = ar_linear_to_srgb(ar__clamp_unit(v->c[i]));
        }
        break;

    case AR_CS_OKLAB:
    case AR_CS_OKLCH:
        if (v->space == AR_CS_OKLCH)
        {
            ar_polar_to_rect(v->c[0], v->c[1], v->c[2], lab);
        }
        else
        {
            lab[0] = v->c[0];
            lab[1] = v->c[1];
            lab[2] = v->c[2];
        }
        ar__oklab_to_lin8(lab, lin);
        for (i = 0; i < 3; ++i)
        {
            rgb8[i] = ar__lin8_to_srgb(lin[i]);
        }
        break;

    case AR_CS_LAB:
    case AR_CS_LCH:
    default:
        if (v->space == AR_CS_LCH)
        {
            ar_polar_to_rect(v->c[0], v->c[1], v->c[2], lab);
        }
        else
        {
            lab[0] = v->c[0];
            lab[1] = v->c[1];
            lab[2] = v->c[2];
        }
        ar_lab_to_xyz(lab, xyz);
        ar__xyz_to_lin8(xyz, lin);
        for (i = 0; i < 3; ++i)
        {
            rgb8[i] = ar__lin8_to_srgb(lin[i]);
        }
        break;
    }

    a = (ar__clamp_unit(v->alpha) * 255 + AR_CFIX / 2) / AR_CFIX;

    return ((ar_u32)a << 24) | ((ar_u32)rgb8[0] << 16) | ((ar_u32)rgb8[1] << 8) | (ar_u32)rgb8[2];
}

void ar_color_unpack(ar_u32 rgba, ar_u8 space, ar_color_val *out)
{
    ar_i32 r = (ar_i32)((rgba >> 16) & 0xFFu);
    ar_i32 g = (ar_i32)((rgba >> 8) & 0xFFu);
    ar_i32 b = (ar_i32)(rgba & 0xFFu);
    ar_i32 lin[3], xyz[3], lab[3];

    out->space = space;
    out->alpha = (ar_i32)((rgba >> 24) & 0xFFu) * AR_CFIX / 255;

    switch (space)
    {
    case AR_CS_SRGB:
        out->c[0] = r * AR_CFIX / 255;
        out->c[1] = g * AR_CFIX / 255;
        out->c[2] = b * AR_CFIX / 255;
        return;

    case AR_CS_SRGB_LINEAR:
        out->c[0] = ar_srgb_to_linear(r);
        out->c[1] = ar_srgb_to_linear(g);
        out->c[2] = ar_srgb_to_linear(b);
        return;

    case AR_CS_HSL:
        /* Only ever an interpolation space; the parser never produces one. */
        out->c[0] = r * AR_CFIX / 255;
        out->c[1] = g * AR_CFIX / 255;
        out->c[2] = b * AR_CFIX / 255;
        return;

    case AR_CS_OKLAB:
    case AR_CS_OKLCH:
        lin[0] = ar_srgb_to_linear(r);
        lin[1] = ar_srgb_to_linear(g);
        lin[2] = ar_srgb_to_linear(b);
        ar_linear_to_oklab(lin, lab);
        if (space == AR_CS_OKLCH)
        {
            ar_rect_to_polar(lab, out->c);
        }
        else
        {
            out->c[0] = lab[0];
            out->c[1] = lab[1];
            out->c[2] = lab[2];
        }
        return;

    default:
        lin[0] = ar_srgb_to_linear(r);
        lin[1] = ar_srgb_to_linear(g);
        lin[2] = ar_srgb_to_linear(b);
        ar_linear_to_xyz(lin, xyz);
        ar_xyz_to_lab(xyz, lab);
        if (space == AR_CS_LCH)
        {
            ar_rect_to_polar(lab, out->c);
        }
        else
        {
            out->c[0] = lab[0];
            out->c[1] = lab[1];
            out->c[2] = lab[2];
        }
        return;
    }
}

/* --- mixing -------------------------------------------------------------- */

/*
 * Hue takes the short way round unless told otherwise, which is the CSS
 * default and is also the only one of the four that people notice when it is
 * wrong: mixing red with magenta through the long arc passes through green.
 */
static ar_i32 ar__mix_hue(ar_i32 h1, ar_i32 h2, ar_i32 w, ar_u8 method)
{
    ar_i32 full = 360 * AR_CFIX;
    ar_i32 d;

    h1 = h1 % full;
    if (h1 < 0)
    {
        h1 += full;
    }
    h2 = h2 % full;
    if (h2 < 0)
    {
        h2 += full;
    }
    d = h2 - h1;

    switch (method)
    {
    case AR_HUE_LONGER:
        if (d > 0 && d < full / 2)
        {
            d -= full;
        }
        else if (d <= 0 && d > -full / 2)
        {
            d += full;
        }
        break;
    case AR_HUE_INCREASING:
        if (d < 0)
        {
            d += full;
        }
        break;
    case AR_HUE_DECREASING:
        if (d > 0)
        {
            d -= full;
        }
        break;
    case AR_HUE_SHORTER:
    default:
        if (d > full / 2)
        {
            d -= full;
        }
        else if (d < -full / 2)
        {
            d += full;
        }
        break;
    }

    /* w is a's share, so b's is what is left. */
    return h1 + AR_CMUL(d, AR_CFIX - w);
}

ar_u32 ar_color_mix(ar_u32 a, ar_u32 b, ar_i32 w, ar_u8 space, ar_u8 hue_method)
{
    ar_color_val va, vb, out;
    int          i;
    int          polar = (space == AR_CS_OKLCH || space == AR_CS_LCH || space == AR_CS_HSL);

    if (w < 0)
    {
        w = 0;
    }
    if (w > AR_CFIX)
    {
        w = AR_CFIX;
    }

    ar_color_unpack(a, space, &va);
    ar_color_unpack(b, space, &vb);

    out.space = space;
    for (i = 0; i < 3; ++i)
    {
        /* In a polar space the third component is an angle and does not
           interpolate arithmetically -- 350 and 10 average to 180, which is
           cyan, when the answer wanted is red. */
        if (polar && i == 2)
        {
            out.c[i] = ar__mix_hue(va.c[i], vb.c[i], w, hue_method);
        }
        else
        {
            out.c[i] = AR_CMUL(va.c[i], w) + AR_CMUL(vb.c[i], AR_CFIX - w);
        }
    }
    out.alpha = AR_CMUL(va.alpha, w) + AR_CMUL(vb.alpha, AR_CFIX - w);

    if (space == AR_CS_HSL)
    {
        ar_i32 rgb[3];

        ar_hsl_to_rgb(out.c[2], out.c[1], out.c[0], rgb);
        out.space = AR_CS_SRGB;
        out.c[0] = rgb[0];
        out.c[1] = rgb[1];
        out.c[2] = rgb[2];
    }

    return ar_color_pack(&out);
}
