#!/usr/bin/env python3
"""Refuse thread-local storage in a production (ROCKETNET_DEV=0) build of sdk/.

Policy: a production build allocates nothing from the operating system at
runtime. On macOS a thread's first touch of any thread-local variable makes
dyld's tlv_get_addr allocate the image's whole thread-local block for that
thread, so one thread-local anywhere in the library costs every network
thread an allocation; on Windows and Linux the loader sizes and allocates a
block per thread for every module that defines one. Development counters and
test seams may be thread-local only inside a region a production build
compiles out.

Three checks, each refusing what the others cannot see:

  source       (default) every C and C++ file under sdk/, comments and
               literals removed and line continuations joined, read through
               its preprocessor conditionals. A thread_local, _Thread_local,
               __thread or __declspec( thread ) token passes only where an
               enclosing branch is false when ROCKETNET_DEV is 0, such as
               #if ROCKETNET_DEV, #if ROCKETNET_DEV && ..., or the #else of
               #if !ROCKETNET_DEV. Every platform's half is read, Windows
               included, whatever the host.

  --preprocess compile_commands.json
               every sdk translation unit of that build run through its own
               compiler with -E and ROCKETNET_DEV forced to 0; a storage token
               whose line marker names a file under sdk/ fails. This is the
               compiler's own reading of this host's conditionals and macros.

  --objects FILE...
               the built objects or archives carry no thread-local section or
               reference: on macOS no __thread_vars, __thread_data or
               __thread_bss section (otool -l) and no __tlv_bootstrap symbol
               (nm); elsewhere no .tdata or .tbss section and no TLS symbol
               (readelf).

Usage:
  check_no_thread_local.py                         the source check
  check_no_thread_local.py --preprocess build/compile_commands.json
  check_no_thread_local.py --objects build/librocketnet.a
  check_no_thread_local.py --self-test             the red and green controls
"""

import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / "sdk"
SUFFIXES = (".c", ".h", ".cpp", ".cc", ".hpp", ".inc")

STORAGE = re.compile(r"\b(?:thread_local|_Thread_local|__thread)\b|__declspec\s*\(\s*thread\s*\)")

LITERALS = re.compile(
    r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*'"
    r"|/\*.*?\*/|//[^\n]*",
    re.DOTALL,
)


def blank(text):
    """Remove comments and literals, keeping every newline in place."""
    return LITERALS.sub(lambda m: "".join("\n" if c == "\n" else " " for c in m.group()), text)


