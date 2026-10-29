#!/usr/bin/env python3
"""Check introductory and production guide code blocks against their cited example ranges.

Every fenced C block that follows a citation of the form
``[examples/<file>](../examples/<file>), lines A to B [and C to D]``
must be exactly the cited lines of that example file, including indentation
and cleanup lines. A citation with no following block before the next heading
(a prose line reference) is checked only that its range lies inside the file.

Prints one OK line per checked block; exits 1 naming the guide line, the
citation, and the first differing line for a refusal.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUIDES = (ROOT / "docs" / "GETTING-STARTED.md", ROOT / "docs" / "PRODUCTION.md")

CITATION = re.compile(
    r"\[(?P<text>[^\]]*)\]\((?P<url>[^)]+)\)"
    r"(?P<between>[^\[`]{0,200}?)"
    r"lines?\s+(?P<a>\d+)\s+to\s+(?P<b>\d+)"
    r"(?:\s+and\s+(?P<c>\d+)\s+to\s+(?P<d>\d+))?",
    re.S,
)
FENCE = re.compile(r"^```", re.M)


def cited_ranges(match):
    ranges = [(int(match.group("a")), int(match.group("b")))]
    if match.group("c"):
        ranges.append((int(match.group("c")), int(match.group("d"))))
    return ranges


def check_guide(GUIDE):
    label = GUIDE.relative_to(ROOT).as_posix()
    if not GUIDE.is_file():
        print(f"FAIL: {GUIDE} is missing", file=sys.stderr)
        return 1
    text = GUIDE.read_text(encoding="utf-8")
    lines = text.splitlines()

    # Fenced blocks in order: (start line, [block lines]).
    blocks = []
    current = None
    for number, line in enumerate(lines, 1):
        if current is None and line.startswith("```c"):
            current = [number, []]
        elif current is not None and line.startswith("```"):
            blocks.append(tuple(current))
            current = None
        elif current is not None:
            current[1].append(line)

    failed = False
    checked = 0
    next_block = 0
    for match in CITATION.finditer(text):
        url = match.group("url")
        if "examples/" not in url:
            continue
        name = url.rsplit("/", 1)[-1]
        source_path = ROOT / "examples" / name
        guide_line = text.count("\n", 0, match.start()) + 1
        ranges = cited_ranges(match)
        if not source_path.is_file():
            print(f"{label}:{guide_line}: cited example {name} is missing", file=sys.stderr)
            failed = True
            continue
        source = source_path.read_text(encoding="utf-8").splitlines()

        # A block belongs to this citation only when it starts after the
        # citation and before the next heading or the next citation.
        end_of_region = len(text)
        heading = re.search(r"^## ", text[match.end():], re.M)
        if heading:
            end_of_region = match.end() + heading.start()
        following = [b for b in blocks
                     if b[0] > guide_line
                     and _offset_of_line(text, b[0]) < end_of_region]
        # Keep only blocks not already consumed by an earlier citation.
        following = [b for b in following if blocks.index(b) >= next_block]

        if not following:
            for a, b in ranges:
                if a < 1 or b > len(source) or a > b:
                    print(f"{label}:{guide_line}: cited range "
                          f"{name}:{a}-{b} lies outside the example ({len(source)} lines)",
                          file=sys.stderr)
                    failed = True
            continue

        if len(following) < len(ranges):
            print(f"{label}:{guide_line}: citation names "
                  f"{len(ranges)} range(s) but only {len(following)} block(s) follow",
                  file=sys.stderr)
            failed = True
            continue

        for (a, b), (block_line, block) in zip(ranges, following):
            checked += 1
            expected = source[a - 1:b] if 1 <= a <= b <= len(source) else None
            if expected is None:
                print(f"{label}:{guide_line}: cited range "
                      f"{name}:{a}-{b} lies outside the example ({len(source)} lines)",
                      file=sys.stderr)
                failed = True
                continue
            if block == expected:
                print(f"OK {label}:{block_line} == examples/{name}:{a}-{b}")
            else:
                failed = True
                print(f"{label}:{block_line}: block does not match "
                      f"examples/{name}:{a}-{b}", file=sys.stderr)
                for i in range(max(len(block), len(expected))):
                    got = block[i] if i < len(block) else "<missing>"
                    want = expected[i] if i < len(expected) else "<missing>"
                    if got != want:
                        print(f"  first difference at block line {i + 1} "
                              f"(example line {a + i}):", file=sys.stderr)
                        print(f"    guide:   {got!r}", file=sys.stderr)
                        print(f"    example: {want!r}", file=sys.stderr)
                        break
        next_block = blocks.index(following[len(ranges) - 1]) + 1

    if checked == 0:
        print(f"FAIL: no cited blocks found in {label}", file=sys.stderr)
        return 1
    if failed:
        print(f"FAIL: {checked} cited block(s) checked, at least one differs", file=sys.stderr)
        return 1
    print(f"OK: {checked} cited block(s) match their example ranges")
    return 0


def _offset_of_line(text, line_number):
    return sum(len(line) + 1 for line in text.splitlines()[:line_number - 1])


if __name__ == "__main__":
    results = [check_guide(guide) for guide in GUIDES]
    sys.exit(1 if any(results) else 0)
