#!/usr/bin/env python3
"""Source-level mutation testing of the core protocol files against ./bin/test.

Each mutant is one textual change on a code line (never a comment, a string or a
preprocessor line). Every worker owns a copy of the tree under --work-dir, built
once and then rebuilt incrementally after the one-file change. The unit suite is
the oracle:

  detected        ./bin/test exits non-zero
  timed_out       ./bin/test runs past the timeout (counted as detected, listed apart)
  survived        ./bin/test exits zero
  did_not_compile the build fails (a text mutator has no type awareness)
  not_run         the time budget ran out first

Every operator this generator emits is risk-class, except a mutation inside a
rocketnet_assert argument, which a Release build compiles out and which is
therefore non-risk. The process exits 1 when a chosen risk-class mutant
survives. --self-test checks that rule on planted source: it does not compile
the SDK and it does not sleep.

--slice K/N runs one deterministic Nth of the list: the mutants whose slice key
(a hash of the file, the line's text, the column in it, the operator and the
replacement; never the line number, so a slice does not move when lines are
added above) is K modulo N. Slices 0 .. N-1 together are every mutant, so N
runs, one per slice, cover the whole list with the same exit rule. The nightly
runs the day's slice (.github/workflows/nightly.yml, docs/TESTING.md).

--platform linux leaves out the files a Linux build does not compile (the
WINDOWS and APPLE lists of cmake/RocketnetSources.cmake and the headers only
they include; whole files, not #if regions) and sets AF_XDP files apart: they
are mutated, but Linux reaches them only in a guest and ./bin/test runs none,
so their survivors are listed as not judged here, never failing. Both lists
are printed.

--baseline tools/mutation/survivors.tsv (the nightly) replaces the failure
rule with a reviewed list, the way the static-analysis gates work: every
known survivor is listed by a key that does not move with line numbers
(file, the line's text, the mutated line's text) and labelled `equivalent`
(with the reason no test can tell it from the library) or `gap` (with the
test owed). The run fails on a survivor the list does not hold, on an
`equivalent` entry whose mutant ran and no longer survives (stale), on a
`gap` entry whose mutant ran and no longer survives (remove it and lower the
ratchet), on an entry for a file in the run that the generator no longer
emits, and when the number of `gap` entries differs from the file's
gap-ratchet line. The gap count is printed as MUTATION-GAPS: it only goes
down. AF_XDP survivors are judged by the list too. --check-baseline checks
the list without a build (per commit). Without --baseline the campaign is
the original one.

Python 3 standard library only. Example:

  python3 tools/mutation/run_mutants.py --files sdk/rocketnet_ack.cpp \
      --max-mutants 40 --workers 4
"""

import argparse
import hashlib
import json
import os
import queue
import random
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

DEFAULT_FILES = [
    "sdk/rocketnet_ack.cpp",
    "sdk/rocketnet_protocol.cpp",
    "sdk/rocketnet_window_ack.cpp",
    "sdk/rocketnet_window_receive.cpp",
    "sdk/rocketnet_window_send.cpp",
]

RELATIONAL = {"<": ["<=", ">"], "<=": ["<", ">="], ">": [">=", "<"], ">=": [">", "<="], "==": ["!="], "!=": ["=="]}
ARITHMETIC = {"+": ["-"], "-": ["+"], "*": ["/"], "/": ["*"], "+=": ["-="], "-=": ["+="]}
BOOLEAN = {"&&": ["||"], "||": ["&&"]}
OPERATOR_RE = re.compile(r"<<=|>>=|<<|>>|->|<=|>=|==|!=|&&|\|\||\+\+|--|\+=|-=|\*=|/=|%=|[<>+\-*/=!]")
INT_RE = re.compile(r"(?<![\w.])(0[xX][0-9a-fA-F]+|\d+)([uUlL]*)(?![\w.])")
COND_RE = re.compile(r"\b(if|while)\s*\(")
ASSIGN_RE = re.compile(r"^(?P<lhs>[^=;]+?)\s*(?:=|\+=|-=|\*=|/=|\|=|&=|\^=|<<=|>>=)\s*(?!=)[^;]*;$")
INCREMENT_RE = re.compile(r"^(?:\+\+|--)[\w\->.\[\] ]+;$|^[\w\->.\[\] ]+(?:\+\+|--);$")
TYPE_WORDS = {"char", "short", "int", "long", "unsigned", "signed", "float", "double", "void", "bool", "const", "auto", "size_t"}
ASSERT_RE = re.compile(r"\brocketnet_assert\s*\(")

# The operators generate() emits. A survivor of one of these is a risk-class
# survivor: the unit suite did not kill it. did_not_compile is not survival.
RISK_OPERATORS = frozenset({
    "relational",
    "arithmetic",
    "boolean",
    "delete_plus_minus_one",
    "off_by_one",
    "negate_condition",
    "delete_statement",
})

# A run that judged nothing passes nothing. Judged: detected, timed_out or
# survived (an equivalent or gap survivor is judged); did_not_compile and
# not_run are not. A run fails when it judged 0 mutants, or when more of its
# chosen mutants than this share did not compile: a broken build or a
# generator that emits non-code shows as most of a run not compiling, and the
# run would otherwise pass on the few left. Measured (2026-11-12): 27 of 397
# (6.8%) on slice 69/90 of nightly run 43219892575 (head 76edea48e); 89 of
# 810 (11.0%) over the first slices of the whole-list campaign, the highest
# lane 23 of 141 (16.3%). The bound keeps the highest measured share at 60%
# of it (the repo's margin rule): 16.3 / 0.6 = 27.2, so 28%. A small --only
# or --files run can go over it by chance; such a run judged too little.
DID_NOT_COMPILE_BOUND = 0.28


def judgement_errors(mutants):
    """Print the did-not-compile share (every run, pass or fail) and return the
    failure lines: 0 judged, or a share over DID_NOT_COMPILE_BOUND."""
    chosen = len(mutants)
    judged = sum(m.get("status") in ("detected", "timed_out", "survived") for m in mutants)
    failed_build = sum(m.get("status") == "did_not_compile" for m in mutants)
    share = failed_build / chosen if chosen else 0.0
    print(f"MUTATION-DID-NOT-COMPILE {failed_build} of {chosen} chosen ({share:.1%}; bound "
          f"{DID_NOT_COMPILE_BOUND:.0%}); judged {judged}", flush=True)
    lines = []
    if judged == 0:
        lines.append(f"mutation run judged 0 mutants ({chosen} chosen, {failed_build} did not compile)")
    if share > DID_NOT_COMPILE_BOUND:
        lines.append(f"{failed_build} of {chosen} chosen mutants did not compile ({share:.1%}), over the "
                     f"{DID_NOT_COMPILE_BOUND:.0%} bound: the build or the generator is broken, and the run "
                     "judged too little to pass")
    return lines


def mutant_class(operator):
    return "risk" if operator in RISK_OPERATORS else "other"


def risk_survivors(mutants):
    """Risk-class mutants whose status is survived."""
    found = []
    for mutant in mutants:
        kind = mutant.get("class") or mutant_class(mutant.get("operator", ""))
        if kind == "risk" and mutant.get("status") == "survived":
            found.append(mutant)
    return found


def slice_key(mutant):
    """A mutant's slice number source: the same for the same change however
    the lines above it move (file, the line's text, the column within that
    text, operator, replacement), never its line number."""
    text = mutant["original"]
    indent = len(text) - len(text.lstrip())
    key = "\0".join((mutant["file"], text.strip(), str(mutant["column"] - indent),
                     mutant["operator"], mutant["mutated_token"]))
    return int.from_bytes(hashlib.sha256(key.encode()).digest()[:8], "big")


