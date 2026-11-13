#!/usr/bin/env python3
"""A functional-suite run skips exactly the rows its leg is allowed to skip.

The functional suite leaves out a row whose runs_here predicate is false on
the host and prints "SKIPPED <case>: <predicate>" for it, then
"skipped: <n>". Before this check a CI leg could lose a capability (a
transport, root, a kernel feature) and every row that needed it would turn
into a SKIPPED line while the job stayed green. tests/functional/
expected_skips.txt lists, per leg, every row that leg may skip and why; this
check holds a run's output to that list both ways.

  python3 tools/check_skips.py --variant dev [--shard I/N] LOG
      the leg is <os>-dev, the os read from this host; fails on an unlisted
      skip, a listed row that ran, a different predicate, a server pin
      (ROCKETNET_SERVER_TRANSPORT, auto when unset) other than the leg's, and
      output with no "skipped: <n>" line or one that disagrees with the
      SKIPPED lines. With --shard I/N the log is one shard's run of the suite
      (tests/suite_runner.h --shard): only the listed rows whose name falls in
      shard I (suite_shard_of: FNV-1a of the name mod N) are expected to skip.
  python3 tools/check_skips.py --lint
      the list is well formed, every predicate it uses has a reason, every
      case and predicate is named in tests/, every leg is run by some
      workflow through tools/run_functional.sh (and every variant a workflow
      runs is declared), and no workflow runs the whole suite any other way.
  python3 tools/check_skips.py --self-test

tools/run_functional.sh runs the suite and then this check; the lint job runs
--lint and --self-test.
"""

import os
import platform
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# ROCKETNET_EXPECTED_SKIPS names another list: a test seam for tools/test_run_functional.sh only.
LIST = os.environ.get("ROCKETNET_EXPECTED_SKIPS") or os.path.join(ROOT, "tests", "functional", "expected_skips.txt")

OS_NAMES = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}
PINS = ("auto", "portable", "gso", "io_uring", "xdp", "msgx", "iocp", "rio")

SKIPPED = re.compile(r"^SKIPPED (\S+): (\S+)\s*$")
SKIPPED_LOOSE = re.compile(r"SKIPPED\b")
COUNT = re.compile(r"^skipped: (\d+)\s*$")

# A whole-suite run in a workflow: the suite binary as a command, not followed
# by --run or --list (one case, or the listing).
SUITE_BINARY = re.compile(
    r"(?:^|[^A-Za-z0-9_/.=-])(?:\./)?(?:[A-Za-z0-9_.-]+/)*bin/(?:Release/|Debug/)?"
    r"functional(?:_production)?(?:\.exe)?(?=[^A-Za-z0-9_.]|$)")
ONE_CASE = re.compile(r"functional(?:_production)?(?:\.exe)?\s+(--run|--list)\b")
WRAPPED = re.compile(r"run_functional\.sh\s+([a-z0-9-]+)")
# The places a workflow names the suite binary without running the whole
# suite, each with its reason. Anything else that names it must be wrapped.
NOT_A_RUN = [
    (re.compile(r"readelf -h \S*bin/functional"), "reads the ELF header only"),
    (re.compile(r"\breplay \w+ \./bin/functional "), "the replay helper runs one named case with --run"),
    (re.compile(r"leaks_cases\.sh\" \./bin/functional "), "leaks_cases.sh runs each case alone with --run"),
]


def host_os():
    name = platform.system()
    if name in OS_NAMES:
        return OS_NAMES[name]
    if name.startswith(("MSYS", "MINGW", "CYGWIN")):
        return "windows"
    return name.lower()


