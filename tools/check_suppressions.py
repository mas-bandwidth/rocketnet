#!/usr/bin/env python3
"""Every in-source analyzer or warning suppression in the library is reviewed.

The static gates (tools/run_tidy.py: clang-tidy and cppcheck; tools/
run_static.py: gcc -fanalyzer; the -Werror warning floor) compare findings
with reviewed baselines, but a suppression written into the source hides a
finding before any baseline sees it: a `// NOLINT` makes clang-tidy report
nothing, cppcheck runs with --inline-suppr, and a diagnostic pragma silences
the compiler. So every such suppression in sdk/ is listed in
tools/suppressions_reviewed.txt with a reason, and this check fails on:

  - a suppression in sdk/ that the file does not list (NEW);
  - a listed suppression whose line is gone or was edited (STALE): the list
    is exact both ways, as the baselines are;
  - a list line with no reason, a malformed line, or a duplicate.

Suppressions recognised (case-sensitive, anywhere on the line): NOLINT,
NOLINTNEXTLINE, NOLINTBEGIN, NOLINTEND, cppcheck-suppress (any form),
`#pragma GCC|clang diagnostic ignored`, `#pragma warning( disable` and
`#pragma warning( suppress`, and the operator forms _Pragma( "... ignored ...")
and __pragma( warning( disable|suppress ... ).

A suppression is keyed by <file> <kind> <sha256 of the line, whitespace
collapsed>[:<ordinal among identical lines in the file>], so a line that
moves keeps its key and an edited line is a new suppression.

  python3 tools/check_suppressions.py            check sdk/
  python3 tools/check_suppressions.py --list     print the keys found (to review and add)
  python3 tools/check_suppressions.py --self-test
  python3 tools/check_suppressions.py --root DIR  check DIR's sdk/ against this list; none there is an error

tools/run_tidy.py runs this check first; the lint job runs it per commit.
"""

import hashlib
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REVIEWED = os.path.join(ROOT, "tools", "suppressions_reviewed.txt")
SCOPE = ("sdk/",)
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")

PATTERNS = [
    ("NOLINT", re.compile(r"\bNOLINT(NEXTLINE|BEGIN|END)?\b")),
    ("cppcheck-suppress", re.compile(r"cppcheck-suppress")),
    ("pragma-diagnostic-ignored", re.compile(r"#\s*pragma\s+(GCC|clang)\s+diagnostic\s+ignored")),
    ("pragma-warning-disable", re.compile(r"#\s*pragma\s+warning\s*\(\s*(disable|suppress)")),
    ("operator-pragma", re.compile(r"(_Pragma\s*\(\s*\"[^\"]*\bignored\b|__pragma\s*\(\s*warning\s*\(\s*(disable|suppress))")),
]


def collapse(line):
    return " ".join(line.split())


def scan_text(path, text):
    """Returns [(key, line_number, line)] for every suppression in text."""
    found = []
    seen = {}
    for number, line in enumerate(text.splitlines(), 1):
        for kind, pattern in PATTERNS:
            if pattern.search(line):
                digest = hashlib.sha256(collapse(line).encode()).hexdigest()[:16]
                base = "%s %s %s" % (path, kind, digest)
                ordinal = seen.get(base, 0)
                seen[base] = ordinal + 1
                key = base if ordinal == 0 else "%s:%d" % (base, ordinal)
                found.append((key, number, line.strip()))
                break
    return found


def scan_tree(root):
    result = subprocess.run(["git", "-C", root, "ls-files", "-z", "--"] + list(SCOPE), capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode(errors="replace"))
        return None, 0
    names = [name for name in result.stdout.decode().split("\0") if name.endswith(SOURCE_SUFFIXES)]
    found = []
    for name in names:
        with open(os.path.join(root, name), encoding="utf-8", errors="replace") as handle:
            found.extend(scan_text(name, handle.read()))
    return found, len(names)


def read_reviewed(path):
    """Returns ({key: reason}, errors). A line is <key fields>\\t<reason>."""
    reviewed, errors = {}, []
    if not os.path.exists(path):
        return {}, ["no reviewed list at %s" % path]
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 2:
                errors.append("%s:%d: want <file> <kind> <hash>, a tab, then the reason: %s" % (path, number, line))
                continue
            key, reason = parts[0].strip(), parts[1].strip()
            if len(key.split(" ")) != 3:
                errors.append("%s:%d: malformed key: %s" % (path, number, key))
                continue
            if not reason:
                errors.append("%s:%d: suppression without a reason: %s" % (path, number, key))
                continue
            if key in reviewed:
                errors.append("%s:%d: duplicate: %s" % (path, number, key))
                continue
            reviewed[key] = reason
    return reviewed, errors


