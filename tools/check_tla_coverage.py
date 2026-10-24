#!/usr/bin/env python3
"""Check every tla/COVERAGE.tsv row against its model and code."""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COVERAGE = ROOT / "tla" / "COVERAGE.tsv"
TLA_DIR = ROOT / "tla"

FUNC_RE = re.compile(r"(?:rn_|rocketnet_)[A-Za-z0-9_]+(?=,|\)|and\b)")
DST_CHECK_RE = re.compile(r'rocketnet_dst_check_fail\(\s*\w+,\s*"([a-z0-9_]+)"')


def iter_rows():
    lines = COVERAGE.read_text().splitlines()
    if not lines:
        return
    header = lines[0].split("\t")
    for line in lines[1:]:
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < len(header):
            continue
        yield dict(zip(header, parts))


def tla_modules():
    modules = []
    for path in sorted(TLA_DIR.glob("*.tla")):
        name = path.stem
        if name.startswith("MC") or name == "Link":
            continue
        modules.append(name)
    return modules


def dst_check_names():
    path = ROOT / "tests" / "dst_checks.h"
    if not path.is_file():
        return set()
    return set(DST_CHECK_RE.findall(path.read_text(errors="replace")))


def check_row(row, dst_names):
    failures = []
    module = row["module"]
    machine = row.get("machine", "")
    status = row.get("status", "")
    code = row.get("code", "")
    note = row.get("note", "")

    if status == "none":
        if module != "-":
            failures.append(f"{module}: {machine} a row with no model must name - as its module")
        if "," in code:
            failures.append(f"{module}: {machine} code column must hold one path, one per row")
            return failures
        if not (ROOT / code).is_file():
            failures.append(f"{module}: {code} does not exist")
            return failures
        if "no model" not in note.lower():
            failures.append(f"{module}: {machine} a row with no model must say why the code is safe")
        return failures

    if status == "dst":
        if module != "-":
            module_file = TLA_DIR / f"{module}.tla"
            if not module_file.exists():
                failures.append(f"{module}: tla/{module}.tla does not exist")
        if "," in code:
            failures.append(f"{module}: {machine} code column must hold one path, one per row")
            return failures
        if not (ROOT / code).is_file():
            failures.append(f"{module}: {code} does not exist")
            return failures
        if not any(re.search(rf"\b{re.escape(name)}\b", note) for name in dst_names):
            failures.append(f"{module}: {machine} a dst row must name the DST check in its note")
        return failures

    if status != "owed" and status not in ("current", "covered"):
        failures.append(f"{module}: {machine} unknown status {status}")
        return failures

    module_file = TLA_DIR / f"{module}.tla"
    if not module_file.exists():
        failures.append(f"{module}: tla/{module}.tla does not exist")

    if "," in code:
        failures.append(f"{module}: {machine} code column must hold one path, one per row")
        return failures

    if status != "owed":
        code_file = ROOT / code
        if not code_file.is_file():
            failures.append(f"{module}: {code} does not exist")
            return failures
        text = code_file.read_text(errors="replace")
        cite = f"tla/{module}.tla"
        if cite not in text:
            failures.append(f"{module}: {code} does not cite {cite}")
        for fn in sorted(FUNC_RE.findall(note)):
            if f"{fn}(" not in text:
                failures.append(f"{module}: {fn} not defined in {code}")
    return failures


def owed_but_cited(rows):
    """An owed row says no code implements its model; a model that a file
    under sdk/ cites is implemented, so its owed row is stale. The citation
    counts with or without the tla/ prefix: sdk/rocketnet_client_receive_
    handshake.h writes RNClientIo.tla bare, seven times."""
    failures = []
    sdk = ROOT / "sdk"
    texts = {}
    if sdk.is_dir():
        for path in sorted(sdk.rglob("*")):
            if path.is_file():
                texts[path.relative_to(ROOT).as_posix()] = path.read_text(errors="replace")
    for row in rows:
        if row.get("status", "") != "owed":
            continue
        module = row["module"]
        forms = (f"tla/{module}.tla", f"{module}.tla")
        for name, text in texts.items():
            cite = next((form for form in forms if form in text), None)
            if cite:
                failures.append(f"{module}: {row.get('machine', '')} is owed but {name} cites {cite}")
                break
    return failures


# ---------------------------------------------------------------------------
# Per-action citation checks (--actions / --actions-strict).
#
# A module's actions are the operators its Next definition reaches, followed
# through operators that are only disjunctions or existentials of other
# operators, stopping at an operator whose body primes a variable or uses
# UNCHANGED.  Parameters and quantifiers are dropped (Connect(s) is Connect).
# ---------------------------------------------------------------------------