def in_slice(mutant, index, count):
    """True when the mutant belongs to slice index of count: every mutant is in
    exactly one slice, so slices 0 .. count-1 together are the whole list."""
    return slice_key(mutant) % count == index


def parse_slice(text):
    """'K/N' -> (K, N) with 0 <= K < N; raises ValueError otherwise."""
    index, sep, count = text.partition("/")
    if not sep or not index.isdigit() or not count.isdigit() or int(count) < 1 or int(index) >= int(count):
        raise ValueError(f"--slice wants K/N with 0 <= K < N, not {text!r}")
    return int(index), int(count)


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<](?:[^">]*/)?([^">/]+)[">]', re.MULTILINE)
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inc")


def cmake_list(text, name):
    """The entries of set(name ...) in a CMake file, or [] when it is absent or empty."""
    m = re.search(r"set\(\s*" + re.escape(name) + r"(?=[\s)])([^)]*)\)", text)
    return m.group(1).split() if m else []


def xdp_whole_file(text):
    """True when everything but comments and #includes sits inside one #if
    ROCKETNET_XDP block that ends the file (an #else stub inside it counts)."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group().count("\n"), text, flags=re.S)
    lines = [re.sub(r"//.*", "", line).strip() for line in text.splitlines()]
    depth = 0
    for i, s in enumerate(lines):
        if depth == 0:
            if re.match(r"#\s*if\s+ROCKETNET_XDP$", s):
                depth = 1
            elif s and not re.match(r"#\s*include\b", s):
                return False
            continue
        if re.match(r"#\s*if", s):
            depth += 1
        elif re.match(r"#\s*endif\b", s):
            depth -= 1
            if depth == 0:
                return not any(lines[i + 1:])
    return False


TEST_HEADER_RE = re.compile(r"^sdk/rocketnet_test_[^/]*\.(h|hh|hpp|hxx|inc)$")


def without_test_code(files, cmake_text):
    """Leave out test code under sdk/ on every platform: (kept, excluded {file: why}).

    Test code is the ROCKETNET_SDK_SOURCES_EXCLUDED list of
    cmake/RocketnetSources.cmake (sources compiled into the test binary only)
    and the sdk/rocketnet_test_*.h headers (test vectors and the test seam).
    A mutant in it changes what a test checks, not what the library does."""
    tests = set(cmake_list(cmake_text, "ROCKETNET_SDK_SOURCES_EXCLUDED"))
    kept, out = [], {}
    for f in files:
        if f in tests:
            out[f] = "ROCKETNET_SDK_SOURCES_EXCLUDED in cmake/RocketnetSources.cmake (the test binary only)"
        elif TEST_HEADER_RE.match(f):
            out[f] = "an sdk/rocketnet_test_* header (test vectors or test seam)"
        else:
            kept.append(f)
    return kept, out


def linux_judgement(files, cmake_text, texts):
    """Split the files for a Linux runner: (kept, excluded, not_judged).

    excluded {file: why}: sources the Linux build does not compile, so a Linux
    ./bin/test cannot kill their mutants: the WINDOWS and APPLE lists of
    cmake/RocketnetSources.cmake, and headers that only those sources include
    (texts {path: text} are the files searched for includes). Files are the unit
    here, not #if regions: a Windows source's ROCKETNET_DEV regions that a
    Linux dev build does compile are left out with the rest of the file.

    not_judged {file: why}: AF_XDP code, which Linux reaches only inside a
    guest and ./bin/test runs no guest: a file named for xdp, sdk/xdp/, or a
    file whose code is all inside one #if ROCKETNET_XDP. Its mutants run; a survivor
    is listed apart as not judged here, never a failure and never a pass.

    kept: every file not excluded, AF_XDP ones too, in the order given
    (io_uring and GSO are judged: Linux runs them)."""
    foreign = set(cmake_list(cmake_text, "ROCKETNET_SDK_SOURCES_WINDOWS"))
    foreign |= set(cmake_list(cmake_text, "ROCKETNET_SDK_SOURCES_APPLE"))
    excluded = {f: "a Windows or Apple source in cmake/RocketnetSources.cmake" for f in foreign}
    includers = {}
    for path, text in texts.items():
        for name in INCLUDE_RE.findall(text):
            includers.setdefault(name, set()).add(path)
    grew = True
    while grew:
        grew = False
        for path in texts:
            base = os.path.basename(path)
            users = includers.get(base, set()) - {path}
            if path not in excluded and path.endswith(HEADER_SUFFIXES) and users and users <= set(excluded):
                excluded[path] = "a header included only by " + ", ".join(sorted(users))
                grew = True
    kept, out, not_judged = [], {}, {}
    for f in files:
        base = os.path.basename(f)
        if f in excluded:
            out[f] = excluded[f]
            continue
        if "xdp" in base.lower() or f.startswith("sdk/xdp/"):
            not_judged[f] = "AF_XDP code by name: Linux reaches it only in a guest"
        elif xdp_whole_file(texts.get(f, "")):
            not_judged[f] = "the whole file is #if ROCKETNET_XDP: Linux reaches it only in a guest"
        kept.append(f)
    return kept, out, not_judged


BASELINE = ROOT / "tools" / "mutation" / "survivors.tsv"
BASELINE_LABELS = ("equivalent", "gap")


def baseline_key(mutant):
    """A survivor's stable name in the baseline: (file, n, the line's text,
    the mutated line's text), texts stripped, n the line's occurrence among the
    file's lines with that same text (1 for a unique line). Never the line
    number, so an entry does not move when other lines are added above it;
    two identical lines in one file (say a reset in the library and the same
    reset in a self-test) are told apart by n."""
    return (mutant["file"], mutant.get("occurrence", 1), mutant["original"].strip(), mutant["mutated"].strip())


def read_baseline(text):
    """Parse tools/mutation/survivors.tsv: -> ({key: (label, reason)}, ratchet).

    One entry a line, tab-separated: label, file, n (the occurrence of the
    line's text in the file, 1 when it is unique), line text, mutated line
    text, reason. label is `equivalent` (the reason says why no test can tell
    the mutant from the library) or `gap` (the reason names the test owed).
    One `gap-ratchet<TAB>N` line records how many gap entries there may be.
    '#' lines and blank lines are comments. Raises ValueError naming the line
    on anything else: a wrong column count, an unknown label, a bad n, an
    empty column, a key given twice, a missing or repeated ratchet."""
    entries, ratchet = {}, None
    for number, line in enumerate(text.split("\n"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        if cols[0] == "gap-ratchet":
            if len(cols) != 2 or not cols[1].isdigit() or ratchet is not None:
                raise ValueError(f"line {number}: want one 'gap-ratchet<TAB>N' line")
            ratchet = int(cols[1])
            continue
        if len(cols) != 6:
            raise ValueError(f"line {number}: want 6 tab-separated columns (label, file, n, line, mutated line, "
                             f"reason), not {len(cols)}")
        label, path, n, original, mutated, reason = (c.strip() for c in cols)
        if label not in BASELINE_LABELS:
            raise ValueError(f"line {number}: label {label!r} is not one of {', '.join(BASELINE_LABELS)}")
        if not n.isdigit() or int(n) < 1:
            raise ValueError(f"line {number}: n {n!r} is not a positive occurrence number")
        if not reason or not path or not original or not mutated:
            raise ValueError(f"line {number}: an empty column")
        key = (path, int(n), original, mutated)
        if key in entries:
            raise ValueError(f"line {number}: {path} #{n} `{original}` -> `{mutated}` is listed twice")
        entries[key] = (label, reason)
    if ratchet is None:
        raise ValueError("no 'gap-ratchet<TAB>N' line")
    return entries, ratchet


def ratchet_errors(entries, ratchet):
    """The ratchet rule: the number of gap entries is exactly the recorded
    value. More fails (a new gap must raise the number in the same change, in
    plain sight); fewer fails too (a closed gap must lower it, so it only goes
    down)."""
    gaps = sum(1 for label, _ in entries.values() if label == "gap")
    if gaps > ratchet:
        return [f"gap entries {gaps} exceed the gap-ratchet {ratchet}: a gap is added only with the ratchet raised "
                "in the same change"]
    if gaps < ratchet:
        return [f"gap entries {gaps} are below the gap-ratchet {ratchet}: lower the gap-ratchet to {gaps}"]
    return []


def judge_with_baseline(chosen, generated_keys, files, entries):
    """Compare a run with the baseline. Sets each chosen mutant's verdict and
    returns {kind: [message]} for the failures:

      new         a survivor whose key is not in the baseline
      stale       an `equivalent` entry whose mutant ran and did not survive
      killed_gap  a `gap` entry whose mutant ran and did not survive (the test
                  exists now: remove the entry and lower the ratchet)
      gone        an entry for a file in this run whose key the generator no
                  longer emits (the line changed or went)

    Survivors in the baseline are `known_equivalent` or `known_gap` and never
    fail. Mutants sharing a key survive as one: the key survives when any of
    them does."""
    by_key = {}
    for m in chosen:
        by_key.setdefault(baseline_key(m), []).append(m)
    failures = {"new": [], "stale": [], "killed_gap": [], "gone": []}
    for key, group in by_key.items():
        survived = any(m.get("status") == "survived" for m in group)
        ran = all(m.get("status") in ("survived", "detected", "timed_out", "did_not_compile") for m in group)
        entry = entries.get(key)
        for m in group:
            if m.get("status") == "survived":
                m["verdict"] = "new" if entry is None else "known_" + entry[0]
        if entry is None:
            if survived:
                failures["new"].append(key)
        elif not survived and ran:
            for m in group:
                m["verdict"] = "stale" if entry[0] == "equivalent" else "killed_gap"
            failures["stale" if entry[0] == "equivalent" else "killed_gap"].append(key)
    for key in entries:
        if key[0] in files and key not in generated_keys:
            failures["gone"].append(key)
    return failures


def baseline_failure_lines(failures, entries):
    """One line per failure, for the log and the summary."""
    titles = {"new": "NEW survivor, not in tools/mutation/survivors.tsv (write the test, or add it as equivalent "
                     "with the reason or as a gap naming the test owed)",
              "stale": "STALE equivalent entry: the mutant no longer survives (remove the entry)",
              "killed_gap": "KILLED gap: the mutant no longer survives (remove the entry and lower the gap-ratchet)",
              "gone": "GONE entry: the generator no longer emits this mutant (the line changed; remove or rewrite it)"}
    lines = []
    for kind in ("new", "stale", "killed_gap", "gone"):
        for path, n, original, mutated in failures[kind]:
            label = entries.get((path, n, original, mutated), ("", ""))[0]
            lines.append(f"{titles[kind]}: {path} #{n} `{original}` -> `{mutated}`" + (f" [{label}]" if label else ""))
    return lines


def check_baseline(path, root):
    """The per-commit check of the baseline, no build: the file parses, the
    ratchet holds, and every entry is a mutant the generator emits from the
    file as it is now. Returns a list of error lines."""
    try:
        entries, ratchet = read_baseline(Path(path).read_text())
    except (OSError, ValueError) as error:
        return [f"{path}: {error}"]
    errors = ratchet_errors(entries, ratchet)
    keys_by_file = {}
    for f in sorted({k[0] for k in entries}):
        source = root / f
        if not source.is_file():
            errors.append(f"{path}: {f} does not exist")
            keys_by_file[f] = set()
            continue
        keys_by_file[f] = {baseline_key(m) for m in generate(f, source.read_text())}
    for key in entries:
        if key not in keys_by_file.get(key[0], set()):
            errors.append(f"{path}: GONE entry: the generator no longer emits {key[0]} #{key[1]} `{key[2]}` -> `{key[3]}`")
    return errors


def rejudge(results_path, baseline_path):
    """Judge a finished run (its results.json) against the baseline, no build.
    The mutants are regenerated from the tree for their keys, so the tree must
    be at the run's head: a mutant id the generator does not emit is refused."""
    run = json.loads(Path(results_path).read_text())
    entries, ratchet = read_baseline(Path(baseline_path).read_text())
    generated = {}
    for f in run["files"]:
        for m in generate(f, (ROOT / f).read_text()):
            generated[m["id"]] = m
    chosen = []
    for m in run["mutants"]:
        if m["id"] not in generated:
            sys.exit(f"{m['id']} is not generated from this tree: check out the run's head {run.get('head')}")
        chosen.append(dict(m, occurrence=generated[m["id"]]["occurrence"]))
    failures = judge_with_baseline(chosen, {baseline_key(m) for m in generated.values()}, set(run["files"]), entries)
    lines = baseline_failure_lines(failures, entries) + ratchet_errors(entries, ratchet) + judgement_errors(chosen)
    for line in lines:
        print(f"FAILING: {line}")
    known = {v: sum(m.get("verdict") == v for m in chosen) for v in ("known_equivalent", "known_gap")}
    gaps = sum(1 for label, _ in entries.values() if label == "gap")
    print(f"MUTATION-GAPS {gaps} gap entries in {baseline_path} (gap-ratchet {ratchet}): this number must only go down")
    print(f"rejudged {results_path}: {sum(m.get('status') == 'survived' for m in chosen)} survivors, "
          f"known equivalent {known['known_equivalent']}, known gap {known['known_gap']}, failing {len(lines)}")
    return 1 if lines else 0


def verdict(mutant, not_judged):
    """The original rule, without a baseline. For a survived mutant: 'fail',
    'report' (a rocketnet_assert survivor) or 'not_judged_here'; None otherwise."""
    if mutant.get("status") != "survived":
        return None
    if mutant["file"] in not_judged:
        return "not_judged_here"
    if not risk_survivors([mutant]):
        return "report"
    return "fail"


def summary_markdown(info, chosen):
    """The job summary: rule, slice, counts, every survivor by verdict, the files left out."""
    lines = [f"## linux / mutation: {info['slice']}", "",
             f"Rule: {info['rule']}", "",
             f"Mutants: {info['generated']} generated, {info['in_slice']} in the slice, {len(chosen)} run; "
             + ", ".join(f"{k} {v}" for k, v in info["counts"].items()), ""]
    if info.get("baseline_failures") is not None:
        lines += [f"Gap entries in tools/mutation/survivors.tsv: {info['gaps']} (gap-ratchet {info['ratchet']}; "
                  "this number must only go down)", ""]
        failing = info["baseline_failures"]
        lines.append(f"### Failing: {len(failing)}")
        lines += [f"- {line}" for line in failing]
        lines.append("")
        titles = (("known_gap", "Known gaps in the baseline (a test is owed)"),
                  ("known_equivalent", "Known equivalent in the baseline"))
    else:
        titles = (("fail", "Failing: risk-class survivors"),
                  ("report", "Reported, not failing: rocketnet_assert survivors"),
                  ("not_judged_here", "Not judged here: survivors in AF_XDP code (Linux reaches it only in a guest; "
                                      "./bin/test runs none)"))
    for key, title in titles:
        rows = [m for m in chosen if m.get("verdict") == key]
        lines.append(f"### {title}: {len(rows)}")
        for m in rows:
            lines.append(f"- `{m['file']}:{m['line']}` {m['operator']} `{m['original'].strip()}` -> `{m['mutated'].strip()}`")
        lines.append("")
    for key, title in (("excluded", "Excluded: test code, and on Linux what the Linux build does not compile"),
                       ("not_judged", "Not judged here: AF_XDP files (mutated, survivors listed above)")):
        lines.append(f"### {title}: {len(info.get(key) or {})}")
        for f, why in sorted((info.get(key) or {}).items()):
            lines.append(f"- `{f}`: {why}")
        lines.append("")
    return "\n".join(lines)


def code_mask(text):
    """One flag per character: True where the character is code."""
    mask = [False] * len(text)
    i, n, state, line_start, pp = 0, len(text), "code", True, False
    while i < n:
        c = text[i]
        if state == "code":
            if line_start and not c.isspace():
                pp = c == "#"
                line_start = False
            if c == "\n":
                pp = pp and i > 0 and text[i - 1] == "\\"
                line_start = True
            elif pp:
                pass
            elif text.startswith("//", i):
                state = "line"
            elif text.startswith("/*", i):
                state = "block"
                i += 1
            elif c in "\"'":
                state = c
            else:
                mask[i] = True
        elif state == "line":
            if c == "\n":
                state, line_start, pp = "code", True, False
        elif state == "block":
            if text.startswith("*/", i):
                state = "code"
                i += 1
        else:
            if c == "\\":
                i += 1
            elif c == state:
                state = "code"
        i += 1
    return mask


def assert_mask(text):
    """One flag per character: True inside a rocketnet_assert call.

    Release builds compile rocketnet_assert out (ROCKETNET_ASSERTS is 0 under
    NDEBUG), so a survivor inside its argument is not a risk-class survivor.
    """
    mask = [False] * len(text)
    code = code_mask(text)
    for m in ASSERT_RE.finditer(text):
        if not code[m.start()]:
            continue
        open_pos = m.end() - 1
        depth = 0
        for j in range(open_pos, len(text)):
            if not code[j]:
                continue
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    for k in range(m.start(), j + 1):
                        mask[k] = True
                    break
    return mask


def in_macro_call(line, pos):
    depth = 0
    for j in range(pos - 1, -1, -1):
        if line[j] == ")":
            depth += 1
        elif line[j] == "(":
            if depth == 0:
                name = re.search(r"([A-Za-z_]\w*)\s*$", line[:j])
                return bool(name and re.fullmatch(r"[A-Z][A-Z0-9_]+", name.group(1)))
            depth -= 1
    return False


def matching_paren(line, open_pos):
    depth = 0
    for j in range(open_pos, len(line)):
        depth += {"(": 1, ")": -1}.get(line[j], 0)
        if depth == 0:
            return j
    return -1


def generate(rel, text):
    mask = code_mask(text)
    amask = assert_mask(text)
    mutants, offset, prev_code, seen = [], 0, "", {}
    for number, line in enumerate(text.split("\n"), 1):
        lmask = mask[offset:offset + len(line)]
        lamask = amask[offset:offset + len(line)]
        offset += len(line) + 1
        # the nth line of this file with this text (stripped): tells apart identical lines in one
        # file in the baseline key, without a line number
        occurrence = seen[line.strip()] = seen.get(line.strip(), 0) + 1

        def code(s, e):
            return all(lmask[s:e])

        def add(op, col, old, new, mutated):
            guarded = any(lamask[col:col + max(1, len(old))]) or old.lstrip().startswith("rocketnet_assert")
            mutants.append({"id": f"{rel}:{number}:{col + 1}:{op}:{new}", "file": rel, "line": number, "column": col + 1,
                            "operator": op, "class": "other" if guarded else mutant_class(op),
                            "original_token": old, "mutated_token": new,
                            "original": line, "mutated": mutated, "occurrence": occurrence})

        for m in OPERATOR_RE.finditer(line):
            tok, s, e = m.group(0), m.start(), m.end()
            if tok in ("++", "--") and code(s, e):
                new = "--" if tok == "++" else "++"
                add("arithmetic", s, tok, new, line[:s] + new + line[e:])
                continue
            if not code(s, e) or s == 0 or e >= len(line) or line[s - 1] != " " or line[e] != " ":
                continue
            prev_word = re.search(r"(\w+)\s*$", line[:s])
            if tok == "*" and prev_word and (prev_word.group(1).endswith("_t") or prev_word.group(1) in TYPE_WORDS):
                continue
            for table, op in ((RELATIONAL, "relational"), (ARITHMETIC, "arithmetic"), (BOOLEAN, "boolean")):
                for new in table.get(tok, []):
                    add(op, s, tok, new, line[:s] + new + line[e:])
        for m in re.finditer(r" ([+-]) 1(?![\w.])", line):
            if code(m.start(), m.end()):
                add("delete_plus_minus_one", m.start(), m.group(0).strip(), "", line[:m.start()] + line[m.end():])
        for m in INT_RE.finditer(line):
            digits, suffix = m.group(1), m.group(2)
            if not code(m.start(), m.end()) or in_macro_call(line, m.start()):
                continue
            # a leading zero is a C octal literal, which Python's base 0 refuses
            octal = len(digits) > 1 and digits[0] == "0" and digits[1].isdigit()
            value = int(digits, 8 if octal else 0)
            for new_value in (value + 1, value - 1):
                if new_value < 0 or new_value >= 1 << 64:
                    continue
                new = (hex(new_value) if digits.lower().startswith("0x") else
                       "0%o" % new_value if octal else str(new_value)) + suffix
                add("off_by_one", m.start(), m.group(0), new, line[:m.start()] + new + line[m.end():])
        for m in COND_RE.finditer(line):
            close = matching_paren(line, m.end() - 1)
            if close > 0 and code(m.start(), m.end()):
                inner = line[m.end():close].strip()
                new = f"!( {inner} )"
                add("negate_condition", m.start(), inner, new, line[:m.end()] + " " + new + " " + line[close:])
        stripped = line.strip()
        body = stripped.split("//")[0].strip()
        if body and code(len(line) - len(line.lstrip()), len(line) - len(line.lstrip()) + len(body)) \
                and body.count("(") == body.count(")") and prev_code[-1:] in (";", "{", "}", ")"):
            a = ASSIGN_RE.match(body)
            lhs = re.sub(r"\[[^\]]*\]", "[]", a.group("lhs")).strip() if a else ""
            is_assign = a and " " not in lhs and not lhs.startswith(("return", "case"))
            if is_assign or INCREMENT_RE.match(body):
                indent = line[:len(line) - len(line.lstrip())]
                add("delete_statement", len(indent), body, "{}", indent + "{}")
        if any(lmask):
            prev_code = "".join(ch for ch, f in zip(line, lmask) if f).rstrip()
    return mutants


def tree_files(root):
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             check=True, capture_output=True).stdout
        return [p for p in out.decode().split("\0") if p and (root / p).is_file()]
    except (OSError, subprocess.CalledProcessError):
        skip = {".git", "build", "bin"}
        return [str(p.relative_to(root)) for p in root.rglob("*") if p.is_file() and not skip & set(p.relative_to(root).parts)]


SESSIONS, SESSIONS_LOCK = set(), threading.RLock()


def kill_session(sid):
    """SIGKILL every live process in session sid.

    ./bin/test spawns each job in a process group of its own
    (tests/suite_runner.h), so killing the leader's group alone orphans the jobs.
    """
    try:
        os.killpg(sid, signal.SIGKILL)
    except OSError:
        pass
    for _ in range(10):
        found = False
        try:
            listing = subprocess.run(["ps", "-A", "-o", "pid=,stat="], capture_output=True, text=True).stdout
        except OSError:
            listing = ""
        for line in listing.splitlines():
            pid, stat = (line.split() + ["", ""])[:2]
            try:
                if not stat.startswith("Z") and int(pid) != os.getpid() and os.getsid(int(pid)) == sid:
                    os.kill(int(pid), signal.SIGKILL)
                    found = True
            except (OSError, ValueError):
                pass
        if not found:
            return


def kill_all_sessions():
    with SESSIONS_LOCK:
        for sid in list(SESSIONS):
            kill_session(sid)


def install_signal_cleanup():
    """On SIGTERM, SIGINT or SIGHUP (a cancelled CI step) kill every test process, then exit."""
    def stop(signum, frame):
        with SESSIONS_LOCK:  # held to the exit: no worker starts a session after this
            kill_all_sessions()
            os._exit(128 + signum)
    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, stop)