def logical_lines(text):
    """Join backslash continuations; yield (first physical line number, text)."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        start = i
        line = lines[i]
        while line.endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line[:-1] + " " + lines[i]
        yield start + 1, line
        i += 1


# Kleene three-valued truth of an #if expression with ROCKETNET_DEV at 0:
# False, True or None (unknown, it depends on something else).

EXPR_TOKEN = re.compile(r"\s*(&&|\|\||!(?!=)|\(|\)|[^\s()&|!]+|!=|[&|])")


def tokenize_expr(expr):
    out = []
    pos = 0
    while pos < len(expr):
        m = EXPR_TOKEN.match(expr, pos)
        if not m or m.end() == pos:
            break
        out.append(m.group(1))
        pos = m.end()
    return out


class Expr:
    def __init__(self, tokens):
        self.t = tokens
        self.i = 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def parse_or(self):
        value = self.parse_and()
        while self.peek() == "||":
            self.take()
            rhs = self.parse_and()
            if value is True or rhs is True:
                value = True
            elif value is False and rhs is False:
                value = False
            else:
                value = None
        return value

    def parse_and(self):
        value = self.parse_unary()
        while self.peek() == "&&":
            self.take()
            rhs = self.parse_unary()
            if value is False or rhs is False:
                value = False
            elif value is True and rhs is True:
                value = True
            else:
                value = None
        return value

    def parse_unary(self):
        if self.peek() == "!":
            self.take()
            value = self.parse_unary()
            return None if value is None else not value
        return self.parse_atom()

    def parse_atom(self):
        tok = self.peek()
        if tok == "(":
            self.take()
            value = self.parse_or()
            if self.peek() == ")":
                self.take()
            return self.relation(value, None)
        if tok == "defined":
            self.take()
            if self.peek() == "(":
                self.take()
                name = self.take()
                if self.peek() == ")":
                    self.take()
            else:
                name = self.take()
            return self.relation(True if name == "ROCKETNET_DEV" else None, None)
        self.take()
        if tok == "ROCKETNET_DEV":
            return self.relation(False, 0)
        if tok is not None and re.fullmatch(r"[0-9]+[uUlL]*", tok):
            return self.relation(int(re.sub(r"[uUlL]", "", tok)) != 0, int(re.sub(r"[uUlL]", "", tok)))
        return self.relation(None, None)

    def relation(self, truth, number):
        # a comparison after an atom: only ROCKETNET_DEV == / != a number is known
        op = self.peek()
        if op in ("==", "!=", "<", ">", "<=", ">="):
            self.take()
            rhs = self.take()
            if number is not None and rhs is not None and re.fullmatch(r"[0-9]+", rhs):
                r = int(rhs)
                return {"==": number == r, "!=": number != r, "<": number < r,
                        ">": number > r, "<=": number <= r, ">=": number >= r}[op]
            return None
        return truth


def production_truth(expr):
    tokens = tokenize_expr(expr)
    if not tokens:
        return None
    return Expr(tokens).parse_or()


DIRECTIVE = re.compile(r"^\s*#\s*(\w+)(.*)$")


def scan_text(text):
    """Return [(line, token)] for storage tokens a production build compiles."""
    findings = []
    # each frame: [dead_in_production_for_this_branch, any_prior_branch_definitely_true,
    #              all_prior_branches_definitely_false]
    stack = []
    for number, line in logical_lines(blank(text)):
        m = DIRECTIVE.match(line)
        if m:
            name, rest = m.group(1), m.group(2)
            if name in ("if", "ifdef", "ifndef"):
                if name == "if":
                    truth = production_truth(rest)
                elif name == "ifdef":
                    truth = True if rest.strip() == "ROCKETNET_DEV" else None
                else:
                    truth = False if rest.strip() == "ROCKETNET_DEV" else None
                stack.append([truth is False, truth is True, truth is False])
                continue
            if name == "elif" and stack:
                frame = stack[-1]
                truth = production_truth(rest)
                if frame[1]:
                    frame[0] = True
                else:
                    frame[0] = truth is False
                frame[1] = frame[1] or truth is True
                frame[2] = frame[2] and truth is False
                continue
            if name == "else" and stack:
                frame = stack[-1]
                frame[0] = frame[1]
                frame[2] = False
                continue
            if name == "endif" and stack:
                stack.pop()
                continue
        if any(frame[0] for frame in stack):
            continue
        for match in STORAGE.finditer(line):
            findings.append((number, match.group(0)))
    return findings


def source_check():
    failures = []
    for path in sorted(SDK.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line, token in scan_text(text):
            failures.append("%s:%d: %s outside a ROCKETNET_DEV region" % (path.relative_to(ROOT), line, token))
    return failures


LINE_MARKER = re.compile(r'^#\s*(?:line\s+)?\d+\s+"([^"]*)"')


def preprocess_unit(job):
    """Preprocess one sdk translation unit at ROCKETNET_DEV=0; return its failures."""
    entry, sdk = job
    sdk = Path(sdk)
    source = Path(entry["directory"], entry["file"]).resolve()
    argv = entry.get("arguments") or shlex.split(entry["command"])
    command = []
    skip = False
    for arg in argv:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if arg in ("-c", "-MD", "-MMD") or arg.startswith("-o") and len(arg) > 2 or arg.startswith("-DROCKETNET_DEV"):
            continue
        command.append(arg)
    command[1:1] = ["-E", "-DROCKETNET_DEV=0"]
    result = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
    if result.returncode != 0:
        return ["%s: the preprocessor failed: %s" % (source.relative_to(sdk.parent), result.stderr.strip()[:400])]
    failures = []
    current = None
    current_line = 0
    for raw in result.stdout.split("\n"):
        marker = LINE_MARKER.match(raw.strip())
        if marker:
            current = Path(entry["directory"], marker.group(1)).resolve()
            current_line = int(re.search(r"\d+", raw).group(0)) - 1
            continue
        current_line += 1
        if current is None:
            continue
        try:
            relative = current.relative_to(sdk)
        except ValueError:
            continue
        for match in STORAGE.finditer(blank(raw)):
            failures.append("sdk/%s:%d: %s in the ROCKETNET_DEV=0 preprocessed %s" %
                            (relative, current_line, match.group(0), source.relative_to(sdk.parent)))
    return failures


def preprocess_check(database):
    """Every sdk translation unit, one compiler per core."""
    entries = json.loads(Path(database).read_text())
    units = {}
    for entry in entries:
        source = Path(entry["directory"], entry["file"]).resolve()
        try:
            source.relative_to(SDK)
        except ValueError:
            continue
        units.setdefault(source, entry)
    if not units:
        return ["%s names no sdk translation unit" % database]
    failures = []
    jobs = [(entry, str(SDK)) for entry in units.values()]
    with ProcessPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for unit_failures in pool.map(preprocess_unit, jobs):
            failures.extend(unit_failures)
    return failures


def objects_check(paths):
    failures = []
    darwin = sys.platform == "darwin"
    for path in paths:
        if not os.path.exists(path):
            failures.append("%s: missing" % path)
            continue
        if darwin:
            sections = subprocess.run(["otool", "-l", path], capture_output=True, text=True)
            if sections.returncode != 0:
                failures.append("%s: otool -l failed: %s" % (path, sections.stderr.strip()))
                continue
            member = path
            for line in sections.stdout.splitlines():
                if line.startswith(path + "("):
                    member = line.rstrip(":")
                m = re.match(r"\s*sectname (__thread_vars|__thread_data|__thread_bss)\s*$", line)
                if m:
                    failures.append("%s: section %s" % (member, m.group(1)))
            symbols = subprocess.run(["nm", "-A", path], capture_output=True, text=True)
            for line in symbols.stdout.splitlines():
                if "__tlv_bootstrap" in line or "$tlv$init" in line:
                    failures.append(line.strip())
        else:
            sections = subprocess.run(["readelf", "-W", "-S", path], capture_output=True, text=True)
            if sections.returncode != 0:
                failures.append("%s: readelf -S failed: %s" % (path, sections.stderr.strip()))
                continue
            member = path
            for line in sections.stdout.splitlines():
                if line.startswith("File: "):
                    member = line[6:].strip()
                m = re.search(r"\]\s+(\.tdata|\.tbss)\S*\s", line)
                if m:
                    failures.append("%s: section %s" % (member, m.group(1)))
            symbols = subprocess.run(["readelf", "-W", "-s", path], capture_output=True, text=True)
            member = path
            for line in symbols.stdout.splitlines():
                if line.startswith("File: "):
                    member = line[6:].strip()
                fields = line.split()
                if len(fields) >= 8 and fields[3] == "TLS":
                    failures.append("%s: TLS symbol %s" % (member, fields[7]))
    return failures


# --- the red and green controls ---------------------------------------------

CONTROLS = [
    ("plain definition", "static thread_local int x;\n", 1),
    ("C11 spelling", "_Thread_local int x;\n", 1),
    ("gcc spelling", "static __thread int x;\n", 1),
    ("msvc spelling", "__declspec( thread ) int x;\n", 1),
    ("extern declaration", "extern thread_local int x;\n", 1),
    ("inside #if ROCKETNET_DEV", "#if ROCKETNET_DEV\nthread_local int x;\n#endif\n", 0),
    ("inside #if ROCKETNET_DEV && platform", "#if ROCKETNET_DEV && ROCKETNET_PLATFORM == 3\nthread_local int x;\n#endif\n", 0),
    ("inside defined( ROCKETNET_DEV ) && ROCKETNET_DEV", "#if defined( ROCKETNET_DEV ) && ROCKETNET_DEV\nthread_local int x;\n#endif\n", 0),
    ("nested under a DEV region", "#if ROCKETNET_DEV\n#if FOO\nthread_local int x;\n#endif\n#endif\n", 0),
    ("#else of #if ROCKETNET_DEV", "#if ROCKETNET_DEV\nint y;\n#else\nthread_local int x;\n#endif\n", 1),
    ("#if !ROCKETNET_DEV", "#if !ROCKETNET_DEV\nthread_local int x;\n#endif\n", 1),
    ("#else of #if !ROCKETNET_DEV", "#if !ROCKETNET_DEV\nint y;\n#else\nthread_local int x;\n#endif\n", 0),
    ("ROCKETNET_DEV || other", "#if ROCKETNET_DEV || FOO\nthread_local int x;\n#endif\n", 1),
    ("#elif after a DEV branch", "#if ROCKETNET_DEV\nint y;\n#elif FOO\nthread_local int x;\n#endif\n", 1),
    ("#elif ROCKETNET_DEV", "#if FOO\nint y;\n#elif ROCKETNET_DEV\nthread_local int x;\n#endif\n", 0),
    ("ROCKETNET_DEV == 1", "#if ROCKETNET_DEV == 1\nthread_local int x;\n#endif\n", 0),
    ("ROCKETNET_DEV != 0", "#if ROCKETNET_DEV != 0\nthread_local int x;\n#endif\n", 0),
    ("ROCKETNET_DEV == 0", "#if ROCKETNET_DEV == 0\nthread_local int x;\n#endif\n", 1),
    ("platform region only", "#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS\nthread_local int x;\n#endif\n", 1),
    ("#ifdef ROCKETNET_DEV is not a DEV region", "#ifdef ROCKETNET_DEV\nthread_local int x;\n#endif\n", 1),
    ("after the DEV region ends", "#if ROCKETNET_DEV\n#endif\nthread_local int x;\n", 1),
    ("a macro that expands to it", "#define TLS thread_local\nTLS int x;\n", 1),
    ("continued directive", "#if ROCKETNET_DEV && \\\n    FOO\nthread_local int x;\n#endif\n", 0),
    ("in a line comment", "// thread_local int x;\n", 0),
    ("in a block comment", "/* thread_local\n int x; */\n", 0),
    ("in a string", "const char * s = \"thread_local\";\n", 0),
    ("a longer identifier", "int thread_locals; int my_thread_local;\n", 0),
]


def self_test():
    failed = 0
    for name, text, expected in CONTROLS:
        got = len(scan_text(text))
        verdict = "ok" if got == expected else "FAIL"
        if got != expected:
            failed += 1
        print("self-test %-48s expected %d finding(s), got %d: %s" % (name, expected, got, verdict))

    # the preprocessor check reads its own controls through the host compiler
    compiler = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as scratch:
        sdk = Path(scratch)
        cases = [
            ("dev_only.cpp", "#if ROCKETNET_DEV\nthread_local int x;\n#endif\n", 0),
            ("production.cpp", "static thread_local int x;\nint f() { return x; }\n", 1),
            ("macro.cpp", "#define TLS thread_local\nstatic TLS int x;\nint f() { return x; }\n", 1),
            ("commented.cpp", "// thread_local\nint y;\n", 0),
        ]
        global SDK
        saved = SDK
        SDK = sdk.resolve()
        try:
            for file_name, text, expected in cases:
                (sdk / file_name).write_text(text)
                database = sdk / (file_name + ".json")
                database.write_text(json.dumps([{
                    "directory": str(sdk),
                    "file": file_name,
                    "arguments": [compiler, "-std=c++17", "-DROCKETNET_DEV=1", "-c", file_name, "-o", file_name + ".o"],
                }]))
                got = len(preprocess_check(str(database)))
                verdict = "ok" if got == expected else "FAIL"
                if got != expected:
                    failed += 1
                print("self-test preprocess %-38s expected %d finding(s), got %d: %s" % (file_name, expected, got, verdict))

            # the object check: a production object and a thread-local one
            for file_name, text, expected in [
                ("object_clean.cpp", "int y; int f() { return y; }\n", 0),
                ("object_tls.cpp", "static thread_local int x; int f() { return ++x; }\n", 1),
            ]:
                (sdk / file_name).write_text(text)
                obj = sdk / (file_name + ".o")
                built = subprocess.run([compiler, "-std=c++17", "-O2", "-c", str(sdk / file_name), "-o", str(obj)],
                                       capture_output=True, text=True)
                if built.returncode != 0:
                    print("self-test objects %s: the compiler failed: %s" % (file_name, built.stderr.strip()))
                    failed += 1
                    continue
                got = 1 if objects_check([str(obj)]) else 0
                verdict = "ok" if got == expected else "FAIL"
                if got != expected:
                    failed += 1
                print("self-test objects %-41s expected %s, got %s: %s" %
                      (file_name, "red" if expected else "green", "red" if got else "green", verdict))
        finally:
            SDK = saved

    if failed:
        print("SELF-TEST FAILED %d" % failed)
        return 1
    print("SELF-TEST OK")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) == 3 and argv[1] == "--preprocess":
        failures = preprocess_check(argv[2])
        what = "preprocess"
    elif len(argv) >= 3 and argv[1] == "--objects":
        failures = objects_check(argv[2:])
        what = "objects"
    elif len(argv) == 1:
        failures = source_check()
        what = "source"
    else:
        print(__doc__)
        return 2
    for failure in failures:
        print("FAIL check_no_thread_local (%s): %s" % (what, failure))
    if failures:
        print("THREAD-LOCAL %s FAILED %d" % (what.upper(), len(failures)))
        return 1
    print("THREAD-LOCAL %s OK" % what.upper())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
