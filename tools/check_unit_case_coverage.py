#!/usr/bin/env python3
"""Every registered unit case runs in at least one CI build configuration.

The unit suite (bin/test) registers its cases three ways:

  * REGISTER_UNIT_TEST( name ) and REGISTER_UNIT_TEST_EXPECTED_FAIL( name, card )
    in tests/test_unit_*.cpp, the files CMakeLists.txt globs into the suite;
  * the { "name", function } rows of rocketnet_unit_tests[] in
    sdk/rocketnet_tests.cpp;
  * the { "name", "Alias", function } rows of channels_tests[] in
    tests/test_channels.cpp.

A registration can sit behind a preprocessor guard, and a whole translation
unit can be left out of a build by CMakeLists.txt, so a case can compile into
no configuration CI builds and never run. Each CI job that runs the suite
saves what it ran as one file: `bin/test --list`, with the environment of its
run (`--list` prints the cases a run in that environment starts:
suite_list_cases, the run's own selection, nightly tier included only under
ROCKETNET_TEST_NIGHTLY=1), or the suite's own run output as a .log file. This
check reads every file and fails on:

  * a registered case named in no file (it runs nowhere);
  * a named case the scan above did not find (the scan has a blind spot, so
    its verdict cannot be trusted);
  * an expected configuration with no file, or an empty one;
  * a REGISTER_UNIT_TEST in a tracked C or C++ file that CMakeLists.txt
    builds into nothing: not one of the tests/test_unit_*.cpp files the
    unit suite globs, and not a source CMakeLists.txt names (the source
    audit binary's files). Such a case is invisible to the scan and runs
    nowhere (VERIFY-3 W10: tests/test_planted_orphan.cpp and
    tests/functional/test_unit_planted2.cpp each passed).

A file ending .log is the suite's own output from a run (`bin/test` prints
each case it ran, by name, and `*** ALL TESTS PASSED ***` at the end): only
the cases that run printed count, and a log without the banner is refused.
A file ending .txt is a `bin/test --list` listing: what a binary compiles in,
which is not what it runs (VERIFY-3 W10, VERIFY-4 W1: --list names the
nightly-tier cases, which a per-commit run leaves out). Lists remain readable
for the configurations that have not moved to run logs.

The nightly-tier cases (tests/test.cpp, rocketnet_unit_nightly_cases) run in
nightly.yml's unit nightly-tier step (ROCKETNET_TEST_NIGHTLY=1 ./bin/test);
they count as run there only while that step exists.

Usage:
  check_unit_case_coverage.py --repo . --expect NAME... -- FILE...

A file's configuration name is its file name without the extension.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REGISTER_RE = re.compile(r"^\s*REGISTER_UNIT_TEST(?:_EXPECTED_FAIL)?\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*[,)]", re.M)
ROW_RE = re.compile(r'^\s*\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,', re.M)

# (file, the line that opens its table)
TABLES = (
    ("sdk/rocketnet_tests.cpp", "rocketnet_unit_tests[] = {"),
    ("tests/test_channels.cpp", "channels_tests[] = {"),
)


def table_rows(text: str, opener: str, path: str) -> list[str]:
    start = text.find(opener)
    if start < 0:
        raise ValueError(f"{path}: table opener '{opener}' not found")
    end = text.find("\n};", start)
    if end < 0:
        raise ValueError(f"{path}: table opened by '{opener}' is not closed")
    names = ROW_RE.findall(text[start + len(opener):end])
    if not names:
        raise ValueError(f"{path}: table opened by '{opener}' has no rows")
    return names


def registered_cases(repo: Path) -> dict[str, str]:
    """Every registered case name, mapped to the file that registers it."""
    cases: dict[str, str] = {}

    def add(name: str, where: str) -> None:
        if name in cases and cases[name] != where:
            raise ValueError(f"{name} is registered in both {cases[name]} and {where}")
        cases[name] = where

    for path in sorted((repo / "tests").glob("test_unit_*.cpp")):
        relative = path.relative_to(repo).as_posix()
        for name in REGISTER_RE.findall(path.read_text(encoding="utf-8", errors="replace")):
            add(name, relative)

    for relative, opener in TABLES:
        text = (repo / relative).read_text(encoding="utf-8", errors="replace")
        for name in table_rows(text, opener, relative):
            add(name, relative)

    return cases


UNIT_GLOB_LINE = '"${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unit_*.cpp"'
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc")
CASE_LINE = re.compile(r"^test_[a-z0-9_]+$")
PASSED_BANNER = "*** ALL TESTS PASSED ***"
NIGHTLY_TABLE = ("tests/test.cpp", "rocketnet_unit_nightly_cases[] = {")
NIGHTLY_ROW = re.compile(r'^\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,', re.M)


def tracked_sources(repo: Path) -> list[str]:
    """Every tracked C or C++ file, relative; every such file on disk when repo is not a git work tree."""
    try:
        out = subprocess.run(["git", "-C", str(repo), "ls-files", "-z"], check=True, capture_output=True).stdout
        names = [n for n in out.decode("utf-8").split("\0") if n]
    except (OSError, subprocess.CalledProcessError):
        names = [p.relative_to(repo).as_posix() for p in repo.rglob("*") if p.is_file()]
    return sorted(n for n in names if n.endswith(SOURCE_SUFFIXES))


def stray_registrations(repo: Path) -> list[str]:
    """A registration CMakeLists.txt builds into nothing, one error each."""
    cmake_path = repo / "CMakeLists.txt"
    if not cmake_path.is_file():
        return ["CMakeLists.txt not found: cannot tell which registrations are built"]
    cmake = cmake_path.read_text(encoding="utf-8", errors="replace")
    errors = []
    if UNIT_GLOB_LINE not in cmake:
        errors.append(f"CMakeLists.txt no longer globs {UNIT_GLOB_LINE}: the scan's unit file set is unknown")
    for relative in tracked_sources(repo):
        if re.fullmatch(r"tests/test_unit_[^/]*\.cpp", relative):
            continue
        names = REGISTER_RE.findall((repo / relative).read_text(encoding="utf-8", errors="replace"))
        if not names or re.search(r"(?<![\w/.-])" + re.escape(relative) + r"(?![\w/.-])", cmake):
            continue
        for name in names:
            errors.append(f"{name} ({relative}) is registered in a file CMakeLists.txt builds into nothing "
                          "(the unit suite globs tests/test_unit_*.cpp), so it never runs")
    return errors


def nightly_cases(repo: Path) -> list[str]:
    relative, opener = NIGHTLY_TABLE
    path = repo / relative
    if not path.is_file():
        return []
    text = path.read_text(encoding="utf-8", errors="replace")
    start = text.find(opener)
    if start < 0:
        return []
    end = text.find("\n};", start)
    return NIGHTLY_ROW.findall(text[start + len(opener):end])


def nightly_step_runs_the_suite(repo: Path) -> bool:
    """nightly.yml has a step with ROCKETNET_TEST_NIGHTLY "1" whose run is ./bin/test."""
    path = repo / ".github" / "workflows" / "nightly.yml"
    if not path.is_file():
        return False
    steps = re.split(r"(?m)^\s*- (?=name:|uses:|run:)", path.read_text(encoding="utf-8", errors="replace"))
    return any(re.search(r'(?m)^\s*ROCKETNET_TEST_NIGHTLY:\s*"1"\s*$', step)
               and re.search(r"(?m)^\s*run:\s*\./bin/test\s*$", step) for step in steps)


def read_list(path: Path) -> set[str]:
    """The cases a file names: a run log's printed cases, or a listing's lines."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".log":
        if PASSED_BANNER not in text:
            raise ValueError(f"{path}: a run log without '{PASSED_BANNER}': not a passing run, so it shows nothing ran")
        return {line.strip() for line in text.splitlines() if CASE_LINE.match(line.strip())}
    return {line.strip() for line in text.splitlines() if line.strip()}


