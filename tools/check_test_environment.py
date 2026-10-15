#!/usr/bin/env python3
"""Run the SDK suite with hostile ambient config; require names-only success.

With the unit binary (bin/test) this also asserts that the set of names the
suite prints -- the name before any " [SKIPPED" -- equals the set of names the
sources register in the registry tables and REGISTER_UNIT_TEST lines, so a
test compiled out of a platform's build is named the moment it vanishes rather
than silently shrinking the printed count. With the functional binary
(bin/functional) it runs the suite the way its CI job does and asserts the
same name-set equality against functional/'s registry.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parent.parent

NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
TABLE_ENTRY_RE = re.compile(r'^\s*\{\s*"(test_[a-z0-9_]+)"\s*,')
REGISTER_RE = re.compile(r"REGISTER_UNIT_TEST(?:_EXPECTED_FAIL)?\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)")


def _table_names(path):
    names = set()
    try:
        text = path.read_text()
    except OSError:
        return names
    for line in text.splitlines():
        m = TABLE_ENTRY_RE.match(line)
        if m:
            names.add(m.group(1))
    return names


def _register_names(path):
    names = set()
    try:
        text = path.read_text()
    except OSError:
        return names
    for m in REGISTER_RE.finditer(text):
        names.add(m.group(1))
    return names


def registered_unit_names():
    names = set()
    names |= _table_names(REPO / "sdk" / "rocketnet_tests.cpp")
    names |= _table_names(REPO / "test_channels.cpp")
    for path in sorted(REPO.glob("test_unit_*.cpp")):
        names |= _register_names(path)
    return names


def registered_functional_names():
    return _table_names(REPO / "functional" / "rocketnet_functional_tests.cpp")


def known_functional_names():
    # the functional binary also runs the sibling exec binaries and prints the
    # skip_lines functional/main.cpp declares; their names may appear in the
    # output without being registered in functional/'s own registry, so they
    # are known names, not unknown ones.
    names = registered_functional_names()
    for rel in (
        "functional/main.cpp",
        "test_serialize.cpp",
        "test_transport_control.cpp",
        "test_msgx.cpp",
        "test_xdp.cpp",
        "test_iocp.cpp",
    ):
        try:
            text = (REPO / rel).read_text()
        except OSError:
            continue
        names |= set(NAME_RE.findall(text))
    return names


def printed_names(lines):
    names = set()
    for line in lines:
        name = line.split(" [SKIPPED", 1)[0]
        if NAME_RE.fullmatch(name) and not name.startswith("test_timing_"):
            names.add(name)
    return names


def assert_name_set(printed, registered, label):
    missing = sorted(registered - printed)
    unknown = sorted(printed - registered)
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
                                timeout=timeout)
    except subprocess.TimeoutExpired:
        sys.exit("%s exceeded its %d-second deadline" % (binary, timeout))
    print(result.stdout, end="")
    return result


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "bin/test").resolve()
    functional = "functional" in binary.name

    env = os.environ.copy()
    env.pop("ROCKETNET_TEST_VERBOSE", None)

    if functional:
        # the functional suite opens loopback sockets and walks the driver
        # ladder; its CI job invokes it with the ambient environment (and any
        # ROCKETNET_DRIVER pin) rather than the unit suite's hostile config.
        result = run_binary(binary, env, timeout=1200)
        if result.returncode:
            sys.exit("functional suite failed: %s" % result.returncode)
        printed = printed_names(result.stdout.splitlines())
        assert_name_set(printed, known_functional_names(), "functional")
        return

    # Preserve any caller's explicit ROCKETNET_DRIVER pin. Other values exercise
    # exact names, legacy aliases, an invalid key and a long value without ever
    # printing their contents. The suite derives its cleanup list from the tables.
    env.update({
        "ROCKETNET_LOG_LEVEL": "4",
        "ROCKETNET_MAX_CLIENTS": "77",
        "ROCKETNET_NUM_REASSEMBLY_SLOTS": "3",
        "ROCKETNET_SERVER_NUM_QUEUES": "invalid",
        "ROCKETNET_SERVER_NUM_OS_CPUS": "invalid",
        "ROCKETNET_SERVER_SECRET_KEY": "x" * 4096,
        "ROCKETNET_SERVER_PUBLIC_ADDRESS": "invalid",
        "ROCKETNET_PUBLIC_ADDRESS": "invalid",
        "ROCKETNET_PUMP": "portable",
    })

    result = run_binary(binary, env, timeout=180)
    if result.returncode:
        sys.exit("SDK suite failed with ambient config overrides: %s" % result.returncode)
    lines = result.stdout.splitlines()
    if not lines or any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]+(?: [^\r\n]*\[SKIPPED[^\r\n]*\])?", line) for line in lines):
        sys.exit("Passing SDK suite emitted output other than test names or named skips")
    if "test_suite_environment" not in lines:
        sys.exit("SDK suite did not execute its environment restoration test")

    printed = printed_names(lines)
    assert_name_set(printed, registered_unit_names(), "SDK suite")


if __name__ == "__main__":
    main()
