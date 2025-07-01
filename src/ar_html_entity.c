/*
 * areole - named character references.
 * SPDX-License-Identifier: MIT
 *
 * ------------------------------------------------------------------------
 * What is here, and what is not
 *
 * The HTML specification defines **2,231** named character references, and
 * 0.9.0's acceptance criterion is that the full table round-trips against the
 * specification's own JSON. This table has 253 of them.
 *
 * That is a gap, not a design. The full table is generated rather than typed --
 * `tools/gen_entities.py` in the release document -- and generating it needs
 * the specification's `entities.json`, which this project will not fetch at
 * build time and which is not vendored yet. Writing 2,231 entries by hand
 * would be two thousand chances to typo a code point into something that
 * renders as a different letter, silently, in one document somewhere.
 *
 * So: the ones here are the ones documents actually contain -- the HTML 4
 * set, which is what every authoring tool emits, plus the handful of
 * typographic references that turn up in real prose. Anything else is left as
 * the literal text that spells it, which is what a browser does for an
 * unrecognised reference anyway, and `ar_html_tok.errors` counts it.
 *
 * When entities.json is vendored with its licence in THIRDPARTY.md, this file
 * is replaced wholesale by the generator's output and the criterion is met.
 *
 * ------------------------------------------------------------------------
 * The lookup
 *
 * A linear scan over a sorted table would be a binary search, and a binary
 * search over 253 entries is eight comparisons. It is a binary search. At
 * 2,231 entries it is eleven, so the shape does not change when the table is
 * generated -- which is the point of choosing it now.
 *
 * The table must stay sorted by name, and there is a check in ar_test.c that
 * says so, because a binary search over an unsorted table does not fail
 * loudly. It fails on one entity, in one document.
 *
 * It earned that on its first run: `sup;` sat before `sup1;`, which reads as
 * sorted to a person and is not -- '1' is 0x31 and ';' is 0x3B, so the longer
 * name comes first. The table is emitted by sorting rather than by eye now.
 */
#include "ar_html.h"

typedef struct ar__entity
{
    const char *name;
    ar_u32      cp;
} ar__entity;

/*
 * Sorted by name, byte for byte. The trailing semicolon is part of the name in
 * the specification and is part of it here: `&amp` without one is a legacy
 * form the specification handles separately, and the tokenizer does that
 * rather than this table carrying both spellings of everything.
 */
