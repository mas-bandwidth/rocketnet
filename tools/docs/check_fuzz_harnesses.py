#!/usr/bin/env python3
"""Check that the guides name the fuzz harnesses tests/fuzz/Makefile builds.

tests/fuzz/Makefile's TARGETS is the owner. The harness list in the macOS
replay loop (docs/MACOS.md), every harness list in .github/workflows/ci.yml
(the per-commit fuzz-smoke replay included) and the CMake fuzz_replay_ list
must name exactly those targets, and every count the guides give ("the ten
harnesses", "each of the ten harnesses") must equal their number.

Prints one OK line per checked list; exits 1 naming the file and the missing,
extra or miscounted harnesses.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NUMBERS = {
    "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7,
    "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12,
}
COUNT = re.compile(r"\b(" + "|".join(NUMBERS) + r")\s+(?:replay binaries|harnesses|committed corpora)\b")
HARNESS = re.compile(r"\brocketnet_fuzz(?:_[a-z_]+)?\b")


def makefile_targets():
    text = (ROOT / "tests" / "fuzz" / "Makefile").read_text(encoding="utf-8")
    match = re.search(r"^TARGETS\s*=\s*((?:.*\\\n)*.*)$", text, re.M)
    return set(match.group(1).replace("\\", " ").split())


def check_list(label, names, targets):
    names = set(names)
    if names == targets:
        print(f"OK: {label} names the {len(targets)} harnesses")
        return 0
    missing = sorted(targets - names)
    extra = sorted(names - targets)
    print(f"FAIL: {label}: missing {missing}, extra {extra}", file=sys.stderr)
    return 1


def job_block(text, job):
    """The text of a two-space job key, up to the next job key."""
    match = re.search(rf"^  {re.escape(job)}:\s*(?:#.*)?$", text, re.M)
    if match is None:
        return None
    rest = text[match.end():]
    following = re.search(r"^  [A-Za-z0-9_][\w-]*:\s*(?:#.*)?$", rest, re.M)
    return rest[:following.start()] if following else rest


def main():
    targets = makefile_targets()
    failed = 0

    macos = (ROOT / "docs" / "MACOS.md").read_text(encoding="utf-8")
    loop = re.search(r"^for harness in (.*?); do$", macos, re.M | re.S)
    if loop is None:
        print("FAIL: docs/MACOS.md: no `for harness in ...; do` replay loop", file=sys.stderr)
        failed = 1
    else:
        failed |= check_list("docs/MACOS.md replay loop", HARNESS.findall(loop.group(1)), targets)

    ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    lists = re.findall(r'harnesses="([^"]*)"', ci)
    if not lists:
        print("FAIL: .github/workflows/ci.yml: no harnesses= list", file=sys.stderr)
        failed = 1
    for index, names in enumerate(lists, 1):
        failed |= check_list(f".github/workflows/ci.yml harness list {index}", names.split(), targets)

    smoke = job_block(ci, "fuzz-smoke")
    if smoke is None:
        print("FAIL: .github/workflows/ci.yml: no fuzz-smoke job", file=sys.stderr)
        failed = 1
    else:
        smoke_lists = re.findall(r'harnesses="([^"]*)"', smoke)
        if not smoke_lists:
            print("FAIL: .github/workflows/ci.yml fuzz-smoke: no harnesses= replay list", file=sys.stderr)
            failed = 1
        for index, names in enumerate(smoke_lists, 1):
            failed |= check_list(f".github/workflows/ci.yml fuzz-smoke harness list {index}", names.split(), targets)
        if re.search(r"\./bin/(?!fuzz_replay_)[A-Za-z0-9_.-]+", smoke):
            print("FAIL: .github/workflows/ci.yml fuzz-smoke: replays a libFuzzer binary, not a fuzz_replay_ driver",
                  file=sys.stderr)
            failed = 1
        if "-max_total_time" in smoke:
            print("FAIL: .github/workflows/ci.yml fuzz-smoke: runs a time-boxed mutation pass; it belongs in fuzz-nightly.yml",
                  file=sys.stderr)
            failed = 1

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    pair = re.search(r"set\(ROCKETNET_FUZZ_PAIR_HARNESSES ([^)]*)\)", cmake)
    replay = re.search(r"foreach\(harness (rocketnet_fuzz[^)]*)\)", cmake)
    if pair is None or replay is None:
        print("FAIL: CMakeLists.txt: no fuzz_replay_ harness list", file=sys.stderr)
        failed = 1
    else:
        names = replay.group(1).replace("${ROCKETNET_FUZZ_PAIR_HARNESSES}", pair.group(1)).split()
        failed |= check_list("CMakeLists.txt fuzz_replay_ targets", names, targets)

    for guide in ("docs/BUILD-REFERENCE.md", "docs/MACOS.md"):
        text = (ROOT / guide).read_text(encoding="utf-8")
        for match in COUNT.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            phrase = " ".join(match.group(0).split())
            if NUMBERS[match.group(1)] != len(targets):
                print(f"FAIL: {guide}:{line}: '{phrase}', tests/fuzz/Makefile builds {len(targets)}", file=sys.stderr)
                failed = 1
            else:
                print(f"OK: {guide}:{line}: '{phrase}'")

    return failed


if __name__ == "__main__":
    sys.exit(main())
