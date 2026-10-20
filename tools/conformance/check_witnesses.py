#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""Witness coverage for the behavioural MUSTs of the standard in standard/.

docs/WITNESSES.tsv names, for each behavioural MUST, the anchor that would
move if the rule moved: the standard line (a document of standard/ and a line
number, written TRANSPORT.md:1088), the TLA+ invariant (if one exists), the
C++ function, and the test. A bare line number names no document and is a
stale anchor. This checker holds those anchors to
the tree. It fails when an anchor is stale -- the named line no longer
carries the rule's keyword, or the function, test or invariant is gone.
That is a coverage audit, not a behaviour test: it never runs the tests.

A row whose tier is `none` names a rule with no unit or functional witness.
The gate does NOT fail on a gap; it counts and lists the gaps, because a gap
is honest and a silent one is not. The gate fails only on a stale anchor.

Run from anywhere:  python3 tools/conformance/check_witnesses.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STANDARD_DIR = os.path.join(ROOT, "standard")
TLA_DIR = os.path.join(ROOT, "tla")
TSV = os.path.join(ROOT, "docs", "WITNESSES.tsv")

COLUMNS = ["standard_line", "rule", "tla_invariant", "code_function", "test", "tier"]


def read_tsv(path):
    with open(path, "r", encoding="utf-8") as handle:
        rows = []
        header = None
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip():
                continue
            cells = line.split("\t")
            if header is None:
                if cells != COLUMNS:
                    raise ValueError("WITNESSES.tsv header is %r, expected %r" % (cells, COLUMNS))
                header = cells
                continue
            if len(cells) != len(COLUMNS):
                raise ValueError("WITNESSES.tsv row has %d cells, expected %d: %r"
                                 % (len(cells), len(COLUMNS), cells))
            rows.append(cells)
    return rows


def read_standard_line(anchor):
    """Resolve DOC:LINE under standard/. Return (text, None) or (None, why)."""
    document, _, number = anchor.rpartition(":")
    if not document or not number:
        return None, "standard_line %r is not DOC:LINE under standard/" % anchor
    try:
        line_number = int(number)
    except ValueError:
        return None, "standard_line %r has no line number" % anchor
    path = os.path.normpath(os.path.join(STANDARD_DIR, document))
    if os.path.dirname(path) != os.path.normpath(STANDARD_DIR) or not os.path.isfile(path):
        return None, "standard/%s is not a document of standard/" % document
    with open(path, "r", encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    if line_number < 1 or line_number > len(lines):
        return None, "standard/%s has no line %d" % (document, line_number)
    return lines[line_number - 1], None


def anchor_invariant_exists(name):
    pattern = re.compile(r"(?m)^\s*" + re.escape(name) + r"\s*==")
    for entry in sorted(os.listdir(TLA_DIR)):
        if not entry.endswith(".tla"):
            continue
        with open(os.path.join(TLA_DIR, entry), "r", encoding="utf-8", errors="replace") as handle:
            if pattern.search(handle.read()):
                return entry
    return None


def anchor_code_exists(anchor):
    path, _, function = anchor.partition(":")
    if not path or not function:
        return None
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        return None
    with open(full, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if re.search(r"\b" + re.escape(function) + r"\b", text):
        return path
    return None


def anchor_test_exists(anchor):
    path, _, name = anchor.partition(":")
    if not path or not name:
        return None
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        return None
    with open(full, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if re.search(r"\b" + re.escape(name) + r"\b", text):
        return path
    return None


def run(root=None):
    """Verify every anchor. Return (exit_code, report_text)."""
    global ROOT, STANDARD_DIR, TLA_DIR, TSV
    if root is not None:
        ROOT = root
        STANDARD_DIR = os.path.join(ROOT, "standard")
        TLA_DIR = os.path.join(ROOT, "tla")
        TSV = os.path.join(ROOT, "docs", "WITNESSES.tsv")

    try:
        rows = read_tsv(TSV)
    except (OSError, ValueError) as error:
        return 1, "WITNESSES.tsv could not be read: %s" % error

    failures = []
    gaps = []
    checked = 0

    for index, cells in enumerate(rows, start=2):
        standard_line, rule, invariant, code, test, tier = cells
        where = "WITNESSES.tsv line %d (standard/%s)" % (index, standard_line)

        line_text, why = read_standard_line(standard_line)
        if line_text is None:
            failures.append("%s: %s" % (where, why))
            continue

        checked += 1
        if rule not in line_text:
            failures.append("%s: the line no longer carries the rule keyword %r -- "
                            "the rule moved or the anchor is stale" % (where, rule))

        if invariant != "-":
            if anchor_invariant_exists(invariant) is None:
                failures.append("%s: TLA+ invariant %r is in no tla/*.tla"
                                % (where, invariant))

        if code != "-":
            if anchor_code_exists(code) is None:
                failures.append("%s: code anchor %r does not resolve" % (where, code))

        if test != "-":
            if anchor_test_exists(test) is None:
                failures.append("%s: test anchor %r does not resolve" % (where, test))

        if tier == "none":
            gaps.append((standard_line, rule))

    lines = []
    if failures:
        lines.append("WITNESSES conformance FAILED (%d stale anchor(s) over %d rows):"
                     % (len(failures), checked))
        for failure in failures:
            lines.append("  - " + failure)
        lines.append("")
        lines.append("An anchor moved and its witness did not follow. Fix the row or the "
                     "rule; do not delete the row.")
        return 1, "\n".join(lines)

    lines.append("WITNESSES conformance: %d rules, every named anchor resolves." % checked)
    lines.append("GAPS (tier none): %d" % len(gaps))
    for standard_line, rule in gaps:
        lines.append("  - standard/%s %s" % (standard_line, rule))
    return 0, "\n".join(lines)


def main():
    code, report = run()
    print(report)
    return code


if __name__ == "__main__":
    sys.exit(main())