def run(cmd, cwd, timeout, env=None):
    """Run cmd in its own session; return (code or None on timeout, seconds, output tail).

    Every process left in the session when cmd ends or times out is killed."""
    start = time.monotonic()
    with SESSIONS_LOCK:
        proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                start_new_session=True)
        SESSIONS.add(proc.pid)
    try:
        out, _ = proc.communicate(timeout=timeout)
        code = proc.returncode
    except subprocess.TimeoutExpired:
        kill_session(proc.pid)
        out, _ = proc.communicate()
        code = None
    finally:
        kill_session(proc.pid)
        with SESSIONS_LOCK:
            SESSIONS.discard(proc.pid)
    tail = "\n".join(out.decode(errors="replace").splitlines()[-6:])
    return code, time.monotonic() - start, tail


class Worker:
    def __init__(self, index, args, files):
        self.dir = Path(args.work_dir).resolve() / f"w{index}"
        self.args = args
        self.env = dict(os.environ, ROCKETNET_TEST_JOBS=str(args.test_jobs))
        if not (self.dir / "build" / "CMakeCache.txt").exists():
            for rel in files:
                (self.dir / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / rel, self.dir / rel)
        # a reused tree may hold a mutant's binary: rewrite each target with a fresh
        # mtime (copy2 would keep the older one and make would skip the rebuild)
        for rel in args.files:
            (self.dir / rel).write_text((ROOT / rel).read_text())

    def build(self):
        cfg = ["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"] + self.args.cmake_arg
        code, seconds, tail = run(cfg, self.dir, self.args.build_timeout)
        if code != 0:
            return code, seconds, tail
        code, more, tail = run(["cmake", "--build", "build", "--target", "test", "--", f"-j{self.args.build_jobs}"],
                               self.dir, self.args.build_timeout)
        return code, seconds + more, tail

    def test(self, timeout):
        return run([str(self.dir / "bin" / "test")], self.dir, timeout, self.env)

    def mutant(self, m, timeout):
        path = self.dir / m["file"]
        original = (ROOT / m["file"]).read_text()
        lines = original.split("\n")
        assert lines[m["line"] - 1] == m["original"]
        lines[m["line"] - 1] = m["mutated"]
        path.write_text("\n".join(lines))
        try:
            code, m["build_seconds"], tail = self.build()
            if code != 0:
                m["status"], m["detail"] = "did_not_compile", tail
                return
            code, m["test_seconds"], tail = self.test(timeout)
            m["status"] = "timed_out" if code is None else "survived" if code == 0 else "detected"
            m["detail"] = tail if code else ""
        finally:
            path.write_text(original)


