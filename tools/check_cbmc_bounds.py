#!/usr/bin/env python3
"""Check that the bounds the CBMC records state match the SDK.

The inventory, results and harness comments under proofs/cbmc state a bound
tied to an SDK constant as the constant's expression followed by its value in
parentheses: `ROCKETNET_RECEIVE_CEILING + 1` (1453) in Markdown, or
ROCKETNET_RECEIVE_PAYLOAD_BYTES (1416) in a C or C++ comment (.c, .cpp, .h);
or as the value with
the expression after it in parentheses: 1452 (`ROCKETNET_RECEIVE_CEILING`),
[0, 1416] (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`). This compiles a probe
against sdk/ that prints every such expression and fails, naming file:line,
when a stated value differs from the SDK's, so a changed constant cannot
leave the records stating a bound the harnesses no longer run at. It also
fails when it finds no statement at all.

--self-test plants a stale statement and requires the check to refuse it,
including one in a .cpp harness comment found by the record walk.
Prints one OK line per file with statements; exits 1 on a refusal.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROOFS = ROOT / "proofs" / "cbmc"
SDK = ROOT / "sdk"

# the headers that define the constants the records name
HEADERS = [
    "rocketnet.h",
    "rocketnet_layers.h",
    "rocketnet_wire.h",
    "rocketnet_protocol.h",
    "rocketnet_message.h",
    "rocketnet_history.h",
    "rocketnet_constants.h",
    "rocketnet_connect_token.h",
]

NAME = r"ROCKETNET_[A-Z0-9_]+"
TERM = rf"(?:{NAME}|\d+)"
EXPRESSION = rf"{NAME}(?:\s*[-+*/]\s*{TERM})*"
NAME_FIRST = re.compile(rf"`?({EXPRESSION})`?\s*\(`?(\d+)`?\)")
VALUE_FIRST = re.compile(rf"\b(\d+)\]?`?\s*\(`?({EXPRESSION})`?\)")


def statements(path):
    found = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        for match in NAME_FIRST.finditer(line):
            found.append((number, match[1], int(match[2])))
        for match in VALUE_FIRST.finditer(line):
            found.append((number, match[2], int(match[1])))
    return found


def evaluate(expressions):
    """The SDK value of each expression, or None when it names a constant no
    listed header defines."""
    names = sorted({name for expression in expressions for name in re.findall(NAME, expression)})
    lines = [f'#include "{header}"' for header in HEADERS] + ["#include <stdio.h>", "int main( void )", "{"]
    for index, expression in enumerate(expressions):
        used = sorted(set(re.findall(NAME, expression)))
        lines.append("#if " + " && ".join(f"defined( {name} )" for name in used))
        lines.append(f'    printf( "%d %lld\\n", {index}, (long long) ( {expression} ) );')
        lines.append("#else")
        lines.append(f'    printf( "%d undefined\\n", {index} );')
        lines.append("#endif")
    lines += ["    return 0;", "}", ""]
    compiler = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory(prefix="cbmc_bounds_") as directory:
        source = Path(directory) / "probe.cpp"
        binary = Path(directory) / "probe"
        source.write_text("\n".join(lines), encoding="utf-8")
        result = subprocess.run([compiler, "-std=c++17", "-ffp-contract=off", f"-I{SDK}", str(source), "-o", str(binary)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"FAIL: the probe over {', '.join(names)} did not compile:\n{result.stderr}")
        output = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout
    values = {}
    for line in output.splitlines():
        index, value = line.split()
        values[int(index)] = None if value == "undefined" else int(value)
    return [values[index] for index in range(len(expressions))]


# every kind of record file under proofs/cbmc: the Markdown records and the
# C and C++ harnesses and their headers (a .cpp harness left out is a stated
# bound nobody checks)
RECORD_PATTERNS = ("**/*.md", "**/*.c", "**/*.cpp", "**/*.h")


def collect(proofs):
    return sorted({path for pattern in RECORD_PATTERNS for path in Path(proofs).glob(pattern)})


def check(files):
    found = [(path, number, expression, stated) for path in files for number, expression, stated in statements(path)]
    if not found:
        print(f"FAIL: 0 bound statements found in {len(files)} record files: nothing was checked", file=sys.stderr)
        return False
    values = evaluate([expression for _, _, expression, _ in found])
    ok = True
    for (path, number, expression, stated), value in zip(found, values):
        where = f"{path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}:{number}"
        if value is None:
            print(f"FAIL {where}: {expression} is not defined by {', '.join(HEADERS)}", file=sys.stderr)
            ok = False
        elif value != stated:
            print(f"FAIL {where}: {expression} is {value} in the SDK, stated ({stated})", file=sys.stderr)
            ok = False
    if ok:
        for path in sorted({path for path, _, _, _ in found}):
            count = sum(1 for p, _, _, _ in found if p == path)
            print(f"OK {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path} ({count})")
    return ok


def self_test():
    with tempfile.TemporaryDirectory(prefix="cbmc_bounds_self_test_") as directory:
        planted = Path(directory) / "planted.md"
        for text in ("payload 0..`ROCKETNET_RECEIVE_PAYLOAD_BYTES` (1439)\n",
                     "`bytes` in `[0, 1439]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`)\n"):
            planted.write_text(text, encoding="utf-8")
            if check([planted]):
                print(f"FAIL: a stale statement passed the check: {text.strip()}", file=sys.stderr)
                return False
        tree = Path(directory) / "tree" / "sub"
        tree.mkdir(parents=True)
        for name in ("harness.c", "harness.cpp", "harness.h", "notes.md"):
            (tree / name).write_text("// ROCKETNET_RECEIVE_PAYLOAD_BYTES (1)\n", encoding="utf-8")
        found = {path.name for path in collect(tree.parent)}
        if found != {"harness.c", "harness.cpp", "harness.h", "notes.md"}:
            print(f"FAIL: the record walk read {sorted(found)}, not every .md, .c, .cpp and .h", file=sys.stderr)
            return False
        # a C++ harness comment found by the walk: its true statement passes
        # (so the file was read and evaluated), its stale one is refused
        cpp_tree = Path(directory) / "cpp_tree" / "harness"
        cpp_tree.mkdir(parents=True)
        harness = cpp_tree / "planted.cpp"
        for text, want in (("/* exactly ROCKETNET_SIGNED_CONNECT_DATA_BYTES (576) signed bytes */\n", True),
                           ("/* exactly ROCKETNET_SIGNED_CONNECT_DATA_BYTES (577) signed bytes */\n", False)):
            harness.write_text(text, encoding="utf-8")
            files = collect(cpp_tree.parent)
            if harness not in files:
                print(f"FAIL: the record walk does not read C++ harnesses: {harness.name} not found", file=sys.stderr)
                return False
            if check(files) != want:
                print(f"FAIL: a {'true' if want else 'stale'} C++ statement was {'refused' if want else 'passed'}: {text.strip()}",
                      file=sys.stderr)
                return False
        empty = Path(directory) / "empty.md"
        empty.write_text("no statements\n", encoding="utf-8")
        if check([empty]):
            print("FAIL: a tree with no statements passed the check", file=sys.stderr)
            return False
    print("OK self-test: stale statements in both forms, a stale C++ harness comment and an empty tree are refused; .md, .c, .cpp and .h are read")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="require a planted stale statement to be refused")
    parser.add_argument("--proofs", type=Path, default=PROOFS,
                        help="the records directory to read (default proofs/cbmc); no statement there is an error")
    args = parser.parse_args()
    if args.self_test:
        return 0 if self_test() else 1
    return 0 if check(collect(args.proofs)) else 1


if __name__ == "__main__":
    sys.exit(main())