_OP_LONG = ["\\notin", "\\subseteq", "\\cup", "\\cap", "\\div", "\\cdot",
            "\\times", "\\x", "\\geq", "\\leq", "\\in", "\\E", "\\A",
            "\\/", "/\\", "\\", "<=>", "~>", "|->", "<<", ">>", ">=",
            "<=", ":=", "/=", "@@", "##", ".."]
_OP_SET = set("()+[]{}.,!:'=<>+-*/%^@#$")

_ACTION_PRIME_RE = re.compile(r"[A-Za-z0-9_)\]']'")
_DEF_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)\s*(?:\((.*?)\))?\s*==", re.M)
_ENV_RE = re.compile(r"environment actions\s+(.+?)(?:\.|$)", re.I)


def strip_comments(text):
    """Remove \\* line comments and (* *) block comments (nesting-aware)."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("\\*", i):
            j = text.find("\n", i)
            if j == -1:
                break
            i = j
            continue
        if text.startswith("(*", i):
            depth = 1
            i += 2
            while i < n and depth > 0:
                if text.startswith("(*", i):
                    depth += 1
                    i += 2
                elif text.startswith("*)", i):
                    depth -= 1
                    i += 2
                else:
                    i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def tokenize(text):
    toks = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        if c.isdigit():
            j = i
            while j < n and text[j].isdigit():
                j += 1
            toks.append(("num", text[i:j]))
            i = j
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            toks.append(("id", text[i:j]))
            i = j
            continue
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            toks.append(("str", ""))
            continue
        if c == "\\":
            matched = False
            for op in _OP_LONG:
                if text.startswith(op, i):
                    toks.append(op)
                    i += len(op)
                    matched = True
                    break
            if not matched:
                toks.append("\\")
                i += 1
            continue
        if c == "<" and text.startswith("<<", i):
            toks.append("<<")
            i += 2
            continue
        if c == ">" and text.startswith(">>", i):
            toks.append(">>")
            i += 2
            continue
        two = text[i:i + 2]
        if two in ("<=", ">=", ":=", "/="):
            toks.append(two)
            i += 2
            continue
        if c in _OP_SET:
            toks.append(c)
            i += 1
            continue
        i += 1
    return toks


def find_definitions(text):
    defs = {}
    matches = list(_DEF_RE.finditer(text))
    for idx, m in enumerate(matches):
        name = m.group(1)
        start = m.end()
        end = matches[idx + 1].start() if idx + 1 < len(matches) else len(text)
        defs[name] = text[start:end]
    return defs


def scan_refs(text, defnames):
    """Named operators applied in text (skipping fields, instance members and
    bound variables)."""
    toks = tokenize(text)
    refs = set()
    bound = set()
    prev = None
    for t in toks:
        if isinstance(t, tuple) and t[0] == "id":
            v = t[1]
            if prev in ("\\E", "\\A", "CHOOSE"):
                bound.add(v)
            elif v in defnames and v not in bound and prev not in (".", "!", "'"):
                refs.add(v)
            prev = v
        else:
            prev = t
    return refs


_CONNECTOR_ALLOWED = {"\\E", "\\/", "\\in", "(", ")", ",", ":", "\\A"}


def is_connector(body):
    for t in tokenize(body):
        if isinstance(t, tuple) and t[0] == "id":
            continue
        if t not in _CONNECTOR_ALLOWED:
            return False
    return True


def state_changing(name, defs, memo, visiting):
    """An operator changes state when its body primes a variable, uses
    UNCHANGED, or reaches (through helpers) an operator that does."""
    if name in memo:
        return memo[name]
    if name in visiting:
        return False
    body = defs.get(name)
    if body is None:
        return False
    if "UNCHANGED" in body or _ACTION_PRIME_RE.search(body):
        memo[name] = True
        return True
    visiting.add(name)
    result = False
    for r in scan_refs(body, set(defs)):
        if state_changing(r, defs, memo, visiting):
            result = True
            break
    visiting.discard(name)
    memo[name] = result
    return result


def module_actions(module):
    path = TLA_DIR / f"{module}.tla"
    if not path.exists():
        return set(), [f"{module}: tla/{module}.tla does not exist"]
    text = strip_comments(path.read_text(errors="replace"))
    defs = find_definitions(text)
    if "Next" not in defs:
        return set(), [f"{module}: no Next definition to follow"]
    actions = set()
    names = set(defs)
    frontier = set(scan_refs(defs["Next"], names))
    seen = set()
    memo = {}
    visiting = set()
    while frontier:
        name = frontier.pop()
        if name in seen:
            continue
        seen.add(name)
        body = defs.get(name)
        if body is None:
            continue
        if is_connector(body):
            frontier |= scan_refs(body, names)
        elif state_changing(name, defs, memo, visiting):
            actions.add(name)
    return actions, []


def code_files_for_module(rows, module):
    files = set()
    for r in rows:
        if r["status"] == "current" and r["module"] == module:
            code = r.get("code", "")
            if code not in ("", "-"):
                files.add(code)
    return sorted(files)


def comment_blocks(text):
    blocks = []
    cur = None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if s.startswith("//"):
            if cur is None:
                cur = []
            cur.append(line)
        elif s.startswith("/*"):
            buf = [line]
            while "*/" not in line and i + 1 < len(lines):
                i += 1
                line = lines[i]
                buf.append(line)
            if cur is None:
                cur = []
            cur.extend(buf)
        else:
            if cur is not None:
                blocks.append("\n".join(cur))
                cur = None
        i += 1
    if cur is not None:
        blocks.append("\n".join(cur))
    return blocks


def is_action_cited(code_text, module, action):
    mod_pat = re.compile(re.escape(module) + r"\.tla")
    act_pat = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(action)
                         + r"(?![A-Za-z0-9_])")
    for block in comment_blocks(code_text):
        if mod_pat.search(block) and act_pat.search(block):
            return True
    return False


def environment_exempt(notes):
    exempt = set()
    for note in notes:
        m = _ENV_RE.search(note)
        if m:
            parts = re.split(r",|\band\b", m.group(1))
            exempt |= {p.strip() for p in parts if p.strip()}
    return exempt


def actions_report(rows, modules):
    lines = []
    total = 0
    failures = []
    for module in modules:
        actions, issues = module_actions(module)
        for issue in issues:
            failures.append(issue)
            lines.append(issue)
        notes = [r.get("note", "") for r in rows
                 if r["status"] == "current" and r["module"] == module]
        files = code_files_for_module(rows, module)
        texts = {}
        for f in files:
            p = ROOT / f
            if p.is_file():
                texts[f] = p.read_text(errors="replace")
        exempt = environment_exempt(notes)
        for name in sorted(exempt - actions):
            fail = f"{module}: environment note names unknown action {name}"
            failures.append(fail)
            lines.append(fail)
        uncited = []
        for a in sorted(actions - exempt):
            cited = any(is_action_cited(t, module, a) for t in texts.values())
            if not cited:
                uncited.append(a)
        for a in uncited:
            lines.append(f"{module} {a}: {', '.join(files)}")
            total += 1
    lines.append(f"ACTIONS UNCITED {total}")
    return lines, total, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require", action="append", default=[], metavar="MODULES",
                        help="comma-separated modules that must each have a coverage row")
    parser.add_argument("--all", action="store_true",
                        help="every tla module (MC wrappers and Link excluded) must have a row")
    parser.add_argument("--actions", action="store_true",
                        help="report per-action citations for current-row modules")
    parser.add_argument("--actions-strict", action="store_true",
                        help="exit 1 while uncited actions remain")
    args = parser.parse_args()

    rows = list(iter_rows())
    failures = []
    dst_names = dst_check_names()
    for row in rows:
        failures.extend(check_row(row, dst_names))

    for machine, count in Counter(row["machine"] for row in rows).items():
        if count > 1:
            failures.append(f"{machine}: row appears twice in tla/COVERAGE.tsv")

    have = {row["module"] for row in rows}
    required = set()
    for group in args.require:
        required.update(m for m in group.split(",") if m.strip())
    for module in sorted(required):
        if module not in have:
            failures.append(f"{module}: no coverage row")

    if args.all:
        for module in tla_modules():
            if module not in have:
                failures.append(f"{module}: no coverage row")

    failures.extend(owed_but_cited(rows))

    if args.actions or args.actions_strict:
        action_modules = sorted({r["module"] for r in rows
                                if r["status"] == "current" and r["module"] != "Link"})
        action_lines, uncited, action_failures = actions_report(rows, action_modules)

    if failures:
        for failure in sorted(failures):
            print(failure)
        print(f"COVERAGE FAIL {len(failures)}")
        return 1
    print(f"COVERAGE OK {len(rows)} rows")

    if args.actions or args.actions_strict:
        for line in action_lines:
            print(line)
        if args.actions_strict and (uncited > 0 or action_failures):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