def check(root, reviewed_path):
    found, files = scan_tree(root)
    if found is None:
        print("check_suppressions: git ls-files failed; nothing was checked")
        return 2
    if files == 0:
        print("check_suppressions: no library source under %s; nothing was checked" % ", ".join(SCOPE))
        return 1
    reviewed, errors = read_reviewed(reviewed_path)
    bad = 0
    for error in errors:
        print("INVALID " + error)
        bad = 1
    keys = {key for key, _, _ in found}
    for key, number, line in found:
        if key not in reviewed:
            path = key.split(" ")[0]
            print("NEW suppression %s:%d not in %s: %s" % (path, number, os.path.relpath(reviewed_path, root), line))
            print("    key: %s" % key)
            bad = 1
    for key in sorted(set(reviewed) - keys):
        print("STALE %s: listed in %s but no such line in the tree; delete the line" % (
            key, os.path.relpath(reviewed_path, root)))
        bad = 1
    if bad:
        print("check_suppressions FAILED (%d suppressions in %d files, %d reviewed)" % (len(found), files, len(reviewed)))
        return 1
    print("ok: %d suppressions in %d library files, each reviewed" % (len(found), files))
    return 0


def self_test():
    failures = 0
    cases = 0

    def expect(name, want, got):
        nonlocal failures, cases
        cases += 1
        if want != got:
            print("FAIL %s: want %s, got %s" % (name, want, got))
            failures += 1
        else:
            print("ok   %s" % name)

    kinds = {
        "x = f( p ); // NOLINT": "NOLINT",
        "// NOLINTNEXTLINE(bugprone-x)": "NOLINT",
        "x = f( p ); // cppcheck-suppress nullPointer": "cppcheck-suppress",
        "// cppcheck-suppress-file unusedFunction": "cppcheck-suppress",
        "#pragma GCC diagnostic ignored \"-Wfoo\"": "pragma-diagnostic-ignored",
        "    #  pragma clang diagnostic ignored \"-Wbar\"": "pragma-diagnostic-ignored",
        "#pragma warning( disable : 4324 )": "pragma-warning-disable",
        "#pragma warning(suppress: 6011)": "pragma-warning-disable",
        "_Pragma( \"GCC diagnostic ignored \\\"-Wfoo\\\"\" )": "operator-pragma",
        "__pragma( warning( disable : 4127 ) )": "operator-pragma",
    }
    for line, kind in kinds.items():
        found = scan_text("sdk/a.cpp", line + "\n")
        expect("recognised: " + line, kind, found[0][0].split(" ")[1] if found else None)
    for line in ["int nolint = 0;", "#pragma GCC diagnostic push", "#pragma warning( push )", "// a lint note"]:
        expect("not a suppression: " + line, 0, len(scan_text("sdk/a.cpp", line + "\n")))
    twice = scan_text("sdk/a.cpp", "f(); // NOLINT\nf(); // NOLINT\n")
    expect("identical lines get distinct keys", 2, len({key for key, _, _ in twice}))

    with tempfile.TemporaryDirectory() as work:
        subprocess.run(["git", "-C", work, "init", "-q"], check=True)
        os.makedirs(os.path.join(work, "sdk"))
        os.makedirs(os.path.join(work, "tools"))
        source = os.path.join(work, "sdk", "a.cpp")
        reviewed = os.path.join(work, "tools", "suppressions_reviewed.txt")

        def write(path, text):
            with open(path, "w") as handle:
                handle.write(text)

        expect("a tree with no library source fails", 1, check(work, reviewed))
        write(source, "int f( int * p ) { return *p; }\n")
        subprocess.run(["git", "-C", work, "add", "sdk/a.cpp"], check=True)
        write(reviewed, "# none\n")
        expect("no suppression, empty list passes", 0, check(work, reviewed))
        write(source, "int f( int * p ) { return *p; } // NOLINT\n")
        expect("a new NOLINT fails", 1, check(work, reviewed))
        key = scan_text("sdk/a.cpp", "int f( int * p ) { return *p; } // NOLINT\n")[0][0]
        write(reviewed, key + "\t\n")
        expect("a listed suppression without a reason fails", 1, check(work, reviewed))
        write(reviewed, key + "\treviewed: the pointer is checked by the caller\n")
        expect("a listed suppression with a reason passes", 0, check(work, reviewed))
        write(source, "int f( int * p ) {   return *p; }   // NOLINT\n")
        expect("whitespace only moves keep the key", 0, check(work, reviewed))
        write(source, "int f( int * q ) { return *q; } // NOLINT\n")
        expect("an edited suppressed line is new (and the old key stale)", 1, check(work, reviewed))
        write(source, "int f( int * p ) { return *p; }\n")
        expect("a listed suppression that is gone is stale", 1, check(work, reviewed))
        write(source, "int f( int * p ) { return *p; } // NOLINT\n#pragma warning( disable : 4324 )\n")
        expect("a second, unlisted kind fails", 1, check(work, reviewed))
        write(reviewed, key + "\tok\n" + key + "\tagain\n")
        write(source, "int f( int * p ) { return *p; } // NOLINT\n")
        expect("a duplicate list line fails", 1, check(work, reviewed))
        expect("git unable to list exits 2", 2, check(os.path.join(work, "missing"), reviewed))

    print("check_suppressions self-test: %d cases, %d failed" % (cases, failures))
    return 1 if failures or cases == 0 else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if sys.argv[1:] == ["--list"]:
        found, _ = scan_tree(ROOT)
        for key, number, line in found or []:
            print("%s\t<reason>    # line %d: %s" % (key, number, line))
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "--root":
        return check(sys.argv[2], REVIEWED)
    if sys.argv[1:]:
        print(__doc__)
        return 2
    return check(ROOT, REVIEWED)


if __name__ == "__main__":
    sys.exit(main())
