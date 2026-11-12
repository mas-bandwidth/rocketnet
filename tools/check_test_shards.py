#!/usr/bin/env python3
"""The suite's shards partition its cases (tests/suite_runner.h, --shard).

Lists the cases a suite binary's run starts (`<binary> --list`) and the cases
each shard's run keeps (`<binary> --list --shard i/n`, 0 <= i < n). The lists
come from the run's own selection (suite_list_cases: suite_collect, then
suite_keep_shard), so a case a shard run drops is missing here too. Exits 1
unless every case is
in exactly one shard, no shard lists a case the suite does not, and every
shard has at least one case. Prints each shard's count and their sum.

Usage: check_test_shards.py <binary> <n>
       check_test_shards.py --self-test
"""

import subprocess
import sys


def check(full, shards, out=sys.stdout):
    """full: the unsharded case list; shards: one case list per shard."""
    errors = []
    seen = {}
    if not full:
        # zero inputs is an error: a suite that lists no case partitions nothing
        errors.append("the unsharded list has 0 cases: nothing was checked")
    for index, names in enumerate(shards):
        print(f"shard {index}/{len(shards)}: {len(names)} cases", file=out)
        if not names:
            errors.append(f"shard {index}/{len(shards)} has no case")
        for name in names:
            if name in seen:
                errors.append(f"{name}: in shard {seen[name]} and shard {index}")
            seen.setdefault(name, index)
    total = sum(len(names) for names in shards)
    print(f"sum {total}, unsharded {len(full)}", file=out)
    for name in full:
        if name not in seen:
            errors.append(f"{name}: in no shard")
    for name in seen:
        if name not in full:
            errors.append(f"{name}: in a shard but not in the unsharded list")
    if len(set(full)) != len(full):
        errors.append("the unsharded list repeats a case")
    for error in errors:
        print(f"SHARD-FAIL {error}", file=out)
    if not errors:
        print(f"SHARD-PASS {len(shards)} shards partition the {len(full)} cases", file=out)
    return 0 if not errors else 1


def listed(binary, *args):
    run = subprocess.run([binary, "--list", *args], check=True, capture_output=True, text=True)
    return [line.strip() for line in run.stdout.splitlines() if line.strip()]


def self_test():
    import io
    full = ["test_a", "test_b", "test_c"]
    cases = [
        ("partition", [["test_a", "test_c"], ["test_b"]], 0, "SHARD-PASS"),
        ("a case in two shards", [["test_a", "test_c"], ["test_b", "test_a"]], 1, "test_a: in shard 0 and shard 1"),
        ("a case in no shard", [["test_a"], ["test_b"]], 1, "test_c: in no shard"),
        ("an unknown case", [["test_a", "test_c"], ["test_b", "test_x"]], 1, "test_x: in a shard but not"),
        ("an empty shard", [full, []], 1, "shard 1/2 has no case"),
        ("zero cases", [[]], 1, "the unsharded list has 0 cases"),
    ]
    failed = 0
    for label, shards, want_rc, want_text in cases:
        out = io.StringIO()
        rc = check([] if label == "zero cases" else full, shards, out)
        if rc != want_rc or want_text not in out.getvalue():
            print(f"self-test FAIL {label}: rc {rc}\n{out.getvalue()}")
            failed = 1
        else:
            print(f"ok {label}")
    return failed


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 2 or not argv[1].isdigit() or int(argv[1]) < 1:
        print(__doc__, file=sys.stderr)
        return 2
    binary, n = argv[0], int(argv[1])
    return check(listed(binary), [listed(binary, "--shard", f"{i}/{n}") for i in range(n)])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
