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

The scanned code is every tests/test_unit_*.cpp and sdk/rocketnet_tests.cpp
(the unit-registered cases live there); every tests/*.h helper they include,
directly or through another tests/*.h helper; and the body of every sdk DEV
fixture they call (a function with _test in its name defined in an sdk/*.cpp
file), where a thread or a timed wait would otherwise hide from this check.
A thread join, and a wake wait with a timeout other than 0, count as well.
Comments are stripped before matching, and linker-wrap symbol declarations
(__real_/__wrap_rocketnet_platform_time) are not calls.

VERIFY-3 W7 found the list short of the spellings a unit case could use:
sleep, Sleep, clock, std::jthread, CreateThread, gethostbyname, socketpair,
poll and select, GetTickCount64, timespec_get, a condition variable's
wait_for, fork, connect and recv each passed, and so did a helper included
by a path with a slash. Each is refused now, with the rest of its family
(the BSD socket calls, the Windows thread, wait and tick calls, the process
calls), and every quoted include is followed when it resolves to a file
under tests/, whatever its path. A bare name followed by `(` is a call; a
member call (`.connect(`, `->send(`) and a qualified name other than std::
are not, so the library's own methods do not count.

Two helpers are not unit-case code and are not scanned: tests/suite_runner.h
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
    ("getaddrinfo", re.compile(r'(?<![A-Za-z0-9_])getaddrinfo\s*\(')),
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
    # VERIFY-3 W7: the rest of each family
    ("sleep", re.compile(r'(?<![A-Za-z0-9_.>:])(?:sleep|Sleep|SleepEx|alarm)\s*\(')),
    ("clock(", re.compile(r'(?<![A-Za-z0-9_.>:])clock\s*\(')),
    ("tick or system clock", re.compile(
        r'(?<![A-Za-z0-9_.>:])(?:GetTickCount|GetTickCount64|timeGetTime|timespec_get|GetSystemTime|'
        r'GetSystemTimeAsFileTime|GetSystemTimePreciseAsFileTime|GetLocalTime|ftime|times)\s*\(')),
    ("std::jthread", re.compile(r'std::jthread')),
    ("Windows thread", re.compile(r'(?<![A-Za-z0-9_.>:])(?:CreateThread|_beginthread|_beginthreadex|'
                                  r'CreateRemoteThread)\s*\(')),
    ("timed wait", re.compile(r'(?<![A-Za-z0-9_])(?:wait_for|wait_until|try_lock_for|try_lock_until|'
                              r'pthread_cond_timedwait|sem_timedwait|WaitForSingleObject|WaitForSingleObjectEx|'
                              r'WaitForMultipleObjects|WaitForMultipleObjectsEx|SleepConditionVariableCS|'
                              r'SleepConditionVariableSRW)\s*\(')),
    ("resolver", re.compile(r'(?<![A-Za-z0-9_.>:])(?:gethostbyname|gethostbyname2|gethostbyaddr|getnameinfo|'
                            r'GetAddrInfoW|GetAddrInfoExW|res_query|res_search)\s*\(')),
    ("socket call", re.compile(r'(?<![A-Za-z0-9_.>:])(?:socketpair|connect|bind|listen|accept|accept4|send|recv|'
                               r'shutdown|WSASocket|WSASocketW|WSASocketA|WSASend|WSARecv|WSASendTo|WSARecvFrom|'
                               r'WSAStartup|WSAPoll)\s*\(')),
    ("poll or select", re.compile(r'(?<![A-Za-z0-9_.>:])(?:poll|ppoll|select|pselect|epoll_wait|epoll_pwait|'
                                  r'kevent|io_uring_enter|io_uring_wait_cqe)\s*\(')),
    ("process", re.compile(r'(?<![A-Za-z0-9_.>:])(?:fork|vfork|system|popen|_popen|execv|execve|execvp|execl|'
                           r'execlp|posix_spawn|posix_spawnp|CreateProcess|CreateProcessA|CreateProcessW)\s*\(')),
    # the real clock or sleep handed over as a function value: a bounded wait
    # (rn_cli_bounded_wait) that polls the real clock and sleeps, which a
    # unit case needs only to wait on a thread the library started
    ("real clock or sleep passed as a value", re.compile(
        r'(?<![A-Za-z0-9_])(?:rocketnet_time|rocketnet_sleep|rocketnet_platform_time|rocketnet_platform_sleep)'
        r'(?![A-Za-z0-9_])(?!\s*\()')),
]

# a wake wait is a real OS wait unless its timeout is zero (a poll)
WAKE_WAIT = re.compile(r'(?<![A-Za-z0-9_])rocketnet_platform_wake_wait\s*\(([^;]*)\)\s*;')
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
            before = source[:match.start()].rstrip()
            if before.endswith(":") and not before.endswith("::"):
                continue  # a constructor's member initializer, `: send( ordered.send )`, is not a call
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


INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+\.(?:h|hpp|inc))"', re.M)
CALL = re.compile(r'(?<![A-Za-z0-9_])([A-Za-z_]\w*)\s*\(')
DEFINITION = re.compile(r'^[A-Za-z_][\w \t\*&:<>]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', re.M)


def helpers(root, unit_files):
    """Every file under tests/ the unit files include, transitively: a quoted
    include is resolved against the including file's directory and then
    tests/, whatever path it names."""
    tests_dir = os.path.realpath(os.path.join(root, "tests"))
    found = []
    pending = []
    for path in unit_files:
        pending.extend((os.path.dirname(path), name) for name in INCLUDE.findall(read(path)))
    seen = set()
    while pending:
        base, name = pending.pop()
        path = None
        for directory in (base, tests_dir):
            candidate = os.path.realpath(os.path.join(directory, name))
            if os.path.isfile(candidate):
                path = candidate
                break
        if path is None or path in seen:
            continue
        seen.add(path)
        if not path.startswith(tests_dir + os.sep) or os.path.basename(path) in HELPERS_NOT_SCANNED:
            continue
        if path in (os.path.realpath(p) for p in unit_files):
            continue
        found.append(path)
        pending.extend((os.path.dirname(path), n) for n in INCLUDE.findall(read(path)))
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
    ]
    for call in ("std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) )", "usleep( 1000 )",
                 "nanosleep( &t, NULL )", "std::chrono::steady_clock::now()", "clock_gettime( CLOCK_MONOTONIC, &t )",
                 "gettimeofday( &v, NULL )", "QueryPerformanceCounter( &c )", "mach_absolute_time()",
                 "pthread_create( &p, NULL, f, NULL )", "std::async( f )", "socket( AF_INET, SOCK_DGRAM, 0 )",
                 "sendto( s, b, n, 0, a, l )", "recvfrom( s, b, n, 0, a, &l )", "sendmmsg( s, m, n, 0 )",
                 "rn_cli_bounded_wait( o, done, NULL, rocketnet_time, NULL, \"o\" )",
                 "rn_cli_bounded_wait( o, done, NULL, NULL, rocketnet_sleep, \"o\" )",
                 "clock_fn = &rocketnet_platform_time",
                 # VERIFY-3 W7's plants
                 "sleep( 1 )", "Sleep( 1 )", "clock()", "std::jthread j( f )", "CreateThread( 0, 0, f, 0, 0, 0 )",
                 "gethostbyname( \"h\" )", "socketpair( AF_UNIX, SOCK_DGRAM, 0, v )", "poll( f, 1, 10 )",
                 "select( n, &r, 0, 0, &t )", "GetTickCount64()", "timespec_get( &t, TIME_UTC )",
                 "cv.wait_for( lock, d )", "fork()", "connect( s, a, l )", "recv( s, b, n, 0 )"):
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
    # a member call is not the OS call
    for call in ("pair.connect( a )", "endpoint->send( p )", "rn::poll( x )"):
        with tempfile.TemporaryDirectory() as root:
            os.mkdir(os.path.join(root, "tests"))
            with open(os.path.join(root, "tests", "test_unit_thing.cpp"), "w") as fh:
                fh.write("static void test_thing() { %s; }\n" % call)
            if check(root):
                print("FAIL check_unit_purity self-test: member call %r was refused: %r" % (call, check(root)))
                return 1
    # a helper included by a path with a slash, and one reached from it, are scanned
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "tests", "planted", "deeper"))
        with open(os.path.join(root, "tests", "test_unit_thing.cpp"), "w") as fh:
            fh.write('#include "planted/helper.h"\nstatic void test_thing() { helper(); }\n')
        with open(os.path.join(root, "tests", "planted", "helper.h"), "w") as fh:
            fh.write('#include "deeper/inner.h"\nstatic inline void helper() { usleep( 1000 ); }\n')
        with open(os.path.join(root, "tests", "planted", "deeper", "inner.h"), "w") as fh:
            fh.write('static inline void inner() { std::jthread t( f ); }\n')
        names = sorted(name for _, _, name in check(root))
        if names != ["std::jthread", "usleep"]:
            print("FAIL check_unit_purity self-test: slash-path helpers: expected usleep and std::jthread, got %r" % names)
            return 1
    print("ok check_unit_purity self-test: a thread, timed wait or sleep in a DEV fixture or helper is refused, "
          "every planted spelling of VERIFY-3 W7 is refused, member calls are not, and slash-path helpers are read")
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
