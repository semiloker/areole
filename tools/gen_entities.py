"""Generate src/ar_html_entity.c from the specification's entities.json.

    python tools/gen_entities.py            rewrite src/ar_html_entity.c
    python tools/gen_entities.py --check    fail if it is out of date
    python tools/gen_entities.py --fetch    re-download tools/entities.json

The table is 2,231 named character references and every one of them is a code
point that renders as a letter. Typing them by hand would be 2,231 chances to
put U+0430 CYRILLIC SMALL LETTER A where U+0061 belongs and never see it --
which is why 0.9.0's third acceptance criterion is that the table round-trips
against the specification's own JSON rather than that it looks right.

`--check` is that criterion, and it runs in CI. It regenerates from the
vendored JSON and compares; a hand-edit to the .c file fails it.

--------------------------------------------------------------------------
Why the file is vendored and not fetched

The rule is no dependencies fetched at build time. entities.json is *data for a
generator*, not a build input: nothing in a clean build reads it, and the
generated .c is committed. Vendoring it means `--check` works offline, on a CI
runner with no network, forever -- and it means the table cannot change
underneath a release because a URL did.

--------------------------------------------------------------------------
The encoding, and the 30 KB it has to fit in

0.9.0's budget allows the entity table 30 KB of the 90 KB the whole parser
gets. The obvious form -- an array of {const char *name; ar_u32 cp;} -- is a
pointer and a code point per entry, 16 bytes each on a 64 bit target once the
struct is padded, plus 16 KB of string data: 51 KB before the relocations,
which on a shared library are another 2,231 entries in .rela.dyn.

So: one blob of names, offsets into it, and code points beside it.

  names   16,641 bytes, sorted, no terminators -- the length of entry i is
                  off[i+1] - off[i], which is why there are N+1 offsets
  off      4,464 bytes, ar_u16; the blob is under 64 KB so 16 bits is enough
  cp16     4,462 bytes, ar_u16, the code point when it fits in one
  big_at     452 bytes, ar_u16, the entries that did not fit, ascending
  big_cp   1,808 bytes, ar_u32 pairs, their real values
                                                        ------
                                                        27,827 bytes

226 entries escape: 133 have a code point above U+FFFF and 93 are two code
points (`&NotEqualTilde;` is U+2242 U+0338, a relation with a slash through
it). None is both rare enough to special-case and common enough to matter, so
they share one overflow table found by binary search on the entry index.

0xFFFF is the escape marker because U+FFFF is a noncharacter and no entity maps
to it. The alternative -- reserving low values as indices -- does not work:
`&Tab;` is U+0009 and `&NewLine;` is U+000A, so small code points are real.
"""

import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSON_PATH = os.path.join(ROOT, "tools", "entities.json")
OUT_PATH = os.path.join(ROOT, "src", "ar_html_entity.c")
URL = "https://html.spec.whatwg.org/entities.json"

ESCAPE = 0xFFFF


def fetch():
    import urllib.request

    data = urllib.request.urlopen(URL, timeout=60).read().decode("utf-8")
    # Reformat so the vendored file is diffable rather than one enormous line.
    obj = json.loads(data)
    with io.open(JSON_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=1, sort_keys=True, ensure_ascii=True)
        f.write("\n")
    print("wrote %s, %d entries" % (JSON_PATH, len(obj)))


def load():
    with io.open(JSON_PATH, encoding="utf-8") as f:
        obj = json.load(f)
    # The keys carry a leading '&'; the table stores what follows it, because
    # that is what the tokenizer has in hand once it has seen the ampersand.
    names = sorted(k[1:] for k in obj)
    cps = [obj["&" + n]["codepoints"] for n in names]
    for n, c in zip(names, cps):
        assert 1 <= len(c) <= 2, (n, c)
        assert all(0 < x <= 0x10FFFF for x in c), (n, c)
    return names, cps


def rows(items, per_line, width):
    """Lay values out in a fixed grid so a diff shows one changed value."""
    out = []
    for i in range(0, len(items), per_line):
        chunk = items[i : i + per_line]
        out.append("    " + " ".join(("%*s," % (width, v)) for v in chunk).rstrip())
    return "\n".join(out)


