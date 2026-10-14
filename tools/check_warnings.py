#!/usr/bin/env python3
"""Clang warning baseline for the sdk.

Compiles each sdk/*.cpp with clang -fsyntax-only and the audit warning set,
plus -ffp-contract=off, using the include paths and defines from the Release
compile commands. Compares the multiset of normalised warnings with
tools/warnings-baseline.txt.

A warning is (file, warning flag, enclosing function or nearest symbol,
normalised message). The line number stays out of the key so a line shift
inside the same symbol does not churn the baseline. The same warning reported
from several translation units counts once per site: the count is the most
one translation unit produces, not the sum, so a header warning is not
repeated for every file that includes it. Two identical warnings inside one
translation unit stay two.

A warning that is new fails the run. A baseline entry the compile no longer
produces also fails it, with the instruction to shrink the baseline: the
baseline only shrinks, and the change that removes a warning deletes that
entry in the same change.

--write-baseline rewrites the baseline from the current compile.
--self-test runs the normaliser and the comparer on sample compiler output
carried in this file. It does not run a compiler, does not read or write a
file, and does not use the clock, a socket, or the network. The compile
itself is the functional part and runs only outside --self-test.
"""

import argparse
import collections
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
COMPILE_DB = os.path.join(BUILD, "compile_commands.json")
BASELINE = os.path.join(ROOT, "tools", "warnings-baseline.txt")

WARNING_FLAGS = [
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wconversion",
    "-Wsign-conversion",
    "-Wcast-align",
    "-Wformat=2",
    "-Wundef",
]

SYNTAX_FLAGS = [
    "-ffp-contract=off",
    "-fsyntax-only",
    "-fno-diagnostics-color",
    "-fno-caret-diagnostics",
    "-fdiagnostics-show-option",
    "-fmessage-length=0",
    # driver flags copied from the build are not source warnings
    "-Qunused-arguments",
]

SHRINK = "shrink the baseline: delete the entry that is no longer produced"

WARNING_RE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?:(?P<col>\d+):)?\s*warning:\s*(?P<msg>.*)$"
)
FLAG_RE = re.compile(r"\s*\[(-W[^\]]+)\]\s*$")
SKIP_NAMES = {
    "if", "while", "for", "switch", "catch", "sizeof", "alignof", "alignas",
    "decltype", "return", "static_assert", "assert", "defined", "typeof",
    "__attribute__", "__declspec",
}


def rel_file(path):
    """Repo-relative path, or None when the path is outside the tree or generated."""
    path = path.strip().replace("\\", "/")
    if path.startswith("./"):
        path = path[2:]
    if os.path.isabs(path):
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        if rel.startswith("../"):
            return None
        path = rel
    if path.startswith("build/") or path.startswith("bin/"):
        return None
    return path


def normalise_message(msg):
    msg = FLAG_RE.sub("", msg).strip()
    return re.sub(r"\s+", " ", msg)


def mask_source(text):
    """Replace comments, strings, and character literals with spaces.

    Newlines stay, so a later scan keeps the original line numbers. A brace
    that lives in a comment or a string is not a scope.
    """
    out = []
    i = 0
    n = len(text)
    state = "code"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                out.append("  ")
                i += 2
                state = "line"
                continue
            if c == "/" and nxt == "*":
                out.append("  ")
                i += 2
                state = "block"
                continue
            if c == '"':
                out.append(" ")
                i += 1
                state = "str"
                continue
            if c == "'":
                out.append(" ")
                i += 1
                state = "chr"
                continue
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                out.append("\n")
                state = "code"
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                out.append("  ")
                i += 2
                state = "code"
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if state == "str":
            if c == "\\" and nxt:
                out.append("  ")
                i += 2
                continue
            if c == '"':
                out.append(" ")
                i += 1
                state = "code"
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if c == "\\" and nxt:
            out.append("  ")
            i += 2
            continue
        if c == "'":
            out.append(" ")
            i += 1
            state = "code"
            continue
        out.append("\n" if c == "\n" else " ")
        i += 1
    return "".join(out)