def self_test():
    """Deterministic check of generation and the risk-class gate. No build, no sleep."""
    text = (
        "int gate(int a, int b) {\n"
        "    if (a < b) {\n"
        "        a = a + 1;\n"
        "    }\n"
        "    return a;\n"
        "}\n"
    )
    first = generate("plant.cpp", text)
    second = generate("plant.cpp", text)
    if not first or [m["id"] for m in first] != [m["id"] for m in second]:
        print("self-test FAILED: generation is empty or not deterministic")
        return 1
    operators = {m["operator"] for m in first}
    need = {"relational", "arithmetic", "off_by_one", "negate_condition", "delete_statement"}
    if not need <= operators:
        print(f"self-test FAILED: planted operators {sorted(operators)}")
        return 1
    for mutant in first:
        if mutant["class"] != "risk" or mutant_class(mutant["operator"]) != "risk":
            print(f"self-test FAILED: {mutant['operator']} is not risk-class")
            return 1
    survived = dict(first[0], status="survived")
    detected = dict(first[0], status="detected")
    missed = dict(first[0], status="did_not_compile")
    other = dict(first[0], status="survived", operator="note")
    other["class"] = "other"
    if len(risk_survivors([survived])) != 1:
        print("self-test FAILED: a survived risk-class mutant was not a failure")
        return 1
    if risk_survivors([detected]) or risk_survivors([missed]) or risk_survivors([other]):
        print("self-test FAILED: a non-survivor or a non-risk mutant failed the gate")
        return 1
    planted_assert = (
        "int guarded(int a, int b) {\n"
        "    rocketnet_assert( a < b );\n"
        "    return a;\n"
        "}\n"
    )
    guards = [m for m in generate("plant.cpp", planted_assert) if m["line"] == 2]
    if not guards:
        print("self-test FAILED: no mutant planted inside rocketnet_assert")
        return 1
    if any(m["class"] == "risk" for m in guards):
        print("self-test FAILED: a rocketnet_assert mutant is risk-class")
        return 1
    if risk_survivors([dict(m, status="survived") for m in guards]):
        print("self-test FAILED: a survived rocketnet_assert mutant failed the gate")
        return 1
    octal = {m["mutated"].strip() for m in generate("plant.cpp", "    mode = 0700;\n") if m["operator"] == "off_by_one"}
    if octal != {"mode = 0701;", "mode = 0677;"}:
        print(f"self-test FAILED: a C octal literal mutated to {sorted(octal)}")
        return 1
    # slices: every mutant in exactly one of N, the same slice on every run, and
    # the same slice when lines are added above it
    for count in (1, 3, 90):
        homes = [[i for i in range(count) if in_slice(m, i, count)] for m in first]
        if any(len(h) != 1 for h in homes):
            print(f"self-test FAILED: a mutant is not in exactly one of {count} slices")
            return 1
    moved = generate("plant.cpp", "// a new first line\n\n" + text)
    if [slice_key(m) for m in moved] != [slice_key(m) for m in first]:
        print("self-test FAILED: a mutant's slice changed when lines were added above it")
        return 1
    for bad in ("3/3", "1", "a/2", "0/0", "-1/4"):
        try:
            parse_slice(bad)
        except ValueError:
            continue
        print(f"self-test FAILED: --slice {bad} was accepted")
        return 1
    if parse_slice("89/90") != (89, 90):
        print("self-test FAILED: --slice 89/90 was not read as (89, 90)")
        return 1
    # --platform linux: Windows and Apple sources and the headers only they
    # include are left out; AF_XDP files stay but are not judged; the rest stay
    cmake = ("set(ROCKETNET_SDK_SOURCES_LINUX\n    sdk/a_uring_linux.cpp)\n"
             "set(ROCKETNET_SDK_SOURCES_APPLE)\n"
             "set(ROCKETNET_SDK_SOURCES_WINDOWS\n    sdk/a_iocp_windows.cpp\n    sdk/a_rio_windows.cpp)\n")
    texts = {
        "sdk/a_iocp_windows.cpp": '#include "a_iocp.h"\n',
        "sdk/a_rio_windows.cpp": '#include "a_rio_layout.h"\n#include "a_iocp.h"\n',
        "sdk/a_rio_layout.h": "int x;\n",
        "sdk/a_iocp.h": "int y;\n",
        "sdk/a_common.cpp": '#include "a_iocp.h"\n',
        "sdk/a_uring_linux.cpp": "#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX\n#endif\n",
        "sdk/a_socket_linux.cpp": "#include <new>\n/*\n  c\n*/\n#if ROCKETNET_XDP\n#if X\n#endif\nint z;\n#else\nint d;\n#endif // x\n",
        "sdk/a_api.cpp": "int q;\n#if ROCKETNET_XDP\nint z;\n#endif\n",
        "sdk/a_api_tail.cpp": "#if ROCKETNET_XDP\nint z;\n#endif\nint q;\n",
        "sdk/a_xdp_frame.h": "int w;\n",
        "sdk/xdp/prog.c": "int v;\n",
    }
    kept, excluded, not_judged = linux_judgement(sorted(texts), cmake, texts)
    if (sorted(excluded) != ["sdk/a_iocp_windows.cpp", "sdk/a_rio_layout.h", "sdk/a_rio_windows.cpp"]
            or sorted(not_judged) != ["sdk/a_socket_linux.cpp", "sdk/a_xdp_frame.h", "sdk/xdp/prog.c"]
            or kept != ["sdk/a_api.cpp", "sdk/a_api_tail.cpp", "sdk/a_common.cpp", "sdk/a_iocp.h", "sdk/a_socket_linux.cpp",
                        "sdk/a_uring_linux.cpp", "sdk/a_xdp_frame.h", "sdk/xdp/prog.c"]):
        print(f"self-test FAILED: --platform linux kept {kept}, excluded {sorted(excluded)}, "
              f"not judged {sorted(not_judged)}")
        return 1
    # the original rule (no baseline): a risk survivor fails, a detected
    # mutant has no verdict, AF_XDP is not judged, an assert survivor reports
    on2 = dict(next(m for m in first if m["line"] == 2), status="survived")
    on3 = dict(next(m for m in first if m["line"] == 3), status="survived")
    if verdict(on2, {}) != "fail" or verdict(dict(on2, status="detected"), {}) is not None:
        print("self-test FAILED: the original rule did not fail a risk survivor, or a detected mutant had a verdict")
        return 1
    if verdict(on3, {"plant.cpp": "xdp"}) != "not_judged_here":
        print("self-test FAILED: a survivor in an AF_XDP file was judged")
        return 1
    guard = dict(guards[0], status="survived")
    if verdict(guard, {}) != "report":
        print("self-test FAILED: a rocketnet_assert survivor was not reported")
        return 1
    failed = baseline_self_test(first)
    if failed:
        print(f"self-test FAILED: {failed}")
        return 1
    # test code is left out on every platform: the cmake EXCLUDED list and the
    # sdk/rocketnet_test_* headers; a library file is kept
    test_cmake = cmake + "set(ROCKETNET_SDK_SOURCES_EXCLUDED\n    sdk/a_tests.cpp)\n"
    kept, gone = without_test_code(["sdk/a_tests.cpp", "sdk/rocketnet_test_seams.h", "sdk/rocketnet_test_vectors.h",
                                    "sdk/a_common.cpp", "sdk/rocketnet_testing_off.cpp"], test_cmake)
    if kept != ["sdk/a_common.cpp", "sdk/rocketnet_testing_off.cpp"] or sorted(gone) != [
            "sdk/a_tests.cpp", "sdk/rocketnet_test_seams.h", "sdk/rocketnet_test_vectors.h"]:
        print(f"self-test FAILED: test code split kept {kept}, left out {sorted(gone)}")
        return 1
    print(f"ok run_mutants self-test: {len(first)} planted mutants, risk-class survival fails the gate, "
          "rocketnet_assert survivors do not, slices partition the list and do not move with line numbers, "
          "test code is left out, "
          "--platform linux leaves out Windows sources and their headers and sets AF_XDP apart, "
          "--baseline fails a new survivor, a stale equivalent, a killed gap, a gone entry and a broken ratchet, "
          "and passes known survivors")
    return 0


