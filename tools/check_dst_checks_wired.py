#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""Every function in tests/dst_checks.h is reached by the DST run.

A DST check that the harness never calls checks nothing, while its unit
cases still read as coverage. This lint takes every function defined in
tests/dst_checks.h and walks the run's call graph from its entry point, main
in tests/dst.cpp, through every function defined in the run's files
(tests/dst.cpp, tests/dst_outage_grid.h, tests/dst_harness.h) and in the checks
file, and fails on any check function the walk cannot reach. A check called
only from a harness helper the run never calls is unreached. A function named
anywhere in a body counts as reached (a call or a function pointer), and so
does one named in the run files' text outside any function body (a table, a
struct member function), which errs toward reached rather than toward a false
red.

    python3 tools/check_dst_checks_wired.py              # the repository
    python3 tools/check_dst_checks_wired.py --self-test  # a planted dead check goes red
    python3 tools/check_dst_checks_wired.py --root DIR   # DIR/tests; 0 functions is an error
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# a definition starts at column 0 with any return type and qualifiers
# (static inline, static, inline static, a bare type ...), so a check spelled
# without "static inline" is still a function of the file
CHECK_PREFIX = "rocketnet_dst_check_"
DEFINITION = re.compile(r"^(?!(?:return|if|while|for|switch|else|do|case|goto|sizeof|typedef)\b)"
                        r"(?:[A-Za-z_][\w:<>,]*[ \t*&]+)+([A-Za-z_]\w*)\s*\(", re.M)
IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")
ENTRY = "main"


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def bodies(text, residue=None):
    """Each defined function's name and body text (prototypes are skipped).

    Overloads of one name share one entry. When residue is a list, it gets the
    text outside every definition and prototype, as one string.
    """
    out = {}
    spans = []
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
        if spans and match.start() < spans[-1][1]:
            continue    # inside a definition already taken
        j = i + 1
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if j >= len(text) or text[j] != "{":
            # a prototype (or a column-0 call): its own text names nothing reached
            if j < len(text) and text[j] == ";":
                spans.append((match.start(), j + 1))
            continue
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
        out[name] = out.get(name, "") + text[j:k + 1]
        spans.append((match.start(), k + 1))
    if residue is not None:
        kept = []
        last = 0
        for start, end in spans:
            kept.append(text[last:start])
            last = end
        kept.append(text[last:])
        residue.append(" ".join(kept))
    return out


def unreached(checks_text, run_texts):
    """The check functions the run cannot reach from main, and how many there are."""
    checks = bodies(strip_comments(checks_text))
    functions = dict(checks)
    residues = []
    for text in run_texts:
        for name, body in bodies(strip_comments(text), residues).items():
            functions[name] = functions.get(name, "") + body
    roots = {ENTRY}
    for residue in residues:
        roots.update(IDENTIFIER.findall(residue))
    reached = set()
    pending = [name for name in roots if name in functions]
    while pending:
        name = pending.pop()
        if name in reached:
            continue
        reached.add(name)
        pending.extend(callee for callee in set(IDENTIFIER.findall(functions[name]))
                       if callee in functions and callee not in reached)
    names = [name for name in checks if name.startswith(CHECK_PREFIX)]
    return sorted(name for name in names if name not in reached), len(names)


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def check(root):
    tests = os.path.join(root, "tests")
    run = [read(os.path.join(tests, name)) for name in RUN_FILES]
    dead, total = unreached(read(os.path.join(tests, "dst_checks.h")), run)
    return dead, total


# the files the DST run is built from; main is in dst.cpp
RUN_FILES = ("dst.cpp", "dst_outage_grid.h", "dst_harness.h")


