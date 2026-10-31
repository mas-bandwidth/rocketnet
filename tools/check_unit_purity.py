#!/usr/bin/env python3
"""Unit-suite purity check.

The owner's rule: unit tests use no real time, no sleep, no threads, no
network and no resolver. This check scans the unit suite's sources and fails,
naming file and line, on any call that would force a real platform primitive:
a sleep, a wall-clock read, a thread start, a hostname resolution, the C
library's time(), a std::thread, or a process-environment read or change. A
unit case that tests the SDK's environment handling passes the reader an
in-memory table it owns (tests/test_env_source.h), so that helper is scanned
like any other.

The scanned code is every tests/test_unit_*.cpp and sdk/rocketnet_tests.cpp
(the unit-registered cases live there); every tests/*.h helper they include,
directly or through another tests/*.h helper; and the body of every sdk DEV
fixture they call (a function with _test in its name defined in an sdk/*.cpp
file), where a thread or a timed wait would otherwise hide from this check.
A thread join, and a wake wait with a timeout other than 0, count as well.
Comments are stripped before matching, and linker-wrap symbol declarations
(__real_/__wrap_rocketnet_platform_time) are not calls.

One helper is not unit-case code and is not scanned: tests/suite_runner.h is
the runner that starts each case's process.

usage: python3 tools/check_unit_purity.py [dir]   (default the repository root)
       python3 tools/check_unit_purity.py --self-test
"""

import os
import re
import sys
import tempfile

