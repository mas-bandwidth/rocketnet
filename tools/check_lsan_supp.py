#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""Every LeakSanitizer suppression names a function that still exists.

tools/lsan.supp is read by nightly-leaks.yml. An entry whose function was
renamed or deleted suppresses nothing today, and suppresses whatever later
takes the name; print_suppressions=1 shows only the entries that matched,
so a dead one is silent. This fails on any `leak:<name>` whose name is not
an identifier in a tracked source under sdk/ or tests/, on an entry of any
other kind, and on a file with no entry.

    python3 tools/check_lsan_supp.py              # the repository
    python3 tools/check_lsan_supp.py --self-test  # planted dead entries go red
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUPP = "tools/lsan.supp"
SOURCES = re.compile(r"^(sdk|tests)/.*\.(c|cc|cpp|h|hpp)$")
ENTRY = re.compile(r"^leak:([A-Za-z_]\w*)$")


def entries(text):
    """(line number, name) per entry, and the lines that are not entries."""
    found, bad = [], []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = ENTRY.match(line)
        if match:
            found.append((number, match.group(1)))
        else:
            bad.append((number, line))
    return found, bad


def problems(supp_text, source_texts):
    found, bad = entries(supp_text)
    out = ["%s:%d: not a leak:<function> entry: %s" % (SUPP, n, line) for n, line in bad]
    if not found:
        out.append("%s: no leak:<function> entry" % SUPP)
    for number, name in found:
        word = re.compile(r"\b%s\b" % re.escape(name))
        if not any(word.search(text) for text in source_texts):
            out.append("%s:%d: leak:%s names no function in sdk/ or tests/; delete the entry" % (SUPP, number, name))
    return out, len(found)


def tracked_sources(root):
    listing = subprocess.run(["git", "ls-files", "-z", "--", "sdk", "tests"], cwd=root,
                             capture_output=True, check=True).stdout.decode()
    names = [name for name in listing.split("\0") if SOURCES.match(name)]
    if not names:
        raise SystemExit("check_lsan_supp: git listed no source under sdk/ or tests/; nothing was checked")
    texts = []
    for name in names:
        with open(os.path.join(root, name), encoding="utf-8", errors="replace") as handle:
            texts.append(handle.read())
    return texts


def self_test():
    sources = ["static void rocketnet_test_server_uring_create_stalled( void ) {}",
               "void TestLiveCase() { /* TestRenamedCaseOld */ }"]
    cases = [
        ("live", "# a comment\nleak:rocketnet_test_server_uring_create_stalled\nleak:TestLiveCase\n", 0),
        ("dead", "leak:rocketnet_test_server_uring_create_stalled\nleak:TestSimScheduleLongSkipStragglers\n", 1),
        ("prefix only", "leak:TestLive\n", 1),
        ("not an entry", "leak:TestLiveCase\nrace:TestLiveCase\n", 1),
        ("empty", "# nothing\n", 1),
    ]
    for name, text, want in cases:
        got, _ = problems(text, sources)
        if len(got) != want:
            print("check_lsan_supp self-test FAILED: %s: want %d problem(s), got %r" % (name, want, got))
            return 1
    print("ok check_lsan_supp self-test: dead, prefix-only, foreign and missing entries refused; live entries pass")
    return 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    with open(os.path.join(ROOT, SUPP), encoding="utf-8") as handle:
        supp = handle.read()
    found, count = problems(supp, tracked_sources(ROOT))
    if found:
        for line in found:
            print(line)
        print("check_lsan_supp FAILED")
        return 1
    print("ok check_lsan_supp: all %d entries in %s name a function in sdk/ or tests/" % (count, SUPP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
