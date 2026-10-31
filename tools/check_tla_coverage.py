#!/usr/bin/env python3
"""Check every proofs/tla/COVERAGE.tsv row against its model and code."""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COVERAGE = ROOT / "proofs" / "tla" / "COVERAGE.tsv"
TLA_DIR = ROOT / "proofs" / "tla"

FUNC_RE = re.compile(r"(?:rn_|rocketnet_)[A-Za-z0-9_]+")
FUNC_WITH_PATH_RE = re.compile(
    r"((?:rn_|rocketnet_)[A-Za-z0-9_]+)\s*\(\s*((?:sdk|tests)/[^)\s]+)\)")
_PATH_TOKEN_RE = re.compile(
    r"(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+\.(?:cpp|h|c|tla|md)(?::[0-9]+(?:-[0-9]+)?)?")
DST_CHECK_RE = re.compile(r'rocketnet_dst_check_fail\(\s*\w+,\s*"([a-z0-9_]+)"')

CITATION_RE = re.compile(
    r"((?:sdk|tests|(?:proofs/)?tla)/[A-Za-z0-9_./-]+\.[a-z]+):"
    r"(?:\s*(?:\*\))?\s*\n\s*(?:\(\*|//|\*)?\s*)?(\d+)(?:-(\d+))?")
CITATION_SCAN_FILES = (
    "sdk/rocketnet_driver.h",
    "sdk/rocketnet_layers.h",
    "sdk/rocketnet_driver_builtin.cpp",
    "sdk/rocketnet_allocator.h",
    "sdk/rocketnet_server_uring_linux.cpp",
    "sdk/rocketnet_server_uring.h",
    "sdk/rocketnet_replay.h",
    "proofs/tla/ReplayWindow.tla",
    "proofs/tla/MCReplayWindow.tla",
    "proofs/tla/ServerSession.tla",
    "proofs/tla/RNSlots.tla",
    "proofs/tla/RNClientReceiveQuit.tla",
    "proofs/tla/README-ReplayWindow.md",
    "proofs/tla/README-ClientConnection.md",
    "proofs/tla/README-SessionAcks.md",
)
_FUNC_HEADER_RE = re.compile(
    r"^\s*(?:static\s+|extern\s+\"C\"\s+|inline\s+)*[A-Za-z_][\w\s\*&<>:,]*?"
    r"\b((?:rn_|rocketnet_|split_|simulated_)[A-Za-z0-9_]+)\s*\(")

_sdk_texts_cache = {}


def sdk_texts():
    key = str(ROOT)
    if key not in _sdk_texts_cache:
        texts = {}
        sdk = ROOT / "sdk"
        if sdk.is_dir():
            for path in sorted(sdk.rglob("*")):
                if path.is_file():
                    texts[path.relative_to(ROOT).as_posix()] = \
                        path.read_text(errors="replace")
        _sdk_texts_cache[key] = texts
    return _sdk_texts_cache[key]


def note_function_citations(note):
    """Every rn_/rocketnet_ function a note names, with its own file.

    A name followed by a parenthesised path is resolved against that
    file; every other name is resolved against all of sdk/.  File
    names and _t type names are not functions and are skipped.
    """
    citations = {}
    for fn, path in FUNC_WITH_PATH_RE.findall(note):
        if not fn.endswith("_t"):
            citations[fn] = path.split(":")[0]
    cleaned = _PATH_TOKEN_RE.sub(" ", note)
    for fn in FUNC_RE.findall(cleaned):
        if not fn.endswith("_t"):
            citations.setdefault(fn, None)
    return sorted(citations.items())


def function_defined(fn, path, code_text):
    if path:
        target = ROOT / path
        if target.is_file():
            return f"{fn}(" in target.read_text(errors="replace")
        return False
    if code_text and f"{fn}(" in code_text:
        return True
    return any(f"{fn}(" in text for text in sdk_texts().values())


def _function_starts(text):
    lines = text.splitlines()
    starts = []
    for i, line in enumerate(lines):
        m = _FUNC_HEADER_RE.match(line)
        if not m:
            continue
        window = "\n".join(lines[i:i + 20])
        brace = window.find("{")
        semi = window.find(";")
        if brace == -1 or (semi != -1 and semi < brace):
            continue
        starts.append((i + 1, m.group(1)))
    return starts