def c_blob(names):
    """The names as one char array, one name per line with its text alongside.

    Not a string literal. C89 caps a string literal at 509 characters *after*
    adjacent literals are concatenated, so a 16 KB blob cannot be written that
    way at all -- and this project's C89 gate is -pedantic-errors, which says
    so out loud. An array of char has no such limit, and is how the generated
    font data is written too.

    One name per line costs about a hundred kilobytes of source and buys a file
    where every entry is legible and a diff names what changed. Nobody reads it
    top to bottom; the person who reads it at all is looking for one entity.
    """
    out = []
    for n in names:
        chars = ",".join("'%s'" % c for c in n) + ","
        out.append("    %-64s /* %s */" % (chars, n))
    return "\n".join(out)


def generate():
    names, cps = load()
    n = len(names)

    off = [0]
    for name in names:
        off.append(off[-1] + len(name))
    assert off[-1] < 0x10000, "the blob outgrew 16 bit offsets"

    cp16 = []
    big_at = []
    big_cp = []
    for i, c in enumerate(cps):
        if len(c) == 1 and c[0] < ESCAPE:
            cp16.append(c[0])
        else:
            cp16.append(ESCAPE)
            big_at.append(i)
            big_cp.append((c[0], c[1] if len(c) > 1 else 0))

    blob_bytes = off[-1]
    total = blob_bytes + 2 * (n + 1) + 2 * n + 2 * len(big_at) + 8 * len(big_at)

    body = HEADER % {
        "count": n,
        "semi": sum(1 for x in names if x.endswith(";")),
        "legacy": sum(1 for x in names if not x.endswith(";")),
        "escapes": len(big_at),
        "above": sum(1 for c in cps if len(c) == 1 and c[0] > 0xFFFF),
        "pairs": sum(1 for c in cps if len(c) == 2),
        "blob": blob_bytes,
        "total": total,
        "kb": total / 1024.0,
        "longest": max(len(x) for x in names),
    }
    body += "\n/* clang-format off */\n"
    body += ("/* Laid out by the generator: one reference per line, with the text it\n"
             "   spells beside it. clang-format would fill the lines instead, which is\n"
             "   tidier and makes the file unreadable and every diff unattributable.\n"
             "   The tables below it are data, not code, and the same applies. */\n")
    body += "static const char AR__NAMES[] = {\n%s\n};\n" % c_blob(names)
    body += "\nstatic const ar_u16 AR__OFF[] = {\n%s};\n" % rows(off, 12, 5)
    body += "\nstatic const ar_u16 AR__CP16[] = {\n%s};\n" % rows(
        ["0x%04X" % v for v in cp16], 8, 6
    )
    body += "\n/* The entries whose value did not fit, ascending. */\n"
    body += "static const ar_u16 AR__BIG_AT[] = {\n%s};\n" % rows(big_at, 12, 5)
    body += "\n/* Their real values. The second is 0 when there is only one. */\n"
    flat = []
    for a, b in big_cp:
        flat.append("{0x%05X, 0x%05X}" % (a, b))
    body += "static const ar_u32 AR__BIG_CP[][2] = {\n%s};\n" % rows(flat, 4, 18)
    body += "\n/* clang-format on */\n"
    body += LOOKUP
    return body


