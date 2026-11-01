#!/usr/bin/env python3
"""Run the SDK suite with hostile ambient config; require the netcode output.

A passing run prints one line per test, its test_ lowercase snake_case name,
then a blank line, "*** ALL TESTS PASSED ***" and a blank line, and nothing else. With the
unit binary (bin/test) the printed names must cover every name the sources
register outside a preprocessor condition (registry tables and
REGISTER_UNIT_TEST lines), and every printed name must be registered, so a
test is never silently lost; a name registered under a condition (a platform
#if, or a functional entry with a runs_here predicate) may or may not print.
With the functional binary (bin/functional) it runs the suite the way its CI
job does and checks the same against tests/functional/'s registry and the names the
sibling executables print.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

from nightly_timeout import execution_timeout

REPO = Path(__file__).resolve().parent.parent

NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# Registry rows name every functional test, including harness and client_io
# cases; a third field is a runs_here predicate (tests/functional/rocketnet_functional_tests.cpp).
TABLE_ENTRY_RE = re.compile(r'^\s*\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*(?:"[^"]*"\s*,\s*\w+|\w+(\s*,\s*\w+)?)\s*\}')
REGISTER_RE = re.compile(r"REGISTER_UNIT_TEST(?:_EXPECTED_FAIL)?\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)")
EXEC_TEST_NAME_RE = re.compile(r'"(test_[a-z0-9_%]+)(?:\\n)?"')
TEST_NAME_RE = re.compile(r"test_[a-z0-9]+(?:_[a-z0-9]+)*")
PASSED_LINE = "*** ALL TESTS PASSED ***"
IF_RE = re.compile(r"^\s*#\s*if(?:def|ndef)?\b")
ELIF_RE = re.compile(r"^\s*#\s*elif\b")
ELSE_RE = re.compile(r"^\s*#\s*else\b")
ENDIF_RE = re.compile(r"^\s*#\s*endif\b")
def _table_names(path):
    # (every name, the names outside any #if and without a runs_here predicate)
    all_names = set()
    always = set()
    depth = 0
    try:
        text = path.read_text()
    except OSError:
        return all_names, always
    for line in _mask_comments_and_literals(text, keep_strings=True).splitlines():
        if IF_RE.match(line):
            depth += 1
            continue
        if ENDIF_RE.match(line):
            depth = max(0, depth - 1)
            continue
        m = TABLE_ENTRY_RE.match(line)
        if m:
            all_names.add(m.group(1))
            if depth == 0 and not m.group(2):
                always.add(m.group(1))
    return all_names, always


RAW_STRING_START_RE = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')


def _mask_comments_and_literals(text, keep_strings=False):
    # Preserve line boundaries and hide comment/string contents before guard
    # matching, so directive-looking examples cannot change source inventory.
    output = list(text)

    def blank(start, end):
        for index in range(start, end):
            if output[index] != "\n":
                output[index] = " "

    index = 0
    length = len(text)
    while index < length:
        raw = RAW_STRING_START_RE.match(text, index)
        if raw:
            terminator = ")" + raw.group(1) + '"'
            end = text.find(terminator, raw.end())
            end = length if end < 0 else end + len(terminator)
            blank(index, end)
            index = end
            continue
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            blank(index, end)
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            blank(index, end)
            index = end
            continue
        if text[index] in ("\"", "'") and not keep_strings:
            quote = text[index]
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == quote:
                    end += 1
                    break
                end += 1
            blank(index, end)
            index = end
            continue
        index += 1
    return "".join(output)

def _register_names(path):
    # [(name, registered outside any #if)]
    names = []
    try:
        text = path.read_text()
    except OSError:
        return names
    depth = 0
    for line in _mask_comments_and_literals(text).splitlines():
        if IF_RE.match(line):
            depth += 1
            continue
        if ENDIF_RE.match(line):
            depth = max(0, depth - 1)
            continue
        for m in REGISTER_RE.finditer(line):
            names.append((m.group(1), depth == 0))
    return names


# A simple guard is one macro tested for truth or definition, possibly
# negated: #if X, #if !X, #ifdef X, #ifndef X, #if defined( X ) and
# #if !defined( X ). Any other condition is opaque and excludes nothing.
SIMPLE_IF_RE = re.compile(r"^\s*#\s*if\s+(!?)\s*([A-Za-z_][A-Za-z0-9_]*)\s*$")
DEFINED_IF_RE = re.compile(r"^\s*#\s*if\s+(!?)\s*defined\s*\(?\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)?\s*$")
IFDEF_RE = re.compile(r"^\s*#\s*if(n?)def\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")


def _guard_literal(line):
    # (macro test, polarity), or None for an opaque condition
    m = SIMPLE_IF_RE.match(line)
    if m:
        return (m.group(2), not m.group(1))
    m = DEFINED_IF_RE.match(line)
    if m:
        return ("defined " + m.group(2), not m.group(1))
    m = IFDEF_RE.match(line)
    if m:
        return ("defined " + m.group(2), not m.group(1))
    return None


def _register_guards(path):
    # [(name, guard)]: guard is the frozenset of simple literals that must hold
    # for the registration to compile
    names = []
    try:
        text = path.read_text()
    except OSError:
        return names
    stack = []
    for line in _mask_comments_and_literals(text).splitlines():
        if IF_RE.match(line):
            stack.append(_guard_literal(line))
            continue
        if ELIF_RE.match(line):
            if stack:
                stack[-1] = None
            continue
        if ELSE_RE.match(line):
            if stack and stack[-1] is not None:
                stack[-1] = (stack[-1][0], not stack[-1][1])
            continue
        if ENDIF_RE.match(line):
            if stack:
                stack.pop()
            continue
        for m in REGISTER_RE.finditer(line):
            names.append((m.group(1), frozenset(literal for literal in stack if literal is not None)))
    return names


def _guards_exclusive(a, b):
    # two registrations never compile together when one guard needs a macro
    # test true and the other needs it false
    return any((macro, not polarity) in b for macro, polarity in a)


# Unit sources a production (ROCKETNET_DEV=OFF) test binary does not compile
# (CMakeLists.txt): the two registry tables, the loopback transport unit and
# the results and stop-spread files it includes, the Windows RIO overflow seam, and every file
# in CMakeLists.txt's ROCKETNET_DEV_ONLY_*_TEST_SOURCES lists, read from there.
PRODUCTION_ABSENT_FIXED = {
    "rocketnet_tests.cpp",
    "test_channels.cpp",
    "test_unit_driver_loopback.cpp",
    "test_unit_driver_results_checked.cpp",
    "test_unit_server_stop_spread.cpp",
    "test_unit_rio_overflow_seam.cpp",
}
DEV_ONLY_LIST_RE = re.compile(r"set\(\s*ROCKETNET_DEV_ONLY_\w+_TEST_SOURCES\s+([^)]*)\)")


def production_absent():
    absent = set(PRODUCTION_ABSENT_FIXED)
    text = (REPO / "CMakeLists.txt").read_text()
    for m in DEV_ONLY_LIST_RE.finditer(text):
        for entry in m.group(1).split():
            absent.add(Path(entry).name)
    return absent


def registered_unit_names(production=False):
    all_names = set()
    always = set()
    for rel in (("sdk", "rocketnet_tests.cpp"), ("tests", "test_channels.cpp")):
        a, p = _table_names(REPO.joinpath(*rel))
        all_names |= a
        if not production:
            always |= p
    # A REGISTER_UNIT_TEST name is unique: test.cpp runs a name from the sdk
    # and channels tables before the unit-file registry, and the registry runs
    # a name's first registration only (rocketnet_unit_file_find in
    # test_unit_registry.h), so a repeated name never runs its later test.
    # The registrar aborts on the first repeat; this names every one. Two
    # registrations under mutually exclusive guards (a ROCKETNET_DEV case and
    # its !ROCKETNET_DEV production twin) never reach one binary together.
    absent = production_absent() if production else set()
    table_names = set(all_names)
    seen = {}
    duplicates = []
    for path in sorted(REPO.glob("tests/test_unit_*.cpp")):
        for name, unconditional in _register_names(path):
            all_names.add(name)
            if unconditional and path.name not in absent:
                always.add(name)
        for name, guard in _register_guards(path):
            earlier = seen.setdefault(name, [])
            clash = name in table_names or any(not _guards_exclusive(guard, other) for _, other in earlier)
            if clash:
                first = earlier[0][0] if earlier else "registry table"
                duplicates.append("%s (%s, %s)" % (name, first, path.name))
            earlier.append((path.name, guard))
    if duplicates:
        sys.exit("unit test names registered more than once: " + "; ".join(duplicates))
    return all_names, always


def registered_functional_names():
    return _table_names(REPO / "tests" / "functional" / "rocketnet_functional_tests.cpp")


def exec_functional_names():
    # The functional binary also runs the sibling exec binaries (test_xdp,
    # test_transport_control and each platform's analogue), whose tests print
    # their own names (e.g. test_xdp_transport_inactive_generation), some
    # through a format (test_disconnect_burst_next_send_%s). Only the
    # name-shaped string literals the exec sources print are known, as
    # patterns, plus each exec case's own name.
    patterns = []
    for rel in (
        "tests/test_serialize.cpp",
        "tests/test_transport_control.cpp",
        "tests/test_msgx.cpp",
        "tests/test_xdp.cpp",
        "tests/test_iocp.cpp",
    ):
        patterns.append(re.escape(Path(rel).stem))
        try:
            text = (REPO / rel).read_text()
        except OSError:
            continue
        for m in EXEC_TEST_NAME_RE.finditer(text):
            patterns.append(re.escape(m.group(1)).replace("%d", "[0-9]+").replace("%s", "[a-z0-9_]+"))
    # the hard-killed client cases name themselves from their cell
    # (rocketnet_posix_hard_killed_client_test_name, tests/functional/posix_hard_killed_client.cpp)
    patterns.append("test_posix_(?:hard_killed_client|send_to_closed_port|client_server_gone|receive_seam)_[a-z0-9_]+")
    return re.compile("(?:" + "|".join(patterns) + ")")


def check_shape(lines, label, once=False):
    # every line a test name, then a blank line, the passed line and a blank line;
    # with once, each name on one line only (the unit registry prints each case once)
    if len(lines) < 4 or lines[-3:] != ["", PASSED_LINE, ""]:
        sys.exit("%s: the run does not end with a blank line, %r and a blank line" % (label, PASSED_LINE))
    bad = [line for line in lines[:-3] if not TEST_NAME_RE.fullmatch(line)]
    if bad:
        sys.exit("%s: lines that are not test_ snake_case names: %s" % (label, "; ".join(bad[:10])))
    if once:
        seen = set()
        repeated = sorted({line for line in lines[:-3] if line in seen or seen.add(line)})
        if repeated:
            sys.exit("%s: names printed more than once: %s" % (label, ", ".join(repeated[:10])))
    return lines[:-3]


# A registered unit case's name is printed by the registry before the case runs
# (tests/test_unit_registry.h); a case that prints its own name again shows it
# twice in a single-case run (./bin/test <name>).
SELF_NAME_PRINT_RE = re.compile(r'printf\(\s*"(test_[a-z0-9_]+)\\n"\s*\)')


def unit_sources_printing_names(paths=None):
    if paths is None:
        paths = sorted((REPO / "tests").glob("test_unit_*.cpp")) + [REPO / "sdk" / "rocketnet_tests.cpp"]
    found = []
    for path in paths:
        try:
            text = Path(path).read_text()
        except OSError:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            m = SELF_NAME_PRINT_RE.search(line)
            if m:
                found.append("%s:%d prints %s" % (Path(path).name, number, m.group(1)))
    return found


def assert_name_set(printed, registered, always, label, exec_names=None):
    missing = sorted(always - printed)
    unknown = sorted(n for n in printed - registered if not (exec_names and exec_names.fullmatch(n)))
    if missing or unknown:
        parts = []
        if missing:
            parts.append("missing from output: " + ", ".join(missing))
        if unknown:
            parts.append("printed but not registered: " + ", ".join(unknown))
        sys.exit("%s: name set mismatch (%d printed, %d registered) -- %s"
                 % (label, len(printed), len(registered), "; ".join(parts)))


def run_binary(binary, env, timeout):
    try:
        result = subprocess.run([str(binary)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=execution_timeout(timeout))
    except subprocess.TimeoutExpired:
        sys.exit("%s exceeded its %d-second deadline" % (binary, timeout))
    print(result.stdout, end="")
    return result


def main():
    args = [arg for arg in sys.argv[1:] if arg != "--production"]
    # --production: the binary links the production library (ROCKETNET_DEV=OFF),
    # which compiles no in-memory environment table and so no restoration test
    production = "--production" in sys.argv[1:]
    binary = Path(args[0] if args else "bin/test").resolve()
    functional = "functional" in binary.name

    env = os.environ.copy()
    env.pop("ROCKETNET_TEST_VERBOSE", None)

    if functional:
        # the functional suite opens loopback sockets and walks the transport
        # ladder; its CI job invokes it with the ambient environment (and any
        # ROCKETNET_SERVER_TRANSPORT / ROCKETNET_CLIENT_TRANSPORT pin) rather than the
        # unit suite's hostile config.
        result = run_binary(binary, env, timeout=1200)
        if result.returncode:
            sys.exit("functional suite failed: %s" % result.returncode)
        printed = set(check_shape(result.stdout.splitlines(), "functional"))
        reg_all, reg_always = registered_functional_names()
        assert_name_set(printed, reg_all, reg_always, "functional", exec_functional_names())
        return

    printing = unit_sources_printing_names()
    if printing:
        sys.exit("unit cases print their own name, which the registry already prints: %s" % "; ".join(printing[:10]))

    # Preserve any caller's explicit ROCKETNET_SERVER_TRANSPORT and
    # ROCKETNET_CLIENT_TRANSPORT pins. Other values exercise
    # exact names, older aliases, an invalid key and a long value without ever
    # printing their contents. The suite derives its cleanup list from the tables.
    env.update({
        "ROCKETNET_LOG_LEVEL": "4",
        "ROCKETNET_MAX_CLIENTS": "77",
        "ROCKETNET_SERVER_NUM_OS_CPUS": "invalid",
        "ROCKETNET_SERVER_SECRET_KEY": "x" * 4096,
        "ROCKETNET_SERVER_IPV4_ADDRESS": "invalid",
        "ROCKETNET_SERVER_IPV6_ADDRESS": "invalid",
        "ROCKETNET_PUBLIC_ADDRESS": "invalid",
        "ROCKETNET_PUMP": "portable",
    })

    result = run_binary(binary, env, timeout=180)
    if result.returncode:
        sys.exit("SDK suite failed with ambient config overrides: %s" % result.returncode)
    names = check_shape(result.stdout.splitlines(), "SDK suite", once=True)
    if not production and "test_suite_environment" not in names:
        sys.exit("SDK suite did not execute its environment restoration test")

    reg_all, reg_always = registered_unit_names(production)
    assert_name_set(set(names), reg_all, reg_always, "SDK suite")


if __name__ == "__main__":
    main()
