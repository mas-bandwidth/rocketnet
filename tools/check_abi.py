#!/usr/bin/env python3
"""Check the 1.x C ABI against its recorded baseline, tools/abi_v1.txt.

The baseline records, for the 1.x line, every export and every public struct
in sdk/rocketnet.h: its size, and each field's offset and size, as the
compiler lays them out on a 64-bit target. The check compiles a small C
program against the current header that prints the same lines, and fails
when a recorded line is missing or different (a removed export, a moved or
resized field, a resized struct: an ABI break) or when the header or
tools/exports.txt holds a struct, field or export the baseline does not
record (a 1.x addition is written into the baseline in the change that adds
it, so it is reviewed). Within 1.x the baseline only grows.

  python3 tools/check_abi.py                 check the current header
  python3 tools/check_abi.py --record OUT    write the lines for --header
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "sdk" / "rocketnet.h"
EXPORTS = ROOT / "tools" / "exports.txt"
BASELINE = ROOT / "tools" / "abi_v1.txt"


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def public_structs(header_text):
    """[(struct, [field, ...])] for every struct defined at file scope."""
    text = strip_comments(header_text)
    structs = []
    for match in re.finditer(r"^struct\s+(rocketnet_\w+_t)\s*\{(.*?)^\};", text, flags=re.S | re.M):
        fields = []
        for declaration in match.group(2).split(";"):
            lines = [line for line in declaration.splitlines() if not line.strip().startswith("#")]
            declaration = " ".join(lines).strip()
            if not declaration:
                continue
            pointer = re.search(r"\(\s*\*\s*(\w+)\s*\)\s*\(", declaration)
            name = pointer or re.search(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$", declaration)
            if not name:
                raise RuntimeError("cannot read a field of %s: %s" % (match.group(1), declaration))
            fields.append(name.group(1))
        structs.append((match.group(1), fields))
    return structs


def read_exports(path):
    return sorted(line.strip() for line in Path(path).read_text().splitlines()
                  if line.strip() and not line.startswith("#"))


def layout_program(structs):
    out = ["#include <stddef.h>", "#include <stdio.h>", '#include "rocketnet.h"', "int main( void )", "{",
           '    printf( "pointer %zu\\n", sizeof( void * ) );']
    for struct, fields in structs:
        out.append('    printf( "struct %s %%zu\\n", sizeof( struct %s ) );' % (struct, struct))
        for field in fields:
            out.append('    printf( "field %s %s %%zu %%zu\\n", offsetof( struct %s, %s ), sizeof( ( (struct %s *) 0 )->%s ) );'
                       % (struct, field, struct, field, struct, field))
    out += ["    return 0;", "}", ""]
    return "\n".join(out)


def measured_lines(header, exports, compiler):
    structs = public_structs(Path(header).read_text())
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "abi.c"
        binary = Path(tmp) / "abi"
        source.write_text(layout_program(structs))
        subprocess.run([compiler, "-std=c11", "-ffp-contract=off", "-I", str(Path(header).parent), "-o", str(binary), str(source)],
                       check=True)
        layout = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.splitlines()
    return ["export %s" % name for name in read_exports(exports)] + layout


def compare(baseline, current):
    """Returns the problems: recorded lines missing or changed, and lines not recorded."""
    recorded, now = set(baseline), set(current)
    problems = ["ABI BREAK: %s (recorded in %s)" % (line, BASELINE.name) for line in baseline if line not in now]
    problems += ["NOT RECORDED: %s (a 1.x addition is added to %s)" % (line, BASELINE.name)
                 for line in current if line not in recorded]
    return problems


def read_baseline(path):
    return [line.strip() for line in Path(path).read_text().splitlines()
            if line.strip() and not line.startswith("#")]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", default=str(HEADER))
    parser.add_argument("--exports", default=str(EXPORTS))
    parser.add_argument("--baseline", default=str(BASELINE))
    parser.add_argument("--record", metavar="OUT", help="write the measured lines to OUT instead of checking")
    args = parser.parse_args()
    current = measured_lines(args.header, args.exports, os.environ.get("CC", "cc"))
    if args.record:
        Path(args.record).write_text("# The 1.x C ABI: exports, and each public struct's size and its fields'\n"
                                     "# offsets and sizes on 64-bit targets. Checked by tools/check_abi.py;\n"
                                     "# within 1.x lines are only added.\n" + "\n".join(current) + "\n")
        return 0
    problems = compare(read_baseline(args.baseline), current)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    structs = sum(1 for line in current if line.startswith("struct "))
    exports = sum(1 for line in current if line.startswith("export "))
    fields = sum(1 for line in current if line.startswith("field "))
    print("ABI matches %s: %d exports, %d structs, %d fields" % (
        Path(args.baseline).name, exports, structs, fields))
    return 0


if __name__ == "__main__":
    sys.exit(main())