HEADER = '''/*
 * areole - named character references.
 *
 * GENERATED by tools/gen_entities.py from tools/entities.json. Do not edit:
 * `python tools/gen_entities.py --check` fails on a hand edit, and that check
 * is 0.9.0 acceptance criterion 3.
 *
 * SPDX-License-Identifier: MIT
 * The table itself is data from the HTML Standard; see THIRDPARTY.md.
 *
 * ------------------------------------------------------------------------
 * What is here
 *
 * All %(count)d of them. %(semi)d end in a semicolon and %(legacy)d do not --
 * the second set is the legacy one the specification keeps for `&amp` and
 * `&copy` and about a hundred others, written without their terminator by a
 * decade of hand-authored pages and by every URL that ever carried `&copy=`.
 *
 * Both spellings are in one table on purpose. The alternative is to strip the
 * semicolon and look up the stem, which cannot work: `&not` and `&notin;` are
 * different references, and so are `&lt` and `&ltcc;`. What separates them is
 * the matching rule, not the table -- see ar_html_entity_match below.
 *
 * ------------------------------------------------------------------------
 * The shape, and the 30 KB it has to fit in
 *
 * An array of {const char *name; ar_u32 cp;} is 16 bytes an entry once padded,
 * plus %(blob)d bytes of names, plus a relocation each: past 50 KB, against a
 * budget of 30. So the names are one blob and everything else indexes it.
 *
 *   AR__NAMES   %(blob)5d bytes, sorted, no terminators -- entry i runs from
 *                            AR__OFF[i] to AR__OFF[i + 1], which is why there
 *                            are %(count)d + 1 offsets
 *   AR__OFF      %(count)5d + 1 ar_u16; the blob is under 64 KB
 *   AR__CP16     %(count)5d ar_u16, the code point when one will hold it
 *   AR__BIG_AT/CP  %(escapes)3d entries that needed more
 *                                                    -----------
 *                                                    %(total)5d bytes, %(kb).1f KB
 *
 * %(above)d references are above U+FFFF and %(pairs)d are two code points --
 * `&NotEqualTilde;` is U+2242 U+0338, a relation with a combining slash
 * through it. They share one overflow table, found by binary search on the
 * entry index.
 *
 * 0xFFFF marks an escape because U+FFFF is a noncharacter and nothing maps to
 * it. Reserving small values as indices instead does not work: `&Tab;` is
 * U+0009 and `&NewLine;` is U+000A.
 *
 * The longest name is %(longest)d bytes, which is what bounds the match loop.
 */
#include "ar_html.h"

#include <string.h>
'''