def parse_list(text):
    """Returns (legs, reasons, skips, errors).

    legs: name -> (os, pin); reasons: predicate -> text;
    skips: case -> (predicate, [legs], line number)."""
    legs, reasons, skips, errors = {}, {}, {}, []
    os_of = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        kind = fields[0]
        where = "expected_skips.txt:%d" % number
        if kind == "leg":
            if len(fields) != 4 or fields[2] not in os_of or fields[3] not in PINS:
                errors.append("%s: want 'leg <name> <Linux|Darwin|Windows> <pin>': %s" % (where, line))
                continue
            name = fields[1]
            if not name.startswith(os_of[fields[2]] + "-"):
                errors.append("%s: leg %s does not start with its os %s-" % (where, name, os_of[fields[2]]))
            if name in legs:
                errors.append("%s: leg %s declared twice" % (where, name))
            legs[name] = (os_of[fields[2]], fields[3])
        elif kind == "reason":
            if len(fields) < 4:
                errors.append("%s: want 'reason <predicate> <text>' with a real reason: %s" % (where, line))
                continue
            if fields[1] in reasons:
                errors.append("%s: reason for %s given twice" % (where, fields[1]))
            reasons[fields[1]] = " ".join(fields[2:])
        elif kind == "skip":
            if len(fields) != 4:
                errors.append("%s: want 'skip <case> <predicate> <leg,leg...>': %s" % (where, line))
                continue
            case, predicate, names = fields[1], fields[2], fields[3].split(",")
            if case in skips:
                errors.append("%s: case %s listed twice" % (where, case))
            if len(set(names)) != len(names):
                errors.append("%s: case %s names a leg twice" % (where, case))
            skips[case] = (predicate, names, number)
        else:
            errors.append("%s: unknown row kind %r" % (where, kind))
    for case, (predicate, names, number) in sorted(skips.items()):
        for name in names:
            if name not in legs:
                errors.append("expected_skips.txt:%d: case %s names undeclared leg %s" % (number, case, name))
        if predicate not in reasons:
            errors.append("expected_skips.txt:%d: predicate %s has no reason row" % (number, predicate))
    used = set(p for p, _, _ in skips.values())
    for predicate in sorted(set(reasons) - used):
        errors.append("expected_skips.txt: reason for %s, which no skip row uses" % predicate)
    if not legs:
        errors.append("expected_skips.txt: no leg declared")
    return legs, reasons, skips, errors


def shard_of(name, shards):
    """tests/suite_runner.h suite_shard_of: FNV-1a (32 bit) of the name, mod shards."""
    h = 2166136261
    for b in name.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h % shards


def parse_shard(text):
    m = re.fullmatch(r"([0-9]+)/([1-9][0-9]*)", text or "")
    if not m or int(m.group(1)) >= int(m.group(2)):
        return None
    return int(m.group(1)), int(m.group(2))


def check_output(legs, skips, leg, pin, output, shard=None):
    """Returns the errors for one run's output against leg's rows (shard: (i, n) for one shard's run)."""
    errors = []
    if leg not in legs:
        return ["leg %s is not declared in tests/functional/expected_skips.txt" % leg]
    want_pin = legs[leg][1]
    if pin != want_pin:
        errors.append("leg %s runs with server pin %s, but ROCKETNET_SERVER_TRANSPORT gives %s" % (leg, want_pin, pin))
    expected = dict((case, predicate) for case, (predicate, names, _) in skips.items() if leg in names
                    and (shard is None or shard_of(case, shard[1]) == shard[0]))
    seen = {}
    counts = []
    for raw in output.splitlines():
        line = raw.rstrip("\r")
        m = SKIPPED.match(line)
        if m:
            if m.group(1) in seen:
                errors.append("SKIPPED %s printed twice" % m.group(1))
            seen[m.group(1)] = m.group(2)
            continue
        if line.startswith("SKIPPED"):
            errors.append("unreadable SKIPPED line: %r" % line)
            continue
        m = COUNT.match(line)
        if m:
            counts.append(int(m.group(1)))
    if len(counts) != 1:
        errors.append("the output has %d 'skipped: <n>' lines, not 1: the suite did not finish, or this was not one whole-suite run" % len(counts))
    elif counts[0] != len(seen):
        errors.append("the suite says skipped: %d, but printed %d SKIPPED lines" % (counts[0], len(seen)))
    for case in sorted(seen):
        if case not in expected:
            errors.append("UNLISTED SKIP leg %s: %s (%s); a row that should run here did not" % (leg, case, seen[case]))
        elif seen[case] != expected[case]:
            errors.append("leg %s: %s skipped by %s, listed as %s" % (leg, case, seen[case], expected[case]))
    for case in sorted(expected):
        if case not in seen:
            errors.append("LISTED NOT SKIPPED leg %s: %s (%s) ran; remove it from the leg's row" % (leg, case, expected[case]))
    return errors