def enclosing_symbol(target_text, line_no):
    """The symbol whose definition encloses line_no, or None."""
    lines = target_text.splitlines()
    starts = _function_starts(target_text)
    enclosing = None
    for start, name in starts:
        if start <= line_no:
            enclosing = name
        else:
            break
    if enclosing:
        return enclosing
    for idx in range(min(line_no, len(lines)) - 1, -1, -1):
        line = lines[idx]
        m = re.search(r"struct\s+((?:rocketnet_|rn_)[A-Za-z0-9_]+)", line)
        if m:
            return m.group(1)
        m = re.search(r"#define\s+((?:ROCKETNET_|RN_)[A-Z0-9_]+)", line)
        if m:
            return m.group(1)
        m = re.match(r"\s*([A-Za-z][A-Za-z0-9_]*)\s*(?:\([^)]*\))?\s*==", line)
        if m:
            return m.group(1)
    return None


def check_citations():
    """Every remaining path:line citation in the scanned files must
    resolve: the cited line lies inside the symbol the citing text
    names (the nearest rn_/rocketnet_ name before the citation)."""
    failures = []
    scan_files = list(CITATION_SCAN_FILES)
    for pattern in ("proofs/tla/MCReplayWindow*.tla", "proofs/tla/MCServerSession*.tla",
                    "proofs/tla/MCRNSlots*.tla", "proofs/tla/MCRNClientReceiveQuit*.tla"):
        for path in sorted(ROOT.glob(pattern)):
            rel = path.relative_to(ROOT).as_posix()
            if rel not in scan_files:
                scan_files.append(rel)
    for rel in scan_files:
        path = ROOT / rel
        if not path.is_file():
            continue
        text = path.read_text(errors="replace")
        for m in CITATION_RE.finditer(text):
            target_rel, line_no = m.group(1), int(m.group(2))
            if target_rel.startswith("tla/"):
                target_rel = "proofs/" + target_rel
            target = ROOT / target_rel
            if not target.is_file():
                failures.append(f"{rel}: citation {target_rel}:{line_no} "
                                f"names no file")
                continue
            context = text[max(0, m.start() - 200):m.start()]
            named = FUNC_RE.findall(context)
            named = named[-1] if named else None
            if named is None:
                failures.append(f"{rel}: citation {target_rel}:{line_no} "
                                f"names no symbol")
                continue
            target_text = target.read_text(errors="replace")
            target_lines = target_text.splitlines()
            header_window = target_lines[line_no - 1] \
                if line_no <= len(target_lines) else ""
            if re.search(rf"\b{re.escape(named)}\s*\(", header_window):
                continue
            enclosing = enclosing_symbol(target_text, line_no)
            if enclosing != named:
                failures.append(
                    f"{rel}: citation {target_rel}:{line_no} is inside "
                    f"{enclosing}, not {named}")
    return failures


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
                failures.append(f"{module}: proofs/tla/{module}.tla does not exist")
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
        failures.append(f"{module}: proofs/tla/{module}.tla does not exist")

    if "," in code:
        failures.append(f"{module}: {machine} code column must hold one path, one per row")
        return failures

    if status != "owed":
        code_file = ROOT / code
        if not code_file.is_file():
            failures.append(f"{module}: {code} does not exist")
            return failures
        text = code_file.read_text(errors="replace")
        cite = f"proofs/tla/{module}.tla"
        if cite not in text:
            failures.append(f"{module}: {code} does not cite {cite}")
        for fn, fn_path in note_function_citations(note):
            if not function_defined(fn, fn_path, text):
                where = fn_path or code
                failures.append(f"{module}: {fn} not defined in {where}")
    return failures