def function_name(text):
    """The function a brace opens, or None when the brace is not a function."""
    cut = []
    paren = 0
    i = 0
    while i < len(text):
        if text[i:i + 2] == "::" and paren == 0:
            cut.append("::")
            i += 2
            continue
        if text[i] == ":" and paren == 0:
            break
        cut.append(text[i])
        if text[i] == "(":
            paren += 1
        elif text[i] == ")":
            paren = max(0, paren - 1)
        i += 1
    head = "".join(cut)
    names = []
    paren = 0
    last = ""
    token = ""
    for ch in head:
        if ch == "(":
            if paren == 0 and last:
                names.append(last)
            paren += 1
            last = ""
            token = ""
            continue
        if ch == ")":
            paren = max(0, paren - 1)
            last = ""
            token = ""
            continue
        if ch.isalnum() or ch == "_" or ch == "~" or ch == ":":
            token += ch
            if not token.endswith(":"):
                last = token
            continue
        if token and not token.endswith(":"):
            last = token
        token = ""
        if not ch.isspace():
            last = ""
    chosen = None
    for name in names:
        base = name.split("::")[-1]
        if base in SKIP_NAMES or name in SKIP_NAMES:
            continue
        chosen = name
    return chosen


def type_name(text):
    """The struct, class, namespace, enum, or union a brace opens."""
    match = re.search(
        r"\b(enum\s+class|enum\s+struct|struct|class|namespace|enum|union)\b",
        text,
    )
    if not match:
        return None
    rest = text[match.end():]
    rest = re.sub(r"\balignas\s*\([^)]*\)", " ", rest)
    rest = re.sub(r"\b(final|public|private|protected)\b", " ", rest)
    name = re.search(r"([A-Za-z_][A-Za-z0-9_]*)", rest)
    if not name:
        return None
    if name.group(1) in SKIP_NAMES:
        return None
    return name.group(1)


def index_symbols(source):
    """Spans of (start_line, end_line, kind, name) for functions and types."""
    masked = mask_source(source)
    spans = []
    stack = []
    buf = []
    buf_start = None
    depth = 0
    paren = 0
    line = 1

    def reset():
        nonlocal buf, buf_start
        buf = []
        buf_start = None

    def in_function():
        return any(item[0] == "function" for item in stack)

    for ch in masked:
        if ch == "\n":
            line += 1
            buf.append(ch)
            continue
        if ch == "(":
            paren += 1
        elif ch == ")":
            paren = max(0, paren - 1)
        elif ch == ";" and paren == 0:
            # a declaration ends here, including a field before the next method
            reset()
            continue
        if ch == "{":
            text = "".join(buf)
            start = buf_start if buf_start is not None else line
            kind = "block"
            name = None
            if not in_function():
                name = function_name(text)
                if name:
                    kind = "function"
                else:
                    name = type_name(text)
                    if name:
                        kind = "type"
            stack.append((kind, name, depth, start))
            depth += 1
            reset()
            continue
        if ch == "}":
            if depth > 0:
                depth -= 1
            while stack and stack[-1][2] > depth:
                stack.pop()
            if stack and stack[-1][2] == depth:
                kind, name, _depth, start = stack.pop()
                if name:
                    spans.append((start, line, kind, name))
            reset()
            continue
        if buf_start is None and ch not in " \t\r":
            buf_start = line
        buf.append(ch)
    return spans


def symbol_at(spans, line_no):
    best = None
    for start, end, kind, name in spans:
        if not name or line_no < start or line_no > end:
            continue
        width = end - start
        if best is None or width < best[0]:
            best = (width, kind, name)
        elif width == best[0] and kind == "function" and best[1] != "function":
            best = (width, kind, name)
    if best is None:
        return "file-scope"
    return best[2]


