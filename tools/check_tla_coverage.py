#!/usr/bin/env python3
"""Check every proofs/tla/COVERAGE.tsv row against its model and code."""

import argparse
import re
import subprocess
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

# path:line: a line number drifts with every edit above it (157 of 171 had,
# VERIFY-6 A5), so a citation names its symbol instead.
LINE_CITATION_RE = re.compile(
    r"((?:sdk|tests|(?:proofs/)?tla)/[A-Za-z0-9_./-]+\.[a-z]+):"
    r"(?:\s*(?:\*\))?\s*\n\s*(?:\(\*|//|\*)?\s*)?(\d+)(?:-(\d+))?")
# A bare line reference after a path named earlier ("`:340`", "(:2091)",
# "at :2091"): it drifts the same way, and the path:line rule above alone
# left 69 of them behind.
BARE_LINE_RE = re.compile(r"(?<![\w.:/`\]\"'])`?:[0-9]{2,5}(?:-[0-9]+)?\b`?")
# A citation pinned to a commit ("sdk/a.cpp:12` at `4e1be4e8`", or a short
# parenthesis closed by "are cited at `14993f6`") names a fixed text and
# cannot drift, so it stands.
PINNED_AT_COMMIT_RE = re.compile(r"(?:(?!\.\s)[^;|]){0,160}?\bat `[0-9a-f]{7,40}`", re.S)
_CITED_PATH = r"((?:sdk|tests|proofs/tla)/[A-Za-z0-9_./-]+\.(?:cpp|h|c|inc|tla))"
_CITED_SYMBOL = r"((?:rn_|rocketnet_|RN_|ROCKETNET_)[A-Za-z0-9_]+)"
# "sdk/a.cpp (rn_x)": the path, then the symbol it defines
PATH_THEN_SYMBOL_RE = re.compile(_CITED_PATH + r"`?\s*\(\s*`?" + _CITED_SYMBOL + r"`?\s*\)")
# "rn_x (sdk/a.cpp)": the symbol, then the path that defines it
SYMBOL_THEN_PATH_RE = re.compile(_CITED_SYMBOL + r"`?\s*\(\s*`?" + _CITED_PATH + r"`?\s*\)")
# Every file that can carry a path:line citation: every tracked source file
# wherever it lives (git ls-files, not a list of directories), and the models'
# own notes and ledgers under proofs/tla/. A hand
# list of 16 files left 74 citations unchecked (VERIFY-6 A5, VERIFY-3 C-14),
# and a directory list missed the test code outside tests/ (VERIFY-3 W16:
# sdk/rocketnet_tests.cpp, tools/, benchmarks/, examples/).
CITATION_SCAN_SUFFIXES = (".tla", ".md", ".tsv", ".h", ".hpp", ".c", ".cpp", ".inc")
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


def defines(fn, text):
    """text holds a definition of fn: a function header with a body, or a
    function-like macro. A call, a declaration or a comment is not one."""
    if any(name == fn for _, name in _function_starts(text)):
        return True
    return re.search(rf"^\s*#\s*define\s+{re.escape(fn)}\(", text, re.M) is not None


_NOT_A_TYPE = {"return", "if", "else", "while", "for", "switch", "case", "sizeof", "delete", "new", "do"}


def declares(fn, text):
    """text declares fn: a prototype line (a type, then fn, then "("), never a
    call, a return statement or a comment."""
    pattern = re.compile(
        rf"^\s*([A-Za-z_]\w*)\b[\w\s\*&<>:,]*?\b{re.escape(fn)}\s*\(", re.M)
    return any(m.group(1) not in _NOT_A_TYPE for m in pattern.finditer(text))


def function_defined(fn, path, code_text):
    """fn is defined (or, in a header a note names, declared) in the file a
    note gives it; a name with no file is
    defined in the row's own code file or in another file under sdk/ (a note
    names the helpers its code calls). A mention (a call site, a comment)
    once satisfied this, so a function whose definition was renamed away
    still passed (VERIFY-6 A4)."""
    if path:
        target = ROOT / path
        if target.is_file():
            text = target.read_text(errors="replace")
            return defines(fn, text) or declares(fn, text)
        return False
    if code_text and defines(fn, code_text):
        return True
    return any(defines(fn, text) for text in sdk_definitions_texts(fn))


def sdk_definitions_texts(fn):
    """The sdk/ files that mention fn followed by "(": the only ones that can
    define it, so the definition scan stays cheap."""
    return [text for text in sdk_texts().values() if f"{fn}(" in text]


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


