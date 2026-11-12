#!/usr/bin/env python3
"""Hold each fuzz target's source coverage of sdk/ to its floor on this OS.

    python3 tools/check_fuzz_coverage.py [--floors FILE] --os OS [--all] RESULTS
    python3 tools/check_fuzz_coverage.py --self-test

RESULTS is the per_target.tsv tools/fuzz_coverage.sh writes: one row per
target (and "all", the union), the target name, a tab, then llvm-cov's
"covered/total pct fn executed/total". OS is uname -s (Linux, Darwin). The
run passes only when, for every row:

- the row carries a number (FUZZ-COV-MISSING otherwise: llvm-cov failed, or
  the target's profile was never written);
- the floors table has a row for the target on this OS
  (FUZZ-COV-FLOOR-MISSING otherwise: a new target gets measured, not waved
  through);
- its covered lines are at least the floor (FUZZ-COV-FLOOR): a harness
  compiled to a no-op, or a corpus that stopped replaying, lands under it;
- a target whose floor is "off" (the harness is compiled out on that OS)
  covers 0 lines (FUZZ-COV-OFF-REACHED otherwise: it runs there now, so it
  gets a floor).

With --all (tools/fuzz_coverage.sh passes it when it ran every target), a
numeric floor row for this OS with no result row is refused too
(FUZZ-COV-NOT-RUN): a target that dropped out of the run is not a pass. An
"off" row needs no result (the target may not be built there at all). Exit 0 on a pass, 1 on a
refusal, 2 on a usage error.
"""

import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FLOORS = os.path.join(HERE, "fuzz_coverage_floors.tsv")
OSES = ("Linux", "Darwin")

NUMBER = re.compile(r"^(\d+)/(\d+)\b")


def read_floors(path):
    """{(target, os): int floor or "off"}."""
    floors = {}
    for number, line in enumerate(open(path), 1):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        cells = line.split("\t")
        if len(cells) != 3 or cells[1] not in OSES or not (cells[2] == "off" or cells[2].isdigit()):
            raise ValueError("%s:%d: want target, os (%s), min_lines or off" % (path, number, " or ".join(OSES)))
        if (cells[0], cells[1]) in floors:
            raise ValueError("%s:%d: a second row for %s on %s" % (path, number, cells[0], cells[1]))
        floors[(cells[0], cells[1])] = "off" if cells[2] == "off" else int(cells[2])
    return floors


def read_results(text):
    """[(target, value text)] in file order."""
    rows = []
    for line in text.splitlines():
        if not line.strip():
            continue
        target, _, value = line.partition("\t")
        rows.append((target.strip(), value.strip()))
    return rows


def check(rows, floors, system, every):
    """Returns (ok, lines)."""
    lines = []
    ok = True
    if not rows:
        # nothing read is never a pass (tools/check_checkers.py: zero inputs are red)
        ok = False
        lines.append("FUZZ-COV-MISSING: the results hold no target row")
    for target, value in rows:
        floor = floors.get((target, system))
        match = NUMBER.match(value)
        if match is None:
            ok = False
            lines.append("FUZZ-COV-MISSING %s: no lines covered/total in %r" % (target, value))
            continue
        covered = int(match.group(1))
        if floor is None:
            ok = False
            lines.append("FUZZ-COV-FLOOR-MISSING %s: no %s row in the floors table (covered %d)" % (target, system, covered))
        elif floor == "off":
            if covered != 0:
                ok = False
                lines.append("FUZZ-COV-OFF-REACHED %s: off on %s but covers %d lines; give it a floor" % (target, system, covered))
            else:
                lines.append("fuzz coverage %s: compiled out on %s (0 lines)" % (target, system))
        elif covered < floor:
            ok = False
            lines.append("FUZZ-COV-FLOOR %s: %d sdk lines, floor %d" % (target, covered, floor))
        else:
            lines.append("fuzz coverage %s: %d sdk lines (floor %d)" % (target, covered, floor))
    if every:
        ran = set(target for target, _ in rows)
        for target, row_system in sorted(floors):
            if row_system == system and target not in ran and floors[(target, row_system)] != "off":
                ok = False
                lines.append("FUZZ-COV-NOT-RUN %s: has a %s floor but no result" % (target, system))
    return ok, lines


