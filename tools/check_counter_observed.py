#!/usr/bin/env python3
"""Every counter the library keeps is observed moving by some test.

The library's counters are the public ROCKETNET_COUNTER_* rows
(sdk/rocketnet_counters.h) and each server transport's own counter index:
gso, io_uring and the XDP program (#define ..._COUNTER_<NAME> <n>), iocp and
rio (enum rocketnet_server_<t>_counter_t). The client transports, msgx and
portable keep no counter index of their own: they count into the public rows.
A counter index the checker does not know (a new *_NUM_COUNTERS or
*_counter_t in sdk/) fails, so a new family cannot arrive unread.

The test code comes from the build, never a list of directories: every source
the compile database (--compile-commands, the build's compile_commands.json)
compiles into a target other than the library (sdk/rocketnet_tests.cpp, in
the suite's support library, among them; examples and benchmarks are programs,
not tests), every header those include by a quoted name, and the
`#if ROCKETNET_DEV` blocks of the library's DEV build and of the sdk/ headers
the tests include. The two sources that list every name as data are left out.
Test binaries differ by platform (test_iocp is Windows only, the XDP and
io_uring suites Linux only), so each platform's build is checked on its own
(--platform): a public row on every platform, a transport's counters on the
platform that runs that transport. An OWED row names the platforms it is owed
on. A test observes a counter when the counter's name appears in that code
(comments and string literals blanked, whole names only: a test function's
name, a string row or a comment never counts) inside a check:
- within the parentheses of a check call (check, posix_check, any call whose
  name has a word check, assert, expect, require or verify), or of a call to
  a helper whose body checks one of its own parameters;
- in an expression assigned to a name (a variable, or a field after . or ->)
  that a check in the same file reads;
- in a `return` of a function a check in the same file calls;
- in a row of a table whose field at that position (or, for an array of
  counters, whose element) a check in the same file reads.
A counter only summed, subtracted, printed, allowed in a list or looped over
with every index is not observed: that test does not say which value it
expects of that counter.

The counters no test observes today are OWED, each a named gap in
TESTS-MUTATION-GAPS.md. The list is a ratchet: a counter not observed and not
owed fails (a new counter without a test), and an owed counter a test now
observes fails until its row is removed, so the list only shrinks.

usage: check_counter_observed.py --build BUILD_DIR --platform linux|macos|windows [--root DIR] [--list]
       check_counter_observed.py --self-test
"""

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PUBLIC_ROW = re.compile(r"^\s*X\(\s*([A-Z0-9_]+)\s*,", re.M)
DEFINE_INDEX = re.compile(r"^#define\s+(ROCKETNET_SERVER_(?:GSO|URING|XDP)_COUNTER_[A-Z0-9_]+)\s+\d+", re.M)
ENUM_INDEX = re.compile(r"enum\s+(rocketnet_server_(?:iocp|rio)_counter_t)\s*\{(.*?)\}", re.S)
ANY_FAMILY = re.compile(r"\b([A-Z0-9_]+_NUM_COUNTERS)\b|\benum\s+([a-z0-9_]+_counter_t)\b")
KNOWN_FAMILIES = {
    "ROCKETNET_NUM_COUNTERS", "ROCKETNET_SERVER_GSO_NUM_COUNTERS", "ROCKETNET_SERVER_URING_NUM_COUNTERS",
    "ROCKETNET_SERVER_XDP_NUM_COUNTERS", "ROCKETNET_IOCP_NUM_COUNTERS", "ROCKETNET_RIO_NUM_COUNTERS",
    "rocketnet_server_iocp_counter_t", "rocketnet_server_rio_counter_t",
}
CHECK_CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
DECLARATION_END = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_*&")
CHECK_WORD = re.compile(r"(?:^|_)(?:check|assert|expect|require|verify)(?:_|$)", re.I)
IDENT = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
TEST_EXTS = (".c", ".cpp", ".h", ".hpp", ".inc")
# these list every counter name as data, the audit of the list itself
NAME_LISTS = ("tests/test_unit_counter_classes.cpp",)
NAME_LIST_PREFIX = "tests/source_audit_"

