#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""Every function in tests/dst_checks.h is reached by the DST run.

A DST check that the harness never calls checks nothing, while its unit
cases still read as coverage. This lint takes every function defined in
tests/dst_checks.h, the calls the run makes (tests/dst_harness.h and
tests/dst.cpp), and the calls each check makes to the others, and fails on
any function the run cannot reach.

    python3 tools/check_dst_checks_wired.py              # the repository
    python3 tools/check_dst_checks_wired.py --self-test  # a planted dead check goes red
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFINITION = re.compile(r"^static inline [^(]*?\b(rocketnet_dst_check_\w+)\s*\(", re.M)
CALL = re.compile(r"\b(rocketnet_dst_check_\w+)\s*\(")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def bodies(text):
    """Each defined function's name and body text (prototypes are skipped)."""
    out = {}
    for match in DEFINITION.finditer(text):
        name = match.group(1)
        depth = 0
        i = match.end() - 1
        # the parameter list
        while i < len(text):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        j = i + 1
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if j >= len(text) or text[j] != "{":
            continue    # a prototype
        depth = 0
        k = j
        while k < len(text):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        out[name] = text[j:k + 1]
    return out


def unreached(checks_text, run_texts):
    checks = bodies(strip_comments(checks_text))
    roots = set()
    for text in run_texts:
        roots.update(CALL.findall(strip_comments(text)))
    reached = set()
    pending = [name for name in roots if name in checks]
    while pending:
        name = pending.pop()
        if name in reached:
            continue
        reached.add(name)
        pending.extend(callee for callee in CALL.findall(checks[name]) if callee in checks and callee != name)
    return sorted(set(checks) - reached), len(checks)


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def check(root):
    tests = os.path.join(root, "tests")
    dead, total = unreached(read(os.path.join(tests, "dst_checks.h")),
                            [read(os.path.join(tests, "dst_harness.h")), read(os.path.join(tests, "dst.cpp"))])
    return dead, total


def self_test():
    checks = """
static inline void rocketnet_dst_check_fail( int * st ) { *st = 1; }
static inline void rocketnet_dst_check_live( int * st ) { rocketnet_dst_check_fail( st ); }
static inline void rocketnet_dst_check_dead( int * st );
static inline void rocketnet_dst_check_dead( int * st ) { rocketnet_dst_check_fail( st ); }
// rocketnet_dst_check_dead( st ) in a comment is not a call
"""
    harness = "void run( int * st ) { rocketnet_dst_check_live( st ); /* rocketnet_dst_check_dead( st ) */ }"
    dead, total = unreached(checks, [harness])
    if dead != ["rocketnet_dst_check_dead"] or total != 3:
        print("check_dst_checks_wired self-test FAILED: planted dead check not reported (got %r of %d)" % (dead, total))
        return 1
    dead, _ = unreached(checks, [harness + " void more( int * st ) { rocketnet_dst_check_dead( st ); }"])
    if dead:
        print("check_dst_checks_wired self-test FAILED: a called check reported dead: %r" % dead)
        return 1
    print("ok check_dst_checks_wired self-test: a planted uncalled check is refused, a called one passes")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    dead, total = check(ROOT)
    if dead:
        for name in dead:
            print("tests/dst_checks.h: %s is never reached by the DST run (tests/dst_harness.h, tests/dst.cpp); "
                  "wire it in or delete it with its unit cases" % name)
        print("check_dst_checks_wired FAILED: %d of %d functions unreached" % (len(dead), total))
        return 1
    print("ok check_dst_checks_wired: all %d functions in tests/dst_checks.h are reached by the DST run" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
