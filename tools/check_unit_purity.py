#!/usr/bin/env python3
"""Unit-suite purity check.

The owner's rule: unit tests use no real time, no sleep, no threads, no
network and no resolver. This check scans the unit suite's sources and fails,
naming file and line, on any call that would force a real platform primitive:
a sleep, a wall-clock read, a thread start, a hostname resolution, the C
library's time(), a std::thread, or a process-environment mutation. The OS
and standard-library forms count too: std::this_thread, usleep, nanosleep,
std::chrono clocks, clock_gettime, gettimeofday, QueryPerformanceCounter,
mach_absolute_time, pthread_create, std::async, and the socket(), sendto,
recvfrom and sendmsg/recvmsg family. The real clock or sleep handed over as
a function value (rocketnet_time, rocketnet_sleep and their platform forms
named without a call) counts too: that is how a bounded wait polls the real
clock and sleeps on a thread the library started, and the call it makes is
in examples/shutdown.h, which no unit case should reach.

The library's own wrappers count by name: rocketnet_platform_sleep, _time,
_thread_create, _thread_join, _thread_wait, _thread_sleep, _hostname_resolve,
and a wake wait (readable or writable) whose timeout is not 0; so do the C
library's sleep() and Windows' Sleep().

The scanned code is every tests/test_unit_*.cpp, tests/test_channels.cpp and
sdk/rocketnet_tests.cpp (the unit-registered cases live there: test.cpp
registers the channels table's rocketnet_channels_test_* cases); every tests/*.h helper they include,
directly or through another tests/*.h helper; and the body of every sdk DEV
fixture they call (a function with _test in its name defined in an sdk/*.cpp
file), where a thread or a timed wait would otherwise hide from this check.
A thread join, and a wake wait with a timeout other than 0, count as well.
Comments are stripped before matching, and linker-wrap symbol declarations
(__real_/__wrap_rocketnet_platform_time) are not calls.

Three files are not unit-case code and are not scanned: tests/test.cpp is the
unit binary's main, which clears and restores the process environment around
the cases it starts and registers no case body of its own; tests/suite_runner.h
is the runner that starts each case's process, and tests/test_env_source.h
sets the real environment only in a ROCKETNET_DEV=0 build, which has no
in-memory overlay (tests/README.md).

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
    ("rocketnet_platform_thread_wait", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_thread_wait\s*\(')),
    ("rocketnet_platform_thread_sleep", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_thread_sleep\s*\(')),
    ("rocketnet_platform_hostname_resolve", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_hostname_resolve\s*\(')),
    ("getaddrinfo", re.compile(r'(?<![A-Za-z0-9_])getaddrinfo\s*\(')),
    ("sleep(", re.compile(r'(?<![A-Za-z0-9_.>:])(?:sleep|Sleep)\s*\(')),
    ("std::thread", re.compile(r'std::thread')),
    ("time(", re.compile(r'(?<![A-Za-z0-9_])time\s*\(')),
    ("setenv", re.compile(r'(?<![A-Za-z0-9_])setenv\s*\(')),
    ("unsetenv", re.compile(r'(?<![A-Za-z0-9_])unsetenv\s*\(')),
    ("putenv", re.compile(r'(?<![A-Za-z0-9_])putenv\s*\(')),
    ("_putenv_s", re.compile(r'(?<![A-Za-z0-9_])_putenv_s\s*\(')),
    ("SetEnvironmentVariable", re.compile(r'(?<![A-Za-z0-9_])SetEnvironmentVariable(?:A|W)?\s*\(')),
    ("rocketnet_platform_thread_join", re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_thread_join\s*\(')),
    ("std::this_thread", re.compile(r'std::this_thread\s*::')),
    ("usleep", re.compile(r'(?<![A-Za-z0-9_])usleep\s*\(')),
    ("nanosleep", re.compile(r'(?<![A-Za-z0-9_])nanosleep\s*\(')),
    ("std::chrono clock", re.compile(r'std::chrono\s*::\s*\w*clock')),
    ("clock_gettime", re.compile(r'(?<![A-Za-z0-9_])clock_gettime\s*\(')),
    ("gettimeofday", re.compile(r'(?<![A-Za-z0-9_])gettimeofday\s*\(')),
    ("QueryPerformanceCounter", re.compile(r'(?<![A-Za-z0-9_])QueryPerformanceCounter\s*\(')),
    ("mach_absolute_time", re.compile(r'(?<![A-Za-z0-9_])mach_absolute_time\s*\(')),
    ("pthread_create", re.compile(r'(?<![A-Za-z0-9_])pthread_create\s*\(')),
    ("std::async", re.compile(r'std::async\s*\(')),
    ("socket(", re.compile(r'(?<![A-Za-z0-9_])socket\s*\(')),
    ("sendto/recvfrom", re.compile(r'(?<![A-Za-z0-9_])(?:sendto|recvfrom|sendmsg|recvmsg|sendmmsg|recvmmsg)\s*\(')),
    # the real clock or sleep handed over as a function value: a bounded wait
    # (rn_cli_bounded_wait) that polls the real clock and sleeps, which a
    # unit case needs only to wait on a thread the library started
    ("real clock or sleep passed as a value", re.compile(
        r'(?<![A-Za-z0-9_])(?:rocketnet_time|rocketnet_sleep|rocketnet_platform_time|rocketnet_platform_sleep)'
        r'(?![A-Za-z0-9_])(?!\s*\()')),
]

# a wake wait (readable or writable) is a real OS wait unless its timeout, the
# last argument, is zero (a poll)
WAKE_WAIT = re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_wake_wait(?:_writable)?\s*\(([^;]*)\)\s*;')
ZERO_TIMEOUT = re.compile(r'^\s*0(?:\.0*)?(?:f)?\s*$')

HELPERS_NOT_SCANNED = {"suite_runner.h", "test_env_source.h"}


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
    for extra in (os.path.join(tests_dir, "test_channels.cpp"), os.path.join(root, "sdk", "rocketnet_tests.cpp")):
        if os.path.exists(extra):
            files.append(extra)
    if not files:
        # zero inputs is an error: a tree with no unit source would otherwise pass unread
        return [(tests_dir, 0, "0 unit source files (tests/test_unit_*.cpp, sdk/rocketnet_tests.cpp): nothing was checked")]

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
    ]
    for call in ("std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) )", "usleep( 1000 )",
                 "nanosleep( &t, NULL )", "std::chrono::steady_clock::now()", "clock_gettime( CLOCK_MONOTONIC, &t )",
                 "gettimeofday( &v, NULL )", "QueryPerformanceCounter( &c )", "mach_absolute_time()",
                 "pthread_create( &p, NULL, f, NULL )", "std::async( f )", "socket( AF_INET, SOCK_DGRAM, 0 )",
                 "sendto( s, b, n, 0, a, l )", "recvfrom( s, b, n, 0, a, &l )", "sendmmsg( s, m, n, 0 )",
                 "rn_cli_bounded_wait( o, done, NULL, rocketnet_time, NULL, \"o\" )",
                 "rn_cli_bounded_wait( o, done, NULL, NULL, rocketnet_sleep, \"o\" )",
                 "clock_fn = &rocketnet_platform_time",
                 "rocketnet_platform_thread_wait( t, 1.0 )", "rocketnet_platform_thread_sleep( t, 0.5 )",
                 "rocketnet_platform_hostname_resolve( \"h\", \"1\", &a )",
                 "rocketnet_platform_wake_wait_writable( w, 0.25 )", "sleep( 1 )", "Sleep( 10 )"):
        cases.append(("helper " + call.split("(")[0].strip(), clean_fixture,
                      "static inline int helper_value() { %s; return 1; }\n" % call, 1))
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
    # tests/test_channels.cpp holds registered unit cases too; a polling
    # writable wait (timeout 0) is not a timed wait
    for label, body, expected in (
            ("channels sleep", "static void c() { rocketnet_platform_sleep( 0.01 ); }\n", 1),
            ("channels zero writable wait", "static void c() { rocketnet_platform_wake_wait_writable( w, 0.0 ); }\n", 0)):
        with tempfile.TemporaryDirectory() as root:
            os.mkdir(os.path.join(root, "tests"))
            with open(os.path.join(root, "tests", "test_channels.cpp"), "w") as fh:
                fh.write(body)
            found = check(root)
            if len(found) != expected or (expected and not found[0][0].endswith("test_channels.cpp")):
                print("FAIL check_unit_purity self-test: %s: expected %d finding(s) in test_channels.cpp, got %r"
                      % (label, expected, found))
                return 1
    with tempfile.TemporaryDirectory() as root:
        os.mkdir(os.path.join(root, "tests"))
        found = check(root)
        if len(found) != 1 or "0 unit source files" not in found[0][2]:
            print("FAIL check_unit_purity self-test: zero unit sources: expected 1 finding, got %r" % (found,))
            return 1
    print("ok check_unit_purity self-test: a thread, timed wait or sleep in a DEV fixture or helper, "
          "and a tree with no unit source, are refused")
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

    print("ok check_unit_purity: no real-time, sleep, thread, timed wait, resolver or process-environment mutation "
          "in the unit suite, its tests/*.h helpers or the sdk DEV fixtures it calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
