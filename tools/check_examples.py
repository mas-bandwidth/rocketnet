#!/usr/bin/env python3
"""Check the examples against the house rules.

Each examples/*.c may include only "rocketnet.h", "dev_keys.h" (the examples'
protocol id and packet filter), "shutdown.h" (the command-line wait for
shutdown), "thread.h" (the platform's threads, start and join, for the
server examples' split-off half of the network update) and the C standard
headers below, and may name only rocketnet_
identifiers that sdk/rocketnet.h declares. A rocketnet_client_connect call's
status is kept in a variable, never compared where it is made, so a refused
connect reports its own status and not the earlier create's
(docs/GETTING-STARTED.md, "A call fails"). A scalar a scanf call writes
through &name is declared with an initializer: a failed conversion leaves it
unwritten, and it must not then be read uninitialized.
Prints one OK line per source; exits 1 naming file:line for a refusal.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ALLOWED_HEADERS = {
    "rocketnet.h",
    "dev_keys.h",
    "shutdown.h",
    "thread.h",
    "stdio.h",
    "time.h",
    "stdlib.h",
    "string.h",
    "stdint.h",
    "stdbool.h",
    "inttypes.h",
    "errno.h",
}

# examples that may also include platform headers, and only these. soak.c is
# a load harness, not a one-feature example: its allocator telemetry takes a
# native lock across callback threads and, on Linux, maps its arena
HARNESS_HEADERS = {
    "soak.c": {"windows.h", "pthread.h", "sys/mman.h"},
    # thread.h is the server examples' stand-in for a game's job system: the
    # platform's own threads and locks, and nothing of rocketnet
    "thread.h": {"windows.h", "pthread.h"},
    # config_env.c runs itself again with one variable set, spawned from its
    # executable path with an explicit environment, never through a shell
    "config_env.c": {"windows.h", "wchar.h", "spawn.h", "sys/wait.h"},
}

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
IDENTIFIER = re.compile(r"\brocketnet_[A-Za-z0-9_]+\b")
CONNECT_COMPARED = re.compile(r"\brocketnet_client_connect\s*\([^;{}]*?\)\s*[!=]=")
SCANF_CALL = re.compile(r"\b[fs]?scanf\s*\(([^;]*)\)\s*")
SCANF_TARGET = re.compile(r"&\s*([A-Za-z_][A-Za-z0-9_]*)\b")


def uninitialized_declaration(name):
    """A scalar declaration of name with no initializer, e.g. "unsigned int byte;"."""
    return re.compile(r"\b(?:unsigned|signed|int|long|short|char|float|double|[A-Za-z0-9_]+_t)\s+" + re.escape(name) + r"\s*;")


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


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check the examples against the house rules.")
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="tree holding examples/*.c and sdk/rocketnet.h (default: this repository)")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    examples = root / "examples"
    header = root / "sdk" / "rocketnet.h"
    sources = sorted(examples.glob("*.c")) + sorted(examples.glob("thread.h"))
    if not sources:
        print(f"FAIL: 0 files match {examples}/*.c", file=sys.stderr)
        return 1
    if not header.is_file():
        print(f"FAIL: {header} is missing", file=sys.stderr)
        return 1

    declared = set(IDENTIFIER.findall(blank_strings(strip_comments(header.read_text(encoding="utf-8")))))
    failed = False

    for source_path in sources:
        rel = source_path.relative_to(root).as_posix()
        source = source_path.read_text(encoding="utf-8")
        cleaned = blank_strings(strip_comments(source))
        source_failed = False

        includes = list(INCLUDE.finditer(source))
        if source_path.suffix == ".c" and not any(m.group(2) == "rocketnet.h" for m in includes):
            print(f'{rel}:1: does not include "rocketnet.h"', file=sys.stderr)
            source_failed = True

        for match in includes:
            name = match.group(2)
            if name not in ALLOWED_HEADERS and name not in HARNESS_HEADERS.get(source_path.name, set()):
                print(f'{rel}:{line_of(source, match.start())}: #include "{name}" is not allowed', file=sys.stderr)
                source_failed = True

        for match in IDENTIFIER.finditer(cleaned):
            name = match.group(0)
            if name not in declared:
                print(f"{rel}:{line_of(cleaned, match.start())}: {name} is not declared in sdk/rocketnet.h", file=sys.stderr)
                source_failed = True

        for match in CONNECT_COMPARED.finditer(cleaned):
            print(f"{rel}:{line_of(cleaned, match.start())}: rocketnet_client_connect's status is compared, not kept; keep it and report it", file=sys.stderr)
            source_failed = True

        for call in SCANF_CALL.finditer(cleaned):
            for target in SCANF_TARGET.finditer(call.group(1)):
                for decl in uninitialized_declaration(target.group(1)).finditer(cleaned):
                    print(f"{rel}:{line_of(cleaned, decl.start())}: {target.group(1)} is written by scanf but declared without an initializer; a failed conversion leaves it uninitialized", file=sys.stderr)
                    source_failed = True

        if source_failed:
            failed = True
        else:
            print(f"OK {rel}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
