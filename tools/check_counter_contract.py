#!/usr/bin/env python3
"""Hold the counter table to its contract: ids never move.

Counters are ABI. sdk/rocketnet_counters.h's ROCKETNET_COUNTER_ROWS is one
table, identical on every platform and in every build, never behind a
preprocessor condition. tests/counter_contract.inc lists every row in id
order as ROCKETNET_COUNTER_CONTRACT( id, NAME, CLASS ). This check fails
(COUNTER CONTRACT FAILED) when:

  renumber   a contract name sits at another id in the header
  rename     a contract id holds another name in the header, the contract
             name gone (a reuse when the new name is one the contract never
             had at that id)
  removal    the header is shorter than the contract
  reclass    a contract id keeps its name but changes class
  unlisted   the header has a row past the contract's end (append it to the
             contract in the same commit)
  released   a "// RELEASED <version> <count> sha256:<hex>" line does not
             match the contract's first <count> rows, the lines do not grow,
             or the shipping version in sdk/rocketnet.h has no line (a
             release freezes its rows; docs/RELEASING.md)
  guarded    the ROCKETNET_COUNTER_ROWS definition or the counter enum sits
             inside a preprocessor condition other than the include guard
  malformed  contract ids not 0, 1, 2, ... in order, or a name twice

Usage:
  check_counter_contract.py                 check the repository
  check_counter_contract.py --root DIR      check DIR (no header or contract is an error)
  check_counter_contract.py --released V    print the RELEASED line for version V
  check_counter_contract.py --self-test     run the red and green controls
"""

import hashlib
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = Path("sdk/rocketnet_counters.h")
CONTRACT = Path("tests/counter_contract.inc")
VERSION_HEADER = Path("sdk/rocketnet.h")

ROW = re.compile(r"X\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)")
CONTRACT_ROW = re.compile(r"^\s*ROCKETNET_COUNTER_CONTRACT\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*$")
RELEASED = re.compile(r"^//\s*RELEASED\s+(v\d+\.\d+\.\d+\S*)\s+(\d+)\s+sha256:([0-9a-f]{64})\s*$")
SHIPPING = re.compile(r'#define\s+ROCKETNET_VERSION_FULL\s+"(v\d+\.\d+\.\d+[^"]*)"')
CONDITION = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")


class Refused(Exception):
    pass


def macro_body(text):
    """The ROCKETNET_COUNTER_ROWS definition and its continuation lines, with
    the line number it starts on."""
    lines = text.split("\n")
    for number, line in enumerate(lines):
        if re.match(r"^\s*#\s*define\s+ROCKETNET_COUNTER_ROWS\s*\(", line):
            body = []
            for continued in lines[number:]:
                body.append(continued)
                if not continued.rstrip().endswith("\\"):
                    break
            return number, "\n".join(body)
    raise Refused("guarded: sdk/rocketnet_counters.h has no ROCKETNET_COUNTER_ROWS definition")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def header_rows(text):
    start, body = macro_body(text)
    rows = [(name, klass) for name, klass, _family in ROW.findall(strip_comments(body))]
    if not rows:
        raise Refused("guarded: ROCKETNET_COUNTER_ROWS holds no X( NAME, CLASS, FAMILY ) row")
    return start, rows


def guard_problems(text, start):
    """The definition and the enum must sit at depth 1 (the include guard)."""
    problems = []
    depth = 0
    enum_line = None
    for number, line in enumerate(text.split("\n")):
        match = CONDITION.match(line)
        if match:
            word = match.group(1)
            if word in ("if", "ifdef", "ifndef"):
                depth += 1
            elif word == "endif":
                depth -= 1
        if number == start and depth != 1:
            problems.append("guarded: sdk/rocketnet_counters.h:%d ROCKETNET_COUNTER_ROWS is defined inside a preprocessor condition (depth %d): counters are never behind #if" % (number + 1, depth))
        if enum_line is None and re.search(r"\bROCKETNET_NUM_COUNTERS\b", line) and not line.lstrip().startswith(("//", "*", "/*")):
            enum_line = number
            if depth != 1:
                problems.append("guarded: sdk/rocketnet_counters.h:%d the counter enum (ROCKETNET_NUM_COUNTERS) is inside a preprocessor condition (depth %d)" % (number + 1, depth))
    return problems


