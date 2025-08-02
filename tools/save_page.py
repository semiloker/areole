"""Save a page from the web as a self-contained document for the real corpus.

    python tools/save_page.py <url> examples/15_real/<name>.html

The corpus exists for acceptance criterion 9 of 0.9.0: ten real documents,
rendered and looked at beside a browser. Real markup is malformed in ways
hand-written markup never is, which is the whole reason the criterion asks for
it -- so the saved file keeps the page's structure exactly and changes only
what would make the comparison dishonest.

What is removed, and why each one:

  - `<script>` and `<noscript>`. areole does not run scripts and says so; a
    browser that does would be laying out a different document, and then the
    two pictures are not comparable.
  - `<link rel="stylesheet">`. External CSS is 0.9.1's. Leaving it in means the
    browser renders a styled page and areole renders an unstyled one, which
    measures the missing feature rather than the layout. Removed so both
    engines see the same input: the document's own `<style>` blocks, its inline
    `style` attributes, and each engine's own default stylesheet.
  - `<img>`, `<svg>`, `<video>`, `<iframe>`, `<picture>`, `<source>`. Images
    are 0.11.0. Same argument.

What is deliberately kept: every `<style>` block, every inline `style`, every
presentational attribute, and every piece of tag soup exactly as served. The
unclosed `<p>`, the stray `</div>`, the attribute with no quotes -- those are
the point.

Nothing here is a network step in the build. The output is committed; this
records how it was made so anyone can check.
"""

import datetime
import io
import re
import subprocess
import sys

# Whole elements whose content goes with them.
WITH_CONTENT = ("script", "noscript", "svg", "video", "iframe", "picture")

# Void or self-contained elements, tag only.
TAG_ONLY = ("img", "source", "track", "embed")


def strip(html):
    for tag in WITH_CONTENT:
        html = re.sub(r"<%s\b[^>]*>.*?</%s\s*>" % (tag, tag), "", html,
                      flags=re.S | re.I)
        # An unclosed one at the end of the document, which does happen.
        html = re.sub(r"<%s\b[^>]*>(?!.*</%s)" % (tag, tag), "", html,
                      flags=re.S | re.I)
    for tag in TAG_ONLY:
        html = re.sub(r"<%s\b[^>]*>" % tag, "", html, flags=re.I)
    html = re.sub(r"<link\b[^>]*rel\s*=\s*[\"']?stylesheet[\"']?[^>]*>", "", html,
                  flags=re.I)
    return html


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    url, out = argv[1], argv[2]

    got = subprocess.run(
        ["curl", "-sL", "-m", "60", "-A", "Mozilla/5.0 (areole corpus)", url],
        capture_output=True)
    if got.returncode != 0 or not got.stdout:
        print("could not fetch %s" % url)
        return 1

    html = got.stdout.decode("utf-8", "replace")
    before = len(html)
    html = strip(html)

    stamp = datetime.date.today().isoformat()
    head = ("<!-- saved from %s on %s by tools/save_page.py\n"
            "     scripts, external stylesheets and images removed; the markup is otherwise\n"
            "     exactly as served. See THIRDPARTY.md for the licence. -->\n" % (url, stamp))

    io.open(out, "w", encoding="utf-8", newline="\n").write(head + html)
    print("%-34s %7d -> %7d bytes" % (out.split("/")[-1], before, len(html)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
