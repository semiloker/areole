#!/usr/bin/env python3
"""Generate the maths and custom-property corpus both engines are asked.

    python tools/gen_calc_corpus.py            # write the cases
    python tools/gen_calc_corpus.py --check    # and fail if they are stale

Writes `tests/ar_calc_cases.h`, the `ar__case` table `tests/ar_calc.c` runs.
`tools/compare_computed.py` puts the same cases through areole and a browser
and reports where they differ.

------------------------------------------------------------------------
Why generated

Because the interesting part of `calc()` is combinatorial and a hand-written
table is where breadth quietly stops. The operators cross the units cross the
nesting, and the cases that matter are the ones nobody thinks to type: a
difference whose operands are in different units, a `round()` whose quotient
is negative, a `min()` with three arguments where the smallest is in the
middle.

------------------------------------------------------------------------
What is deliberately not here

**Viewport units.** areole renders this corpus into a fixed surface and the
browser lays the twin out in whatever a headless window turns out to be, so
`50vw` is a different number on each and both are right. That is the trap
tools/compare_media.py climbed out of by asking the browser what it actually
got; this corpus has nowhere to put the answer. `calc/viewport` in the gallery
covers them instead, where both engines are handed an iframe of a stated size.

**Expressions whose exact answer is not a whole number.** A length here is a
whole pixel and a browser keeps the fraction, so `calc(100px / 3)` is 33 and
33.3333 -- inside the one pixel this corpus is scored to, but only just, and a
row that is only just passing is a row that will fail for the wrong reason
later. Every expression below lands on an integer.
"""

import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'tests', 'ar_calc_cases.h')

# (id, declarations for #x, the property to report)
CASES = []


def case(name, decl, prop='width'):
    CASES.append((name, decl, prop))


def expr(name, e, prop='width'):
    case(name, '#x { %s:%s; }' % (prop, e), prop)


# -- sums and differences, across units --------------------------------------
for i, (a, b) in enumerate([
        ('100px', '50px'), ('200px', '30px'), ('1in', '24px'), ('2em', '8px'),
        ('3rem', '16px'), ('12pt', '4px'), ('1pc', '2px'), ('2cm', '10px'),
        ('40px', '1em'), ('5em', '2rem')]):
    expr('sum-%d' % i, 'calc(%s + %s)' % (a, b))
    expr('dif-%d' % i, 'calc(%s - %s)' % (a, b))

# -- products and quotients --------------------------------------------------
for i, e in enumerate([
        'calc(2 * 100px)', 'calc(100px * 2)', 'calc(400px / 2)', 'calc(50px * 4)',
        'calc(1em * 3)', 'calc(4rem / 2)', 'calc(0.5 * 200px)', 'calc(200px * 0.5)',
        'calc(1in / 2)', 'calc(3 * 2em)']):
    expr('mul-%d' % i, e)

# -- nesting and precedence --------------------------------------------------
for i, e in enumerate([
        'calc((10px + 5px) * 4)', 'calc(10px + 5px * 4)', 'calc(((100px / 2) + 10px) * 2)',
        'calc(2 * (30px + 20px))', 'calc((8em - 2em) * 2)', 'calc(100px - (20px + 30px))',
        'calc(calc(50px + 50px) * 2)', 'calc((1in - 2em) / 2)']):
    expr('nest-%d' % i, e)

# -- min, max ----------------------------------------------------------------
for i, e in enumerate([
        'min(200px, 300px)', 'min(300px, 200px)', 'min(300px, 150px, 250px)',
        'min(20em, 250px)', 'min(1in, 120px)', 'max(200px, 300px)',
        'max(300px, 200px)', 'max(100px, 150px, 250px)', 'max(10em, 250px)',
        'max(1in, 120px)', 'min(calc(100px + 50px), 200px)',
        'max(calc(100px + 50px), 200px)']):
    expr('mm-%d' % i, e)

# -- clamp, including the crossed bounds -------------------------------------
for i, e in enumerate([
        'clamp(100px, 250px, 300px)', 'clamp(260px, 250px, 300px)',
        'clamp(100px, 350px, 300px)', 'clamp(400px, 250px, 300px)',
        'clamp(0px, 180px, 400px)', 'clamp(10em, 100px, 20em)',
        'clamp(100px, calc(50px * 4), 300px)']):
    expr('clamp-%d' % i, e)

# -- round, all four strategies, positive and negative quotients -------------
for i, e in enumerate([
        'round(105px, 25px)', 'round(112px, 25px)', 'round(113px, 25px)',
        'round(up, 101px, 25px)', 'round(up, 100px, 25px)',
        'round(down, 124px, 25px)', 'round(down, 125px, 25px)',
        'round(to-zero, 124px, 25px)', 'round(nearest, 105px, 25px)',
        'calc(200px + round(-30px, 25px))', 'calc(200px + round(up, -30px, 25px))',
        'calc(200px + round(down, -30px, 25px))',
        'calc(200px + round(to-zero, -30px, 25px))']):
    expr('round-%d' % i, e)