def contract_rows(text):
    rows = []
    released = []
    problems = []
    seen = {}
    for number, line in enumerate(text.split("\n"), 1):
        match = CONTRACT_ROW.match(line)
        if match:
            ident, name, klass = int(match.group(1)), match.group(2), match.group(3)
            if ident != len(rows):
                problems.append("malformed: tests/counter_contract.inc:%d id %d where %d comes next" % (number, ident, len(rows)))
            if name in seen:
                problems.append("malformed: tests/counter_contract.inc:%d %s is listed twice (ids %d and %d)" % (number, name, seen[name], ident))
            seen[name] = ident
            rows.append((name, klass))
            continue
        match = RELEASED.match(line)
        if match:
            released.append((number, match.group(1), int(match.group(2)), match.group(3)))
            continue
        if "ROCKETNET_COUNTER_CONTRACT" in line and not line.lstrip().startswith("//"):
            problems.append("malformed: tests/counter_contract.inc:%d not a contract row: %s" % (number, line.strip()))
    return rows, released, problems


def digest(rows, count):
    h = hashlib.sha256()
    for ident in range(count):
        name, klass = rows[ident]
        h.update(("%d\t%s\t%s\n" % (ident, name, klass)).encode())
    return h.hexdigest()


def compare(contract, header):
    problems = []
    header_index = {name: ident for ident, (name, _klass) in enumerate(header)}
    contract_names = {name for name, _klass in contract}
    for ident, (name, klass) in enumerate(contract):
        if ident >= len(header):
            if name in header_index:
                continue
            problems.append("removal: counter %d %s is gone from the header (ids are never removed)" % (ident, name))
            continue
        header_name, header_class = header[ident]
        if header_name != name:
            if name in header_index:
                problems.append("renumber: %s is id %d in the contract and id %d in the header" % (name, ident, header_index[name]))
            else:
                problems.append("rename: id %d is %s in the contract and %s in the header (ids are never renamed)" % (ident, name, header_name))
            if header_name not in contract_names:
                problems.append("reuse: id %d now holds %s, a name the contract never had (a removed id is never reused)" % (ident, header_name))
        elif header_class != klass:
            problems.append("reclass: %s (id %d) is %s in the contract and %s in the header" % (name, ident, klass, header_class))
    if len(header) < len(contract):
        problems.append("removal: the header has %d counters and the contract %d" % (len(header), len(contract)))
    for ident in range(len(contract), len(header)):
        name, klass = header[ident]
        problems.append("unlisted: header id %d %s %s is not in tests/counter_contract.inc: append ROCKETNET_COUNTER_CONTRACT( %d, %s, %s ) in the same commit" % (ident, name, klass, ident, name, klass))
    return problems


def released_problems(contract, released, shipping):
    problems = []
    if not released:
        problems.append("released: tests/counter_contract.inc has no RELEASED line")
    count = 0
    for number, version, n, hexdigest in released:
        if n < count:
            problems.append("released: tests/counter_contract.inc:%d %s freezes %d rows, fewer than an earlier release's %d" % (number, version, n, count))
        count = max(count, n)
        if n > len(contract):
            problems.append("released: tests/counter_contract.inc:%d %s freezes %d rows and the contract has %d" % (number, version, n, len(contract)))
            continue
        actual = digest(contract, n)
        if actual != hexdigest:
            problems.append("released: tests/counter_contract.inc:%d %s's first %d rows changed (sha256 %s, the release shipped %s): a released id never moves" % (number, version, n, actual, hexdigest))
    if shipping is not None and shipping not in {version for _n, version, _c, _h in released}:
        problems.append("released: sdk/rocketnet.h ships %s and tests/counter_contract.inc has no RELEASED line for it: add `%s`" % (shipping, released_line(contract, shipping)))
    return problems