static const ar__entity AR__ENTITIES[] = {
    {"AElig;", 0x00C6},   {"Aacute;", 0x00C1},  {"Acirc;", 0x00C2},    {"Agrave;", 0x00C0},
    {"Alpha;", 0x0391},   {"Aring;", 0x00C5},   {"Atilde;", 0x00C3},   {"Auml;", 0x00C4},
    {"Beta;", 0x0392},    {"Ccedil;", 0x00C7},  {"Chi;", 0x03A7},      {"Dagger;", 0x2021},
    {"Delta;", 0x0394},   {"ETH;", 0x00D0},     {"Eacute;", 0x00C9},   {"Ecirc;", 0x00CA},
    {"Egrave;", 0x00C8},  {"Epsilon;", 0x0395}, {"Eta;", 0x0397},      {"Euml;", 0x00CB},
    {"Gamma;", 0x0393},   {"Iacute;", 0x00CD},  {"Icirc;", 0x00CE},    {"Igrave;", 0x00CC},
    {"Iota;", 0x0399},    {"Iuml;", 0x00CF},    {"Kappa;", 0x039A},    {"Lambda;", 0x039B},
    {"Mu;", 0x039C},      {"Ntilde;", 0x00D1},  {"Nu;", 0x039D},       {"OElig;", 0x0152},
    {"Oacute;", 0x00D3},  {"Ocirc;", 0x00D4},   {"Ograve;", 0x00D2},   {"Omega;", 0x03A9},
    {"Omicron;", 0x039F}, {"Oslash;", 0x00D8},  {"Otilde;", 0x00D5},   {"Ouml;", 0x00D6},
    {"Phi;", 0x03A6},     {"Pi;", 0x03A0},      {"Prime;", 0x2033},    {"Psi;", 0x03A8},
    {"Rho;", 0x03A1},     {"Scaron;", 0x0160},  {"Sigma;", 0x03A3},    {"THORN;", 0x00DE},
    {"Tau;", 0x03A4},     {"Theta;", 0x0398},   {"Uacute;", 0x00DA},   {"Ucirc;", 0x00DB},
    {"Ugrave;", 0x00D9},  {"Upsilon;", 0x03A5}, {"Uuml;", 0x00DC},     {"Xi;", 0x039E},
    {"Yacute;", 0x00DD},  {"Yuml;", 0x0178},    {"Zeta;", 0x0396},     {"aacute;", 0x00E1},
    {"acirc;", 0x00E2},   {"acute;", 0x00B4},   {"aelig;", 0x00E6},    {"agrave;", 0x00E0},
    {"alefsym;", 0x2135}, {"alpha;", 0x03B1},   {"amp;", 0x0026},      {"and;", 0x2227},
    {"ang;", 0x2220},     {"apos;", 0x0027},    {"aring;", 0x00E5},    {"asymp;", 0x2248},
    {"atilde;", 0x00E3},  {"auml;", 0x00E4},    {"bdquo;", 0x201E},    {"beta;", 0x03B2},
    {"brvbar;", 0x00A6},  {"bull;", 0x2022},    {"cap;", 0x2229},      {"ccedil;", 0x00E7},
    {"cedil;", 0x00B8},   {"cent;", 0x00A2},    {"chi;", 0x03C7},      {"circ;", 0x02C6},
    {"clubs;", 0x2663},   {"cong;", 0x2245},    {"copy;", 0x00A9},     {"crarr;", 0x21B5},
    {"cup;", 0x222A},     {"curren;", 0x00A4},  {"dArr;", 0x21D3},     {"dagger;", 0x2020},
    {"darr;", 0x2193},    {"deg;", 0x00B0},     {"delta;", 0x03B4},    {"diams;", 0x2666},
    {"divide;", 0x00F7},  {"eacute;", 0x00E9},  {"ecirc;", 0x00EA},    {"egrave;", 0x00E8},
    {"empty;", 0x2205},   {"emsp;", 0x2003},    {"ensp;", 0x2002},     {"epsilon;", 0x03B5},
    {"equiv;", 0x2261},   {"eta;", 0x03B7},     {"eth;", 0x00F0},      {"euml;", 0x00EB},
    {"euro;", 0x20AC},    {"exist;", 0x2203},   {"fnof;", 0x0192},     {"forall;", 0x2200},
    {"frac12;", 0x00BD},  {"frac14;", 0x00BC},  {"frac34;", 0x00BE},   {"frasl;", 0x2044},
    {"gamma;", 0x03B3},   {"ge;", 0x2265},      {"gt;", 0x003E},       {"hArr;", 0x21D4},
    {"harr;", 0x2194},    {"hearts;", 0x2665},  {"hellip;", 0x2026},   {"iacute;", 0x00ED},
    {"icirc;", 0x00EE},   {"iexcl;", 0x00A1},   {"igrave;", 0x00EC},   {"image;", 0x2111},
    {"infin;", 0x221E},   {"int;", 0x222B},     {"iota;", 0x03B9},     {"iquest;", 0x00BF},
    {"isin;", 0x2208},    {"iuml;", 0x00EF},    {"kappa;", 0x03BA},    {"lArr;", 0x21D0},
    {"lambda;", 0x03BB},  {"lang;", 0x2329},    {"laquo;", 0x00AB},    {"larr;", 0x2190},
    {"lceil;", 0x2308},   {"ldquo;", 0x201C},   {"le;", 0x2264},       {"lfloor;", 0x230A},
    {"lowast;", 0x2217},  {"loz;", 0x25CA},     {"lrm;", 0x200E},      {"lsaquo;", 0x2039},
    {"lsquo;", 0x2018},   {"lt;", 0x003C},      {"macr;", 0x00AF},     {"mdash;", 0x2014},
    {"micro;", 0x00B5},   {"middot;", 0x00B7},  {"minus;", 0x2212},    {"mu;", 0x03BC},
    {"nabla;", 0x2207},   {"nbsp;", 0x00A0},    {"ndash;", 0x2013},    {"ne;", 0x2260},
    {"ni;", 0x220B},      {"not;", 0x00AC},     {"notin;", 0x2209},    {"nsub;", 0x2284},
    {"ntilde;", 0x00F1},  {"nu;", 0x03BD},      {"oacute;", 0x00F3},   {"ocirc;", 0x00F4},
    {"oelig;", 0x0153},   {"ograve;", 0x00F2},  {"oline;", 0x203E},    {"omega;", 0x03C9},
    {"omicron;", 0x03BF}, {"oplus;", 0x2295},   {"or;", 0x2228},       {"ordf;", 0x00AA},
    {"ordm;", 0x00BA},    {"oslash;", 0x00F8},  {"otilde;", 0x00F5},   {"otimes;", 0x2297},
    {"ouml;", 0x00F6},    {"para;", 0x00B6},    {"part;", 0x2202},     {"permil;", 0x2030},
    {"perp;", 0x22A5},    {"phi;", 0x03C6},     {"pi;", 0x03C0},       {"piv;", 0x03D6},
    {"plusmn;", 0x00B1},  {"pound;", 0x00A3},   {"prime;", 0x2032},    {"prod;", 0x220F},
    {"prop;", 0x221D},    {"psi;", 0x03C8},     {"quot;", 0x0022},     {"rArr;", 0x21D2},
    {"radic;", 0x221A},   {"rang;", 0x232A},    {"raquo;", 0x00BB},    {"rarr;", 0x2192},
    {"rceil;", 0x2309},   {"rdquo;", 0x201D},   {"real;", 0x211C},     {"reg;", 0x00AE},
    {"rfloor;", 0x230B},  {"rho;", 0x03C1},     {"rlm;", 0x200F},      {"rsaquo;", 0x203A},
    {"rsquo;", 0x2019},   {"sbquo;", 0x201A},   {"scaron;", 0x0161},   {"sdot;", 0x22C5},
    {"sect;", 0x00A7},    {"shy;", 0x00AD},     {"sigma;", 0x03C3},    {"sigmaf;", 0x03C2},
    {"sim;", 0x223C},     {"spades;", 0x2660},  {"sub;", 0x2282},      {"sube;", 0x2286},
    {"sum;", 0x2211},     {"sup1;", 0x00B9},    {"sup2;", 0x00B2},     {"sup3;", 0x00B3},
    {"sup;", 0x2283},     {"supe;", 0x2287},    {"szlig;", 0x00DF},    {"tau;", 0x03C4},
    {"there4;", 0x2234},  {"theta;", 0x03B8},   {"thetasym;", 0x03D1}, {"thinsp;", 0x2009},
    {"thorn;", 0x00FE},   {"tilde;", 0x02DC},   {"times;", 0x00D7},    {"trade;", 0x2122},
    {"uArr;", 0x21D1},    {"uacute;", 0x00FA},  {"uarr;", 0x2191},     {"ucirc;", 0x00FB},
    {"ugrave;", 0x00F9},  {"uml;", 0x00A8},     {"upsih;", 0x03D2},    {"upsilon;", 0x03C5},
    {"uuml;", 0x00FC},    {"weierp;", 0x2118},  {"xi;", 0x03BE},       {"yacute;", 0x00FD},
    {"yen;", 0x00A5},     {"yuml;", 0x00FF},    {"zeta;", 0x03B6},     {"zwj;", 0x200D},
    {"zwnj;", 0x200C}};

