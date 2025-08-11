#!/usr/bin/env python3
"""Compare what areole computes a property to against what a browser does.

    python tools/compare_computed.py --run ./build/ar_hints.exe tests/hints.html

which runs both sides itself: the binary with no arguments for areole, and a
headless browser on the twin for the other.

With two files instead, it compares dumps that already exist:

    python tools/compare_computed.py areole.txt browser.txt

Both files are the same format: comment lines, then `# case <id>` sections,
each followed by one `<property> <value>` line:

    # case bgcolor-td
    background-color rgb(255, 0, 0)

This is a different question from tools/compare_layout.py and exists because
the answer is different. That one asks where a box landed, which is the right
question for a layout algorithm and the wrong one for a *mapping*: half of
HTML's presentational attributes are colours and move no box at all, and
`<table border=3>` moves every box in a browser and none here, because a
border in areole reserves no space. What both engines can state exactly is the
computed value.

Values are compared as strings after whitespace is collapsed, because both
sides are made to spell them the same way -- `rgb(255, 0, 0)`, `40px`,
`center`. A disagreement in spelling is generally a disagreement worth seeing
rather than one worth normalising away: it means one of the two is not saying
what it means.

Two exceptions, both browser-internal spellings of a value CSS already has,
both named in ALIAS below rather than hidden inside a regular expression:

  `-webkit-left` and its two siblings are what Chrome computes `text-align` to
  when an `align` *attribute* set it rather than a stylesheet. The difference
  is real -- the legacy values inherit into table cells where the CSS ones do
  not -- and it is not a difference about what the attribute meant, which is
  the only question this corpus asks.

  `start` is the initial value of `text-align` in a browser and `left` is the
  initial value here. They are the same thing in a left-to-right document, and
  this engine has no direction for them to differ in.
"""

import os
import re
import subprocess
import sys
import tempfile
from html import unescape

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_layout import find_browser, run_browser  # noqa: E402


def load(path):
    """-> [(case_id, property, value)], in file order."""
    cases = []
    cur = None
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('# case '):
                cur = line[len('# case '):].strip()
                continue
            if line.startswith('#') or not line.strip():
                continue
            if cur is None:
                continue
            prop, _, value = line.partition(' ')
            cases.append((cur, prop.strip(), norm(value)))
            cur = None
    return cases


ALIAS = {
    '-webkit-left': 'left',
    '-webkit-right': 'right',
    '-webkit-center': 'center',
    'start': 'left',
}


def norm(s):
    s = re.sub(r'\s+', ' ', s).strip()
    return ALIAS.get(s, s)


def main(argv):
    if len(argv) in (3, 4) and argv[1] == '--run':
        browser = find_browser()
        if not browser:
            print('no chromium found; pass two dump files instead')
            return 2
        tmp = tempfile.mkdtemp(prefix='areole-computed-')
        a_path = os.path.join(tmp, 'areole.txt')
        b_path = os.path.join(tmp, 'browser.txt')
        with open(a_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(subprocess.run([argv[2]], capture_output=True, text=True,
                                   encoding='utf-8', errors='replace').stdout)
        run_browser(browser, argv[3], b_path)
        print('areole  : %s' % argv[2])
        print('browser : %s' % os.path.basename(browser))
        print()
        argv = [argv[0], a_path, b_path]

    if len(argv) != 3:
        print(__doc__)
        return 2

    a = load(argv[1])
    b = dict((case, (prop, value)) for case, prop, value in load(argv[2]))

    if not b:
        print('THE BROWSER PRODUCED NOTHING -- this is not a pass.')
        print('The harness returns an empty dump about one run in three.')
        print('Run it again on its own.')
        return 2

    agree = []
    differ = []
    missing = []
    for case, prop, value in a:
        if case not in b:
            missing.append((case, prop))
            continue
        b_prop, b_value = b[case]
        if b_prop != prop:
            differ.append((case, prop, value, '(the twin reported %s)' % b_prop))
        elif b_value == value:
            agree.append(case)
        else:
            differ.append((case, prop, value, b_value))

    print('cases in areole : %d' % len(a))
    print('matched by name : %d' % (len(a) - len(missing)))
    print()

    if missing:
        print('IN AREOLE, NOT IN THE TWIN (%d)' % len(missing))
        for case, prop in missing:
            print('  %-40s %s' % (case, prop))
        print()

    if differ:
        print('DISAGREE (%d)' % len(differ))
        for case, prop, mine, theirs in differ:
            print('  %-40s %-18s areole %-22s browser %s' % (case, prop, mine, theirs))
        print()
    else:
        print('all agree exactly')
        print()

    print('%d of %d agree' % (len(agree), len(a)))
    return 1 if (differ or missing) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
