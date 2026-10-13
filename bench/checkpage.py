#!/usr/bin/env python3
"""checkpage.py - every number in the page must come from a results file.

The mode page accumulated figures over several sessions and ended up
quoting a throughput from one run beside a key census from another, with
a footer naming a commit the measuring binary could not have been built
from.  Nothing caught it because nothing was checking.

This does the checking: it pulls the numeric cells out of the HTML
tables and asserts each one appears verbatim in one of the results files
the harnesses wrote, and that those files all name the SAME build.  It
does not look at prose - a sentence can legitimately round or compare -
but a table cell is a measurement and must be traceable.

Small integers are skipped: counts, fleet sizes and tolerances are
structural facts asserted by test/ecgeomtest.sh, not measurements, and
matching them by value would be noise rather than a check.

usage: bench/checkpage.py <page.html> <results.tsv> [results.tsv ...]
exit:  0 all cells traced, 1 something in the table has no source
"""
import re
import sys

MIN_CHECKED = 1000          # below this a number is structural, not measured

# a measured number is a standalone token - not the 1305 inside Poly1305
NUM = r"(?<![A-Za-z0-9.])[0-9][0-9,]*(?:\.[0-9]+)?(?![A-Za-z])"


def page_numbers(html):
    """numeric values from table cells AND card headlines.

    The cards are checked too: the figures that went stale unnoticed were
    the cross-node read numbers, and those live in a card, not a table.
    """
    out = []
    for card in re.findall(r'<div class="card">(.*?)</div>\s*</div>', html, re.S):
        head = re.search(r"<h3>(.*?)</h3>", card, re.S)
        big = re.search(r'<div class="big">(.*?)</div>', card, re.S)
        if not big:
            continue
        label = re.sub(r"<[^>]+>", " ", head.group(1)) if head else "card"
        label = "card: " + " ".join(label.split())[:34]
        text = re.sub(r"<[^>]+>", " ", big.group(1))
        for tok in re.findall(NUM, text):
            val = tok.replace(",", "")
            try:
                if float(val) >= MIN_CHECKED:
                    out.append((label, val))
            except ValueError:
                pass
    for table in re.findall(r"<table>(.*?)</table>", html, re.S):
        for row in re.findall(r"<tr>(.*?)</tr>", table, re.S):
            cells = re.findall(r"(<t[dh][^>]*>)(.*?)</t[dh]>", row, re.S)
            if not cells:
                continue
            label = re.sub(r"<[^>]+>", " ", cells[0][1])
            label = " ".join(label.split())[:40]
            for tag, cell in cells[1:]:
                # only NUMERIC cells: a prose cell holds numbers that are
                # not measurements ("ChaCha20-Poly1305" offered up 1305),
                # and a measured value always carries class="num"
                if "num" not in tag:
                    continue
                text = re.sub(r"<[^>]+>", " ", cell)
                for tok in re.findall(NUM, text):
                    val = tok.replace(",", "")
                    try:
                        num = float(val)
                    except ValueError:
                        continue
                    if num >= MIN_CHECKED:
                        out.append((label, val))
    return out


def file_tokens(path):
    """every number appearing in a results file, however it is packed"""
    toks = set()
    build = None
    with open(path) as fh:
        for line in fh:
            if line.startswith("#"):
                m = re.search(r"build=(\S+)", line)
                if m:
                    build = m.group(1)
                continue
            for tok in re.findall(r"[0-9]+(?:\.[0-9]+)?", line):
                toks.add(tok)
                # results pack several figures per cell (a/b/c) and the
                # page prints them with thousands separators; normalise
                # both directions so a match is a match
                if "." in tok:
                    toks.add(tok.rstrip("0").rstrip("."))
    return toks, build


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 2
    html = open(sys.argv[1]).read()

    known = set()
    builds = {}
    for path in sys.argv[2:]:
        toks, build = file_tokens(path)
        known |= toks
        builds[path] = build

    rc = 0
    unstamped = [p for p, b in builds.items() if not b]
    if unstamped:
        print("NO BUILD STAMP: " + ", ".join(unstamped))
        print("  -> re-run the harness; an unsourced number is not a result")
        rc = 1
    revs = {b for b in builds.values() if b}
    if len(revs) > 1:
        print("MIXED BUILDS: " + ", ".join(sorted(revs)))
        for path, build in sorted(builds.items()):
            print("  %-40s %s" % (path, build))
        print("  -> one page must describe one build")
        rc = 1

    missing = [(lab, val) for lab, val in page_numbers(html) if val not in known]
    if missing:
        print("\n%d table cell(s) with no source in the results files:" %
              len(missing))
        for lab, val in missing:
            print("  %-42s %s" % (lab, val))
        rc = 1
    else:
        print("all %d checked table cells trace to a results file" %
              len(page_numbers(html)))
    if revs and len(revs) == 1:
        print("build: %s" % revs.pop())
    return rc


if __name__ == "__main__":
    sys.exit(main())