#define AR__ENTITY_COUNT ((ar_i32)(sizeof AR__ENTITIES / sizeof AR__ENTITIES[0]))

/* strcmp against a span, which is not null terminated. Shorter sorts first,
   which is what a byte-for-byte ordering means when one is a prefix. */
static int ar__span_cmp(const char *lit, const char *p, ar_u32 n)
{
    ar_u32 i;

    for (i = 0; i < n; ++i)
    {
        unsigned char a = (unsigned char)lit[i];
        unsigned char b = (unsigned char)p[i];

        if (a == 0)
        {
            return -1; /* the literal ended first */
        }
        if (a != b)
        {
            return a < b ? -1 : 1;
        }
    }
    return lit[n] == 0 ? 0 : 1;
}

ar_u32 ar_html_entity(const char *name, ar_u32 n)
{
    ar_i32 lo = 0;
    ar_i32 hi = AR__ENTITY_COUNT - 1;

    if (!name || n == 0)
    {
        return 0;
    }
    while (lo <= hi)
    {
        ar_i32 mid = lo + (hi - lo) / 2;
        int    c = ar__span_cmp(AR__ENTITIES[mid].name, name, n);

        if (c == 0)
        {
            return AR__ENTITIES[mid].cp;
        }
        if (c < 0)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return 0;
}

/* For the check in ar_test.c that the table is sorted: it cannot read the
   table directly, so it walks it through these two. Declared in ar_html.h. */
ar_i32 ar_html_entity_count(void)
{
    return AR__ENTITY_COUNT;
}

const char *ar_html_entity_name(ar_i32 i)
{
    return (i >= 0 && i < AR__ENTITY_COUNT) ? AR__ENTITIES[i].name : 0;
}