PATTERNS = [
    ("rocketnet_platform_sleep", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_sleep\s*\(')),
    ("rocketnet_platform_time", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_time\s*\(')),
    ("rocketnet_platform_thread_create", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_thread_create\s*\(')),
    ("getaddrinfo", re.compile(r'(?<![A-Za-z0-9_])getaddrinfo\s*\(')),
    ("std::thread", re.compile(r'std::thread')),
    ("time(", re.compile(r'(?<![A-Za-z0-9_])time\s*\(')),
    ("setenv", re.compile(r'(?<![A-Za-z0-9_])setenv\s*\(')),
    ("unsetenv", re.compile(r'(?<![A-Za-z0-9_])unsetenv\s*\(')),
    ("putenv", re.compile(r'(?<![A-Za-z0-9_])putenv\s*\(')),
    ("_putenv_s", re.compile(r'(?<![A-Za-z0-9_])_putenv_s\s*\(')),
    ("SetEnvironmentVariable", re.compile(r'(?<![A-Za-z0-9_])SetEnvironmentVariable(?:A|W)?\s*\(')),
    ("getenv", re.compile(r'(?<![A-Za-z0-9_])(?:secure_)?getenv(?:_s)?\s*\(')),
    ("_dupenv_s", re.compile(r'(?<![A-Za-z0-9_])_dupenv_s\s*\(')),
    ("GetEnvironmentVariable", re.compile(r'(?<![A-Za-z0-9_])GetEnvironmentVariable(?:A|W)?\s*\(')),
    ("environ", re.compile(r'(?<![A-Za-z0-9_])_?environ(?![A-Za-z0-9_])')),
    ("rocketnet_platform_getenv", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_getenv\s*\(')),
    ("rocketnet_test_getenv", re.compile(r'(?<![A-Za-z0-9_])rocketnet_test_getenv\s*\(')),
    ("rocketnet_env_process", re.compile(r'(?<![A-Za-z0-9_])rocketnet_env_process\s*\(')),
    ("rocketnet_platform_thread_join", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_thread_join\s*\(')),
]

# a wake wait is a real OS wait unless its timeout is zero (a poll)
WAKE_WAIT = re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_wake_wait\s*\(([^;]*)\)\s*;')
ZERO_TIMEOUT = re.compile(r'^\s*0(?:\.0*)?(?:f)?\s*$')

HELPERS_NOT_SCANNED = {"suite_runner.h"}


def strip_comments(text):
    """Mask comments and literals while preserving source line positions."""
    # Raw strings must precede normal strings. The delimiter backreference
    # prevents embedded quotes or comment markers from ending a raw literal.
    tokens = re.compile(
        r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
        r'|"(?:\\.|[^"\\])*"'
        r"|'(?:\\.|[^'\\])*'"
        r'|/\*.*?\*/|//[^\n]*',
        re.DOTALL,
    )
    return tokens.sub(lambda match: ''.join('\n' if c == '\n' else ' '
                                           for c in match.group()), text)


def scan_source(source, first_line=1):
    """Findings in comment-stripped source, as (line, name)."""
    findings = []
    for name, regex in PATTERNS:
        for match in regex.finditer(source):
            findings.append((first_line + source.count('\n', 0, match.start()), name))
    for match in WAKE_WAIT.finditer(source):
        timeout = match.group(1).split(',')[-1]
        if not ZERO_TIMEOUT.match(timeout):
            findings.append((first_line + source.count('\n', 0, match.start()), "rocketnet_platform_wake_wait with a timeout"))
    return sorted(findings)


def scan_lines(lines, prefix):
    return scan_source(strip_comments(''.join(lines)))


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"/]+\.h)"', re.M)
CALL = re.compile(r'(?<![A-Za-z0-9_])([A-Za-z_]\w*)\s*\(')
DEFINITION = re.compile(r'^[A-Za-z_][\w \t\*&:<>]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', re.M)


def helpers(root, unit_files):
    """The tests/*.h helpers the unit files include, transitively within tests/."""
    tests_dir = os.path.join(root, "tests")
    found = []
    pending = []
    for path in unit_files:
        pending.extend(INCLUDE.findall(read(path)))
    seen = set()
    while pending:
        name = pending.pop()
        if name in seen or name in HELPERS_NOT_SCANNED:
            continue
        seen.add(name)
        path = os.path.join(tests_dir, name)
        if not os.path.exists(path):
            continue
        found.append(path)
        pending.extend(INCLUDE.findall(read(path)))
    return sorted(found)


def fixture_bodies(root, unit_files):
    """(path, first line, body) of each sdk DEV fixture the unit files call."""
    called = set()
    for path in unit_files:
        called.update(name for name in CALL.findall(strip_comments(read(path))) if "_test" in name)
    out = []
    sdk_dir = os.path.join(root, "sdk")
    if not os.path.isdir(sdk_dir):
        return out
    for entry in sorted(os.listdir(sdk_dir)):
        if not entry.endswith(".cpp") or entry == "rocketnet_tests.cpp":
            continue
        path = os.path.join(sdk_dir, entry)
        source = strip_comments(read(path))
        for match in DEFINITION.finditer(source):
            if match.group(1) not in called:
                continue
            start = match.end() - 1
            depth = 0
            end = start
            for end in range(start, len(source)):
                if source[end] == "{":
                    depth += 1
                elif source[end] == "}":
                    depth -= 1
                    if depth == 0:
                        break
            out.append((path, source.count('\n', 0, start) + 1, source[start:end + 1]))
    return out


def check(root):
    """Every finding as (path, line, name)."""
    files = []
    tests_dir = os.path.join(root, "tests")
    for entry in sorted(os.listdir(tests_dir)):
        if entry.startswith("test_unit_") and entry.endswith(".cpp"):
            files.append(os.path.join(tests_dir, entry))
    sdk_tests = os.path.join(root, "sdk", "rocketnet_tests.cpp")
    if os.path.exists(sdk_tests):
        files.append(sdk_tests)

    failures = []
    for path in files + helpers(root, files):
        for lineno, name in scan_source(strip_comments(read(path))):
            failures.append((path, lineno, name))
    for path, first_line, body in fixture_bodies(root, files):
        for lineno, name in scan_source(body, first_line):
            failures.append((path, lineno, name))
    return failures


def self_test():
    """A thread or timed wait planted in a DEV fixture or a helper goes red."""
    clean_fixture = "bool rocketnet_thing_test_steps()\n{\n    rocketnet_platform_wake_wait( w, false, 0.0 );\n    return true;\n}\n"
    cases = [
        ("clean", clean_fixture, "static inline int helper_value() { return 1; }\n", 0),
        ("fixture thread", clean_fixture.replace("return true;", "t = rocketnet_platform_thread_create( NULL, f, a );\n    return true;"),
         "static inline int helper_value() { return 1; }\n", 1),
        ("fixture timed wait", clean_fixture.replace("0.0 )", "0.1 )"), "static inline int helper_value() { return 1; }\n", 1),
        ("helper sleep", clean_fixture, "static inline int helper_value() { rocketnet_platform_sleep( 0.01 ); return 1; }\n", 1),
        ("helper real setenv", clean_fixture, "static inline int helper_value() { return setenv( \"ROCKETNET_SERVER_MAX_CLIENTS\", \"2\", 1 ); }\n", 1),
        ("helper real getenv", clean_fixture, "static inline int helper_value() { return getenv( \"ROCKETNET_SERVER_MAX_CLIENTS\" ) != 0; }\n", 1),
        ("helper process source", clean_fixture, "static inline int helper_value() { return rocketnet_env_process() != 0; }\n", 1),
    ]
    for label, fixture, helper, expected in cases:
        with tempfile.TemporaryDirectory() as root:
            os.mkdir(os.path.join(root, "tests"))
            os.mkdir(os.path.join(root, "sdk"))
            with open(os.path.join(root, "tests", "test_unit_thing.cpp"), "w") as fh:
                fh.write('#include "thing_helper.h"\nbool rocketnet_thing_test_steps();\n'
                         'static void test_thing() { check( rocketnet_thing_test_steps() && helper_value() ); }\n')
            with open(os.path.join(root, "tests", "thing_helper.h"), "w") as fh:
                fh.write(helper)
            with open(os.path.join(root, "sdk", "rocketnet_thing.cpp"), "w") as fh:
                fh.write("static void worker() { rocketnet_platform_sleep( 1.0 ); }\n" + fixture)
            found = check(root)
            if len(found) != expected:
                print("FAIL check_unit_purity self-test: %s: expected %d finding(s), got %r" % (label, expected, found))
                return 1
    # the environment table helper is scanned by its real name: a real
    # environment change planted in it, or in a unit case, is refused
    for label, helper, case_line, expected in [
        ("table helper", "static inline int helper_value() { return 1; }\n", "", 0),
        ("table helper setenv", "static inline int helper_value() { return setenv( \"ROCKETNET_CLIENT_TRANSPORT\", \"loopback\", 1 ); }\n", "", 1),
        ("unit case setenv", "static inline int helper_value() { return 1; }\n",
         'static void test_planted() { setenv( "ROCKETNET_SERVER_MAX_CLIENTS", "2", 1 ); }\n', 1),
        ("unit case _putenv_s", "static inline int helper_value() { return 1; }\n",
         'static void test_planted() { _putenv_s( "ROCKETNET_SERVER_MAX_CLIENTS", "2" ); }\n', 1),
    ]:
        with tempfile.TemporaryDirectory() as root:
            os.mkdir(os.path.join(root, "tests"))
            os.mkdir(os.path.join(root, "sdk"))
            with open(os.path.join(root, "tests", "test_unit_thing.cpp"), "w") as fh:
                fh.write('#include "test_env_source.h"\n'
                         'static void test_thing() { check( helper_value() == 1 ); }\n' + case_line)
            with open(os.path.join(root, "tests", "test_env_source.h"), "w") as fh:
                fh.write(helper)
            found = check(root)
            if len(found) != expected:
                print("FAIL check_unit_purity self-test: %s: expected %d finding(s), got %r" % (label, expected, found))
                return 1
    print("ok check_unit_purity self-test: a thread, timed wait, sleep or process-environment read or change "
          "in a unit case, a DEV fixture or a helper (test_env_source.h included) is refused")
    return 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    try:
        failures = check(root)
    except OSError as exc:
        print("FAIL check_unit_purity: %s" % exc)
        return 1

    if failures:
        for path, lineno, name in failures:
            print("FAIL check_unit_purity: %s:%d: %s" % (os.path.relpath(path, root), lineno, name))
        return 1

    print("ok check_unit_purity: no real-time, sleep, thread, timed wait, resolver or process-environment read or change "
          "in the unit suite, its tests/*.h helpers or the sdk DEV fixtures it calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
