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
function, test or invariant is gone. A citation Module.tla:Property, or
proofs/tla/Module.tla:Property, is stale unless that module defines
Property; a bare name still resolves in any proofs/tla module. That is a
coverage audit, not a behaviour test: it never runs the tests.

A row whose tier is `none` names a rule with no unit or functional witness.
The gate does NOT fail on a gap; it counts and lists the gaps, because a gap
is honest and a silent one is not. The gate fails only on a stale anchor.
A row whose tier is `model` is witnessed by a TLA+ model check: its rule
column states the modelled property, so the rule-text check does not apply
to it; its invariant, model-check configuration and code anchors must all
resolve. Its configuration must also be a proofs/tla/CASES.tsv case whose
module defines the property (itself or through what it EXTENDS or
INSTANCEs; a Module.tla citation must be in that set), must list the
property under INVARIANT(S) or PROPERTY(IES) (directly, or inside a listed
definition that names it), and its RUNS.tsv record must be the declared
outcome: a reversed witness is declared and measured as a violation of
that property, not of any other.

`review` names an explicit source-review disposition in docs/audits, not an
executed test or a timing proof. `deferred` names a boundary test for a mode
the SDK does not implement; it does not claim that mode's rule is satisfied.
Both categories are printed separately, so zero unclassified gaps cannot be
read as complete executable coverage.