def citation_scan_files():
    # the checkout's tracked files; a tree that is not its own checkout (a
    # self-test's) is walked whole
    rels = None
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
        if Path(top).resolve() == ROOT.resolve():
            out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
            rels = out.decode("utf-8", "replace").split("\0")
    except (OSError, subprocess.CalledProcessError):
        rels = None
    if rels is None:
        rels = [p.relative_to(ROOT).as_posix() for p in ROOT.rglob("*") if ".git" not in p.parts]
    files = []
    for rel in sorted(rels):
        notes = rel.endswith((".md", ".tsv")) and not rel.startswith("proofs/tla/")
        if rel.endswith(CITATION_SCAN_SUFFIXES) and not notes and (ROOT / rel).is_file():
            files.append(rel)
    return files


def check_citations():
    """Every tracked file of a CITATION_SCAN_SUFFIXES kind: no path:line citation (cite
    the symbol), and a path cited beside a symbol, either way round, must
    still name that symbol: a rename or a removal leaves the citation
    pointing at nothing. (A note may cite the header that declares a
    function or the model of a type, so naming is the test, not defining.)"""
    failures = []
    scan_files = citation_scan_files()
    if not scan_files:
        failures.append("no file to scan for citations under " + str(ROOT))
    texts = {}

    def target_text(rel):
        if rel not in texts:
            path = ROOT / rel
            texts[rel] = path.read_text(errors="replace") if path.is_file() else None
        return texts[rel]

    for rel in scan_files:
        text = (ROOT / rel).read_text(errors="replace")
        for m in LINE_CITATION_RE.finditer(text):
            if PINNED_AT_COMMIT_RE.match(text, m.end()):
                continue
            line = text.count("\n", 0, m.start()) + 1
            failures.append(
                f"{rel}:{line}: line citation {m.group(1)}:{m.group(2)}; cite the symbol "
                f"instead, as {m.group(1)} (symbol): line numbers drift")
        for m in BARE_LINE_RE.finditer(text):
            if PINNED_AT_COMMIT_RE.match(text, m.end()):
                continue
            line = text.count("\n", 0, m.start()) + 1
            failures.append(
                f"{rel}:{line}: bare line reference {m.group(0).strip('`')}; cite the "
                f"symbol instead: line numbers drift")
        pairs = [(m.group(1), m.group(2), m.start()) for m in PATH_THEN_SYMBOL_RE.finditer(text)]
        pairs += [(m.group(2), m.group(1), m.start()) for m in SYMBOL_THEN_PATH_RE.finditer(text)]
        for target, sym, start in pairs:
            line = text.count("\n", 0, start) + 1
            body = target_text(target)
            if body is None:
                failures.append(f"{rel}:{line}: citation {target} ({sym}) names no file")
            elif not re.search(rf"\b{re.escape(sym)}\b", body):
                failures.append(f"{rel}:{line}: citation {target} ({sym}): {target} does not name {sym}")
    return failures


# A model citation in the code: "proofs/tla/RNFoo.tla" or "RNFoo.tla", and the
# operators named right after it ("RNFoo.tla Open and Close", "RNFoo.tla's
# Open, Close"). The module must exist, and an operator it names must be
# defined by the module or a module it extends or instances: a citation of a
# model that was never written, or of an action it does not have, binds the
# code to nothing.
CODE_SUFFIXES = (".h", ".hpp", ".c", ".cpp", ".inc")
MODEL_CITATION_RE = re.compile(r"(?<![A-Za-z0-9_./-])(?:proofs/tla/)?([A-Za-z][A-Za-z0-9_]*)\.tla\b")
_OPERATOR = r"[A-Z][A-Za-z0-9_]*"
CITED_OPERATORS_RE = re.compile(
    r"`?\)?(?:'s)?[ \t]+`?(" + _OPERATOR + r"(?:`?(?:[ \t]*,[ \t]*|[ \t]+and[ \t]+|[ \t]+or[ \t]+|[ \t]*/[ \t]*)`?"
    + _OPERATOR + r")*)")
# an EXTENDS list runs on past a line that ends in a comma
_MODULE_REF_RE = re.compile(r"\bEXTENDS\b((?:[^\n]*,[ \t]*\n)*[^\n]*)|\bINSTANCE\s+([A-Za-z_][A-Za-z0-9_]*)")
# a CONSTANT(S) or VARIABLE(S) list runs to the next line that starts in column 0
_DECLARATION_RE = re.compile(r"\b(?:CONSTANTS?|VARIABLES?)\b(.*?)(?=^\S|\Z)", re.S | re.M)


