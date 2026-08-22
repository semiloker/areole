"""Vendor the html5lib test suites into third_party/html5lib/.

    python tools/fetch_html5lib.py

Run it when the suites change, which is rarely, and commit what it writes. The
tests themselves never fetch anything: `ar_html5lib` reads the vendored files,
so the gate works on a runner with no network and the exact bytes a release was
measured against are in the tree.

--------------------------------------------------------------------------
Two suites, two homes

The tokenizer tests are still maintained at html5lib/html5lib-tests. The tree
construction tests are not: that repository's README now says one sentence --
"The HTML parser tree construction tests are now solely maintained on
web-platform-tests" -- and the .dat files live under
html/syntax/parsing/resources/ there.

Same format, same authors, different repository. Both licences are recorded in
THIRDPARTY.md.
"""

import io
import json
import os
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "html5lib")

TOKENIZER_TARBALL = "https://codeload.github.com/html5lib/html5lib-tests/tar.gz/refs/heads/master"
WPT_API = ("https://api.github.com/repos/web-platform-tests/wpt/contents/"
           "html/syntax/parsing/resources")
WPT_RAW = ("https://raw.githubusercontent.com/web-platform-tests/wpt/master/"
           "html/syntax/parsing/resources/")


def get(url, accept=None):
    headers = {"User-Agent": "areole-fetch-html5lib"}
    if accept:
        headers["Accept"] = accept
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=180).read()


def write(rel, data):
    path = os.path.join(DEST, rel)
    d = os.path.dirname(path)
    if not os.path.isdir(d):
        os.makedirs(d)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


def main():
    total = 0
    count = 0

    print("tokenizer tests, from html5lib/html5lib-tests")
    tar = tarfile.open(fileobj=io.BytesIO(get(TOKENIZER_TARBALL)))
    for m in tar.getmembers():
        if not m.isfile():
            continue
        if "/tokenizer/" in m.name and m.name.endswith(".test"):
            total += write("tokenizer/" + os.path.basename(m.name), tar.extractfile(m).read())
            count += 1
        elif m.name.endswith("/LICENSE"):
            write("LICENSE.html5lib-tests", tar.extractfile(m).read())
    print("  %d files" % count)

    print("tree construction tests, from web-platform-tests")
    n = 0
    for e in json.loads(get(WPT_API, "application/vnd.github+json").decode("utf-8")):
        if e["type"] == "file" and e["name"].endswith(".dat"):
            total += write("tree-construction/" + e["name"], get(WPT_RAW + e["name"]))
            n += 1
    print("  %d files" % n)

    print("%d files, %d bytes, into %s" % (count + n, total, DEST))
    return 0


if __name__ == "__main__":
    sys.exit(main())