What it does not check: that every MUST of the standard has a row. It holds
the rows the ledger has, and holds the ledger to docs/WITNESSES.floor.tsv,
one line per row (section, tier, a hash of the rule text): a row deleted, a
row whose tier changed (a unit witness downgraded to none), or a row added
fails until the floor says the same, so each is a diff someone makes on
purpose (VERIFY-3 A-2). `--write-floor` rewrites the floor from the ledger.
A ledger with no rows fails. A unit or deferred test anchor must be a
registered unit case (tools/check_unit_case_coverage.py's registry) of the
file it names: a name left in a comment after its test was renamed, which
resolved before, does not.

Run from anywhere:  python3 tools/conformance/check_witnesses.py
"""

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import check_rn_standard as rn_standard  # noqa: E402

sys.path.insert(0, os.path.dirname(HERE))
import check_unit_case_coverage as unit_cases  # noqa: E402

TSV = os.path.join(ROOT, "docs", "WITNESSES.tsv")
TLA_DIR = os.path.join(ROOT, "proofs", "tla")

COLUMNS = ["section", "rule", "tla_invariant", "code_function", "test", "tier"]
TIERS = ("unit", "none", "model", "review", "deferred")
FLOOR_COLUMNS = ["section", "tier", "rule_sha256"]
FLOOR_HEADER = ("# docs/WITNESSES.floor.tsv: one line per docs/WITNESSES.tsv row. A row deleted,\n"
                "# a tier changed or a row added fails check_witnesses.py until this file says the\n"
                "# same: python3 tools/conformance/check_witnesses.py --write-floor\n")


def rule_key(rule):
    return hashlib.sha256(normalize(rule).encode("utf-8")).hexdigest()[:16]


def floor_lines(rows):
    lines = ["\t".join(FLOOR_COLUMNS)]
    for cells in rows:
        lines.append("\t".join([cells[0], cells[5], rule_key(cells[1])]))
    return lines


def write_floor(path, rows):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(FLOOR_HEADER + "\n".join(floor_lines(rows)) + "\n")


def floor_failures(path, rows):
    """The ledger against its floor, row by row, by (section, rule hash)."""
    try:
        with open(path, "r", encoding="utf-8") as handle:
            lines = [line.rstrip("\n") for line in handle
                     if line.strip() and not line.startswith("#")]
    except OSError as error:
        return ["docs/WITNESSES.floor.tsv could not be read: %s" % error]
    if not lines or lines[0].split("\t") != FLOOR_COLUMNS:
        return ["docs/WITNESSES.floor.tsv header is not %r" % ("\t".join(FLOOR_COLUMNS),)]
    # (section, rule hash) -> tiers, as multisets: the ledger repeats a rule
    # in a section when two witnesses carry it, and each row counts.
    floor = {}
    for line in lines[1:]:
        cells = line.split("\t")
        if len(cells) != len(FLOOR_COLUMNS):
            return ["docs/WITNESSES.floor.tsv row %r has %d cells" % (line, len(cells))]
        floor.setdefault((cells[0], cells[2]), []).append(cells[1])
    ledger = {}
    rules = {}
    for cells in rows:
        key = (cells[0], rule_key(cells[1]))
        ledger.setdefault(key, []).append(cells[5])
        rules[key] = cells[1]
    failures = []
    for key in sorted(set(floor) | set(ledger)):
        want = sorted(floor.get(key, []))
        have = sorted(ledger.get(key, []))
        if want == have:
            continue
        if not have:
            failures.append("floor: section %s row %s (tier %s) is gone from WITNESSES.tsv; a deleted "
                            "or reworded row changes the floor in the same commit"
                            % (key[0], key[1], ", ".join(want)))
        elif not want:
            failures.append("floor: section %s %r is not in docs/WITNESSES.floor.tsv; add it "
                            "(--write-floor)" % (key[0], rules[key][:60]))
        else:
            failures.append("floor: section %s %r has tiers %s, the floor %s; a row deleted or a tier "
                            "changed changes the floor in the same commit"
                            % (key[0], rules[key][:60], "/".join(have), "/".join(want)))
    return failures


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


def strip_comments(text, line_comment, block_open, block_close, nested):
    """Blank comments and string literals, keeping line breaks, so a name left
    only in a comment or a string is not a definition."""
    out = list(text)
    i, n = 0, len(text)

    def blank(start, end):
        for k in range(start, end):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        if text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            blank(i, j)
            i = j
        elif text.startswith(block_open, i):
            depth, j = 1, i + len(block_open)
            while j < n and depth > 0:
                if nested and text.startswith(block_open, j):
                    depth, j = depth + 1, j + len(block_open)
                elif text.startswith(block_close, j):
                    depth, j = depth - 1, j + len(block_close)
                else:
                    j += 1
            blank(i, j)
            i = j
        elif text.startswith(line_comment, i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        else:
            i += 1
    return "".join(out)


def tla_code(text):
    """TLA+ text with its (* *) and \\* comments and strings blanked."""
    return strip_comments(text, "\\*", "(*", "*)", True)


def c_code(text):
    """C or C++ text with its // and /* */ comments and strings blanked."""
    return strip_comments(text, "//", "/*", "*/", False)


C_SOURCES = (".c", ".cpp", ".h", ".hpp", ".inc")


def anchor_invariant_exists(root, name):
    tla_dir = os.path.join(root, "proofs", "tla")
    pattern = re.compile(r"(?m)^\s*" + re.escape(name) + r"\s*==")
    for entry in sorted(os.listdir(tla_dir)):
        if not entry.endswith(".tla"):
            continue
        with open(os.path.join(tla_dir, entry), "r", encoding="utf-8", errors="replace") as handle:
            if pattern.search(tla_code(handle.read())):
                return entry
    return None


def split_module_citation(invariant):
    """Return (proofs/tla path, property) when the cell cites a module, else None."""
    if ":" not in invariant:
        return None
    module, name = invariant.split(":", 1)
    if not re.match(r"[A-Za-z][A-Za-z0-9_]*$", name):
        return None
    if module.startswith("proofs/tla/") and module.endswith(".tla") and ".." not in module.split("/"):
        return module, name
    if "/" not in module and module.endswith(".tla"):
        return "proofs/tla/" + module, name
    return None


def property_defined(root, module_relpath, name):
    """True when module_relpath defines name with ==."""
    path = os.path.join(root, module_relpath)
    if not os.path.isfile(path):
        return False
    pattern = re.compile(r"(?m)^\s*" + re.escape(name) + r"\s*==")
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return pattern.search(tla_code(handle.read())) is not None


def anchor_name_exists(root, anchor, code_anchor=False):
    """The anchor's path when its file exists and, when it names one, the name
    is in the file. code_anchor (the code_function column): the name is
    required, and in a C or C++ file it must be outside comments and strings,
    so a deleted function whose name a comment keeps does not resolve. (A test
    anchor's name is the registration string, which the unit registry checks.)"""
    path, _, function = anchor.partition(":")
    if not path:
        return None
    full = os.path.join(root, path)
    if not os.path.isfile(full):
        return None
    if not function:
        return None if code_anchor else path
    with open(full, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if code_anchor and path.endswith(C_SOURCES):
        text = c_code(text)
    if re.search(r"\b" + re.escape(function) + r"\b", text):
        return path
    return None


CFG_KEYWORDS = ("SPECIFICATION", "INIT", "NEXT", "CONSTANT", "CONSTANTS", "INVARIANT", "INVARIANTS",
                "PROPERTY", "PROPERTIES", "CHECK_DEADLOCK", "SYMMETRY", "VIEW", "ALIAS", "POSTCONDITION",
                "CONSTRAINT", "CONSTRAINTS", "ACTION_CONSTRAINT", "ACTION_CONSTRAINTS")
VIOLATIONS = ("invariant", "action", "temporal")


def read_ledger(path):
    """Rows of a proofs/tla TSV ledger as dicts keyed by its header, by config."""
    with open(path, "r", encoding="utf-8") as handle:
        lines = [line.rstrip("\n").split("\t") for line in handle if line.strip()]
    header = lines[0]
    return {cells[0]: dict(zip(header, cells)) for cells in lines[1:]}


def cfg_checks(path):
    """The names a TLC configuration lists under INVARIANT(S) and PROPERTY(IES)."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = re.sub(r"\\\*.*", "", handle.read())
    names = []
    listing = False
    for token in text.split():
        if token in CFG_KEYWORDS:
            listing = token.startswith("INVARIANT") or token.startswith("PROPERT")
            continue
        if listing:
            names.append(token)
    return names


def module_closure(root, module_file):
    """The proofs/tla modules a module EXTENDS or INSTANCEs, transitively, itself included."""
    tla_dir = os.path.join(root, "proofs", "tla")
    seen = []
    pending = [module_file[:-len(".tla")] if module_file.endswith(".tla") else module_file]
    while pending:
        name = pending.pop()
        path = os.path.join(tla_dir, name + ".tla")
        if name in seen or not os.path.isfile(path):
            continue
        seen.append(name)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = tla_code(handle.read())
        for match in re.finditer(r"\bEXTENDS\b([^\n]*)", text):
            pending.extend(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", match.group(1)))
        pending.extend(re.findall(r"\bINSTANCE\s+([A-Za-z_][A-Za-z0-9_]*)", text))
    return seen


def definition_body(root, modules, name):
    """The text of name's definition in the first module of modules that defines it."""
    pattern = re.compile(r"(?m)^\s*" + re.escape(name) + r"\s*==(.*(?:\n(?![A-Za-z_][A-Za-z0-9_]*\s*(?:\(|==)).*)*)")
    for module in modules:
        with open(os.path.join(root, "proofs", "tla", module + ".tla"), "r", encoding="utf-8",
                  errors="replace") as handle:
            match = pattern.search(tla_code(handle.read()))
        if match:
            return match.group(1)
    return None


def model_row_failures(root, where, invariant, test, cases, runs):
    """A model row's witness: its configuration is a case of a module that
    defines the property, it lists the property (directly or in a listed
    definition that names it), and its record is the declared outcome: for a
    reversed witness, a violation of that property and no other."""
    failures = []
    if invariant == "-":
        return ["%s: a tier model row names no property" % where]
    cited = split_module_citation(invariant)
    name = cited[1] if cited is not None else invariant
    if not (test.startswith("proofs/tla/") and test.endswith(".cfg") and "/" not in test[len("proofs/tla/"):]):
        return ["%s: a tier model row's test is a proofs/tla/*.cfg case, not %r" % (where, test)]
    config = test[len("proofs/tla/"):]
    if not os.path.isfile(os.path.join(root, test)):
        return ["%s: model-check configuration %s does not exist" % (where, test)]
    case = cases.get(config)
    if case is None:
        return ["%s: %s has no proofs/tla/CASES.tsv row" % (where, config)]
    modules = module_closure(root, case["module"])
    if cited is not None and os.path.basename(cited[0])[:-len(".tla")] not in modules:
        failures.append("%s: %s is a case of %s, which does not extend or instance %s"
                        % (where, config, case["module"], cited[0]))
    if definition_body(root, modules, name) is None:
        failures.append("%s: %s is not defined in %s or a module it extends"
                        % (where, name, case["module"]))
    listed = cfg_checks(os.path.join(root, test))
    if name not in listed and not any(
            re.search(r"\b" + re.escape(name) + r"\b", definition_body(root, modules, other) or "")
            for other in listed):
        failures.append("%s: %s does not list %s under INVARIANT(S) or PROPERTY(IES)"
                        % (where, config, name))
    if case["expected"] in VIOLATIONS and case["property"] != name:
        failures.append("%s: %s is declared to violate %s, not %s"
                        % (where, config, case["property"], name))
    record = runs.get(config)
    if record is None:
        failures.append("%s: %s has no proofs/tla/RUNS.tsv record" % (where, config))
    elif (record["result"], record["expected"], record["property"]) != ("PASS", case["expected"], case["property"]):
        failures.append("%s: %s is recorded %s %s %s, not the declared %s %s"
                        % (where, config, record["result"], record["expected"], record["property"],
                           case["expected"], case["property"]))
    return failures


def load_documents(root):
    standard = os.path.join(root, "standard")
    docs = [
        rn_standard.Document("RN-1", os.path.join(standard, "TRANSPORT.md")),
        rn_standard.Document("RN-2", os.path.join(standard, "MESSAGES.md")),
    ]
    rationale = rn_standard.Document("RN-R", os.path.join(standard, "RATIONALE.md"))
    return docs, rationale


def registered_test_failure(root, where, test, registry):
    """A unit or deferred witness names a registered unit case of its file."""
    path, _, name = test.partition(":")
    if registry.get(name) != path:
        return ("%s: test anchor %r is not a registered unit case of %s (registered: %s)"
                % (where, test, path, registry.get(name) or "nowhere"))
    return None


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
    dispositions = {"review": [], "deferred": []}
    checked = 0
    cases = runs = None
    registry = None

    if not rows:
        return 1, "WITNESSES.tsv has no rows: a ledger with only its header witnesses nothing"
    failures.extend(floor_failures(os.path.join(root, "docs", "WITNESSES.floor.tsv"), rows))

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
            cited = split_module_citation(invariant)
            if cited is not None:
                module_relpath, name = cited
                if not property_defined(root, module_relpath, name):
                    failures.append("%s: TLA+ property %r is not defined in %s"
                                    % (where, name, module_relpath))
            elif anchor_invariant_exists(root, invariant) is None:
                failures.append("%s: TLA+ invariant %r is in no proofs/tla/*.tla"
                                % (where, invariant))

        if tier == "model":
            if cases is None:
                try:
                    cases = read_ledger(os.path.join(root, "proofs", "tla", "CASES.tsv"))
                    runs = read_ledger(os.path.join(root, "proofs", "tla", "RUNS.tsv"))
                except (OSError, IndexError) as error:
                    return 1, "proofs/tla/CASES.tsv or RUNS.tsv could not be read: %s" % error
            failures.extend(model_row_failures(root, where, invariant, test, cases, runs))

        if code != "-":
            if anchor_name_exists(root, code, code_anchor=True) is None:
                failures.append("%s: code anchor %r does not resolve (a code anchor is path:function, "
                                "the function outside comments)" % (where, code))

        if tier == "none":
            if test != "-":
                failures.append("%s: a tier none row names no test, but names %r" % (where, test))
            gaps.append((section_id, rule))
        else:
            if test == "-":
                failures.append("%s: a tier %s row names no test" % (where, tier))
            elif anchor_name_exists(root, test) is None:
                failures.append("%s: test anchor %r does not resolve" % (where, test))
            elif tier in ("unit", "deferred"):
                if registry is None:
                    try:
                        registry = unit_cases.registered_cases(__import__("pathlib").Path(root))
                    except (OSError, ValueError) as error:
                        return 1, "the unit case registry could not be read: %s" % error
                failure = registered_test_failure(root, where, test, registry)
                if failure:
                    failures.append(failure)
            if tier == "review":
                path, separator, anchor = test.partition(":")
                if not path.startswith("docs/audits/") or not path.endswith(".md") or not separator or not anchor:
                    failures.append("%s: a review needs a named docs/audits Markdown disposition" % where)
            if tier in dispositions:
                dispositions[tier].append((section_id, rule, test))

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

    lines.append("WITNESSES conformance: %d rules, every named anchor resolves, the floor holds." % checked)
    tiers = {}
    for cells in rows:
        tiers[cells[5]] = tiers.get(cells[5], 0) + 1
    lines.append("TIERS: " + ", ".join("%s %d" % (tier, tiers.get(tier, 0)) for tier in TIERS))
    lines.append("GAPS (tier none): %d" % len(gaps))
    for section_id, rule in gaps:
        lines.append("  - %s %s" % (section_id, rule))
    for tier, label in (("review", "SOURCE REVIEWS (not executable proof)"),
                        ("deferred", "DEFERRED MODES (not implemented)")):
        lines.append("%s: %d" % (label, len(dispositions[tier])))
        for section_id, rule, evidence in dispositions[tier]:
            lines.append("  - %s %s [%s]" % (section_id, rule, evidence))
    return 0, "\n".join(lines)


def main():
    if sys.argv[1:] == ["--write-floor"]:
        rows = read_tsv(TSV)
        write_floor(os.path.join(ROOT, "docs", "WITNESSES.floor.tsv"), rows)
        print("wrote docs/WITNESSES.floor.tsv: %d rows" % len(rows))
        return 0
    if sys.argv[1:]:
        print("usage: check_witnesses.py [--write-floor]")
        return 2
    code, report = run()
    print(report)
    return code


if __name__ == "__main__":
    sys.exit(main())
