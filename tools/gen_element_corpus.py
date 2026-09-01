import io, re, subprocess

ROOT = r"C:\Users\smlkr\Desktop\areole"

ua = io.open(ROOT + r"\src\ar_ua_css.c", encoding="utf-8", newline="").read()
names = set()
for m in re.finditer(r'"([a-z0-9]+(?:\s*,\s*[a-z0-9]+)*)\s*\{', ua):
    for n in m.group(1).split(","):
        names.add(n.strip())

# Elements that cannot be put inside a body div and compared: the document's
# own structure, and the frameset family, which a body-context fragment parser
# drops on the floor.
SKIP = {"html", "head", "body", "frame", "frameset", "noframes", "template"}

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr"}

# What each element needs around it to be the thing it is. A `<td>` outside a
# table is not a table cell in either engine, and comparing two boxes that are
# both wrong in the same way proves nothing.
WRAP = {
    "td": ("<table><tr>", "</tr></table>"),
    "th": ("<table><tr>", "</tr></table>"),
    "tr": ("<table>", "</table>"),
    "thead": ("<table>", "</table>"),
    "tbody": ("<table>", "</table>"),
    "tfoot": ("<table>", "</table>"),
    "caption": ("<table>", "</table>"),
    "col": ("<table><colgroup>", "</colgroup></table>"),
    "colgroup": ("<table>", "</table>"),
    "li": ("<ul>", "</ul>"),
    "dd": ("<dl>", "</dl>"),
    "dt": ("<dl>", "</dl>"),
    "option": ("<select>", "</select>"),
    "optgroup": ("<select>", "</select>"),
    "rt": ("<ruby>", "</ruby>"),
    "rp": ("<ruby>", "</ruby>"),
    # `source` and `track` are NOT wrapped in a media element, and that is
    # the one exception to the rule above. Inside a `<video>` a browser
    # creates no computed style for them at all -- getComputedStyle returns an
    # empty declaration, so there is no value to compare against. Bare, both
    # engines can state what they are.
    "param": ("<object>", "</object>"),
    "area": ("<map>", "</map>"),
    "legend": ("<fieldset>", "</fieldset>"),
    "figcaption": ("<figure>", "</figure>"),
    "summary": ("<details>", "</details>"),
    "datalist": ("<form>", "</form>"),
}

PROPS = ("display margin-top margin-bottom margin-left padding-left "
         "font-size font-weight font-style text-align")

rows = []
for n in sorted(names - SKIP):
    open_tag = '<%s id=\\"x\\">' % n
    frag = open_tag if n in VOID else open_tag + "t</%s>" % n
    pre, post = WRAP.get(n, ("", ""))
    rows.append((n, pre + frag + post))

lines = []
for n, html in rows:
    entry = '    {"%s", "", "%s", PROPS},' % (n, html)
    lines.append(entry)

BODY = "\n".join(lines)

SRC = '''/*
 * areole - the user-agent stylesheet corpus.
 * SPDX-License-Identifier: MIT
 *
 * What every element's defaults compute to, checked against a browser rather
 * than against my reading of the rendering section.
 *
 *     ar_elements                                 areole's computed values
 *     ar_elements --html > tests/elements.html    the browser twin
 *     python tools/compare_computed.py --run ./build/ar_elements.exe \\
 *            tests/elements.html
 *
 * 0.9.1's first acceptance criterion. The machinery is in ar_computed.h, which
 * says why this is a corpus of computed values and not of rectangles; what is
 * here is one case per element the stylesheet names.
 *
 * GENERATED from the selectors in src/ar_ua_css.c. An element added to the
 * sheet and not to this file is an element with no case, which is the failure
 * this corpus exists to make impossible -- so regenerate it rather than
 * editing it, and CI diffs the result.
 *
 * ------------------------------------------------------------------------
 * What each element is wrapped in
 *
 * A `<td>` outside a table is not a table cell in either engine, and two boxes
 * that are wrong in the same way prove nothing. Everything with a required
 * parent gets one. The document's own structure -- html, head, body -- and the
 * frameset family are left out: they cannot be put inside a body and compared,
 * because a body-context fragment parser drops them.
 *
 * ------------------------------------------------------------------------
 * Nine properties, not one
 *
 * A default is a handful of declarations at once and asking for them one case
 * at a time would mean parsing the same fragment nine times. `display` first
 * because it is the one that decides whether anything else means anything: a
 * `<p>` that came out `inline` has the right margins on the wrong kind of box.
 */
#include "ar_computed.h"

#define PROPS "%s"

static const ar__case CASES[] = {
%s};

#define CASE_N ((int)(sizeof CASES / sizeof CASES[0]))

int main(int argc, char **argv)
{
    return ar__corpus_main(argc, argv, CASES, CASE_N, "user-agent stylesheet", "ar_elements");
}
''' % (PROPS, BODY)

OUT = ROOT + r"\tests\ar_elements.c"
io.open(OUT, "w", encoding="utf-8", newline="\n").write(SRC)

# Formatted here rather than left to whoever runs clang-format next, because
# CI regenerates this file and diffs it: a generator whose output is not
# already formatted fails that check every time, for a reason that has nothing
# to do with the corpus.
fmt = None
for cand in ("clang-format", "clang-format.exe"):
    try:
        subprocess.run([cand, "--version"], capture_output=True, check=True)
        fmt = cand
        break
    except Exception:
        pass
if fmt:
    subprocess.run([fmt, "--style=file", "-i", OUT], cwd=ROOT, check=True)
    print("wrote and formatted %d element cases" % len(rows))
else:
    print("wrote %d element cases (clang-format not found; format it yourself)" % len(rows))