def git_files(root, *patterns):
    result = subprocess.run(["git", "-C", root, "ls-files", "--"] + list(patterns),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if result.returncode != 0:
        raise RuntimeError("git ls-files failed: %s" % result.stderr.strip())
    return [f for f in result.stdout.splitlines() if f]


def read(root, name):
    with open(os.path.join(root, name), encoding="utf-8", errors="replace") as handle:
        return handle.read()


def lint(root):
    errors = []
    try:
        text = read(root, "tests/functional/expected_skips.txt")
    except OSError as error:
        return ["cannot read tests/functional/expected_skips.txt: %s" % error], 0, 0
    legs, reasons, skips, errors = parse_list(text)

    sources = git_files(root, "tests")
    sources = [f for f in sources if f.endswith((".cpp", ".h", ".inc", ".c")) and f != "tests/functional/expected_skips.txt"]
    if not sources:
        errors.append("no test sources listed under tests/: nothing to check the names against")
    corpus = "\n".join(read(root, f) for f in sources)
    for case, (predicate, _, number) in sorted(skips.items()):
        # a functional row's name is test_<x>; its function is rocketnet_test_<x>
        # or the row string itself
        bare = case[len("test_"):] if case.startswith("test_") else case
        if not re.search(r"\b(%s|rocketnet_test_%s)\b" % (re.escape(case), re.escape(bare)), corpus):
            errors.append("expected_skips.txt:%d: case %s is not named in tests/" % (number, case))
        if not re.search(r"\b%s\b" % re.escape(predicate), corpus):
            errors.append("expected_skips.txt:%d: predicate %s is not named in tests/" % (number, predicate))

    workflows = git_files(root, ".github/workflows")
    workflows = [f for f in workflows if f.endswith((".yml", ".yaml"))]
    if not workflows:
        errors.append("no workflow files listed under .github/workflows")
    variants = {}
    for name in workflows:
        for number, line in enumerate(read(root, name).splitlines(), 1):
            if line.strip().startswith("#"):
                continue
            if "ROCKETNET_EXPECTED_SKIPS" in line:
                errors.append("%s:%d: ROCKETNET_EXPECTED_SKIPS is a self-test seam; a workflow never sets it" % (name, number))
            for m in WRAPPED.finditer(line):
                variants.setdefault(m.group(1), "%s:%d" % (name, number))
            if not SUITE_BINARY.search(line) or ONE_CASE.search(line):
                continue
            if WRAPPED.search(line):
                continue
            if any(p.search(line) for p, _ in NOT_A_RUN):
                continue
            errors.append("%s:%d: a whole functional-suite run not through tools/run_functional.sh <variant>: %s"
                          % (name, number, line.strip()))
    declared = set(leg.split("-", 1)[1] for leg in legs)
    for variant in sorted(set(variants) - declared):
        errors.append("%s: runs variant %s, which no leg in expected_skips.txt declares" % (variants[variant], variant))
    for leg in sorted(legs):
        if leg.split("-", 1)[1] not in variants:
            errors.append("expected_skips.txt: leg %s, but no workflow runs variant %s" % (leg, leg.split("-", 1)[1]))
    return errors, len(legs), len(skips)


def self_test():
    failures = []
    base = (
        "leg linux-dev Linux auto\n"
        "leg linux-dev-portable Linux portable\n"
        "reason p_root needs root here\n"
        "reason p_pin the case checks a pin\n"
        "skip test_a p_root linux-dev,linux-dev-portable\n"
        "skip test_b p_pin linux-dev\n")
    legs, _, skips, errors = parse_list(base)
    if errors:
        failures.append("clean list: %s" % errors)

    def run(name, output, want, leg="linux-dev", pin="auto", text=base):
        legs, _, skips, _ = parse_list(text)
        got = check_output(legs, skips, leg, pin, output)
        if bool(got) != (want == "red"):
            failures.append("%s: want %s, got %s" % (name, want, got or "green"))
        return got

    good = "case output\nSKIPPED test_a: p_root\nSKIPPED test_b: p_pin\nskipped: 2\n\n*** ALL TESTS PASSED ***\n"
    run("exact skips", good, "green")
    run("exact skips, CRLF", good.replace("\n", "\r\n"), "green")
    run("unlisted skip", good.replace("skipped: 2", "SKIPPED test_c: p_root\nskipped: 3"), "red")
    run("listed row ran", "SKIPPED test_a: p_root\nskipped: 1\n", "red")
    run("other predicate", good.replace("test_b: p_pin", "test_b: p_root"), "red")
    run("no count line (suite cut short)", "SKIPPED test_a: p_root\nSKIPPED test_b: p_pin\n", "red")
    run("count disagrees", good.replace("skipped: 2", "skipped: 3"), "red")
    # a shard run: only the listed rows of the shard are expected (FNV-1a mod 2 of the name)
    if shard_of("test_a", 2) == shard_of("test_b", 2):
        failures.append("self-test names test_a and test_b fall in one shard: pick other names")
    else:
        mine, other = ("test_a", "p_root"), ("test_b", "p_pin")
        if shard_of("test_a", 2) != 0:
            mine, other = other, mine
        shard_text = "shard 0/2: 1 of 2 cases\nSKIPPED %s: %s\nskipped: 1\n" % mine
        legs_, _, skips_, _ = parse_list(base)
        if check_output(legs_, skips_, "linux-dev", "auto", shard_text, (0, 2)):
            failures.append("shard 0/2 with its own listed skip: %s" % check_output(legs_, skips_, "linux-dev", "auto", shard_text, (0, 2)))
        if not check_output(legs_, skips_, "linux-dev", "auto", "shard 0/2: 1 of 2 cases\nskipped: 0\n", (0, 2)):
            failures.append("shard 0/2 whose listed row ran was green")
        if not check_output(legs_, skips_, "linux-dev", "auto", shard_text.replace("skipped: 1", "SKIPPED %s: %s\nskipped: 2" % other), (0, 2)):
            failures.append("shard 0/2 that skipped the other shard's row was green")
    run("two count lines (two runs in one log)", good + good, "red")
    run("duplicate SKIPPED", good.replace("skipped: 2", "SKIPPED test_b: p_pin\nskipped: 3"), "red")
    run("unreadable SKIPPED", good.replace("SKIPPED test_b: p_pin", "SKIPPED test_b p_pin"), "red")
    run("empty output", "", "red")
    run("pin mismatch", good, "red", pin="portable")
    run("portable leg exact", "SKIPPED test_a: p_root\nskipped: 1\n", "green", leg="linux-dev-portable", pin="portable")
    run("undeclared leg", good, "red", leg="linux-prod")

    for name, text in [
        ("undeclared leg in a row", base + "skip test_c p_root linux-other\n"),
        ("predicate with no reason", base + "skip test_c p_new linux-dev\n"),
        ("reason with no text", base.replace("reason p_pin the case checks a pin", "reason p_pin x")),
        ("unused reason", base + "reason p_gone nothing uses this\n"),
        ("case twice", base + "skip test_a p_root linux-dev\n"),
        ("leg twice", base + "leg linux-dev Linux auto\n"),
        ("leg os prefix", base + "leg macos-dev Linux auto\n"),
        ("bad pin", base + "leg linux-x Linux turbo\n"),
        ("unknown kind", base + "allow test_a\n"),
        ("no legs", "reason p x y\n"),
    ]:
        if not parse_list(text)[3]:
            failures.append("list plant %s: want an error, got none" % name)

    # lint on a temp tree: a wrapped run, an unwrapped one, unknown variants
    with tempfile.TemporaryDirectory() as tree:
        def write(path, content):
            full = os.path.join(tree, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w") as handle:
                handle.write(content)

        write("tests/functional/expected_skips.txt", base)
        write("tests/functional/a.cpp", "void rocketnet_test_a(); bool p_root(); bool p_pin(); void rocketnet_test_b();\n")
        clean = ("jobs:\n  x:\n    steps:\n"
                 "      - run: bash tools/run_functional.sh dev ./bin/functional\n"
                 "      - run: bash tools/run_functional.sh dev-portable ./bin/functional\n"
                 "      - run: ./bin/functional --run test_b\n"
                 "      - run: readelf -h bin/functional\n")
        write(".github/workflows/w.yml", clean)
        subprocess.run(["git", "init", "-q", tree], check=True)
        subprocess.run(["git", "-C", tree, "add", "-A"], check=True)

        def lint_case(name, workflow, want, extra=None):
            write(".github/workflows/w.yml", workflow)
            if extra:
                write(*extra)
            subprocess.run(["git", "-C", tree, "add", "-A"], check=True)
            got = lint(tree)[0]
            if bool(got) != (want == "red"):
                failures.append("lint %s: want %s, got %s" % (name, want, got or "green"))

        lint_case("clean tree", clean, "green")
        lint_case("bare whole-suite run", clean + "      - run: ./bin/Release/functional.exe\n", "red")
        lint_case("bare run behind a pipe", clean + "      - run: |\n          ./bin/functional | tee f.log\n", "red")
        lint_case("bare production run", clean + "      - run: ./build-production/bin/functional_production\n", "red")
        lint_case("undeclared variant", clean + "      - run: bash tools/run_functional.sh dev-tsan ./bin/functional\n", "red")
        lint_case("declared leg nobody runs", clean.replace("      - run: bash tools/run_functional.sh dev-portable ./bin/functional\n", ""), "red")
        lint_case("workflow sets the list seam", clean + "      - run: ROCKETNET_EXPECTED_SKIPS=x bash tools/run_functional.sh dev ./bin/functional\n", "red")
        lint_case("case missing from sources", clean, "red",
                  ("tests/functional/a.cpp", "bool p_root(); bool p_pin(); void rocketnet_test_b();\n"))
        lint_case("predicate missing from sources", clean, "red",
                  ("tests/functional/a.cpp", "void rocketnet_test_a(); bool p_root(); void rocketnet_test_b();\n"))

    total = 15 + 10 + 9
    if failures:
        for failure in failures:
            print("FAIL %s" % failure)
        print("check_skips self-test: %d of %d cases failed" % (len(failures), total))
        return 1
    print("ok: check_skips self-test, %d cases (red and green controls)" % total)
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if argv[1:] == ["--lint"]:
        try:
            errors, nlegs, nskips = lint(ROOT)
        except RuntimeError as error:
            print("check_skips: %s" % error)
            return 2
        for error in errors:
            print(error)
        if errors:
            print("check_skips --lint FAILED: %d problem(s)" % len(errors))
            return 1
        print("ok: %d legs, %d skippable rows, each with a reason, every leg run through run_functional.sh" % (nlegs, nskips))
        return 0
    args = argv[1:]
    variant, pin, shard = None, None, None
    while args and args[0].startswith("--"):
        if args[0] == "--variant" and len(args) > 1:
            variant, args = args[1], args[2:]
        elif args[0] == "--shard" and len(args) > 1:
            shard = parse_shard(args[1])
            if shard is None:
                print("check_skips: --shard is I/N with 0 <= I < N: %s" % args[1])
                return 2
            args = args[2:]
        elif args[0] == "--pin" and len(args) > 1:
            pin, args = args[1], args[2:]
        else:
            break
    if variant is None or len(args) != 1:
        print(__doc__)
        return 2
    try:
        with open(LIST, encoding="utf-8") as handle:
            legs, _, skips, errors = parse_list(handle.read())
        with open(args[0], encoding="utf-8", errors="replace") as handle:
            output = handle.read()
    except OSError as error:
        print("check_skips: %s" % error)
        return 2
    if errors:
        for error in errors:
            print(error)
        print("check_skips: tests/functional/expected_skips.txt is not well formed")
        return 1
    if pin is None:
        pin = os.environ.get("ROCKETNET_SERVER_TRANSPORT", "") or "auto"
    leg = "%s-%s" % (host_os(), variant)
    problems = check_output(legs, skips, leg, pin, output, shard)
    for problem in problems:
        print(problem)
    if problems:
        print("check_skips FAILED for leg %s: the suite's skips are not the leg's list (tests/functional/expected_skips.txt)" % leg)
        return 1
    count = sum(1 for case, (_, names, _) in skips.items() if leg in names
                and (shard is None or shard_of(case, shard[1]) == shard[0]))
    print("ok: leg %s skipped exactly its %d listed rows" % (leg, count))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