def declared_names(text):
    """The operators, constants and variables a comment-stripped module declares."""
    names = set(find_definitions(text))
    for m in _DECLARATION_RE.finditer(text):
        names.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", m.group(1)))
    return names


def model_definitions(module, cache):
    """The operator names module and every proofs/tla module it EXTENDS or
    INSTANCEs define, or None when proofs/tla/<module>.tla does not exist."""
    if module in cache:
        return cache[module]
    if not (TLA_DIR / f"{module}.tla").is_file():
        cache[module] = None
        return None
    names, seen, queue = set(), set(), [module]
    while queue:
        name = queue.pop()
        path = TLA_DIR / f"{name}.tla"
        if name in seen or not path.is_file():
            continue
        seen.add(name)
        text = strip_comments(path.read_text(errors="replace"))
        names.update(declared_names(text))
        for m in _MODULE_REF_RE.finditer(text):
            queue.extend(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", m.group(1)) if m.group(1) else [m.group(2)])
    cache[module] = names
    return names


def operator_like(word, defined_anywhere):
    """A word after a model citation that names an operator rather than prose:
    defined by some model, or a CamelCase name of two or more words."""
    return word in defined_anywhere or (sum(c.isupper() for c in word) >= 2 and any(c.islower() for c in word))


def check_model_citations():
    """Every model a C or C++ file cites exists, and every operator (or
    constant, or variable) a citation names right after the module is declared
    by that module or a module it extends or instances."""
    failures = []
    cache = {}
    defined_anywhere = set()
    if TLA_DIR.is_dir():
        for path in sorted(TLA_DIR.glob("*.tla")):
            defined_anywhere.update(declared_names(strip_comments(path.read_text(errors="replace"))))
    for rel in citation_scan_files():
        if not rel.endswith(CODE_SUFFIXES):
            continue
        text = (ROOT / rel).read_text(errors="replace")
        for m in MODEL_CITATION_RE.finditer(text):
            module = m.group(1)
            line = text.count("\n", 0, m.start()) + 1
            names = model_definitions(module, cache)
            if names is None:
                failures.append(f"{rel}:{line}: cites {m.group(0)}, which is not a model in proofs/tla")
                continue
            ops = CITED_OPERATORS_RE.match(text, m.end())
            if not ops:
                continue
            # a name the chain runs into that is itself a file ("ServerStopSpread.tla
            # SealOne", "MCFoo.cfg") is the next citation, not an operator
            for w in re.finditer(_OPERATOR, ops.group(1)):
                word, after = w.group(0), ops.start(1) + w.end()
                if re.match(r"\.(?:tla|cfg)\b", text[after:after + 5]):
                    break
                if operator_like(word, defined_anywhere) and word not in names:
                    failures.append(f"{rel}:{line}: cites {module}.tla {word}, which {module} does not declare "
                                    f"(nor any module it extends or instances)")
    return failures


def iter_rows():
    if not COVERAGE.is_file():
        raise ValueError(f"{COVERAGE}: missing: 0 rows were read")
    lines = COVERAGE.read_text().splitlines()
    if not lines:
        return
    header = lines[0].split("\t")
    for line_no, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) != len(header):
            raise ValueError(
                f"proofs/tla/COVERAGE.tsv:{line_no}: expected {len(header)} columns, "
                f"found {len(parts)}")
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

    # "partial" is the DST-mirror distinction: a check runs and observes a
    # narrower predicate than the named property (or a related one), so the
    # row is neither "covered" nor "owed" ("owed" would also conflict with
    # owed_but_cited for modules sdk/ cites). The note states what is checked
    # and what mirror remains owed.
    if status != "owed" and status not in ("current", "covered", "partial"):
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
                where = fn_path or f"{code} nor elsewhere under sdk/"
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
    parser.add_argument("--root", type=Path, default=None,
                        help="the repository to check (default: this one); 0 coverage rows is an error")
    args = parser.parse_args()
    if args.root is not None:
        global ROOT, COVERAGE, TLA_DIR
        ROOT = args.root
        COVERAGE = ROOT / "proofs" / "tla" / "COVERAGE.tsv"
        TLA_DIR = ROOT / "proofs" / "tla"

    try:
        rows = list(iter_rows())
    except ValueError as error:
        print(error)
        print("COVERAGE FAIL 1")
        return 1
    if not rows:
        # zero inputs is an error: a coverage table with no row checks nothing
        print(f"{COVERAGE}: 0 rows: nothing was checked")
        print("COVERAGE FAIL 1")
        return 1
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
    failures.extend(check_model_citations())

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
