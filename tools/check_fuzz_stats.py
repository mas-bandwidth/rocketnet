#!/usr/bin/env python3
"""Hold a libFuzzer run to its target's floor: coverage reached and units executed.

    python3 tools/check_fuzz_stats.py --target NAME --seconds N [--floors FILE] LOG
    python3 tools/check_fuzz_stats.py --self-test

LOG is the run's output with -print_final_stats=1. The run passes only when:

- it printed "INITED cov: C", a later "#N ... cov: C" line or none, and
  stat::number_of_executed_units (FUZZ-STATS-MISSING otherwise: a run that
  never initialised or never finished is not a pass);
- its last cov is at least the target's min_cov (FUZZ-COVERAGE-FLOOR): a
  harness compiled to a no-op reaches cov 1 and fuzzes nothing;
- its executed units are at least min_units_per_second x N, N being the
  seconds the leg asked for (FUZZ-EXECS-FLOOR): a run that stalled, or ended
  early, executed less.

Floors are rows of tools/fuzz_floors.tsv; a target with no row is refused
(FUZZ-FLOOR-MISSING). Exit 0 on a pass, 1 on a refusal, 2 on a usage error.
"""

import math
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FLOORS = os.path.join(HERE, "fuzz_floors.tsv")

INITED = re.compile(r"#\d+\s+INITED\b.*?\bcov: (\d+)")
COV = re.compile(r"#\d+\s+\S+\s+cov: (\d+)")
UNITS = re.compile(r"stat::number_of_executed_units:\s*(\d+)")


def read_floors(path):
    floors = {}
    for number, line in enumerate(open(path), 1):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        cells = line.split("\t")
        if len(cells) != 3:
            raise ValueError("%s:%d: want target, min_cov, min_units_per_second" % (path, number))
        floors[cells[0]] = (int(cells[1]), float(cells[2]))
    return floors


def check(text, target, seconds, floors):
    """Returns (ok, line)."""
    if target not in floors:
        return False, "FUZZ-FLOOR-MISSING %s: no row in the floors table" % target
    min_cov, rate = floors[target]
    inited = INITED.search(text)
    units = UNITS.findall(text)
    if not inited or not units:
        missing = [what for what, found in (("INITED cov", inited), ("stat::number_of_executed_units", units)) if not found]
        return False, "FUZZ-STATS-MISSING %s: the log has no %s" % (target, " and no ".join(missing))
    covs = [int(c) for c in COV.findall(text)]
    cov = covs[-1] if covs else int(inited.group(1))
    executed = int(units[-1])
    min_units = int(math.ceil(rate * seconds))
    if cov < min_cov:
        return False, "FUZZ-COVERAGE-FLOOR %s: cov %d, floor %d" % (target, cov, min_cov)
    if executed < min_units:
        return False, "FUZZ-EXECS-FLOOR %s: %d units in %d s, floor %d" % (target, executed, seconds, min_units)
    return True, "fuzz stats %s: cov %d (floor %d), %d units in %d s (floor %d)" % (target, cov, min_cov, executed, seconds, min_units)


def self_test():
    floors = {"t": (100, 10.0)}
    good = "#2\tINITED cov: 150 ft: 300 corp: 2/10b\n#100\tNEW    cov: 160 ft: 320\nDone 2000 runs in 100 second(s)\nstat::number_of_executed_units: 2000\n"
    cases = [
        ("a run over both floors passes", good, "t", 100, True, "fuzz stats t: cov 160"),
        ("a no-op harness, cov 1, is refused", good.replace("150", "1").replace("NEW    cov: 160", "NEW    cov: 1"), "t", 100, False, "FUZZ-COVERAGE-FLOOR"),
        ("the last cov counts, not INITED", good.replace("NEW    cov: 160", "NEW    cov: 99"), "t", 100, False, "FUZZ-COVERAGE-FLOOR t: cov 99"),
        ("too few units for the seconds asked", good, "t", 900, False, "FUZZ-EXECS-FLOOR"),
        ("no final stats is refused", good.replace("stat::number_of_executed_units: 2000\n", ""), "t", 100, False, "FUZZ-STATS-MISSING"),
        ("no INITED line is refused", good.replace("INITED", "READ"), "t", 100, False, "FUZZ-STATS-MISSING"),
        ("an empty log is refused", "", "t", 100, False, "FUZZ-STATS-MISSING"),
        ("a target with no row is refused", good, "other", 100, False, "FUZZ-FLOOR-MISSING"),
    ]
    failed = 0
    for name, text, target, seconds, want_ok, want_text in cases:
        ok, line = check(text, target, seconds, floors)
        right = ok == want_ok and want_text in line
        print("self-test %-44s %s" % (name, "ok" if right else "FAIL (%s)" % line))
        failed += 0 if right else 1
    # the committed table parses and names every harness under tests/fuzz/
    table = read_floors(FLOORS)
    fuzz_dir = os.path.join(HERE, "..", "tests", "fuzz")
    harnesses = sorted(f[:-4] for f in os.listdir(fuzz_dir) if re.match(r"rocketnet_fuzz[a-z_]*\.cpp$", f))
    absent = [h for h in harnesses if h not in table]
    right = bool(harnesses) and not absent
    print("self-test %-44s %s" % ("every harness has a floor row", "ok" if right else "FAIL (no row: %s)" % " ".join(absent or ["no harnesses"])))
    failed += 0 if right else 1
    # a malformed row is an error, not a skipped row
    with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as handle:
        handle.write("t\t100\n")
    try:
        read_floors(handle.name)
        right = False
    except ValueError:
        right = True
    finally:
        os.unlink(handle.name)
    print("self-test %-44s %s" % ("a two-column row is an error", "ok" if right else "FAIL"))
    failed += 0 if right else 1
    total = len(cases) + 2
    print("check_fuzz_stats self-test: %d cases, %d failed" % (total, failed))
    return 1 if failed else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    target = seconds = log = None
    floors_path = FLOORS
    i = 0
    while i < len(argv):
        if argv[i] in ("--target", "--seconds", "--floors") and i + 1 < len(argv):
            if argv[i] == "--target":
                target = argv[i + 1]
            elif argv[i] == "--seconds":
                seconds = argv[i + 1]
            else:
                floors_path = argv[i + 1]
            i += 2
        elif log is None and not argv[i].startswith("--"):
            log = argv[i]
            i += 1
        else:
            print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
            return 2
    if target is None or seconds is None or log is None or not seconds.isdigit() or int(seconds) == 0:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        text = open(log, errors="replace").read()
    except OSError as error:
        print("FUZZ-STATS-MISSING %s: cannot read %s: %s" % (target, log, error))
        return 1
    ok, line = check(text, target, int(seconds), read_floors(floors_path))
    print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
