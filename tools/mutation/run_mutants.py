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
are printed. --changed-since REF narrows the failure rule: a risk-class
survivor fails only on a line new or modified in git diff REF HEAD; every
other survivor is reported (results.json, --summary-md) and never fails.
Without these two flags the campaign is the original one.

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


HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def changed_lines(diff_text):
    """{path: set of new-side line numbers} added or modified in a git diff -U0."""
    changed, path = {}, None
    for line in diff_text.splitlines():
        if line.startswith("+++ "):
            name = line[4:].strip()
            path = name[2:] if name.startswith("b/") else None
            continue
        m = HUNK_RE.match(line)
        if m and path:
            start, count = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
            changed.setdefault(path, set()).update(range(start, start + count))
    return changed


def verdict(mutant, changed, not_judged):
    """For a survived mutant: 'fail', 'report' or 'not_judged_here'; None otherwise.

    changed None is the campaign's original rule: every risk-class survivor
    fails. With changed {path: lines}, only a risk-class survivor on a line
    new or modified since the reference fails; every other survivor reports."""
    if mutant.get("status") != "survived":
        return None
    if mutant["file"] in not_judged:
        return "not_judged_here"
    if not risk_survivors([mutant]):
        return "report"
    if changed is None or mutant["line"] in changed.get(mutant["file"], ()):
        return "fail"
    return "report"


