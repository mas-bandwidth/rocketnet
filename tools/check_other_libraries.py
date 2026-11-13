#!/usr/bin/env python3
"""Refuse any mention of another network library anywhere in the repository.

RocketNet stands on its own (owner, 2026-11-12): no source, test, tool,
document, model or comment names another networking library, compares
RocketNet with one, or depends on one. A reason is stated in RocketNet's own
terms, or by citing a standard (an RFC) as its source. This check fails when
one of the names below appears, in any case, in a tracked file's path or
text, outside the dated records listed in RECORDS_ALLOWED.

Each name is spelled here built from parts, so this file and its tests never
hold one.

Usage: check_other_libraries.py [--root DIR]   (default: this repository;
0 tracked files under DIR is an error)
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (name, pattern): each pattern names one library, its files or its types.
NAMES = (
    ("net" + "code",
     re.compile("net" + "code", re.IGNORECASE)),
    ("yo" + "jimbo",
     re.compile("yo" + "jimbo", re.IGNORECASE)),
    ("rak" + "net",
     re.compile("rak" + "net", re.IGNORECASE)),
    ("e" + "net",
     re.compile(r"\be" + r"net\b", re.IGNORECASE)),
    ("the reli" + "able library",
     re.compile(r"reli" + r"able\.io|mas-bandwidth/reli" + r"able|\breli" + r"able\.[ch]\b|\breli" + r"able_endpoint"
                r"|\blibreli" + r"able\b|\breli" + r"able's\b|\breli" + r"able (?:library|endpoint|pin)\b",
                re.IGNORECASE)),
    ("the seri" + "alize library",
     re.compile(r"mas-bandwidth/seri" + r"alize|\bseri" + r"alize\.h\b|\bseri" + r"alize library\b|\bseri" + r"alize's"
                r"|\bseri" + r"alize (?:pin|header|stream)\b|\b(?:Bit" + r"Reader|Bit" + r"Writer|Read" + r"Stream"
                r"|Write" + r"Stream|Measure" + r"Stream)\b",
                re.IGNORECASE)),
    ("Network " + "Next",
     re.compile(r"network ?" + r"next|\bnext\.h\b|\bnext_[a-z0-9_]+\.(?:h|c|cpp)\b", re.IGNORECASE)),
    ("Game" + "NetworkingSockets",
     re.compile(r"game" + r"networkingsockets|\bst" + r"eam\b", re.IGNORECASE)),
    ("QU" + "IC",
     re.compile(r"\bqu" + r"ic\b", re.IGNORECASE)),
)

# Path prefixes and patterns of dated records that keep what they said when
# they were written, with the reason.
RECORDS_ALLOWED = (
    (re.compile(r"^docs/audits/"), "dated audits, kept as written"),
    (re.compile(r"^docs/runs/"), "dated run records, kept as written"),
    (re.compile(r"^docs/QUALIFICATION-[^/]*\.md$"), "dated qualification records, kept as written"),
    (re.compile(r"^docs/RELEASE-NOTES-[^/]*\.md$"), "published release notes, kept as written"),
)


def allowed(path):
    """The reason path is a kept record, or None."""
    for pattern, reason in RECORDS_ALLOWED:
        if pattern.search(path):
            return reason
    return None


def violations_in_text(path, text):
    """Every (path, line number, name, line) in text that names another library."""
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        for name, pattern in NAMES:
            if pattern.search(line):
                found.append((path, number, name, line.strip()))
                break
    return found


def violations_in_path(path):
    """The path itself, when its name names another library."""
    for name, pattern in NAMES:
        if pattern.search(path):
            return [(path, 0, name, "file name")]
    return []


def tracked_files(root):
    output = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                            stdout=subprocess.PIPE).stdout
    return [name for name in output.decode("utf-8").split("\0") if name]


def check(root, files):
    found = []
    for name in files:
        if allowed(name):
            continue
        found.extend(violations_in_path(name))
        path = root / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" in data[:8192]:
            continue
        found.extend(violations_in_text(name, data.decode("utf-8", errors="replace")))
    return found


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    root = Path(argv[1]) if argv[:1] == ["--root"] and len(argv) > 1 else ROOT
    try:
        files = tracked_files(root)
    except subprocess.CalledProcessError:
        files = []
    if not files:
        # zero inputs is an error: no tracked file would otherwise pass unread
        print(f"FAIL: 0 tracked files under {root}: nothing was checked")
        return 1
    found = check(root, files)
    for path, number, name, line in found:
        print(f"{path}:{number}: names another library ({name}): {line}")
    if found:
        print(f"FAIL: {len(found)} line(s) name another network library. RocketNet stands on its own: "
              "state the reason in RocketNet's terms, or cite the standard it comes from. "
              "The dated records that are kept live in tools/check_other_libraries.py.")
        return 1
    kept = sum(1 for name in files if allowed(name))
    print(f"OK: no other network library named in {len(files) - kept} tracked files "
          f"({kept} dated records kept as written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
