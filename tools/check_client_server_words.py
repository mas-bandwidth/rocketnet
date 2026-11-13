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

Usage: check_client_server_words.py [--root DIR]   (default: this repository;
0 tracked files under DIR is an error)

Every exception is tied to its sites (VERIFY-3 A-6): a whole file is
allowed only at the content its sha256 pins, so new text in it is read like
any other file's; a span is allowed only in the files it names; and an
exception with no site left (a pinned file gone, a named file with no match
of its span) fails as stale.
"""

import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WORD = "pe" + "er"
PATTERN = re.compile(WORD, re.IGNORECASE)

WORD2 = "snap" + "shot"
PATTERN2 = re.compile(WORD2, re.IGNORECASE)

# Whole files that keep the word: the sha256 of the content allowed, and the
# reason. Changed content is read line by line like any other file.
FILES_ALLOWED = {
    "LICENSE":
        ("0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0",
         "the GNU AGPL v3 text, third-party and verbatim"),
    "docs/RELEASE-NOTES-v1.0.0-preview-2.md":
        ("639b58572eb213e9b1ecec9d83a8deb6149e2a8c62721907088c9db1ee18cc65",
         "the published preview-2 record; its owner rewords it separately"),
    "docs/QUALIFICATION-v1.0.0-preview-2.md":
        ("3d7958e9a82c214716d6de549e010613596c0768446b12b5204a9c65d78cd162",
         "the published preview-2 record; its owner rewords it separately"),
}

# Spans inside a line that hold the word as part of an outside name: the
# span, the files it is allowed in, and the reason. A match is allowed when
# it lies inside one of these spans and the file is one the span names.
SPANS_ALLOWED = (
    (re.compile(r"\btype veth " + WORD + r" name\b"),
     (".github/workflows/nightly-soak.yml", "scripts/harden-cell", "scripts/netem-pair-soak", "scripts/outage-pair-real",
      "tests/functional/posix_lifecycle_stress.cpp", "tests/functional/posix_socket_errors.cpp",
      "tests/functional/rocketnet_functional_tests.cpp", "tests/test_xdp_filter.cpp"),
     "iproute2 syntax: `ip link add A type veth " + WORD + " name B`"),
    (re.compile(r"\b(?:Argument)?TypeError\b"),
     ("scripts/live-xdp-fleet-merge", "tools/check_client_server_words.py", "tools/plan_tlc_nightly.py",
      "tools/profile_1000_load.py", "tools/real_load_report.py", "tools/replay_ci_steps.py",
      "tools/run_backend_examples.py", "tools/test_check_client_server_words.py",
      "tools/test_profile_1000_load.py"),
     "Python exception names: 'TypeError' holds the letters, not the word"),
    (re.compile(r"\bPE" + r"Error\b"),
     ("tools/check_hardening.py",),
     "tools/check_hardening.py's Windows PE image parse error class: 'PE' "
     "then 'Error' holds the letters, not the word"),
)

# The same, for the second banned word.
SPANS_ALLOWED2 = (
    (re.compile(r"\bCreateToolhelp32" + WORD2.capitalize() + r"\b"),
     ("tests/load/harden_load.cpp", "tests/load/load.c", "tests/load/load_census.h", "tests/process_resources.h"),
     "Windows API name: CreateToolhelp32" + WORD2.capitalize() + " is the "
     "operating system's name, not this repository's word"),
    (re.compile(r"\bhost_" + WORD2 + r"\b"),
     ("tools/run_xdp_filter_guest.sh",),
     "tools/run_xdp_filter_guest.sh host-state helper: that script is outside "
     "this check's renaming paths and its owner renames it separately"),
)


def span_sites(path, text, spans1=None, spans2=None):
    """The index (0-based, over spans1 then spans2) of every span that excuses a banned word in text."""
    spans1 = SPANS_ALLOWED if spans1 is None else spans1
    spans2 = SPANS_ALLOWED2 if spans2 is None else spans2
    used = set()
    numbered = list(enumerate(spans1))
    numbered2 = list(enumerate(spans2, len(spans1)))
    for line in text.splitlines():
        for pattern, spans in ((PATTERN, numbered), (PATTERN2, numbered2)):
            for index, (span, files, _) in spans:
                if path in files and any(pattern.search(m.group(0)) for m in span.finditer(line)):
                    used.add(index)
    return used


def violations_in_text(path, text, spans1=None, spans2=None):
    """Every (path, line number, line) in text where a banned word is not allowed."""
    spans1 = SPANS_ALLOWED if spans1 is None else spans1
    spans2 = SPANS_ALLOWED2 if spans2 is None else spans2
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        for pattern, spans in ((PATTERN, spans1), (PATTERN2, spans2)):
            allowed = []
            for span, files, _ in spans:
                if path in files:
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


def check(root, files, allowed_files=None, spans1=None, spans2=None):
    allowed_files = FILES_ALLOWED if allowed_files is None else allowed_files
    spans1 = SPANS_ALLOWED if spans1 is None else spans1
    spans2 = SPANS_ALLOWED2 if spans2 is None else spans2
    found = []
    pinned_seen = set()
    sites = set()
    for name in files:
        found.extend(violations_in_path(name))
        path = root / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        if name in allowed_files:
            pinned_seen.add(name)
            digest = hashlib.sha256(data).hexdigest()
            if digest == allowed_files[name][0]:
                continue
            found.append((name, 0, "allowed whole file changed: sha256 %s, pinned %s; its new text is "
                                   "read like any other file's" % (digest, allowed_files[name][0])))
        if b"\0" in data[:8192]:
            continue
        text = data.decode("utf-8", errors="replace")
        found.extend(violations_in_text(name, text, spans1, spans2))
        for index in span_sites(name, text, spans1, spans2):
            sites.add((index, name))
    for name in sorted(allowed_files):
        if name not in pinned_seen:
            found.append((name, 0, "stale whole-file exception: no such tracked file"))
    for index, (span, span_files, _) in enumerate(list(spans1) + list(spans2)):
        for name in span_files:
            if (index, name) not in sites:
                found.append((name, 0, "stale span exception %r: it excuses nothing in this file" % span.pattern))
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
    for path, number, line in found:
        print(f"{path}:{number}: says a banned word ('{WORD}' or '{WORD2}'): {line}")
    if found:
        print(f"FAIL: {len(found)} line(s) say a banned word. RocketNet is client/server: say client, server, "
              "the remote endpoint, the other side of the connection, the sender or the receiver; "
              f"and for '{WORD2}' say a read, a copy, a record or the state. "
              "Exceptions live in tools/check_client_server_words.py.")
        return 1
    sites = sum(len(named) for _, named, _ in SPANS_ALLOWED + SPANS_ALLOWED2)
    print(f"OK: no '{WORD}' or '{WORD2}' in {len(files)} tracked files outside {len(FILES_ALLOWED)} allowed files "
          f"(content pinned) and {len(SPANS_ALLOWED) + len(SPANS_ALLOWED2)} allowed spans in {sites} named files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
