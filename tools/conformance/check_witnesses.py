#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""Witness coverage for the behavioural MUSTs of the RocketNet standard.

docs/WITNESSES.tsv names, for each behavioural rule, the anchor that would
move if the rule moved: the section id in standard/TRANSPORT.md (RN-1) or
standard/MESSAGES.md (RN-2), the TLA+ invariant (if one exists), the C++
function, and the test. This checker holds those anchors to the tree. It
fails when an anchor is stale -- the section id no longer resolves through
check_rn_standard.py's resolver, the named section (or, where the standard
keeps the rule's text only in standard/RATIONALE.md under the same section
id, that rationale section) no longer carries the rule text, or the
function, test or invariant is gone. That is a coverage audit, not a
behaviour test: it never runs the tests.

A row whose tier is `none` names a rule with no unit or functional witness.
The gate does NOT fail on a gap; it counts and lists the gaps, because a gap
is honest and a silent one is not. The gate fails only on a stale anchor.
A row whose tier is `model` is witnessed by a TLA+ model check: its rule
column states the modelled property, so the rule-text check does not apply
to it; its invariant, model-check configuration and code anchors must all
resolve.

Run from anywhere:  python3 tools/conformance/check_witnesses.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import check_rn_standard as rn_standard  # noqa: E402

TSV = os.path.join(ROOT, "docs", "WITNESSES.tsv")
TLA_DIR = os.path.join(ROOT, "proofs", "tla")

COLUMNS = ["section", "rule", "tla_invariant", "code_function", "test", "tier"]
TIERS = ("unit", "none", "model")


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


def normalize(text):
    return re.sub(r"\s+", " ", text).strip()


def section_text(doc, ident):
    """The normalized text of one section, subsections included."""
    section = doc.anchors.get(ident)
    if section is None:
        return None
    end = len(doc.lines)
    for number in range(section.line + 1, len(doc.lines) + 1):
        match = rn_standard.HEADING_RE.match(doc.lines[number - 1])
        if match and len(match.group(1)) <= section.level:
            end = number - 1
            break
    return normalize("\n".join(doc.lines[section.line - 1:end]))


def rule_carried(docs, rationale, ident, rule):
    """True when the anchored section, or the rationale section of the same
    id where the standard keeps the text there, carries the rule text."""
    needle = normalize(rule)
    for doc in docs:
        text = section_text(doc, ident)
        if text is not None and needle in text:
            return True
    if rationale is not None:
        for candidate in (ident, ident.split(".")[0]):
            text = section_text(rationale, candidate)
            if text is not None and needle in text:
                return True
    return False


def anchor_invariant_exists(root, name):
    tla_dir = os.path.join(root, "proofs", "tla")
    pattern = re.compile(r"(?m)^\s*" + re.escape(name) + r"\s*==")
    for entry in sorted(os.listdir(tla_dir)):
        if not entry.endswith(".tla"):
            continue
        with open(os.path.join(tla_dir, entry), "r", encoding="utf-8", errors="replace") as handle:
            if pattern.search(handle.read()):
                return entry
    return None


def anchor_name_exists(root, anchor):
    path, _, function = anchor.partition(":")
    if not path:
        return None
    full = os.path.join(root, path)
    if not os.path.isfile(full):
        return None
    if not function:
        return path
    with open(full, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if re.search(r"\b" + re.escape(function) + r"\b", text):
        return path
    return None


def load_documents(root):
    standard = os.path.join(root, "standard")
    docs = [
        rn_standard.Document("RN-1", os.path.join(standard, "TRANSPORT.md")),
        rn_standard.Document("RN-2", os.path.join(standard, "MESSAGES.md")),
    ]
    rationale = rn_standard.Document("RN-R", os.path.join(standard, "RATIONALE.md"))
    return docs, rationale


def run(root=None):
    """Verify every anchor. Return (exit_code, report_text)."""
    if root is None:
        root = ROOT
    tsv = os.path.join(root, "docs", "WITNESSES.tsv")

    try:
        docs, rationale = load_documents(root)
    except OSError as error:
        return 1, "the standard could not be read: %s" % error

    try:
        rows = read_tsv(tsv)
    except (OSError, ValueError) as error:
        return 1, "WITNESSES.tsv could not be read: %s" % error

    failures = []
    gaps = []
    checked = 0

    for index, cells in enumerate(rows, start=2):
        section_id, rule, invariant, code, test, tier = cells
        where = "WITNESSES.tsv line %d (section %s)" % (index, section_id)

        if tier not in TIERS:
            failures.append("%s: tier %r is not one of %s" % (where, tier, ", ".join(TIERS)))
            continue

        resolved = any(section_id in doc.anchors for doc in docs)
        if not resolved:
            failures.append("%s: section id %r does not resolve in "
                            "standard/TRANSPORT.md or standard/MESSAGES.md" % (where, section_id))
            continue

        checked += 1

        if tier != "model" and not rule_carried(docs, rationale, section_id, rule):
            failures.append("%s: the section no longer carries the rule text %r -- "
                            "the rule moved or the anchor is stale" % (where, rule))

        if invariant != "-":
            if anchor_invariant_exists(root, invariant) is None:
                failures.append("%s: TLA+ invariant %r is in no proofs/tla/*.tla"
                                % (where, invariant))

        if code != "-":
            if anchor_name_exists(root, code) is None:
                failures.append("%s: code anchor %r does not resolve" % (where, code))

        if tier == "none":
            if test != "-":
                failures.append("%s: a tier none row names no test, but names %r" % (where, test))
            gaps.append((section_id, rule))
        else:
            if test == "-":
                failures.append("%s: a tier %s row names no test" % (where, tier))
            elif anchor_name_exists(root, test) is None:
                failures.append("%s: test anchor %r does not resolve" % (where, test))

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
    for section_id, rule in gaps:
        lines.append("  - %s %s" % (section_id, rule))
    return 0, "\n".join(lines)


def main():
    code, report = run()
    print(report)
    return code


if __name__ == "__main__":
    sys.exit(main())