# Counter -> (the platforms it is owed on, the gap). Each is a named gap in
# TESTS-MUTATION-GAPS.md (fix-verify-e, E26). Drop a platform, then the row,
# as a test comes to observe the counter there: the list only shrinks.
ALL = ("linux", "macos", "windows")
OWED = {
    "ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SOURCE": (("linux",), "a source family the boundary cannot convert"),
    "ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SOURCE": (("linux",), "a source family the boundary cannot convert"),
    "ROCKETNET_IOCP_COMPLETION_BATCHES": (("windows",), "a completion-port dequeue that returned entries"),
    "ROCKETNET_IOCP_RECEIVE_TOO_SHORT": (("windows",), "a datagram below the wire minimum; only subtracted from RECEIVE_DROPPED"),
    "ROCKETNET_IOCP_URO_BATCHES": (("windows",), "a receive completion split into more than one segment; checked only == 0"),
    "ROCKETNET_IOCP_USO_FALLBACKS": (("windows",), "a USO send refused and resent frame by frame; checked only == 0"),
    "ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE": (("windows",), "a datagram outside the wire's size bounds; checked only == 0"),
    "ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE": (("windows",), "a datagram past the receive ceiling"),
    "ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT": (("windows",), "a datagram below the wire minimum"),
}


def code_only(text):
    """Comments and string or character literals become spaces; newlines stay."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "/" and nxt == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = out[i + 1] = " "
                i += 2
            continue
        if c in "\"'":
            quote = c
            out[i] = " "
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out[i] = " "
                    i += 1
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def balanced(code, open_at):
    """The text inside the parenthesis or brace that opens at open_at."""
    pair = {"(": ")", "{": "}"}[code[open_at]]
    depth = 0
    for j in range(open_at, len(code)):
        if code[j] == code[open_at]:
            depth += 1
        elif code[j] == pair:
            depth -= 1
            if depth == 0:
                return code[open_at + 1:j]
    return code[open_at + 1:]


def library_counters(root):
    sdk = root / "sdk"
    counters = {}
    text = (sdk / "rocketnet_counters.h").read_text()
    for name in PUBLIC_ROW.findall(text):
        counters["ROCKETNET_COUNTER_" + name] = "public"
    problems = []
    for path in sorted(sdk.glob("*.h")) + sorted(sdk.glob("*.cpp")):
        text = code_only(path.read_text(errors="replace"))
        for name in DEFINE_INDEX.findall(text):
            counters[name] = path.name
        for _, body in ENUM_INDEX.findall(text):
            for name in IDENT.findall(body):
                if not name.endswith("NUM_COUNTERS"):
                    counters[name] = path.name
        for m in ANY_FAMILY.finditer(text):
            family = m.group(1) or m.group(2)
            if family not in KNOWN_FAMILIES:
                problems.append("sdk/%s: counter index %s is not one this checker reads; add its family" % (path.name, family))
    if not any(v == "public" for v in counters.values()):
        problems.append("sdk/rocketnet_counters.h: no X( NAME, ... ) counter rows found")
    return counters, sorted(set(problems))


LIBRARY_TARGET = re.compile(r"^rocketnet(?:_shared)?(?:_dev)?$|^rocketnet_header_warnings$")
NOT_TESTS = ("examples/", "benchmarks/")
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def build_targets(root, build):
    """target -> the repo-relative sources it compiles, read from a configured CMake build directory:
    the CMake File API's codemodel reply when the build has one (every generator, Visual Studio
    included; ask for it with an empty .cmake/api/v1/query/codemodel-v2 before configuring), else
    its compile_commands.json."""
    build = Path(build)
    targets = {}

    def native(path):
        # a Windows build's reply read from WSL: C:/x -> /mnt/c/x
        m = re.match(r"^([A-Za-z]):[/\\](.*)$", str(path))
        if m and Path("/mnt").is_dir():
            return Path("/mnt") / m.group(1).lower() / m.group(2).replace("\\", "/")
        return Path(path)

    def add(target, path, base):
        path, base = native(path), native(base)
        if not path.is_absolute():
            path = Path(base) / path
        path = path.resolve()
        if path.suffix not in TEST_EXTS or not path.is_file() or path.is_relative_to(build.resolve()):
            return  # a custom target's rule or stamp, or a generated file: not a source of the repository
        try:
            rel = str(path.relative_to(root))
        except ValueError:
            return
        targets.setdefault(target, set()).add(rel)

    reply = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json")) if reply.is_dir() else []
    if indexes:
        index = json.loads(indexes[-1].read_text())
        for obj in index.get("objects", []):
            if obj.get("kind") != "codemodel":
                continue
            model = json.loads((reply / obj["jsonFile"]).read_text())
            source_dir = model["paths"]["source"]
            for config in model["configurations"][:1]:
                for ref in config["targets"]:
                    target = json.loads((reply / ref["jsonFile"]).read_text())
                    for source in target.get("sources", []):
                        add(target["name"], source["path"], source_dir)
        return targets
    database = build / "compile_commands.json"
    if not database.is_file():
        return targets
    for entry in json.loads(database.read_text()):
        output = entry.get("output") or (re.search(r"\s-o\s+(\S+)", entry.get("command", "")) or [None, ""])[1]
        target = re.search(r"CMakeFiles/([^/]+)\.dir", output or "")
        add(target.group(1) if target else "", entry["file"], entry.get("directory", "."))
    return targets


def build_sources(root, builds):
    """From the configured builds: (test code files, files counted by their DEV blocks only, library files).

    A test code file is a source a target other than the library compiles (examples and benchmarks
    are programs, not tests), and every header those include by a quoted name. A library source,
    wherever it is compiled (a test binary may compile one directly), and an sdk/ header a test
    includes, count only by their ROCKETNET_DEV blocks."""
    tests, library = set(), set()
    for build in builds:
        for target, files in build_targets(root, build).items():
            if LIBRARY_TARGET.match(target):
                library.update(files)
            else:
                tests.update(f for f in files if not f.startswith(NOT_TESTS))
    dev = tests & library
    tests -= library
    dev |= {f for f in tests if f.startswith("sdk/") and not f.endswith((".c", ".cpp"))}
    tests -= dev
    queue = sorted(tests)
    while queue:
        rel = queue.pop()
        try:
            text = (root / rel).read_text(errors="replace")
        except OSError:
            continue
        for inc in INCLUDE.findall(code_only_keep_strings(text)):
            for base in (Path(rel).parent, Path("tests"), Path("sdk"), Path(".")):
                cand = (root / base / inc).resolve()
                if not cand.is_file():
                    continue
                try:
                    crel = str(cand.relative_to(root))
                except ValueError:
                    break
                if crel.startswith("sdk/") or crel in library:
                    dev.add(crel)
                elif crel not in tests:
                    tests.add(crel)
                    queue.append(crel)
                break
    tests = {t for t in tests if t not in NAME_LISTS and not t.startswith(NAME_LIST_PREFIX)}
    dev |= {f for f in library if f.endswith((".c", ".cpp"))}
    return sorted(tests), sorted(dev - tests), sorted(library)


def code_only_keep_strings(text):
    """Comments blanked, literals kept (to read #include lines)."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


def dev_blocks(code):
    """The lines inside each #if ROCKETNET_DEV block (not its #else part); every other line blanked."""
    out = []
    keep = []  # one entry per open #if: whether it is a DEV block being kept
    for line in code.split("\n"):
        stripped = line.strip()
        if re.match(r"#\s*if", stripped):
            keep.append(re.match(r"#\s*if\s+ROCKETNET_DEV\b", stripped) is not None)
            out.append("")
        elif re.match(r"#\s*(?:else|elif)", stripped) and keep:
            keep[-1] = False
            out.append("")
        elif re.match(r"#\s*endif", stripped) and keep:
            keep.pop()
            out.append("")
        else:
            out.append(line if any(keep) else "")
    return "\n".join(out)


def check_spans(code, helpers=frozenset()):
    """The argument text of every check call, and of every call to a helper that checks its parameters."""
    spans = []
    for m in CHECK_CALL.finditer(code):
        name = m.group(1)
        if not (CHECK_WORD.search(name) or name in helpers):
            continue
        # the text before the name, its trailing whitespace dropped, read in
        # place: a copy of the prefix per call made the check quadratic in a
        # file's length (56 s of a Linux unit row's 120 s budget)
        end = m.start()
        while end > 0 and code[end - 1].isspace():
            end -= 1
        if end > 0 and code[end - 1] in DECLARATION_END and not code.endswith(("return", "else"), 0, end):
            continue  # a declaration or definition, not a call
        spans.append(balanced(code, m.end() - 1))
    return spans


def checking_helpers(codes):
    """Functions whose body checks one of their own parameters: a call to one is a check of its arguments."""
    helpers = set()
    for _ in range(2):
        for code in codes:
            for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*(?:const\s*)?\{", code):
                fname, params = m.group(1), m.group(2)
                if fname in helpers or CHECK_WORD.search(fname) or fname in ("if", "for", "while", "switch"):
                    continue
                names = set()
                for param in params.split(","):
                    words = IDENT.findall(re.sub(r"\[[^\]]*\]", "", param))
                    if len(words) >= 2:
                        names.add(words[-1])
                if not names:
                    continue
                body = balanced(code, m.end() - 1)
                if names & set(IDENT.findall(" ".join(check_spans(body, helpers)))):
                    helpers.add(fname)
    return frozenset(helpers)


def struct_fields(codes):
    """struct name -> its field names in order."""
    fields = {}
    for code in codes:
        for m in re.finditer(r"\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", code):
            fields.setdefault(m.group(1), _field_names(balanced(code, m.end() - 1)))
        for m in re.finditer(r"\btypedef\s+struct\s*(?:[A-Za-z_][A-Za-z0-9_]*)?\s*\{", code):
            body = balanced(code, m.end() - 1)
            after = code[m.end() + len(body) + 1:]
            name = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*;", after)
            if name:
                fields.setdefault(name.group(1), _field_names(body))
    return fields


def _field_names(body):
    names = []
    for decl in body.split(";"):
        for part in decl.split(","):
            words = IDENT.findall(re.sub(r"\[[^\]]*\]|:\s*\d+", "", part))
            if words and len(IDENT.findall(re.sub(r"\[[^\]]*\]", "", decl))) >= 2:
                names.append(words[-1])
    return names


def enclosing_brace(code, at):
    depth = 0
    for j in range(at - 1, -1, -1):
        if code[j] == "}":
            depth += 1
        elif code[j] == "{":
            if depth == 0:
                return j
            depth -= 1
    return -1


def table_fields(code, at, fields):
    """For a name at offset at inside an array initializer: (array name, field read for it or None)."""
    row = enclosing_brace(code, at)
    if row < 0:
        return None
    decl = re.compile(r"(?:struct\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[[^\]]*\]\s*=\s*$")
    m = decl.search(code[:row])
    if m:
        return m.group(2), None  # an array of counters: its elements are read by index
    outer = enclosing_brace(code, row)
    if outer < 0:
        return None
    m = decl.search(code[:outer])
    if not m or m.group(1) not in fields:
        return None
    depth, position = 0, 0
    for c in code[row + 1:at]:
        if c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        elif c == "," and depth == 0:
            position += 1
    names = fields[m.group(1)]
    return m.group(2), (names[position] if position < len(names) else None)


def functions(code):
    """name -> body for each function definition in the text."""
    found = {}
    for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{}()]|\([^()]*\))*\)\s*(?:const\s*)?\{", code):
        found.setdefault(m.group(1), balanced(code, m.end() - 1))
    return found


def moves(span, word):
    """Whether a check span reads the counter other than as `counter == 0` (which says it never moved)."""
    for clause in re.split(r"&&|\|\||,", span):
        if not word.search(clause):
            continue
        if re.search(r"==\s*0\s*\)*\s*$", clause) or re.match(r"^\s*\(*\s*0\s*==", clause):
            continue
        return True
    return False


def observed_in(code, names, helpers=frozenset(), fields=None, called=frozenset()):
    fields = fields or {}
    spans = check_spans(code, helpers)
    checked = " ".join(spans)
    checked_words = set(IDENT.findall(checked))
    seen = set()
    for name in names:
        if name not in code:
            continue
        word = re.compile(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(name))
        if any(moves(span, word) for span in spans):
            seen.add(name)
            continue
        # a row of a table whose field (or element) a check reads
        for m in word.finditer(code):
            hit = table_fields(code, m.start(), fields)
            if not hit:
                continue
            array, field = hit
            if field is None:
                if re.search(r"(?<![A-Za-z0-9_])%s\s*\[" % re.escape(array), checked) or array in checked_words:
                    seen.add(name)
            elif re.search(r"(?:\.|->)\s*%s(?![A-Za-z0-9_])" % re.escape(field), checked):
                seen.add(name)
            if name in seen:
                break
        if name in seen:
            continue
        # assigned to a name a check reads
        for stmt in re.split(r"[;{}]", code):
            if not word.search(stmt) or "=" not in stmt:
                continue
            lhs = re.split(r"(?<![=!<>+\-*/%&|^])=(?!=)", stmt, maxsplit=1)
            if len(lhs) != 2 or not word.search(lhs[1]):
                continue
            target = IDENT.findall(lhs[0])
            if target and target[-1] in checked_words:
                seen.add(name)
                break
        if name in seen:
            continue
        # returned by a function a check calls (in any test source): in its return, or folded
        # into a name it returns (`valid &= counter == 1; ... return valid;`), or read into a
        # name a check in this file reads
        for fname, body in functions(code).items():
            if fname in checked_words or fname in called:
                returned = set()
                for ret in re.findall(r"\breturn\b([^;]*);", body):
                    if word.search(ret):
                        seen.add(name)
                        break
                    returned.update(IDENT.findall(ret))
                if name not in seen and word.search(body):
                    for stmt in re.split(r"[;{}]", body):
                        if word.search(stmt) and "=" in stmt:
                            parts = re.split(r"(?<![=!<>])(?:[&|]?=)(?!=)", stmt, maxsplit=1)
                            left = IDENT.findall(parts[0])
                            if len(parts) == 2 and left and (left[-1] in checked_words or left[-1] in returned):
                                seen.add(name)
                                break
            if name in seen:
                break
    return seen


def observed(root, counters, tests, dev):
    names = sorted(counters)
    seen = {}
    codes = {rel: code_only((root / rel).read_text(errors="replace")) for rel in tests}
    for rel in dev:
        codes[rel] = dev_blocks(code_only((root / rel).read_text(errors="replace")))
    helpers = checking_helpers(codes.values())
    fields = struct_fields(codes.values())
    called = set()
    for code in codes.values():
        called.update(IDENT.findall(" ".join(check_spans(code, helpers))))
    called = frozenset(called)
    for rel, code in codes.items():
        for name in observed_in(code, names, helpers, fields, called):
            seen.setdefault(name, rel)
    return seen


# the platform whose build runs a transport's counters; a public row is judged on every platform
FAMILY_PLATFORMS = {
    "rocketnet_server_gso.h": ("linux",), "rocketnet_server_uring.h": ("linux",), "rocketnet_wire.h": ("linux",),
    "rocketnet_server_iocp.h": ("windows",), "rocketnet_server_rio.h": ("windows",),
    "public": ("linux", "macos", "windows"),
}


def check(root, owed, builds, platform):
    counters, problems = library_counters(root)
    for family in sorted(set(counters.values()) - set(FAMILY_PLATFORMS)):
        problems.append("counter family in %s has no platform in FAMILY_PLATFORMS" % family)
    tests, dev, _ = build_sources(root, builds)
    if not tests:
        problems.append("the build names no test source: give a configured build directory")
    scope = {n: f for n, f in counters.items() if platform in FAMILY_PLATFORMS.get(f, ())}
    seen = observed(root, scope, tests, dev)
    owed_here = {n for n, (platforms, _) in owed.items() if platform in platforms}
    for name in sorted(scope):
        if name in seen and name in owed_here:
            problems.append("%s is observed by %s on %s: drop %s from its OWED row (the list only shrinks)"
                            % (name, seen[name], platform, platform))
        elif name not in seen and name not in owed_here:
            problems.append("%s (%s) is observed by no test the %s build compiles: a test must drive it and check its value"
                            % (name, scope[name], platform))
    for name, (platforms, _) in sorted(owed.items()):
        if name not in counters:
            problems.append("OWED names %s, which is no library counter" % name)
        elif platform in platforms and name not in scope:
            problems.append("OWED lists %s on %s, where its transport does not run" % (name, platform))
    return scope, seen, tests, problems


def self_test():
    failures = []

    def tree(sdk_extra="", test_text="", written=True):
        d = Path(tempfile.mkdtemp(prefix="cco-"))
        (d / "sdk").mkdir()
        (d / "tests").mkdir()
        (d / "sdk" / "rocketnet_counters.h").write_text(
            "#define ROWS(X) \\\n    X( ALPHA, ARRIVAL, NONE ) \\\n    X( BETA, REFUSAL, NONE )\n"
            "enum { ROCKETNET_NUM_COUNTERS };\n")
        extra = "rocketnet_server_gso.h" if "GSO" in sdk_extra else "rocketnet_server_rio.h" if "rio" in sdk_extra else "extra.h"
        (d / "sdk" / extra).write_text(sdk_extra)
        (d / "sdk" / "rocketnet_x.cpp").write_text('#include "' + extra + '"\nvoid w( uint64_t * c ) { c[0]++; }\n'
                                                   + (" ".join(IDENT.findall(sdk_extra)) if written else "") + "\n")
        (d / "tests" / "test_unit_x.cpp").write_text(test_text)
        (d / "support").mkdir()
        (d / "support" / "suite_support.cpp").write_text(support)
        entries = [{"directory": str(d / "build"), "file": str(d / f), "output": "CMakeFiles/%s.dir/x.o" % t,
                    "command": "c++ -c " + f}
                   for f, t in (("tests/test_unit_x.cpp", "test"), ("support/suite_support.cpp", "test_support"),
                                ("sdk/rocketnet_x.cpp", "rocketnet_dev"))]
        (d / "build").mkdir()
        (d / "build" / "compile_commands.json").write_text(json.dumps(entries))
        return d

    support = ""

    cases = [0]

    def run(name, d, owed, want_problems, platform="linux"):
        cases[0] += 1
        _, _, _, problems = check(d.resolve(), owed, [d / "build"], platform)
        got = bool(problems)
        if got != want_problems:
            failures.append("%s: problems=%s" % (name, problems))

    both = ("static void t() { check( rocketnet_counter_value( c, ROCKETNET_COUNTER_ALPHA ) == 1 );\n"
            "    uint64_t beta = c->values[ROCKETNET_COUNTER_BETA];\n    check( beta == 2 ); }\n")
    run("both observed", tree(test_text=both), {}, False)
    run("comment only", tree(test_text=both.replace("check( beta == 2 );", "// check( ROCKETNET_COUNTER_BETA );")), {}, True)
    run("string only", tree(test_text=both.replace("uint64_t beta = c->values[ROCKETNET_COUNTER_BETA];\n    check( beta == 2 );",
                                                   "check( strcmp( n, \"ROCKETNET_COUNTER_BETA\" ) == 0 );")), {}, True)
    run("test name only", tree(test_text=both.replace("uint64_t beta = c->values[ROCKETNET_COUNTER_BETA];\n    check( beta == 2 );",
                                                      "} static void test_ROCKETNET_COUNTER_BETA_x() { check( 1 );")), {}, True)
    run("named, never checked", tree(test_text=both.replace("check( beta == 2 );", "printf( \"%d\", (int) beta );")), {}, True)
    gap = (("linux",), "gap")
    run("owed and unobserved", tree(test_text=both.replace("check( beta == 2 );", "")), {"ROCKETNET_COUNTER_BETA": gap}, False)
    run("owed on another platform only", tree(test_text=both.replace("check( beta == 2 );", "")),
        {"ROCKETNET_COUNTER_BETA": (("windows",), "gap")}, True)
    run("owed but observed: ratchet", tree(test_text=both), {"ROCKETNET_COUNTER_BETA": gap}, True)
    run("owed names no counter", tree(test_text=both), {"ROCKETNET_COUNTER_GAMMA": gap}, True)
    support = "void beta_case() { check( c->values[ROCKETNET_COUNTER_BETA] == 1 ); }\n"
    run("a test source outside tests/, compiled into a test binary, observes",
        tree(test_text=both.replace("check( beta == 2 );", "")), {}, False)
    support = "void beta_case() { check( c->values[ROCKETNET_COUNTER_BETA] == 1 ); }\n"
    d = tree(test_text=both.replace("check( beta == 2 );", ""))
    entries = json.loads((d / "build" / "compile_commands.json").read_text())
    (d / "build" / "compile_commands.json").write_text(json.dumps([e for e in entries if "suite_support" not in e["file"]]))
    run("the same source, not compiled into any test binary, does not", d, {}, True)
    support = ""
    run("new transport counter with no test",
        tree(sdk_extra="#define ROCKETNET_SERVER_GSO_COUNTER_NEW 0\n#define ROCKETNET_SERVER_GSO_NUM_COUNTERS 1\n", test_text=both), {}, True)
    run("new counter family", tree(sdk_extra="enum rocketnet_server_foo_counter_t { FOO_A, FOO_NUM_COUNTERS };\n", test_text=both), {}, True)
    run("enum family observed", tree(sdk_extra="enum rocketnet_server_rio_counter_t { ROCKETNET_RIO_COUNTER_A, ROCKETNET_RIO_NUM_COUNTERS };\n",
                                     test_text=both + "void u() { check( counters[ROCKETNET_RIO_COUNTER_A] == 3 ); }\n"), {}, False,
        platform="windows")
    run("enum family unobserved on its platform", tree(sdk_extra="enum rocketnet_server_rio_counter_t { ROCKETNET_RIO_COUNTER_A, ROCKETNET_RIO_NUM_COUNTERS };\n",
                                                      test_text=both), {}, True, platform="windows")
    run("enum family not judged off its platform", tree(sdk_extra="enum rocketnet_server_rio_counter_t { ROCKETNET_RIO_COUNTER_A, ROCKETNET_RIO_NUM_COUNTERS };\n",
                                                       test_text=both), {}, False, platform="linux")
    run("read into a field a check reads",
        tree(test_text=both.replace("uint64_t beta = c->values[ROCKETNET_COUNTER_BETA];\n    check( beta == 2 );",
                                    "out->beta = c->values[ROCKETNET_COUNTER_BETA]; check( now.beta - start.beta == 1 );")), {}, False)
    run("returned by a function a check calls",
        tree(test_text=both.replace("uint64_t beta = c->values[ROCKETNET_COUNTER_BETA];\n    check( beta == 2 );",
                                    "} static uint64_t read_beta( const c_t * c ) { return c->values[ROCKETNET_COUNTER_BETA]; }\n"
                                    "static void v() { check( read_beta( c ) == 2 );")), {}, False)
    table = ("struct row_t { int kind; int outcome; int other; };\n"
             "static const row_t rows[] = { { 1, ROCKETNET_COUNTER_ALPHA, 0 }, { 2, ROCKETNET_COUNTER_BETA, 0 } };\n"
             "static void run_row( const row_t * row ) { check( value( row->outcome ) == 1 ); }\n")
    run("table rows whose field a check reads", tree(test_text=table), {}, False)
    run("table rows in a field no check reads", tree(test_text=table.replace("row->outcome", "row->kind")), {}, True)
    helper = ("static void drive_and_read( const counters_t * c, int counter ) { check( c->values[counter] == 1 ); }\n"
              "static void t() { drive_and_read( c, ROCKETNET_COUNTER_ALPHA ); drive_and_read( c, ROCKETNET_COUNTER_BETA ); }\n")
    run("passed to a helper that checks its parameter", tree(test_text=helper), {}, False)
    run("passed to a helper that only prints it",
        tree(test_text=helper.replace("check( c->values[counter] == 1 );", "printf( \"%d\", counter );")), {}, True)
    scalar = ("static const int refusals[] = { ROCKETNET_COUNTER_ALPHA, ROCKETNET_COUNTER_BETA };\n"
              "static void t() { for ( int i = 0; i < 2; i++ ) { check( value( refusals[i] ) == 1 ); } }\n")
    run("array of counters a check indexes", tree(test_text=scalar), {}, False)
    run("checked only as never moved (== 0)",
        tree(test_text=both.replace("check( beta == 2 );", "check( c->values[ROCKETNET_COUNTER_BETA] == 0 );")), {}, True)
    run("folded into a result a check reads in another file",
        tree(test_text=both.replace("uint64_t beta = c->values[ROCKETNET_COUNTER_BETA];\n    check( beta == 2 );",
                                    "check( seam_ok( c ) );")
             + "bool seam_ok( const c_t * c ) { bool valid = true; valid &= c->values[ROCKETNET_COUNTER_BETA] == 1; return valid; }\n"),
        {}, False)
    if failures:
        for f in failures:
            print("FAIL self-test:", f)
        return 1
    print("ok check_counter_observed self-test: %d cases" % cases[0])
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--build", action="append", default=[],
                    help="a configured CMake build directory (repeatable); the test sources are read from it")
    ap.add_argument("--platform", choices=("linux", "macos", "windows"), help="the platform that build is for")
    ap.add_argument("--list", action="store_true", help="print each unobserved counter, one per line")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve()
    if not (root / "sdk" / "rocketnet_counters.h").is_file():
        print("FAIL check_counter_observed: no sdk/rocketnet_counters.h under %s: 0 counters, nothing was checked" % root)
        return 1
    if not args.build or not args.platform:
        print("FAIL check_counter_observed: give --build (a configured CMake build directory) and --platform")
        return 2
    counters, seen, tests, problems = check(root, OWED, args.build, args.platform)
    if args.list:
        for name in sorted(counters):
            if name not in seen:
                print("%s\t%s" % (name, counters[name]))
        return 0
    if problems:
        for p in problems:
            print("FAIL check_counter_observed:", p)
        print("%d of %d counters judged on %s are observed by a test (%d test sources from the build)"
              % (len(seen), len(counters), args.platform, len(tests)))
        return 1
    print("ok check_counter_observed: %d of %d counters judged on %s are observed by a test (%d test sources from the build); "
          "the rest owed, each a named gap" % (len(seen), len(counters), args.platform, len(tests)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