def baseline_self_test(first):
    """The baseline rule on planted mutants and planted baselines: returns a
    failure description, or '' when every rule holds."""
    def tsv(*rows, ratchet=None):
        lines = ["# planted"]
        if ratchet is not None:
            lines.append(f"gap-ratchet\t{ratchet}")
        lines += ["\t".join(row) for row in rows]
        return "\n".join(lines) + "\n"
    a, b = first[0], first[1]
    row_a = ("equivalent", a["file"], "1", a["original"].strip(), a["mutated"].strip(), "planted: same code")
    row_b = ("gap", b["file"], "1", b["original"].strip(), b["mutated"].strip(), "test_planted_owed")
    entries, ratchet = read_baseline(tsv(row_a, row_b, ratchet=1))
    if entries != {baseline_key(a): ("equivalent", "planted: same code"), baseline_key(b): ("gap", "test_planted_owed")} \
            or ratchet != 1:
        return f"baseline read as {entries}, ratchet {ratchet}"
    for bad, why in ((tsv(row_a), "no ratchet line"),
                     (tsv(row_a, ratchet=0) + "gap-ratchet\t0\n", "two ratchet lines"),
                     (tsv(row_a, row_a, ratchet=0), "a key listed twice"),
                     (tsv(("maybe",) + row_a[1:], ratchet=0), "an unknown label"),
                     (tsv(row_a[:5] + ("",), ratchet=0), "an empty reason"),
                     (tsv(row_a[:5], ratchet=0), "five columns"), (tsv(row_a[:2] + ("0",) + row_a[3:], ratchet=0), "n 0")):
        try:
            read_baseline(bad)
        except ValueError:
            continue
        return f"a baseline with {why} was accepted"
    if ratchet_errors(entries, 1) or not ratchet_errors(entries, 0) or not ratchet_errors(entries, 2):
        return "the ratchet did not hold the gap count exactly to the recorded value"
    files = {a["file"]}
    every = {baseline_key(m) for m in first}

    def run(statuses):
        chosen = [dict(m, status=s) for m, s in zip(first, statuses)]
        return chosen, judge_with_baseline(chosen, every, files, entries)
    rest = ["detected"] * (len(first) - 2)
    # known survivors pass: the equivalent and the gap both survive, the rest are killed
    chosen, failures = run(["survived", "survived"] + rest)
    if any(failures.values()) or [m.get("verdict") for m in chosen[:2]] != ["known_equivalent", "known_gap"]:
        return f"known survivors failed: {failures}"
    # planted survivor 1: a survivor the baseline does not list is NEW
    chosen, failures = run(["survived", "survived", "survived"] + rest[1:])
    if failures["new"] != [baseline_key(first[2])] or chosen[2].get("verdict") != "new":
        return f"a survivor not in the baseline did not fail as new: {failures}"
    # planted survivor 2: an equivalent entry whose mutant is killed is STALE
    _, failures = run(["detected", "survived"] + rest)
    if failures["stale"] != [baseline_key(a)]:
        return f"an equivalent entry that no longer survives was not stale: {failures}"
    # a gap whose mutant is killed fails until its entry goes and the ratchet drops
    _, failures = run(["survived", "detected"] + rest)
    if failures["killed_gap"] != [baseline_key(b)]:
        return f"a killed gap did not fail: {failures}"
    # a mutant that did not run (time budget) judges nothing
    _, failures = run(["not_run", "not_run"] + rest)
    if any(failures.values()):
        return f"mutants that did not run were judged: {failures}"
    # an entry the generator no longer emits is GONE, for a file in the run only
    failures = judge_with_baseline([], every - {baseline_key(a)}, files, entries)
    if failures["gone"] != [baseline_key(a)]:
        return f"an entry whose line changed was not gone: {failures}"
    failures = judge_with_baseline([], set(), {"other.cpp"}, entries)
    if failures["gone"]:
        return "an entry for a file outside the run was called gone"
    # two identical lines in one file are two keys (n 1 and 2), the same
    # however many lines are added above them
    twice = "void f() {\n    x = 0;\n}\nvoid g() {\n    x = 0;\n}\n"
    keys = [baseline_key(m) for m in generate("plant.cpp", twice) if m["operator"] == "delete_statement"]
    moved = [baseline_key(m) for m in generate("plant.cpp", "// above\n" + twice) if m["operator"] == "delete_statement"]
    if [k[1] for k in keys] != [1, 2] or len(set(keys)) != 2 or moved != keys:
        return f"identical lines read as keys {keys} (moved {moved})"
    return ""


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--self-test", action="store_true",
                   help="check the generator and the risk-class gate on planted source, then exit (no build, no sleep)")
    p.add_argument("--files", nargs="+", default=DEFAULT_FILES, help="repository-relative sources to mutate")
    p.add_argument("--workers", type=int, default=4, help="parallel trees (default 4)")
    p.add_argument("--max-mutants", type=int, default=0, help="sample this many (0: all)")
    p.add_argument("--slice", default="",
                   help="K/N: keep only the mutants of slice K of N (a hash of each mutant's file, line text, "
                        "column in the line, operator and replacement, modulo N) before --max-mutants; "
                        "slices 0..N-1 together are every mutant. Default: no slicing")
    p.add_argument("--seed", type=int, default=1, help="sampling seed (default 1)")
    p.add_argument("--time-budget-minutes", type=float, default=0, help="stop starting mutants after this (0: none)")
    p.add_argument("--include", nargs="*", default=[], help="mutant ids always run (see --list)")
    p.add_argument("--only", nargs="*", default=[], help="run exactly these mutant ids")
    p.add_argument("--list", action="store_true", help="print every mutant id and exit")
    p.add_argument("--build-jobs", type=int, default=2, help="make -j per worker (default 2)")
    p.add_argument("--test-jobs", type=int, default=2, help="ROCKETNET_TEST_JOBS per worker (default 2)")
    p.add_argument("--timeout-factor", type=float, default=3.0, help="test timeout = factor x unmutated run")
    p.add_argument("--build-timeout", type=float, default=900, help="seconds per configure or build step")
    p.add_argument("--cmake-arg", action="append", default=[], help="extra cmake configure argument")
    p.add_argument("--work-dir", default=str(ROOT / "build" / "mutation"), help="per-worker trees live here")
    p.add_argument("--out", default="", help="JSON results file (default <work-dir>/results.json)")
    p.add_argument("--platform", choices=("linux",), default=None,
                   help="linux: leave out the files the Linux build does not compile (Windows and Apple sources and "
                        "the headers only they include) and list AF_XDP files' survivors as not judged here. "
                        "Default: every file given, the original campaign")
    p.add_argument("--baseline", default="",
                   help="PATH (the nightly: tools/mutation/survivors.tsv): every survivor must be in this reviewed "
                        "list as equivalent or gap; a new survivor, an equivalent entry that no longer survives, "
                        "a gap entry that no longer survives, an entry the generator no longer emits and a gap "
                        "count other than the file's gap-ratchet each fail. Default: the original rule, every "
                        "risk-class survivor fails")
    p.add_argument("--check-baseline", default="", metavar="PATH",
                   help="check the baseline without a build (format, ratchet, every entry still generated), then exit")
    p.add_argument("--rejudge", default="", metavar="RESULTS",
                   help="judge a finished run's results.json against --baseline without building (the tree must be "
                        "at the run's head), then exit: to review a night's artifact after editing the baseline")
    p.add_argument("--summary-md", default="", help="write a Markdown summary of the verdicts here")
    args = p.parse_args()
    if args.self_test:
        return self_test()
    if args.rejudge:
        if not args.baseline:
            p.error("--rejudge needs --baseline")
        return rejudge(args.rejudge, args.baseline)
    if args.check_baseline:
        errors = check_baseline(args.check_baseline, ROOT)
        for line in errors:
            print(line)
        if errors:
            print(f"baseline check failed: {len(errors)}")
            return 1
        entries, ratchet = read_baseline(Path(args.check_baseline).read_text())
        print(f"ok baseline {args.check_baseline}: {len(entries)} entries, "
              f"{sum(1 for label, _ in entries.values() if label == 'equivalent')} equivalent, "
              f"{ratchet} gap (gap-ratchet {ratchet}), every entry still generated")
        return 0
    survivors = None
    if args.baseline:
        try:
            survivors = read_baseline(Path(args.baseline).read_text())
        except (OSError, ValueError) as error:
            sys.exit(f"--baseline {args.baseline}: {error}")
    try:
        slice_of = parse_slice(args.slice) if args.slice else None
    except ValueError as error:
        p.error(str(error))
    started = time.monotonic()

    cmake_text = (ROOT / "cmake" / "RocketnetSources.cmake").read_text()
    args.files, excluded = without_test_code(args.files, cmake_text)
    for f, why in sorted(excluded.items()):
        print(f"excluded (test code, mutating it measures nothing): {f}: {why}", flush=True)
    excluded = {f: "test code: " + why for f, why in excluded.items()}
    not_judged = {}
    if args.platform == "linux":
        texts = {}
        for pattern in ("sdk/*", "sdk/*/*", "tests/*", "tests/*/*"):
            for path in ROOT.glob(pattern):
                if path.is_file() and path.suffix in (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"):
                    texts[path.relative_to(ROOT).as_posix()] = path.read_text(errors="replace")
        args.files, foreign, not_judged = linux_judgement(args.files, cmake_text, texts)
        excluded.update(foreign)
        excluded_linux = foreign
        for f, why in sorted(excluded_linux.items()):
            print(f"excluded (the Linux build does not compile it): {f}: {why}", flush=True)
        if survivors:
            # the baseline judges every survivor: an AF_XDP survivor is listed in
            # it like any other (as a gap naming the guest test owed, or equivalent)
            for f, why in sorted(not_judged.items()):
                print(f"AF_XDP (./bin/test reaches it only through seams; judged by the baseline): {f}: {why}",
                      flush=True)
            not_judged = {}
        for f, why in sorted(not_judged.items()):
            print(f"not judged here (mutated, survivors listed apart, never failing): {f}: {why}", flush=True)
    if survivors:
        print(f"rule: every survivor must be in the reviewed baseline {args.baseline} ({len(survivors[0])} entries, "
              f"gap-ratchet {survivors[1]}); new survivors, stale or killed entries, gone entries and a broken "
              "ratchet fail", flush=True)
    else:
        print("rule: every risk-class survivor fails (the original campaign rule)", flush=True)

    mutants = []
    for rel in args.files:
        mutants += generate(rel, (ROOT / rel).read_text())
    generated = len(mutants)
    generated_keys = {baseline_key(m) for m in mutants}
    order = {id(m): n for n, m in enumerate(mutants)}
    # --only and --include name mutants of the whole list, in or out of the slice
    by_id = {m["id"]: m for m in mutants}
    if slice_of:
        mutants = [m for m in mutants if in_slice(m, *slice_of)]
        print(f"slice {slice_of[0]}/{slice_of[1]}: {len(mutants)} of {generated} generated mutants",
              file=sys.stderr if args.list else sys.stdout, flush=True)
    if args.list:
        for m in mutants:
            print(f"{m['id']}\t{m['mutated'].strip()}")
        return 0
    missing = [i for i in args.only + args.include if i not in by_id]
    if missing:
        sys.exit(f"unknown mutant id: {missing[0]}")
    if args.only:
        chosen = [by_id[i] for i in args.only]
    else:
        chosen = [by_id[i] for i in args.include]
        rest = [m for m in mutants if m["id"] not in args.include]
        room = len(rest) if args.max_mutants <= 0 else max(0, args.max_mutants - len(chosen))
        chosen += random.Random(args.seed).sample(rest, min(room, len(rest)))
        chosen.sort(key=lambda m: order[id(m)])
    print(f"{generated} mutants generated, {len(mutants)} in the slice, {len(chosen)} chosen, {args.workers} workers"
          if slice_of else f"{generated} mutants generated, {len(chosen)} chosen, {args.workers} workers", flush=True)
    if not chosen:
        # nothing to build or judge: an empty slice, a generator that emits
        # nothing, or every file left out
        for line in judgement_errors(chosen):
            print(f"FAILING: {line}", flush=True)
        return 1

    files = tree_files(ROOT)
    workers = [Worker(i, args, files) for i in range(args.workers)]
    setup = [None] * len(workers)

    def prepare(i):
        code, _, tail = workers[i].build()
        if code != 0:
            setup[i] = f"unmutated build failed in {workers[i].dir}:\n{tail}"
            return
        code, seconds, tail = workers[i].test(args.build_timeout)
        setup[i] = seconds if code == 0 else f"unmutated ./bin/test failed in {workers[i].dir}:\n{tail}"

    threads = [threading.Thread(target=prepare, args=(i,)) for i in range(len(workers))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for s in setup:
        if not isinstance(s, float):
            sys.exit(s)
    baseline = max(setup)
    timeout = args.timeout_factor * baseline
    print(f"unmutated ./bin/test {baseline:.1f} s (slowest worker); timeout {timeout:.1f} s", flush=True)

    deadline = started + args.time_budget_minutes * 60 if args.time_budget_minutes > 0 else None
    todo, lock, done = queue.Queue(), threading.Lock(), [0]
    for m in chosen:
        m["status"] = "not_run"
        todo.put(m)

    def loop(w):
        while True:
            try:
                m = todo.get_nowait()
            except queue.Empty:
                return
            if deadline and time.monotonic() > deadline:
                continue
            w.mutant(m, timeout)
            with lock:
                done[0] += 1
                print(f"[{done[0]}/{len(chosen)}] {m['status']:<15} {m['file']}:{m['line']} {m['operator']} "
                      f"{m['original_token']!r} -> {m['mutated_token']!r}", flush=True)

    threads = [threading.Thread(target=loop, args=(w,)) for w in workers]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    counts = {s: sum(m["status"] == s for m in chosen) for s in ("detected", "timed_out", "survived", "did_not_compile", "not_run")}
    judged = counts["detected"] + counts["timed_out"] + counts["survived"]
    score = (counts["detected"] + counts["timed_out"]) / judged if judged else 0.0
    wall = time.monotonic() - started
    head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    run_lines = judgement_errors(chosen)
    baseline_lines, gaps = None, None
    if survivors:
        entries, ratchet = survivors
        failures = judge_with_baseline(chosen, generated_keys, set(args.files), entries)
        baseline_lines = baseline_failure_lines(failures, entries) + ratchet_errors(entries, ratchet) + run_lines
        gaps = sum(1 for label, _ in entries.values() if label == "gap")
        verdict_names = ("new", "known_equivalent", "known_gap", "stale", "killed_gap")
    else:
        for m in chosen:
            v = verdict(m, not_judged)
            if v:
                m["verdict"] = v
        verdict_names = ("fail", "report", "not_judged_here")
    verdicts = {v: sum(m.get("verdict") == v for m in chosen) for v in verdict_names}
    summary = dict(counts, total=len(chosen), generated=generated, in_slice=len(mutants), score=round(score, 4),
                   wall_seconds=round(wall, 1), baseline_test_seconds=round(baseline, 2), timeout_seconds=round(timeout, 2),
                   verdicts=verdicts, judgement_failures=run_lines)
    if survivors:
        summary.update(baseline_failures=len(baseline_lines), gap_entries=gaps, gap_ratchet=survivors[1])
    rule = ("every survivor must be in the reviewed baseline " + args.baseline if survivors
            else "every risk-class survivor fails (the original rule)")
    out = Path(args.out) if args.out else Path(args.work_dir) / "results.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"head": head, "files": args.files, "seed": args.seed, "workers": args.workers,
                               "slice": {"index": slice_of[0], "count": slice_of[1]} if slice_of else None,
                               "platform": args.platform, "rule": rule, "baseline": args.baseline or None,
                               "baseline_failures": baseline_lines,
                               "excluded_files": excluded, "not_judged_files": not_judged,
                               "summary": summary, "mutants": chosen}, indent=2) + "\n")
    if args.summary_md:
        info = {"slice": f"slice {slice_of[0]}/{slice_of[1]}" if slice_of else "the whole list", "rule": rule,
                "generated": generated, "in_slice": len(mutants), "counts": counts,
                "baseline_failures": baseline_lines, "gaps": gaps, "ratchet": survivors[1] if survivors else None,
                "excluded": excluded, "not_judged": not_judged}
        Path(args.summary_md).parent.mkdir(parents=True, exist_ok=True)
        Path(args.summary_md).write_text(summary_markdown(info, chosen) + "\n")

    print(f"\n{'status':<15} {'file:line':<45} {'operator':<22} original -> mutated")
    for m in chosen:
        print(f"{m['status']:<15} {m['file'] + ':' + str(m['line']):<45} {m['operator']:<22} "
              f"{m['original'].strip()}  ->  {m['mutated'].strip()}")
    print(f"\nsummary: {json.dumps(summary)}\nresults: {out}")
    if survivors:
        for line in baseline_lines:
            print(f"FAILING: {line}")
        print(f"MUTATION-GAPS {gaps} gap entries in {args.baseline} (gap-ratchet {survivors[1]}): this number must "
              "only go down", flush=True)
        if baseline_lines:
            print(f"the run does not match the baseline: {len(baseline_lines)} failing", flush=True)
            return 1
        print(f"every survivor is in the baseline; known equivalent {verdicts['known_equivalent']}, "
              f"known gap {verdicts['known_gap']}", flush=True)
        return 0
    for key, title in (("report", "REPORTED (not failing)"), ("not_judged_here", "NOT JUDGED HERE (AF_XDP)"), ("fail", "FAILING")):
        for m in chosen:
            if m.get("verdict") == key:
                print(f"{title}: {m['file']}:{m['line']} {m['class']}-class {m['operator']} "
                      f"{m['original'].strip()}  ->  {m['mutated'].strip()}")
    for line in run_lines:
        print(f"FAILING: {line}")
    if verdicts["fail"] or run_lines:
        print(f"risk-class survivors failing the rule ({rule}): {verdicts['fail']}; run failures {len(run_lines)}",
              flush=True)
        return 1
    print(f"no survivor fails the rule ({rule}); reported {verdicts['report']}, "
          f"not judged here {verdicts['not_judged_here']}", flush=True)
    return 0


if __name__ == "__main__":
    install_signal_cleanup()
    try:
        sys.exit(main())
    finally:
        kill_all_sessions()