def parse_warnings(text, sources):
    """Turn clang diagnostics into a Counter of normalised warning keys.

    sources is either None (read the repo) or a dict of path to source text.
    A dict does not fall through to the disk.
    """
    cache = {}

    def source_for(path):
        if path in cache:
            return cache[path]
        if sources is not None:
            cache[path] = sources.get(path)
            return cache[path]
        full = os.path.join(ROOT, path)
        if not os.path.isfile(full):
            cache[path] = None
            return None
        with open(full, "r", encoding="utf-8", errors="replace") as handle:
            cache[path] = handle.read()
        return cache[path]

    span_cache = {}
    counts = collections.Counter()
    for raw in text.splitlines():
        match = WARNING_RE.match(raw.strip())
        if not match:
            continue
        path = rel_file(match.group("path"))
        if path is None:
            continue
        flag_match = FLAG_RE.search(match.group("msg"))
        flag = flag_match.group(1) if flag_match else "unflagged"
        message = normalise_message(match.group("msg"))
        if path not in span_cache:
            source = source_for(path)
            span_cache[path] = index_symbols(source) if source is not None else []
        symbol = symbol_at(span_cache[path], int(match.group("line")))
        counts[(path, flag, symbol, message)] += 1
    return counts


def merge_counts(per_file):
    """Keep the highest count of each key across translation units."""
    merged = collections.Counter()
    for counts in per_file:
        for key, count in counts.items():
            if count > merged[key]:
                merged[key] = count
    return merged