def summary_markdown(info, chosen):
    """The job summary: rule, slice, counts, every survivor by verdict, the files left out."""
    lines = [f"## linux / mutation: {info['slice']}", "",
             f"Rule: {info['rule']}", "",
             f"Mutants: {info['generated']} generated, {info['in_slice']} in the slice, {len(chosen)} run; "
             + ", ".join(f"{k} {v}" for k, v in info["counts"].items()), ""]
    titles = (("fail", "Failing: risk-class survivors on lines changed since " + str(info.get("changed_since"))
               if info.get("changed_since") else "Failing: risk-class survivors"),
              ("report", "Reported, not failing: survivors on unchanged lines (and rocketnet_assert survivors)"),
              ("not_judged_here", "Not judged here: survivors in AF_XDP code (Linux reaches it only in a guest; ./bin/test runs none)"))
    for key, title in titles:
        rows = [m for m in chosen if m.get("verdict") == key]
        lines.append(f"### {title}: {len(rows)}")
        for m in rows:
            lines.append(f"- `{m['file']}:{m['line']}` {m['operator']} `{m['original'].strip()}` -> `{m['mutated'].strip()}`")
        lines.append("")
    for key, title in (("excluded", "Excluded: the Linux build does not compile them"),
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
    mutants, offset, prev_code = [], 0, ""
    for number, line in enumerate(text.split("\n"), 1):
        lmask = mask[offset:offset + len(line)]
        lamask = amask[offset:offset + len(line)]
        offset += len(line) + 1

        def code(s, e):
            return all(lmask[s:e])

        def add(op, col, old, new, mutated):
            guarded = any(lamask[col:col + max(1, len(old))]) or old.lstrip().startswith("rocketnet_assert")
            mutants.append({"id": f"{rel}:{number}:{col + 1}:{op}:{new}", "file": rel, "line": number, "column": col + 1,
                            "operator": op, "class": "other" if guarded else mutant_class(op),
                            "original_token": old, "mutated_token": new,
                            "original": line, "mutated": mutated})

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
    # the pass/fail rule on changed lines: a survivor on an unchanged line
    # reports, the same survivor with its line touched fails, a survivor in
    # AF_XDP code is not judged, the original rule fails every risk survivor
    diff = ("diff --git a/plant.cpp b/plant.cpp\n--- a/plant.cpp\n+++ b/plant.cpp\n"
            "@@ -3 +3 @@\n-        a = a + 2;\n+        a = a + 1;\n@@ -9,0 +10,2 @@\n+x\n+y\n"
            "diff --git a/gone.cpp b/gone.cpp\n--- a/gone.cpp\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-a\n-b\n")
    lines = changed_lines(diff)
    if lines != {"plant.cpp": {3, 10, 11}}:
        print(f"self-test FAILED: changed lines read as {lines}")
        return 1
    on2 = dict(next(m for m in first if m["line"] == 2), status="survived")
    on3 = dict(next(m for m in first if m["line"] == 3), status="survived")
    if verdict(on2, lines, {}) != "report" or verdict(on3, lines, {}) != "fail":
        print("self-test FAILED: an unchanged line's survivor did not report or a changed line's did not fail")
        return 1
    if verdict(on2, None, {}) != "fail" or verdict(dict(on2, status="detected"), lines, {}) is not None:
        print("self-test FAILED: the original rule did not fail a risk survivor, or a detected mutant had a verdict")
        return 1
    if verdict(on3, lines, {"plant.cpp": "xdp"}) != "not_judged_here":
        print("self-test FAILED: a survivor in an AF_XDP file was judged")
        return 1
    guard = dict(guards[0], status="survived")
    if verdict(guard, lines, {}) != "report" or verdict(guard, None, {}) != "report":
        print("self-test FAILED: a rocketnet_assert survivor was not reported")
        return 1
    print(f"ok run_mutants self-test: {len(first)} planted mutants, risk-class survival fails the gate, "
          "rocketnet_assert survivors do not, slices partition the list and do not move with line numbers, "
          "--platform linux leaves out Windows sources and their headers and sets AF_XDP apart, "
          "--changed-since fails only survivors on changed lines")
    return 0


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
    p.add_argument("--changed-since", default="",
                   help="REF: a risk-class survivor fails only on a line new or modified in git diff REF HEAD; "
                        "every other survivor is reported (results, --summary-md) and never fails. "
                        "Default: the original rule, every risk-class survivor fails")
    p.add_argument("--summary-md", default="", help="write a Markdown summary of the verdicts here")
    args = p.parse_args()
    if args.self_test:
        return self_test()
    try:
        slice_of = parse_slice(args.slice) if args.slice else None
    except ValueError as error:
        p.error(str(error))
    started = time.monotonic()

    excluded, not_judged = {}, {}
    if args.platform == "linux":
        texts = {}
        for pattern in ("sdk/*", "sdk/*/*", "tests/*", "tests/*/*"):
            for path in ROOT.glob(pattern):
                if path.is_file() and path.suffix in (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"):
                    texts[path.relative_to(ROOT).as_posix()] = path.read_text(errors="replace")
        cmake_text = (ROOT / "cmake" / "RocketnetSources.cmake").read_text()
        args.files, excluded, not_judged = linux_judgement(args.files, cmake_text, texts)
        for f, why in sorted(excluded.items()):
            print(f"excluded (the Linux build does not compile it): {f}: {why}", flush=True)
        for f, why in sorted(not_judged.items()):
            print(f"not judged here (mutated, survivors listed apart, never failing): {f}: {why}", flush=True)
    changed = None
    if args.changed_since:
        diff = subprocess.run(["git", "-C", str(ROOT), "diff", "-U0", "--no-color", "--no-renames", "--no-ext-diff",
                               args.changed_since, "HEAD", "--", *args.files], capture_output=True, text=True)
        if diff.returncode != 0:
            sys.exit(f"git diff {args.changed_since} HEAD failed: {diff.stderr.strip()}")
        changed = changed_lines(diff.stdout)
        print(f"rule: a risk-class survivor fails only on a line changed since {args.changed_since} "
              f"({sum(len(v) for v in changed.values())} new or modified lines in {len(changed)} files); "
              "every other survivor is reported", flush=True)
    else:
        print("rule: every risk-class survivor fails (the original campaign rule)", flush=True)

    mutants = []
    for rel in args.files:
        mutants += generate(rel, (ROOT / rel).read_text())
    generated = len(mutants)
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
    for m in chosen:
        v = verdict(m, changed, not_judged)
        if v:
            m["verdict"] = v
    verdicts = {v: sum(m.get("verdict") == v for m in chosen) for v in ("fail", "report", "not_judged_here")}
    summary = dict(counts, total=len(chosen), generated=generated, in_slice=len(mutants), score=round(score, 4),
                   wall_seconds=round(wall, 1), baseline_test_seconds=round(baseline, 2), timeout_seconds=round(timeout, 2),
                   survivors_failing=verdicts["fail"], survivors_reported=verdicts["report"],
                   survivors_not_judged_here=verdicts["not_judged_here"])
    rule = (f"a risk-class survivor fails only on a line new or modified since {args.changed_since}; every other "
            "survivor is reported" if changed is not None else "every risk-class survivor fails (the original rule)")
    out = Path(args.out) if args.out else Path(args.work_dir) / "results.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"head": head, "files": args.files, "seed": args.seed, "workers": args.workers,
                               "slice": {"index": slice_of[0], "count": slice_of[1]} if slice_of else None,
                               "platform": args.platform, "rule": rule, "changed_since": args.changed_since or None,
                               "excluded_files": excluded, "not_judged_files": not_judged,
                               "summary": summary, "mutants": chosen}, indent=2) + "\n")
    if args.summary_md:
        info = {"slice": f"slice {slice_of[0]}/{slice_of[1]}" if slice_of else "the whole list", "rule": rule,
                "generated": generated, "in_slice": len(mutants), "counts": counts,
                "changed_since": args.changed_since or None, "excluded": excluded, "not_judged": not_judged}
        Path(args.summary_md).parent.mkdir(parents=True, exist_ok=True)
        Path(args.summary_md).write_text(summary_markdown(info, chosen) + "\n")

    print(f"\n{'status':<15} {'file:line':<45} {'operator':<22} original -> mutated")
    for m in chosen:
        print(f"{m['status']:<15} {m['file'] + ':' + str(m['line']):<45} {m['operator']:<22} "
              f"{m['original'].strip()}  ->  {m['mutated'].strip()}")
    for key, title in (("report", "REPORTED (not failing)"), ("not_judged_here", "NOT JUDGED HERE (AF_XDP)"), ("fail", "FAILING")):
        for m in chosen:
            if m.get("verdict") == key:
                print(f"{title}: {m['file']}:{m['line']} {m['operator']} "
                      f"{m['original'].strip()}  ->  {m['mutated'].strip()}")
    print(f"\nsummary: {json.dumps(summary)}\nresults: {out}")
    if verdicts["fail"]:
        print(f"risk-class survivors failing the rule ({rule}): {verdicts['fail']}", flush=True)
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
