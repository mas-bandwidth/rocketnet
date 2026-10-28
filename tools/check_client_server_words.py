#!/usr/bin/env python3
"""Refuse the owner-banned words anywhere in the repository.

RocketNet is client/server: a client talks to a server and never to another
client. Every name, comment, log string, document and model says client,
server, the remote endpoint, the other side of the connection, the sender or
the receiver, whichever is exactly right (owner, 2026-10-22). This check
fails when that word appears, in any case and inside any compound, in a
tracked file's path or text, outside the exceptions below.

The second banned word (lens audit L5.4) names a frozen copy of live state;
say a read, a copy, a record or the state, whichever is exactly right. It is
refused in any case and inside any compound, outside the exceptions below.

Each word is spelled here built from two halves, so this file and its
tests never hold either.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WORD = "pe" + "er"
PATTERN = re.compile(WORD, re.IGNORECASE)

WORD2 = "snap" + "shot"
PATTERN2 = re.compile(WORD2, re.IGNORECASE)

# Whole files that keep the word, with the reason.
FILES_ALLOWED = {
    "LICENSE":
        "the GNU AGPL v3 text, third-party and verbatim",
    "docs/RELEASE-NOTES-v1.0.0-preview-2.md":
        "the published preview-2 record; its owner rewords it separately",
    "docs/QUALIFICATION-v1.0.0-preview-2.md":
        "the published preview-2 record; its owner rewords it separately",
}

# Spans inside a line that hold the word as part of an outside name, with the
# reason. A match is allowed when it lies inside one of these spans.
SPANS_ALLOWED = (
    (re.compile(r"\btype veth " + WORD + r" name\b"),
     "iproute2 syntax: `ip link add A type veth " + WORD + " name B`"),
    (re.compile(r"\b(?:Argument)?TypeError\b"),
     "Python exception names: 'TypeError' holds the letters, not the word"),
    (re.compile(r"\bPE" + r"Error\b"),
     "tools/check_hardening.py's Windows PE image parse error class: 'PE' "
     "then 'Error' holds the letters, not the word"),
)

# The same, for the second banned word.
SPANS_ALLOWED2 = (
    (re.compile(r"\bCreateToolhelp32" + WORD2.capitalize() + r"\b"),
     "Windows API name: CreateToolhelp32" + WORD2.capitalize() + " is the "
     "operating system's name, not this repository's word"),
    (re.compile(r"\bhost_" + WORD2 + r"\b"),
     "tools/run_xdp_filter_guest.sh host-state helper: that script is outside "
     "this check's renaming paths and its owner renames it separately"),
)


def violations_in_text(path, text):
    """Every (path, line number, line) in text where a banned word is not allowed."""
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        for pattern, spans in ((PATTERN, SPANS_ALLOWED), (PATTERN2, SPANS_ALLOWED2)):
            allowed = []
            for span, _ in spans:
                allowed.extend(m.span() for m in span.finditer(line))
            hit = False
            for match in pattern.finditer(line):
                start, end = match.span()
                if not any(a <= start and end <= b for a, b in allowed):
                    found.append((path, number, line.strip()))
                    hit = True
                    break
            if hit:
                break
    return found


def violations_in_path(path):
    """The path itself, when its name holds a banned word."""
    if PATTERN.search(path) or PATTERN2.search(path):
        return [(path, 0, "file name")]
    return []


def tracked_files(root):
    output = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                            stdout=subprocess.PIPE).stdout
    return [name for name in output.decode("utf-8").split("\0") if name]


def check(root, files):
    found = []
    for name in files:
        found.extend(violations_in_path(name))
        if name in FILES_ALLOWED:
            continue
        path = root / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" in data[:8192]:
            continue
        found.extend(violations_in_text(name, data.decode("utf-8", errors="replace")))
    return found


def main():
    found = check(ROOT, tracked_files(ROOT))
    for path, number, line in found:
        print(f"{path}:{number}: says a banned word ('{WORD}' or '{WORD2}'): {line}")
    if found:
        print(f"FAIL: {len(found)} line(s) say a banned word. RocketNet is client/server: say client, server, "
              "the remote endpoint, the other side of the connection, the sender or the receiver; "
              f"and for '{WORD2}' say a read, a copy, a record or the state. "
              "Exceptions live in tools/check_client_server_words.py.")
        return 1
    print(f"OK: no '{WORD}' or '{WORD2}' outside {len(FILES_ALLOWED)} allowed files and "
          f"{len(SPANS_ALLOWED) + len(SPANS_ALLOWED2)} allowed spans")
    return 0


if __name__ == "__main__":
    sys.exit(main())