def released_line(contract, version):
    return "// RELEASED %s %d sha256:%s" % (version, len(contract), digest(contract, len(contract)))


def check(root):
    header_path = root / HEADER
    contract_path = root / CONTRACT
    if not header_path.is_file():
        raise Refused("no %s under %s" % (HEADER, root))
    if not contract_path.is_file():
        raise Refused("no %s under %s" % (CONTRACT, root))
    header_text = header_path.read_text(encoding="utf-8")
    start, header = header_rows(header_text)
    contract, released, problems = contract_rows(contract_path.read_text(encoding="utf-8"))
    if not contract:
        raise Refused("tests/counter_contract.inc holds no ROCKETNET_COUNTER_CONTRACT row")
    shipping = None
    version_path = root / VERSION_HEADER
    if version_path.is_file():
        match = SHIPPING.search(version_path.read_text(encoding="utf-8"))
        shipping = match.group(1) if match else None
    problems += guard_problems(header_text, start)
    problems += compare(contract, header)
    problems += released_problems(contract, released, shipping)
    return header, problems


# ---------------------------------------------------------------------------
# self-test

SELF_HEADER = """#ifndef G
#define G
// THE COUNTER LIST
#define ROCKETNET_COUNTER_ROWS( X ) \\
    X( A_RECEIVED, ARRIVAL, NONE ) /* X( NOT_A_ROW, REFUSAL, NONE ) */ \\
    X( A_TOO_SHORT, REFUSAL, A_RECEIVED ) \\
    X( A_SEEN, EXPECTED, A_RECEIVED )
enum
{
#define E( name, klass, family ) ROCKETNET_COUNTER_##name,
    ROCKETNET_COUNTER_ROWS( E )
#undef E
    ROCKETNET_NUM_COUNTERS
};
#endif
"""

SELF_VERSION = '#if !ROCKETNET_DEV\n#define ROCKETNET_VERSION_FULL "v1.0.0"\n#else\n#define ROCKETNET_VERSION_FULL "dev"\n#endif\n'


def self_contract(rows, released_count=2, version="v1.0.0", extra=""):
    lines = ["// contract"]
    if released_count is not None:
        lines.append(released_line(rows[:released_count], version))
    for ident, (name, klass) in enumerate(rows):
        lines.append("ROCKETNET_COUNTER_CONTRACT( %3d, %s, %s )" % (ident, name, klass))
    return "\n".join(lines) + "\n" + extra


GOOD = [("A_RECEIVED", "ARRIVAL"), ("A_TOO_SHORT", "REFUSAL"), ("A_SEEN", "EXPECTED")]


def self_case(header, contract, version=SELF_VERSION):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "sdk").mkdir()
        (root / "tests").mkdir()
        (root / HEADER).write_text(header, encoding="utf-8")
        (root / CONTRACT).write_text(contract, encoding="utf-8")
        (root / VERSION_HEADER).write_text(version, encoding="utf-8")
        try:
            _rows, problems = check(root)
        except Refused as refusal:
            return ["refused: %s" % refusal]
        return problems


