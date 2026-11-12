#!/usr/bin/env python3
"""Refuse a checker refusal that no self-test notices.

A fail-closed checker says no by `raise ValueError(...)`. For each line that
starts such a raise, this tool writes a copy of the checker with that one
raise turned into a call that raises nothing (a multi-line raise stays valid
Python), runs the checker's own self-test suite against the copy, and counts
the mutant killed when the suite fails. A mutant whose suite still passes is a
refusal that can be deleted with every test green: it is printed and the tool
exits 1. The unmutated suite must pass first.

Each run gets its own temporary tree holding the checker and its test at their
repository paths, and Python runs with -I -B: no bytecode cache, so a mutant
never runs an earlier mutant's stale .pyc. Mutants run in parallel.

usage: check_refusal_mutants.py [-j N] <checker.py> <test_checker.py>
       check_refusal_mutants.py --self-test
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RAISE = "raise ValueError("
NO_OP = "(lambda *a: None)("


def mutants(source):
    """(line number, mutated source) for each refusal line."""
    lines = source.split("\n")
    for index, text in enumerate(lines):
        if RAISE not in text:
            continue
        mutated = list(lines)
        mutated[index] = text.replace(RAISE, NO_OP, 1).replace(") from error", ")", 1)
        yield index + 1, "\n".join(mutated)


def suite_passes(checker_path, checker_source, test_path, test_source):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for relative, text in ((checker_path, checker_source), (test_path, test_source)):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
        refuse_compiled_cache(root, "before")
        result = subprocess.run([sys.executable, "-I", "-B", str(root / test_path)], cwd=root,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        refuse_compiled_cache(root, "after")
        return result.returncode == 0


class CompiledCache(Exception):
    pass


def refuse_compiled_cache(root, when):
    """A mutant must run from its own source, never a compiled cache: Python
    validates a .pyc by source mtime and size, and mutants of one size written
    within one second share both, so a cached earlier mutant runs instead (the
    first O7 scan counted wrong this way). Any cache in the tree refuses."""
    found = sorted(str(path.relative_to(root)) for path in root.rglob("*")
                   if path.name == "__pycache__" or path.suffix in (".pyc", ".pyo"))
    if found:
        raise CompiledCache(f"compiled cache {when} a mutant run: {', '.join(found)}")


def scan(checker_path, checker_source, test_path, test_source, jobs):
    """Returns (refusal count, survivor line numbers), or None when the
    unmutated suite fails."""
    if not suite_passes(checker_path, checker_source, test_path, test_source):
        return None
    found = list(mutants(checker_source))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        passed = list(pool.map(lambda pair: suite_passes(checker_path, pair[1], test_path, test_source), found))
    return len(found), [number for (number, _), survived in zip(found, passed) if survived]


SELF_TEST_CHECKER = '''
def check(value):
    if value < 0:
        raise ValueError("negative")
    if value > 9:
        raise ValueError(
            "over nine")
    return value
'''

SELF_TEST_SUITE = '''
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tools.checker import check

class Tests(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(check(3), 3)
    def test_negative(self):
        with self.assertRaisesRegex(ValueError, "negative"):
            check(-1)
COVER_OVER

if __name__ == "__main__":
    unittest.main()
'''

SELF_TEST_OVER = '''    def test_over(self):
        with self.assertRaisesRegex(ValueError, "over nine"):
            check(10)
'''


def self_test():
    failures = []
    checker, test = Path("tools/checker.py"), Path("tools/test_checker.py")
    covered = SELF_TEST_SUITE.replace("COVER_OVER", SELF_TEST_OVER)
    if scan(checker, SELF_TEST_CHECKER, test, covered, 2) != (2, []):
        failures.append("both refusals tested: expected 2 refusals, no survivor")
    uncovered = SELF_TEST_SUITE.replace("COVER_OVER", "")
    if scan(checker, SELF_TEST_CHECKER, test, uncovered, 2) != (2, [6]):
        failures.append("the multi-line refusal untested: expected line 6 to survive")
    broken = covered.replace('check(3), 3', 'check(3), 4')
    if scan(checker, SELF_TEST_CHECKER, test, broken, 2) is not None:
        failures.append("a failing unmutated suite was not refused")
    with tempfile.TemporaryDirectory() as directory:
        cache = Path(directory) / "tools" / "__pycache__"
        cache.mkdir(parents=True)
        (cache / "checker.cpython-313.pyc").write_bytes(b"")
        try:
            refuse_compiled_cache(Path(directory), "before")
            failures.append("a compiled cache in the mutant tree was not refused")
        except CompiledCache:
            pass
    for failure in failures:
        print(f"check_refusal_mutants self-test: FAIL {failure}")
    if not failures:
        print("check_refusal_mutants self-test: ok (4 cases)")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-j", "--jobs", type=int, default=4)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("checker", nargs="?")
    parser.add_argument("test", nargs="?")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.checker or not args.test:
        parser.error("a checker and its test suite are required")
    checker, test = Path(args.checker), Path(args.test)
    if checker.is_absolute() or test.is_absolute():
        parser.error("give repository-relative paths, from the repository root")
    try:
        result = scan(checker, checker.read_text(), test, test.read_text(), max(1, args.jobs))
    except CompiledCache as error:
        print(f"check_refusal_mutants: refused, {error}")
        return 1
    if result is None:
        print(f"check_refusal_mutants: {test} fails on the unmutated {checker}")
        return 1
    total, survivors = result
    for number in survivors:
        print(f"SURVIVED {checker}:{number}: the refusal can be deleted with {test} green")
    print(f"check_refusal_mutants {checker}: refusals={total} killed={total - len(survivors)} "
          f"survived={len(survivors)}")
    if total == 0:
        print(f"check_refusal_mutants: no `{RAISE}` refusal found in {checker}")
        return 1
    return 1 if survivors else 0


if __name__ == "__main__":
    raise SystemExit(main())