LOOKUP = '''
#define AR__ENT_COUNT ((ar_i32)(sizeof AR__CP16 / sizeof AR__CP16[0]))

/*
 * The i-th byte of entry k, or -1 when the name is shorter than that.
 *
 * -1 rather than 0 because that is what makes the array sorted in the same
 * order the match loop walks it: a name is ordered before any name that
 * extends it, so `not` comes before `notin;`, and a range narrowed on i
 * characters has every shorter match sitting at its front.
 */
static int ar__ent_ch(ar_i32 k, ar_u32 i)
{
    ar_u32 from = AR__OFF[k];
    ar_u32 to = AR__OFF[k + 1];

    return i < to - from ? (int)(unsigned char)AR__NAMES[from + i] : -1;
}

static ar_u32 ar__ent_len(ar_i32 k)
{
    return (ar_u32)(AR__OFF[k + 1] - AR__OFF[k]);
}

static void ar__ent_value(ar_i32 k, ar_u32 *out, ar_i32 *count)
{
    if (AR__CP16[k] != 0xFFFFu)
    {
        out[0] = AR__CP16[k];
        *count = 1;
        return;
    }
    {
        ar_i32 lo = 0;
        ar_i32 hi = (ar_i32)(sizeof AR__BIG_AT / sizeof AR__BIG_AT[0]);

        while (lo < hi)
        {
            ar_i32 mid = lo + (hi - lo) / 2;

            if ((ar_i32)AR__BIG_AT[mid] < k)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        out[0] = AR__BIG_CP[lo][0];
        out[1] = AR__BIG_CP[lo][1];
        *count = AR__BIG_CP[lo][1] ? 2 : 1;
    }
}

/*
 * The longest table name that is a prefix of these bytes.
 *
 * Returns how many bytes it used, or 0 for no match, and writes the code
 * points to out[0..1] with their number in *count.
 *
 * ------------------------------------------------------------------------
 * Why this is a longest match and not a lookup
 *
 * The specification says to consume the maximum number of characters that
 * spell an entry in this table, and that is not a formality once the
 * semicolon-less names are present. `&notit;` is `&not` followed by the
 * literal text `it;` -- a negation sign and three characters -- because `not`
 * is in the table and `notit` is not. Reading to the first non-alphanumeric
 * and looking that up gives no match at all and emits six characters of
 * literal text, which is what this parser did while the table held only the
 * 253 references it held before this file was generated, all of which end
 * in a semicolon, and the question could not arise.
 *
 * ------------------------------------------------------------------------
 * How
 *
 * Narrow a range of the sorted table one input byte at a time. After i steps
 * every entry in [lo, hi) begins with the same i bytes the input does, and
 * because a name sorts before anything that extends it, an entry of length
 * exactly i is the one at lo. That is the match; keep the last one seen and
 * carry on looking for a longer one.
 *
 * Two binary searches per byte over a shrinking range, at most 32 bytes
 * because that is the longest name. In practice it stops after four or five:
 * almost nothing in a document begins an entity.
 */
ar_u32 ar_html_entity_match(const char *p, ar_u32 avail, ar_u32 *out, ar_i32 *count)
{
    ar_i32 lo = 0;
    ar_i32 hi = AR__ENT_COUNT;
    ar_u32 best = 0;
    ar_i32 best_at = -1;
    ar_u32 i;

    *count = 0;
    for (i = 0; i < avail && lo < hi; ++i)
    {
        int    c = (int)(unsigned char)p[i];
        ar_i32 a = lo;
        ar_i32 b = hi;
        ar_i32 lo2;

        /* First entry in [lo, hi) whose i-th byte is >= c. */
        while (a < b)
        {
            ar_i32 mid = a + (b - a) / 2;

            if (ar__ent_ch(mid, i) < c)
            {
                a = mid + 1;
            }
            else
            {
                b = mid;
            }
        }
        lo2 = a;

        /* And the first whose i-th byte is > c. */
        b = hi;
        while (a < b)
        {
            ar_i32 mid = a + (b - a) / 2;

            if (ar__ent_ch(mid, i) <= c)
            {
                a = mid + 1;
            }
            else
            {
                b = mid;
            }
        }
        lo = lo2;
        hi = a;

        if (lo < hi && ar__ent_len(lo) == i + 1)
        {
            best = i + 1;
            best_at = lo;
        }
    }

    if (best_at < 0)
    {
        return 0;
    }
    ar__ent_value(best_at, out, count);
    return best;
}

/* The table, walked rather than read, so ar_test can check it is sorted -- a
   binary search over an unsorted table does not fail loudly. It fails on one
   entity, in one document. */
ar_i32 ar_html_entity_count(void)
{
    return AR__ENT_COUNT;
}

/* Into `buf`, NUL-terminated, because the names are not. Returns the length,
   or 0 if `i` is out of range or the buffer is too small. */
ar_u32 ar_html_entity_name(ar_i32 i, char *buf, ar_u32 cap)
{
    ar_u32 n;

    if (i < 0 || i >= AR__ENT_COUNT)
    {
        return 0;
    }
    n = ar__ent_len(i);
    if (n + 1 > cap)
    {
        return 0;
    }
    memcpy(buf, AR__NAMES + AR__OFF[i], n);
    buf[n] = 0;
    return n;
}
'''


def main(argv):
    if "--fetch" in argv:
        fetch()
        return 0

    if not os.path.exists(JSON_PATH):
        print("tools/entities.json is missing. Run: python tools/gen_entities.py --fetch")
        return 1

    text = generate()

    if "--check" in argv:
        if not os.path.exists(OUT_PATH):
            print("src/ar_html_entity.c does not exist; run without --check")
            return 1
        with io.open(OUT_PATH, encoding="utf-8", newline="") as f:
            have = f.read().replace("\r\n", "\n")
        if have != text:
            print("src/ar_html_entity.c does not match tools/entities.json.")
            print("Run: python tools/gen_entities.py")
            return 1
        names, _ = load()
        print("the entity table round-trips: %d references, generated" % len(names))
        return 0

    with io.open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    names, _ = load()
    print("wrote %s, %d references" % (OUT_PATH, len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