def check(repo: Path, lists: list[Path], expect: list[str]) -> list[str]:
    errors: list[str] = []

    try:
        cases = registered_cases(repo)
    except (OSError, ValueError) as error:
        return [str(error)]
    errors.extend(stray_registrations(repo))

    configurations: dict[str, set[str]] = {}
    for path in lists:
        name = path.stem
        if name in configurations:
            errors.append(f"two list files name configuration {name}")
            continue
        try:
            names = read_list(path)
        except ValueError as error:
            errors.append(str(error))
            continue
        if not names:
            errors.append(f"{path}: empty list for configuration {name}")
        configurations[name] = names

    for name in expect:
        if name not in configurations:
            errors.append(f"no list file for expected configuration {name}")

    listed: set[str] = set().union(*configurations.values()) if configurations else set()
    if nightly_step_runs_the_suite(repo):
        listed |= set(nightly_cases(repo)) & cases.keys()

    for name in sorted(listed - cases.keys()):
        where = ", ".join(sorted(c for c, names in configurations.items() if name in names))
        errors.append(f"{name} is listed by {where} but no registration was found for it: the scan has a blind spot")

    for name in sorted(cases.keys() - listed):
        errors.append(f"{name} ({cases[name]}) is compiled into no CI configuration, so it never runs")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--expect", nargs="*", default=[], help="configuration names that must each have a list file")
    parser.add_argument("lists", nargs="*", type=Path, help="bin/test --list output, one file per configuration")
    args = parser.parse_args(argv)

    if not args.lists:
        print("ERROR: no list files", file=sys.stderr)
        return 1

    errors = check(args.repo, args.lists, args.expect)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    cases = registered_cases(args.repo)
    print(f"{len(cases)} registered unit cases; each runs in at least one of {len(args.lists)} CI configurations "
          f"({', '.join(sorted(p.stem for p in args.lists))})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
