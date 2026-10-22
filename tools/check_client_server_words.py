#!/usr/bin/env python3
"""Refuse the word for a client-to-client party anywhere in the repository.

RocketNet is client/server: a client talks to a server and never to another
client. Every name, comment, log string, document and model says client,
server, the remote endpoint, the other side of the connection, the sender or
the receiver, whichever is exactly right (Glenn, 2026-10-22). This check
fails when the word appears, in any case and inside any compound, in a
tracked file's path or text, outside the exceptions below.

The word is spelled here as WORD, built from two halves, so this file and its
tests never hold it.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WORD = "pe" + "er"
PATTERN = re.compile(WORD, re.IGNORECASE)

# Whole files that keep the word, with the reason.
FILES_ALLOWED = {
    "LICENSE":
        "the GNU AGPL v3 text, third-party and verbatim",
    "docs/RELEASE-NOTES-v1.0.0-preview-2.md":
        "the published preview-2 record; its owner rewords it separately",
    "docs/QUALIFICATION-v1.0.0-preview-2.md":
        "the published preview-2 record; its owner rewords it separately",
    # TEMPORARY: measured TLC records are never edited by hand (tla/README.md).
    # The next tlacheck run over the stale groups rewrites this row with the
    # renamed invariant; remove this entry in that change.
    "tla/RUNS.tsv":
        "measured TLC records, rewritten only by a tlacheck run",
}

# Spans inside a line that hold the word as part of an outside name, with the
# reason. A match is allowed when it lies inside one of these spans.
SPANS_ALLOWED = (
    (re.compile(r"\btype veth " + WORD + r" name\b"),
     "iproute2 syntax: `ip link add A type veth " + WORD + " name B`"),
    (re.compile(r"\b(?:Argument)?TypeError\b"),
     "Python exception names: 'TypeError' holds the letters, not the word"),
)


def violations_in_text(path, text):
    """Every (path, line number, line) in text where the word is not allowed."""
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        allowed = []
        for span, _ in SPANS_ALLOWED:
            allowed.extend(m.span() for m in span.finditer(line))
        for match in PATTERN.finditer(line):
            start, end = match.span()
            if not any(a <= start and end <= b for a, b in allowed):
                found.append((path, number, line.strip()))
                break
    return found


def violations_in_path(path):
    """The path itself, when its name holds the word."""
    if PATTERN.search(path):
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
        print(f"{path}:{number}: says '{WORD}': {line}")
    if found:
        print(f"FAIL: {len(found)} line(s) say '{WORD}'. RocketNet is client/server: say client, server, "
              "the remote endpoint, the other side of the connection, the sender or the receiver. "
              "Exceptions live in tools/check_client_server_words.py.")
        return 1
    print(f"OK: no '{WORD}' outside {len(FILES_ALLOWED)} allowed files and {len(SPANS_ALLOWED)} allowed spans")
    return 0


if __name__ == "__main__":
    sys.exit(main())
