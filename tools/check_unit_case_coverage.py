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
saves `bin/test --list` as one list file. This check reads every list file and
fails on:

  * a registered case named in no list (it runs nowhere);
  * a listed case the scan above did not find (the scan has a blind spot, so
    its verdict cannot be trusted);
  * an expected configuration with no list file, or an empty list.

Usage:
  check_unit_case_coverage.py --repo . --expect NAME... -- LIST...

A list file's configuration name is its file name without the extension.
"""

from __future__ import annotations

import argparse
import re
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


def read_list(path: Path) -> set[str]:
    return {line.strip() for line in path.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()}


def check(repo: Path, lists: list[Path], expect: list[str]) -> list[str]:
    errors: list[str] = []

    try:
        cases = registered_cases(repo)
    except (OSError, ValueError) as error:
        return [str(error)]

    configurations: dict[str, set[str]] = {}
    for path in lists:
        name = path.stem
        if name in configurations:
            errors.append(f"two list files name configuration {name}")
            continue
        names = read_list(path)
        if not names:
            errors.append(f"{path}: empty list for configuration {name}")
        configurations[name] = names

    for name in expect:
        if name not in configurations:
            errors.append(f"no list file for expected configuration {name}")

    listed: set[str] = set().union(*configurations.values()) if configurations else set()

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