# -- mod and rem, where the signs part company -------------------------------
for i, e in enumerate([
        'mod(118px, 25px)', 'rem(118px, 25px)',
        'calc(200px + mod(-30px, 25px))', 'calc(200px + rem(-30px, 25px))',
        'calc(200px + mod(30px, -25px))', 'calc(200px + rem(30px, -25px))',
        'mod(100px, 30px)', 'rem(100px, 30px)']):
    expr('modrem-%d' % i, e)

# -- abs and sign ------------------------------------------------------------
for i, e in enumerate([
        'abs(-120px)', 'abs(120px)', 'calc(100px * sign(4px))',
        'calc(200px + 100px * sign(-4px))', 'calc(abs(-2em) * 3)',
        'calc(200px + abs(-50px))']):
    expr('abs-%d' % i, e)

# -- custom properties -------------------------------------------------------
# Declared on a wrapper and *not* on `:root`, which this corpus cannot ask
# about at all.
#
# The twin shares one page between every case and keeps them apart by
# rewriting each case's selectors to sit under `[data-page="..."]`. That turns
# `:root` into `[data-page="x"] :root`, which matches nothing -- the root
# element is not inside the case box. areole parses each case's sheet against
# its own subtree and sees the declaration, so the two disagreed on thirteen
# rows with areole right on every one of them.
#
# `:root` is where a page actually writes these, so it is covered in ar_test
# and in the `vars/*` demos, where the document is a whole document.
VARS = '.v { --w: 300px; --half: 150px; --gap: 2rem; --n: 3; }'
for i, e in enumerate([
        'var(--w)', 'var(--half)', 'var(--gap)',
        'var(--missing, 120px)', 'var(--missing, 20em)', 'var(--w, 50px)',
        'calc(var(--w) / 2)', 'calc(var(--half) * 2)', 'calc(var(--w) + 20px)',
        'calc(var(--w) - var(--half))', 'calc(var(--gap) * 4)',
        'min(var(--w), 200px)', 'max(var(--half), 200px)',
        'clamp(100px, var(--half), 200px)',
        'calc(var(--missing, 100px) + 40px)']):
    case('var-%d' % i, VARS + ' #x { width:%s; }' % e)

# -- scoping -----------------------------------------------------------------
CASES.append(('var-scope-near', '.v { --w: 300px; } .s { --w: 120px; }', 'width'))
CASES.append(('var-scope-far', '.v { --w: 300px; } .s { --unused: 1px; }', 'width'))

# -- what is refused, and costs only itself ----------------------------------
for i, (e, wit) in enumerate([
        ('calc(100px + 2)', 'height:24px'),
        ('calc(100px * 2px)', 'height:24px'),
        ('calc(100px / 0)', 'height:24px'),
        ('calc(50% - 10px)', 'height:24px'),
        ('calc(1px -2px)', 'height:24px'),
        ('var(--nothing)', 'height:24px'),
        ('calc(var(--nothing) + 10px)', 'height:24px'),
        ('min(200px)', 'height:24px')]):
    CASES.append(('bad-%d' % i, '#x { width:%s; %s; }' % (e, wit), 'height'))


BANNER = """/*
 * GENERATED by tools/gen_calc_corpus.py. Do not edit.
 *
 * The cases tests/ar_calc.c runs. See that file for what the corpus is for
 * and the generator for what is deliberately not in it.
 */
"""


def render():
    out = [BANNER, 'static const ar__case CASES[] = {']
    for i, (name, decl, prop) in enumerate(CASES):
        html = '<div id=\\"x\\">t</div>'
        if name.startswith('var-scope'):
            html = ('<div class=\\"v\\"><div class=\\"s\\">'
                    '<div id=\\"x\\">t</div></div></div>')
            decl = decl + ' #x { width:var(--w); }'
        elif name.startswith('var-'):
            html = '<div class=\\"v\\"><div id=\\"x\\">t</div></div>'
        comma = ',' if i + 1 < len(CASES) else ''
        out.append('    {"%s", "%s", "%s", "%s"}%s'
                   % (name, decl.replace('"', '\\"'), html, prop, comma))
    out.append('};')
    out.append('')
    out.append('#define CASE_N ((int)(sizeof CASES / sizeof CASES[0]))')
    out.append('')
    return '\n'.join(out)


def main():
    text = render()
    if '--check' in sys.argv:
        if not os.path.exists(OUT):
            print('FAIL: %s does not exist' % os.path.relpath(OUT, ROOT))
            return 1
        with io.open(OUT, encoding='utf-8') as f:
            if f.read() != text:
                print('FAIL: %s is stale -- run tools/gen_calc_corpus.py'
                      % os.path.relpath(OUT, ROOT))
                return 1
        print('the calc corpus is %d cases' % len(CASES))
        return 0
    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('wrote %s, %d cases' % (os.path.relpath(OUT, ROOT), len(CASES)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
