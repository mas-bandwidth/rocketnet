#!/usr/bin/env python3
"""Unit-suite purity check.

The owner's rule: unit tests use no real time, no sleep, no threads, no
network and no resolver. This check scans the unit suite's sources and fails,
naming file and line, on any call that would force a real platform primitive:
a sleep, a wall-clock read, a thread start, a hostname resolution, the C
library's time() or a std::thread.

The scanned files are every test_unit_*.cpp at the repository root and
sdk/rocketnet_tests.cpp (the unit-registered cases live there). Comments are
stripped before matching, and linker-wrap symbol declarations
(__real_/__wrap_rocketnet_platform_time) are not calls.

usage: python3 tools/check_unit_purity.py [dir=   (default the repository root)
"""

import os
import re
import sys

PATTERNS = [
    ("rocketnet_platform_sleep", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_sleep\s*\(')),
    ("rocketnet_platform_time", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_time\s*\(')),
    ("rocketnet_platform_thread_create", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_thread_create\s*\(')),
    ("getaddrinfo", re.compile(r'(?<![A-Za-z0-9_])getaddrinfo\s*\(')),
    ("std::thread", re.compile(r'std::thread')),
    ("time(", re.compile(r'(?<![A-Za-z0-9_])time\s*\(')),
]


def strip_comments(text):
    """Remove /* */ and // comments from source text."""
    out = []
    i = 0
    n = len(text)
    in_block = False
    while i < n:
        if in_block:
            end = text.find("*/", i)
            if end == -1:
                break
            in_block = False
            i = end + 2
            continue
        if text[i] == "/" and i + 1 < n and text[i + 1] == "*":
            in_block = True
            i += 2
            continue
        if text[i] == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            if end == -1:
                break
            i = end
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def scan_lines(lines, prefix):
    findings = []
    for lineno, line in enumerate(lines, 1):
        stripped = strip_comments(line)
        for name, regex in PATTERNS:
            if regex.search(stripped):
                findings.append((lineno, name))
    return findings


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    files = []
    for entry in sorted(os.listdir(root)):
        if entry.startswith("test_unit_") and entry.endswith(".cpp"):
            files.append(os.path.join(root, entry))
    sdk_tests = os.path.join(root, "sdk", "rocketnet_tests.cpp")
    if os.path.exists(sdk_tests):
        files.append(sdk_tests)

    failures = []
    for path in files:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError as exc:
            print("FAIL check_unit_purity: cannot read %s: %s" % (path, exc))
            return 1
        for lineno, name in scan_lines(lines, path):
            failures.append((path, lineno, name))

    if failures:
        for path, lineno, name in failures:
            print("FAIL check_unit_purity: %s:%d: %s" % (os.path.relpath(path, root), lineno, name))
        return 1

    print("ok check_unit_purity: no real-time, sleep, thread or resolver call in the unit suite")
    return 0


if __name__ == "__main__":
    sys.exit(main())