def read_baseline(path):
    if not os.path.exists(path):
        return None
    counts = collections.Counter()
    with open(path, "r", encoding="utf-8") as handle:
        for line_no, raw in enumerate(handle, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 4:
                raise SystemExit(
                    "baseline %s:%d is not four columns" % (path, line_no)
                )
            counts[tuple(parts)] += 1
    return counts


def format_entry(key):
    return "%s %s %s %s" % key


def diff_lines(current, baseline):
    """Return printed new-warning lines and shrink lines."""
    new_lines = []
    gone_lines = []
    for key in sorted(set(current) | set(baseline)):
        extra = current.get(key, 0) - baseline.get(key, 0)
        if extra > 0:
            for _ in range(extra):
                new_lines.append("new warning: " + format_entry(key))
        elif extra < 0:
            for _ in range(-extra):
                gone_lines.append(SHRINK + ": " + format_entry(key))
    return new_lines, gone_lines


def write_baseline(path, counts, version):
    lines = [
        "# " + version,
        "# columns: file, warning flag, enclosing function, normalised message",
        "# the baseline only shrinks: delete an entry in the same change that stops producing it",
    ]
    for key in sorted(counts):
        row = "\t".join(key)
        for _ in range(counts[key]):
            lines.append(row)
    text = "\n".join(lines) + "\n"
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    os.replace(tmp, path)


def compiler_version(compiler):
    result = subprocess.run(
        [compiler, "--version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    first = result.stdout.splitlines()[0] if result.stdout else ""
    match = re.search(r"clang version [0-9]+(?:\.[0-9]+)*", first)
    if match:
        return match.group(0)
    return "clang version unknown"


def ensure_compile_db():
    if os.path.exists(COMPILE_DB):
        return
    print("configuring build/ so the warning gate can use its compile commands")
    subprocess.run(
        [
            "cmake", "-B", BUILD,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        cwd=ROOT,
        check=True,
    )
    if not os.path.exists(COMPILE_DB):
        raise SystemExit("cmake did not write " + COMPILE_DB)


def load_compile_db():
    import json
    with open(COMPILE_DB, "r", encoding="utf-8") as handle:
        return json.load(handle)


def entry_arguments(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def command_rank(args):
    text = " ".join(args)
    score = 0
    if "-DROCKETNET_DEVELOPMENT=0" in text:
        score += 8
    if "-DROCKETNET_DEVELOPMENT=1" in text:
        score -= 4
    if "-DROCKETNET_SHARED" in text or "-DROCKETNET_EXPORT" in text:
        score -= 2
    if "ROCKETNET_XDP_TESTS" in text or "ROCKETNET_IOCP_TESTS" in text:
        score -= 6
    return score


def choose_commands():
    """One Release production command per sdk/*.cpp."""
    chosen = {}
    directories = {}
    for entry in load_compile_db():
        rel = rel_file(entry.get("file", ""))
        if rel is None or not rel.startswith("sdk/") or not rel.endswith(".cpp"):
            continue
        args = entry_arguments(entry)
        if rel not in chosen or command_rank(args) > command_rank(chosen[rel]):
            chosen[rel] = args
            directories[rel] = entry.get("directory", ROOT)
    missing = []
    sdk = os.path.join(ROOT, "sdk")
    for name in sorted(os.listdir(sdk)):
        if not name.endswith(".cpp"):
            continue
        rel = "sdk/" + name
        if rel not in chosen:
            missing.append(rel)
    if missing:
        raise SystemExit("no compile command for " + ", ".join(missing))
    return chosen, directories


def is_source_arg(arg, source, directory):
    if arg == source:
        return True
    candidates = [arg]
    if not os.path.isabs(arg):
        candidates.append(os.path.join(directory, arg))
    source_abs = os.path.abspath(source)
    for candidate in candidates:
        if os.path.abspath(candidate) == source_abs:
            return True
    return False


def rewrite_command(args, source, directory):
    """Clang syntax-only, with the audit flags and -ffp-contract=off."""
    compiler = "clang++" if source.endswith(".cpp") else "clang"
    out = [compiler]
    skip = False
    for arg in args[1:]:
        if skip:
            skip = False
            continue
        if arg == "-c" or arg == "-o":
            if arg == "-o":
                skip = True
            continue
        if arg == "-Werror" or arg.startswith("-Werror="):
            continue
        if is_source_arg(arg, source, directory):
            continue
        out.append(arg)
    out.extend(WARNING_FLAGS)
    out.extend(SYNTAX_FLAGS)
    out.append(source)
    return out


def compile_warnings():
    ensure_compile_db()
    chosen, directories = choose_commands()
    per_file = []
    failures = []
    for rel in sorted(chosen):
        source = os.path.join(ROOT, rel)
        command = rewrite_command(chosen[rel], source, directories[rel])
        print("checking " + rel, file=sys.stderr)
        result = subprocess.run(
            command,
            cwd=directories[rel],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        text = result.stdout + "\n" + result.stderr
        if result.returncode != 0:
            failures.append(rel)
            sys.stderr.write(text)
            continue
        per_file.append(parse_warnings(text, None))
    if failures:
        raise SystemExit("compile failed: " + ", ".join(failures))
    return merge_counts(per_file)


def run_gate(baseline_path, write):
    version = compiler_version("clang++")
    counts = compile_warnings()
    if write:
        write_baseline(baseline_path, counts, version)
        print("baseline %s: %d warnings (%s)" % (baseline_path, sum(counts.values()), version))
        return 0
    baseline = read_baseline(baseline_path)
    if baseline is None:
        print("no baseline at %s; run with --write-baseline" % baseline_path, file=sys.stderr)
        return 1
    new_lines, gone_lines = diff_lines(counts, baseline)
    print("warning gate: %d current, %d baseline" % (sum(counts.values()), sum(baseline.values())))
    for line in new_lines:
        print(line)
    for line in gone_lines:
        print(line)
    if new_lines or gone_lines:
        print("warning gate FAILED")
        return 1
    print("warning gate passed")
    return 0


SAMPLE_SOURCE = """\
int kept(int value)
{
    const char * text = "brace { not a function";
    int other = value;
    return other;
}

struct beta
{
    int field;
    int method(int value)
        : field(value)
    {
        int copy = value;
        return copy;
    }
};

int looped(int value)
{
    int kept = value;
    for ( int i = 0; i < kept; i++ )
    {
        kept = i;
    }
    return kept;
}
"""

SAMPLE_DIAG = """\
sample.cpp:4:9: warning:   declaration   of 'other' shadows nothing   [-Wshadow]
sample.cpp:3:11: note: previous declaration is here
sample.cpp:4:9: warning: declaration of 'other' shadows nothing [-Wshadow]
/usr/include/stdio.h:1:1: warning: ignore a system header [-Wunused-value]
sample.cpp:10:9: warning: unused field [-Wunused-private-field]
sample.cpp:15:13: warning: conversion inside the method [-Wconversion]
sample.cpp:24:9: warning: assignment inside the loop [-Wconversion]
"""


def run_self_test():
    failures = []

    def check(cond, message):
        if not cond:
            failures.append(message)

    spans = index_symbols(SAMPLE_SOURCE)
    check(symbol_at(spans, 4) == "kept", "line 4 symbol is %s" % symbol_at(spans, 4))
    check(symbol_at(spans, 10) == "beta", "line 10 symbol is %s" % symbol_at(spans, 10))
    check(symbol_at(spans, 15) == "method", "line 15 symbol is %s" % symbol_at(spans, 15))
    check(symbol_at(spans, 24) == "looped", "line 24 symbol is %s" % symbol_at(spans, 24))

    counts = parse_warnings(SAMPLE_DIAG, {"sample.cpp": SAMPLE_SOURCE})
    shadow = ("sample.cpp", "-Wshadow", "kept", "declaration of 'other' shadows nothing")
    field = ("sample.cpp", "-Wunused-private-field", "beta", "unused field")
    method = ("sample.cpp", "-Wconversion", "method", "conversion inside the method")
    loop = ("sample.cpp", "-Wconversion", "looped", "assignment inside the loop")
    check(counts[shadow] == 2, "shadow count is %d" % counts[shadow])
    check(field in counts, "field warning missing")
    check(method in counts, "method warning missing")
    check(loop in counts, "loop warning missing")
    check(sum(counts.values()) == 5, "system header or note was counted: %s" % (counts,))

    new_lines, gone_lines = diff_lines(counts, counts)
    check(new_lines == [] and gone_lines == [], "equal multisets must pass")

    extra = collections.Counter(counts)
    extra[shadow] += 1
    new_lines, gone_lines = diff_lines(extra, counts)
    check(len(new_lines) == 1 and "-Wshadow" in new_lines[0] and new_lines[0].startswith("new warning:"),
          "added shadow did not name -Wshadow: %s" % (new_lines,))
    check(gone_lines == [], "added shadow must not ask to shrink")

    missing = collections.Counter(counts)
    missing[shadow] -= 1
    new_lines, gone_lines = diff_lines(missing, counts)
    check(len(gone_lines) == 1 and gone_lines[0].startswith(SHRINK),
          "missing entry did not print the shrink instruction: %s" % (gone_lines,))
    check(new_lines == [], "a missing entry is not a new warning")

    merged = merge_counts([
        collections.Counter({shadow: 2, field: 1}),
        collections.Counter({shadow: 1, field: 1}),
    ])
    check(merged[shadow] == 2, "header-style merge must keep the per-unit count")

    shown_new = diff_lines(extra, counts)[0]
    shown_gone = diff_lines(missing, counts)[1]
    if failures:
        print("self-test failed:")
        for message in failures:
            print("  " + message)
        return 1
    print("self-test " + shown_new[0])
    print("self-test " + shown_gone[0])
    print("self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="check the normaliser and comparer on in-script sample output")
    parser.add_argument("--write-baseline", action="store_true",
                        help="rewrite the baseline from the current clang compile")
    parser.add_argument("--baseline", default=BASELINE,
                        help="baseline file to compare against")
    args = parser.parse_args()
    if args.self_test:
        return run_self_test()
    return run_gate(args.baseline, args.write_baseline)


if __name__ == "__main__":
    sys.exit(main())
