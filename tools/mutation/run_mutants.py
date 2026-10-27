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

Python 3 standard library only. Example:

  python3 tools/mutation/run_mutants.py --files sdk/rocketnet_ack.cpp \
      --max-mutants 40 --workers 4
"""

import argparse
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
    "sdk/rocketnet_fragment.cpp",
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
    mutants, offset, prev_code = [], 0, ""
    for number, line in enumerate(text.split("\n"), 1):
        lmask = mask[offset:offset + len(line)]
        offset += len(line) + 1

        def code(s, e):
            return all(lmask[s:e])

        def add(op, col, old, new, mutated):
            mutants.append({"id": f"{rel}:{number}:{col + 1}:{op}:{new}", "file": rel, "line": number, "column": col + 1,
                            "operator": op, "original_token": old, "mutated_token": new,
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
            value = int(digits, 0)
            for new_value in (value + 1, value - 1):
                if new_value < 0 or new_value >= 1 << 64:
                    continue
                new = (hex(new_value) if digits.lower().startswith("0x") else str(new_value)) + suffix
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


def run(cmd, cwd, timeout, env=None):
    """Run cmd in its own process group; return (code or None on timeout, seconds, output tail)."""
    start = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=timeout)
        code = proc.returncode
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        out, _ = proc.communicate()
        code = None
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


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--files", nargs="+", default=DEFAULT_FILES, help="repository-relative sources to mutate")
    p.add_argument("--workers", type=int, default=4, help="parallel trees (default 4)")
    p.add_argument("--max-mutants", type=int, default=0, help="sample this many (0: all)")
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
    args = p.parse_args()
    started = time.monotonic()

    mutants = []
    for rel in args.files:
        mutants += generate(rel, (ROOT / rel).read_text())
    if args.list:
        for m in mutants:
            print(f"{m['id']}\t{m['mutated'].strip()}")
        return 0
    by_id = {m["id"]: m for m in mutants}
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
        chosen.sort(key=mutants.index)
    print(f"{len(mutants)} mutants generated, {len(chosen)} chosen, {args.workers} workers", flush=True)

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
    summary = dict(counts, total=len(chosen), generated=len(mutants), score=round(score, 4), wall_seconds=round(wall, 1),
                   baseline_test_seconds=round(baseline, 2), timeout_seconds=round(timeout, 2))
    out = Path(args.out) if args.out else Path(args.work_dir) / "results.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"head": head, "files": args.files, "seed": args.seed, "workers": args.workers,
                               "summary": summary, "mutants": chosen}, indent=2) + "\n")

    print(f"\n{'status':<15} {'file:line':<45} {'operator':<22} original -> mutated")
    for m in chosen:
        print(f"{m['status']:<15} {m['file'] + ':' + str(m['line']):<45} {m['operator']:<22} "
              f"{m['original'].strip()}  ->  {m['mutated'].strip()}")
    print(f"\nsummary: {json.dumps(summary)}\nresults: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