def owed_but_cited(rows):
    """An owed row says no code implements its model; a model that a file
    under sdk/ cites is implemented, so its owed row is stale. The citation
    counts with or without the proofs/tla/ prefix: sdk/rocketnet_client_receive_
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
        forms = (f"proofs/tla/{module}.tla", f"{module}.tla")
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
        return set(), [f"{module}: proofs/tla/{module}.tla does not exist"]
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


_CFG_CONST_RE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$", re.M)
_GUARD_IN_RE = re.compile(r'^"([A-Za-z0-9_]+)"\s*\\in\s+([A-Za-z][A-Za-z0-9_]*)$')
_GUARD_BOOL_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)$")


def top_conjuncts(body):
    """The conjuncts of an action body's outermost /\\ list: each line at
    the shallowest /\\ indentation, split on /\\."""
    lines = [l for l in body.splitlines() if l.strip().startswith("/\\")]
    if not lines:
        return []
    depth = min(len(l) - len(l.lstrip()) for l in lines)
    out = []
    for l in lines:
        if len(l) - len(l.lstrip()) == depth:
            out.extend(p.strip() for p in l.split("/\\") if p.strip())
    return out


def module_cfgs(module):
    """The model-checking configs of a module: proofs/tla/MC<Module>.cfg and
    proofs/tla/MC<Module><Suffix>.cfg, skipping a suffix that names a longer
    module (MCRNSlotsFoo.cfg belongs to RNSlotsFoo when proofs/tla/RNSlotsFoo.tla
    exists)."""
    cfgs = {}
    modules = {p.stem for p in TLA_DIR.glob("*.tla")}
    for path in sorted(TLA_DIR.glob(f"MC{module}*.cfg")):
        rest = path.stem[len("MC") + len(module):]
        if rest and not rest[0].isupper():
            continue
        if any(m != module and m.startswith(module) and path.stem[2:].startswith(m)
               for m in modules):
            continue
        consts = {}
        for name, value in _CFG_CONST_RE.findall(strip_comments(path.read_text(errors="replace"))):
            consts[name] = value
        cfgs[path.stem] = consts
    return cfgs


def guard_holds(kind, const, tag, consts):
    value = consts.get(const)
    if value is None:
        return None
    if kind == "in":
        return f'"{tag}"' in value
    return value.strip() == "TRUE"


def witness_only(body, cfgs):
    """The witness cfgs that enable an action, when every passing cfg (a
    stem without "Broken") disables it through a guard conjunct on a
    constant ("tag" \\in Const, or a bare boolean Const); else None."""
    guards = []
    for part in top_conjuncts(body):
        m = _GUARD_IN_RE.match(part)
        if m:
            guards.append(("in", m.group(2), m.group(1)))
        elif _GUARD_BOOL_RE.match(part):
            guards.append(("bool", part, None))
    passing = {n: c for n, c in cfgs.items() if "Broken" not in n}
    broken = {n: c for n, c in cfgs.items() if "Broken" in n}
    if not passing or not broken:
        return None
    for kind, const, tag in guards:
        if not all(const in c for c in cfgs.values()):
            continue
        if any(guard_holds(kind, const, tag, c) for c in passing.values()):
            continue
        enabling = sorted(n for n, c in broken.items() if guard_holds(kind, const, tag, c))
        if enabling:
            return enabling
    return None


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
            # A trailing comment after code is a comment of its own.
            idx = line.find("//")
            if idx >= 0:
                blocks.append(line[idx:])
        i += 1
    if cur is not None:
        blocks.append("\n".join(cur))
    return blocks


def is_action_cited(code_text, module, action):
    # The module name starts the word: line start, whitespace or proofs/tla/
    # before it, so proofs/tla/MCActMod.tla never cites ActMod.
    mod_pat = re.compile(r"(?:^|(?<=\s)|(?<=proofs/tla/))" + re.escape(module)
                         + r"\.tla", re.MULTILINE)
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
    witness = []
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
        cfgs = module_cfgs(module)
        defs = find_definitions(strip_comments((TLA_DIR / f"{module}.tla").read_text(errors="replace"))) \
            if (TLA_DIR / f"{module}.tla").exists() else {}
        for a in sorted(actions):
            enabling = witness_only(defs.get(a, ""), cfgs)
            if enabling:
                witness.append(f"{module} {a}: {', '.join(enabling)}")
                exempt.add(a)
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
    lines.append("WITNESS-ONLY ACTIONS (enabled only by Broken cfgs, exempt from citation)")
    lines.extend(witness)
    lines.append(f"ACTIONS WITNESS-ONLY {len(witness)}")
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
            failures.append(f"{machine}: row appears twice in proofs/tla/COVERAGE.tsv")

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
    failures.extend(check_citations())

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