def self_test():
    checks = """
static inline void rocketnet_dst_check_fail( int * st ) { *st = 1; }
static inline void rocketnet_dst_check_live( int * st ) { rocketnet_dst_check_fail( st ); }
static inline void rocketnet_dst_check_dead( int * st );
static inline void rocketnet_dst_check_dead( int * st ) { rocketnet_dst_check_fail( st ); }
// rocketnet_dst_check_dead( st ) in a comment is not a call
"""
    harness = ("static void run( int * st );\n"
               "int main( void )\n{\n    int st = 0;\n    run( &st );\n    return st;\n}\n"
               "static void run( int * st ) { rocketnet_dst_check_live( st ); /* rocketnet_dst_check_dead( st ) */ }\n")
    dead, total = unreached(checks, [harness])
    if dead != ["rocketnet_dst_check_dead"] or total != 3:
        print("check_dst_checks_wired self-test FAILED: planted dead check not reported (got %r of %d)" % (dead, total))
        return 1
    # a check called only from a helper the run never calls is still dead
    more = "static void more( int * st ) { rocketnet_dst_check_dead( st ); }\n"
    dead, _ = unreached(checks, [harness + more])
    if dead != ["rocketnet_dst_check_dead"]:
        print("check_dst_checks_wired self-test FAILED: a check called only from an uncalled helper "
              "was counted reached (got %r)" % dead)
        return 1
    # the same helper, called from run (here through a function pointer in a
    # second run file), reaches it
    called = harness.replace("rocketnet_dst_check_live( st ); ", "rocketnet_dst_check_live( st ); helper( st ); ")
    dead, _ = unreached(checks, [called, "static void (* const helper_table[])( int * ) = { more };\n"
                                 "static void helper( int * st ) { helper_table[0]( st ); }\n" + more])
    if dead:
        print("check_dst_checks_wired self-test FAILED: a check reached through a helper reported dead: %r" % dead)
        return 1
    # no main, no run: every check is unreached
    dead, _ = unreached(checks, ["static void run( int * st ) { rocketnet_dst_check_live( st ); }\n"])
    if dead != ["rocketnet_dst_check_dead", "rocketnet_dst_check_fail", "rocketnet_dst_check_live"]:
        print("check_dst_checks_wired self-test FAILED: checks called only outside main's reach "
              "counted reached (got %r)" % dead)
        return 1
    # a dead check spelled without "static inline" is still found
    for spelling in ("static int", "inline static int", "int", "static inline const char *", "static bool"):
        planted = checks + "%s rocketnet_dst_check_spelled( int * st )\n{\n    return 0;\n}\n" % spelling
        dead, _ = unreached(planted, [harness])
        if dead != ["rocketnet_dst_check_dead", "rocketnet_dst_check_spelled"]:
            print("check_dst_checks_wired self-test FAILED: a dead check spelled %r not reported (got %r)" % (spelling, dead))
            return 1
    import tempfile
    with tempfile.TemporaryDirectory() as root:
        if main(["--root", root]) != 1:
            print("check_dst_checks_wired self-test FAILED: a tree with no DST files was accepted")
            return 1
        os.mkdir(os.path.join(root, "tests"))
        for name in ("dst_checks.h",) + RUN_FILES:
            open(os.path.join(root, "tests", name), "w").close()
        if main(["--root", root]) != 1:
            print("check_dst_checks_wired self-test FAILED: zero DST check functions was accepted")
            return 1
    print("ok check_dst_checks_wired self-test: a planted uncalled check is refused, a check called only from an uncalled helper is refused, a called one passes, "
          "a dead check spelled without static inline is refused, zero checks is red")
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if "--self-test" in argv:
        return self_test()
    root = argv[argv.index("--root") + 1] if "--root" in argv[:-1] else ROOT
    try:
        dead, total = check(root)
    except OSError as error:
        print("check_dst_checks_wired FAILED: 0 functions read: %s" % error)
        return 1
    if total == 0:
        # zero inputs is an error: an empty tests/dst_checks.h would otherwise pass
        print("check_dst_checks_wired FAILED: 0 functions in %s: nothing was checked"
              % os.path.join(root, "tests", "dst_checks.h"))
        return 1
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