def self_test():
    failures = 0

    def expect(label, problems, word):
        nonlocal failures
        if word is None:
            ok = not problems
        else:
            ok = any(problem.startswith(word) for problem in problems)
        detail = ""
        if not ok:
            detail = ": " + ("; ".join(problems) if problems else "nothing found")
        print("%s  %s%s" % ("ok  " if ok else "FAIL", label, detail))
        if not ok:
            failures += 1

    expect("clean table passes", self_case(SELF_HEADER, self_contract(GOOD)), None)
    expect("renumber is red", self_case(SELF_HEADER.replace("X( A_TOO_SHORT, REFUSAL, A_RECEIVED ) \\\n    X( A_SEEN, EXPECTED, A_RECEIVED )", "X( A_SEEN, EXPECTED, A_RECEIVED ) \\\n    X( A_TOO_SHORT, REFUSAL, A_RECEIVED )"), self_contract(GOOD)), "renumber")
    expect("rename is red", self_case(SELF_HEADER.replace("A_SEEN", "A_SEEN_TWICE"), self_contract(GOOD)), "rename")
    expect("reuse is red", self_case(SELF_HEADER.replace("A_SEEN", "B_NEW"), self_contract(GOOD)), "reuse")
    expect("removal is red", self_case(SELF_HEADER.replace(" \\\n    X( A_SEEN, EXPECTED, A_RECEIVED )", ""), self_contract(GOOD)), "removal")
    expect("reclass is red", self_case(SELF_HEADER.replace("X( A_SEEN, EXPECTED", "X( A_SEEN, REFUSAL"), self_contract(GOOD)), "reclass")
    expect("an appended counter not in the contract is red", self_case(SELF_HEADER.replace("X( A_SEEN, EXPECTED, A_RECEIVED )", "X( A_SEEN, EXPECTED, A_RECEIVED ) \\\n    X( B_NEW, REFUSAL, NONE )"), self_contract(GOOD)), "unlisted")
    expect("an appended counter in the contract passes", self_case(SELF_HEADER.replace("X( A_SEEN, EXPECTED, A_RECEIVED )", "X( A_SEEN, EXPECTED, A_RECEIVED ) \\\n    X( B_NEW, REFUSAL, NONE )"), self_contract(GOOD + [("B_NEW", "REFUSAL")])), None)
    contract = self_contract(GOOD).replace("A_TOO_SHORT", "A_SHORT")
    expect("a released row changed in the header and the contract together is red", self_case(SELF_HEADER.replace("A_TOO_SHORT", "A_SHORT"), contract), "released")
    expect("an unreleased row renamed in both passes", self_case(SELF_HEADER.replace("A_SEEN", "A_SEEN_ONCE"), self_contract([GOOD[0], GOOD[1], ("A_SEEN_ONCE", "EXPECTED")])), None)
    expect("the shipping version without a RELEASED line is red", self_case(SELF_HEADER, self_contract(GOOD), SELF_VERSION.replace("v1.0.0", "v1.0.1")), "released")
    expect("no RELEASED line is red", self_case(SELF_HEADER, self_contract(GOOD, released_count=None)), "released")
    expect("a skipped contract id is red", self_case(SELF_HEADER, self_contract(GOOD).replace("CONTRACT(   2,", "CONTRACT(   3,")), "malformed")
    expect("the table behind #if is red", self_case(SELF_HEADER.replace("// THE COUNTER LIST\n", "#if PLATFORM_X\n").replace("enum\n", "#endif\nenum\n"), self_contract(GOOD)), "guarded")
    expect("the enum behind #if is red", self_case(SELF_HEADER.replace("enum\n{", "#if PLATFORM_X\nenum\n{").replace("};\n#endif", "};\n#endif\n#endif"), self_contract(GOOD)), "guarded")
    expect("a row in a comment is not a row", self_case(SELF_HEADER, self_contract(GOOD)), None)
    return 1 if failures else 0


def main(argv):
    root = ROOT
    args = list(argv)
    if args[:1] == ["--self-test"]:
        return self_test()
    if args[:1] == ["--released"] and len(args) == 2:
        contract, _released, problems = contract_rows((ROOT / CONTRACT).read_text(encoding="utf-8"))
        if problems or not contract:
            print("COUNTER CONTRACT FAILED: the contract is malformed: " + "; ".join(problems))
            return 1
        print(released_line(contract, args[1]))
        return 0
    if args[:1] == ["--root"] and len(args) == 2:
        root = Path(args[1])
    elif args:
        print(__doc__)
        return 2
    try:
        header, problems = check(root)
    except Refused as refusal:
        print("COUNTER CONTRACT FAILED: %s" % refusal)
        return 1
    if problems:
        for problem in problems:
            print("COUNTER CONTRACT FAILED: %s" % problem)
        return 1
    print("counter contract ok: %d counters, ids unchanged" % len(header))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
