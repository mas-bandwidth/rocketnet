#!/usr/bin/env python3
"""Check the examples against the house rules.

Each examples/*.c may include only "rocketnet.h", "dev_keys.h" (the published
development keys as data) and the C standard headers below, and may name only rocketnet_ identifiers that sdk/rocketnet.h declares.
Prints one OK line per source; exits 1 naming file:line for a refusal.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "examples"
HEADER = ROOT / "sdk" / "rocketnet.h"

ALLOWED_HEADERS = {
    "rocketnet.h",
    "dev_keys.h",
    "stdio.h",
    "time.h",
    "stdlib.h",
    "string.h",
    "stdint.h",
    "stdbool.h",
    "inttypes.h",
}

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
IDENTIFIER = re.compile(r"\brocketnet_[A-Za-z0-9_]+\b")


def strip_comments(text):
    """Blank out comments while keeping every newline, so line numbers hold."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", lambda m: " " * len(m.group(0)), text)


def blank_strings(text):
    """Blank out string literals while keeping every newline, so an include
    name or a message is not mistaken for an identifier."""
    return re.sub(r'"(?:\\.|[^"\\])*"', lambda m: " " * len(m.group(0)), text)


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def main():
    sources = sorted(EXAMPLES.glob("*.c"))
    if not sources:
        print("FAIL: no examples/*.c", file=sys.stderr)
        return 1
    if not HEADER.is_file():
        print(f"FAIL: {HEADER} is missing", file=sys.stderr)
        return 1

    declared = set(IDENTIFIER.findall(blank_strings(strip_comments(HEADER.read_text(encoding="utf-8")))))
    failed = False

    for source_path in sources:
        rel = source_path.relative_to(ROOT).as_posix()
        source = source_path.read_text(encoding="utf-8")
        cleaned = blank_strings(strip_comments(source))
        source_failed = False

        includes = list(INCLUDE.finditer(source))
        if not any(m.group(2) == "rocketnet.h" for m in includes):
            print(f'{rel}:1: does not include "rocketnet.h"', file=sys.stderr)
            source_failed = True

        for match in includes:
            name = match.group(2)
            if name not in ALLOWED_HEADERS:
                print(f'{rel}:{line_of(source, match.start())}: #include "{name}" is not allowed', file=sys.stderr)
                source_failed = True

        for match in IDENTIFIER.finditer(cleaned):
            name = match.group(0)
            if name not in declared:
                print(f"{rel}:{line_of(cleaned, match.start())}: {name} is not declared in sdk/rocketnet.h", file=sys.stderr)
                source_failed = True

        if source_failed:
            failed = True
        else:
            print(f"OK {rel}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
