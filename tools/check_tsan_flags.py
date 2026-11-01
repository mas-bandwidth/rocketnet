#!/usr/bin/env python3
"""Every ThreadSanitizer compile in the workflows and docs is optimized (-O1 or above).

At -O0 a struct assignment lowers to a memcpy, and tsan does not report a
memcpy write that follows an unordered plain read; per-field stores are
reported. A tsan build at -O0 is blind to that whole class of race, so every
line that passes -fsanitize=thread to the compiler, in a workflow or in a
command a doc gives, must also pass an optimization level of 1 or more.
Linker-only lines (CMAKE_*_LINKER_FLAGS) and YAML comments are exempt.

Usage: check_tsan_flags.py [--self-test]
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def scanned():
    """The workflows, and every Markdown page that can hand a reader a command."""
    paths = sorted((ROOT / ".github" / "workflows").glob("*.yml"))
    paths += sorted(p for p in ROOT.rglob("*.md")
                    if not any(part in (".git", "build", "node_modules") or part.startswith("build-")
                               for part in p.relative_to(ROOT).parts))
    return paths

TSAN = re.compile(r"-fsanitize=[\w,]*\bthread\b")
OPTIMIZED = re.compile(r"(?<![\w-])-O([0-3]|s|z|fast|g)?(?![\w-])")
LINKER = re.compile(r"LINKER_FLAGS")


def violations(name, text):
    """(file, line number, line) for each tsan compile line with no -O1 or above."""
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("#") or not TSAN.search(line) or LINKER.search(line):
            continue
        levels = [m.group(1) for m in OPTIMIZED.finditer(line)]
        # the last level on the line is the one the compiler uses; a bare -O is -O1
        if not levels or levels[-1] in ("0", "g"):
            found.append((name, number, stripped))
    return found


def tsan_lines(text):
    return sum(1 for line in text.splitlines()
               if TSAN.search(line) and not line.strip().startswith("#") and not LINKER.search(line))


def self_test():
    cases = [
        ('-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1"', 0),
        ('-DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"', 1),
        ('-DCMAKE_C_FLAGS="-fsanitize=thread -O0"', 1),
        ('-DCMAKE_C_FLAGS="-O1 -fsanitize=thread -O0"', 1),
        ('-DCMAKE_C_FLAGS="-fsanitize=thread -Og"', 1),
        ('-DCMAKE_C_FLAGS="-fsanitize=thread -O2"', 0),
        ('-DCMAKE_C_FLAGS="-fsanitize=thread -O"', 0),
        ('tsan) flags="-fsanitize=thread -fno-omit-frame-pointer"; dir=build-tsan ;;', 1),
        ('tsan) flags="-fsanitize=thread -fno-omit-frame-pointer -O1"; dir=build-tsan ;;', 0),
        ('-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"', 0),
        ('#   tsan        Debug, -fsanitize=thread', 0),
        ('-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"', 0),
        ('-DCMAKE_C_FLAGS="-fsanitize=thread,undefined -fno-omit-frame-pointer"', 1),
        ('-DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1-extra"', 1),
    ]
    failed = 0
    for line, expected in cases:
        got = len(violations("fixture.yml", line))
        if got != expected:
            print(f"self-test FAILED: {line!r}: {got} violation(s), expected {expected}")
            failed += 1
    if failed:
        return 1
    print(f"check_tsan_flags self-test: {len(cases)} cases pass")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if argv[1:]:
        print(__doc__)
        return 2
    found = []
    total = 0
    for path in scanned():
        text = path.read_text(errors="replace")
        found += violations(str(path.relative_to(ROOT)), text)
        total += tsan_lines(text)
    for name, number, line in found:
        print(f"{name}:{number}: tsan compile without -O1 or above: {line}")
    if found:
        print(f"check_tsan_flags: {len(found)} of {total} tsan compile lines are unoptimized")
        return 1
    if total == 0:
        print("check_tsan_flags: no tsan compile line found in the workflows or docs")
        return 1
    print(f"check_tsan_flags: {total} tsan compile lines, all at -O1 or above")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