def self_test():
    floors = {("t", "Linux"): 100, ("u", "Linux"): "off", ("all", "Linux"): 150}
    good = "t\t120/1000 12.00% fn 5/50\nu\t0/1000 0.00% fn 0/50\nall\t160/1000 16.00% fn 6/50\n"
    cases = [
        ("every target over its floor passes", good, "Linux", True, True, "fuzz coverage t: 120 sdk lines (floor 100)"),
        ("a no-op harness, 0 lines, is refused", good.replace("120/1000", "0/1000"), "Linux", False, False, "FUZZ-COV-FLOOR t: 0 sdk lines, floor 100"),
        ("one line under the floor is refused", good.replace("120/1000", "99/1000"), "Linux", False, False, "FUZZ-COV-FLOOR t: 99"),
        ("the floor itself passes", good.replace("120/1000", "100/1000"), "Linux", False, True, "fuzz coverage t: 100 sdk lines"),
        ("an empty value is refused", good.replace("120/1000 12.00% fn 5/50", ""), "Linux", False, False, "FUZZ-COV-MISSING t"),
        ("a target with no row is refused", good + "v\t500/1000 50% fn 9/50\n", "Linux", False, False, "FUZZ-COV-FLOOR-MISSING v"),
        ("a row for another OS does not count", good, "Darwin", False, False, "FUZZ-COV-FLOOR-MISSING t: no Darwin row"),
        ("an off target that reaches lines is refused", good.replace("0/1000 0.00% fn 0/50", "7/1000 0.70% fn 1/50"), "Linux", False, False, "FUZZ-COV-OFF-REACHED u"),
        ("an off target at 0 passes", good, "Linux", False, True, "fuzz coverage u: compiled out on Linux"),
        ("the union under its floor is refused", good.replace("160/1000", "149/1000"), "Linux", False, False, "FUZZ-COV-FLOOR all: 149"),
        ("--all: a floor row that did not run is refused", "t\t120/1000 12.00% fn 5/50\nu\t0/1000 0.00% fn 0/50\n", "Linux", True, False, "FUZZ-COV-NOT-RUN all"),
        ("without --all a subset run passes", "t\t120/1000 12.00% fn 5/50\n", "Linux", False, True, "fuzz coverage t"),
        ("an empty results file with --all is refused", "", "Linux", True, False, "FUZZ-COV-NOT-RUN t"),
        ("an empty results file is refused", "", "Linux", False, False, "FUZZ-COV-MISSING: the results hold no target row"),
        ("--all: an off row needs no result", "t\t120/1000 12.00% fn 5/50\nall\t160/1000 16.00% fn 6/50\n", "Linux", True, True, "fuzz coverage all"),
    ]
    failed = 0
    for name, text, system, every, want_ok, want_text in cases:
        ok, lines = check(read_results(text), floors, system, every)
        right = ok == want_ok and any(want_text in line for line in lines)
        print("self-test %-48s %s" % (name, "ok" if right else "FAIL (%s)" % " | ".join(lines)))
        failed += 0 if right else 1
    # the committed table parses and gives every coverage target and the union
    # a row on each OS: the targets are the harnesses CMakeLists.txt builds a
    # fuzz_replay_ driver for, as tools/fuzz_coverage.sh picks them
    table = read_floors(FLOORS)
    fuzz_dir = os.path.join(HERE, "..", "tests", "fuzz")
    cmake = open(os.path.join(HERE, "..", "CMakeLists.txt")).read()
    harnesses = sorted(f[:-4] for f in os.listdir(fuzz_dir) if re.match(r"rocketnet_fuzz[a-z_]*\.cpp$", f) and f[:-4] in cmake)
    absent = ["%s/%s" % (h, s) for h in harnesses + ["all"] for s in OSES if (h, s) not in table]
    right = bool(harnesses) and not absent
    print("self-test %-48s %s" % ("every coverage target has a row on each OS", "ok" if right else "FAIL (no row: %s)" % " ".join(absent or ["no harnesses"])))
    failed += 0 if right else 1
    # malformed and duplicate rows are errors, not skipped rows
    bad_tables = [
        ("a two-column row is an error", "t\tLinux\n"),
        ("an unknown OS is an error", "t\tWindows\t5\n"),
        ("a floor that is not a number is an error", "t\tLinux\tmany\n"),
        ("a second row for one target and OS is an error", "t\tLinux\t5\nt\tLinux\t6\n"),
    ]
    for name, body in bad_tables:
        with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as handle:
            handle.write(body)
        try:
            read_floors(handle.name)
            right = False
        except ValueError:
            right = True
        finally:
            os.unlink(handle.name)
        print("self-test %-48s %s" % (name, "ok" if right else "FAIL"))
        failed += 0 if right else 1
    total = len(cases) + 1 + len(bad_tables)
    print("check_fuzz_coverage self-test: %d cases, %d failed" % (total, failed))
    return 1 if failed else 0


def usage():
    print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
    return 2


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    floors_path = FLOORS
    system = results = None
    every = False
    i = 0
    while i < len(argv):
        if argv[i] in ("--floors", "--os") and i + 1 < len(argv):
            if argv[i] == "--floors":
                floors_path = argv[i + 1]
            else:
                system = argv[i + 1]
            i += 2
        elif argv[i] == "--all":
            every = True
            i += 1
        elif results is None and not argv[i].startswith("--"):
            results = argv[i]
            i += 1
        else:
            return usage()
    if system not in OSES or results is None:
        return usage()
    try:
        text = open(results, errors="replace").read()
    except OSError as error:
        print("FUZZ-COV-MISSING: cannot read %s: %s" % (results, error))
        return 1
    try:
        floors = read_floors(floors_path)
    except (OSError, ValueError) as error:
        print("FUZZ-COV-FLOOR-MISSING: cannot read the floors table: %s" % error)
        return 1
    ok, lines = check(read_results(text), floors, system, every)
    for line in lines:
        print(line)
    if not ok:
        print("fuzz coverage: under a floor or missing a number on %s (tools/fuzz_coverage_floors.tsv)" % system)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
