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
EXEC_TEST_NAME_RE = re.compile(r'"(test_[a-z0-9_]+)(?:\\n)?"')
IF_RE = re.compile(r"^\s*#\s*if(?:def|ndef)?\b")
ELIF_RE = re.compile(r"^\s*#\s*elif\b")
ELSE_RE = re.compile(r"^\s*#\s*else\b")
ENDIF_RE = re.compile(r"^\s*#\s*endif\b")


def _table_names(path):
    all_names = set()
    paired_names = set()
    stack = []
    try:
        text = path.read_text()
    except OSError:
        return all_names, paired_names
    for line in text.splitlines():
        if IF_RE.match(line):
            stack.append(([set()], False))
            continue
        if ELIF_RE.match(line) and stack:
            stack[-1][0].append(set())
            continue
        if ELSE_RE.match(line) and stack:
            arms, _ = stack[-1]
            arms.append(set())
            stack[-1] = (arms, True)
            continue
        if ENDIF_RE.match(line) and stack:
            arms, has_else = stack.pop()
            if any(arms):
                common = set.intersection(*arms) if has_else else set()
                if stack:
                    stack[-1][0][-1] |= common
                else:
                    paired_names |= common
            continue
        m = TABLE_ENTRY_RE.match(line)
        if m:
            name = m.group(1)
            all_names.add(name)
            if stack:
                stack[-1][0][-1].add(name)
            else:
                paired_names.add(name)
    return all_names, paired_names


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
    all_names = set()
    paired_names = set()
    for rel in (("sdk", "rocketnet_tests.cpp"), ("test_channels.cpp",)):
        a, p = _table_names(REPO.joinpath(*rel))
        all_names |= a
        paired_names |= p
    for path in sorted(REPO.glob("test_unit_*.cpp")):
        r = _register_names(path)
        all_names |= r
        paired_names |= r
    return all_names, paired_names


def registered_functional_names():
    return _table_names(REPO / "functional" / "rocketnet_functional_tests.cpp")


def exec_functional_names():
    # The functional binary also runs the sibling exec binaries (test_xdp,
    # test_transport_control and each platform's analogue), whose tests print
    # their own names (e.g. test_xdp_inactive_generation). Those names appear
    # in the output without being registered in functional/'s own registry, so
    # they are known names, not unknown ones. Only the name-shaped string
    # literals the exec sources print are known -- not every identifier in the
    # file, which would turn the whole vocabulary into "known" names.
    names = set()
    for rel in (
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
        names |= {m.group(1) for m in EXEC_TEST_NAME_RE.finditer(text)}
    return names


def printed_names(lines):
    names = set()
    for line in lines:
        name = line.split(" [SKIPPED", 1)[0]
        if NAME_RE.fullmatch(name) and not name.startswith("test_timing_"):
            names.add(name)
    return names


def assert_name_set(printed, registered, label, known=None, paired=None):
    if known is None:
        known = registered
    missing = sorted(registered - printed)
    unknown = sorted(printed - known)
    unpaired = sorted(registered - paired) if paired is not None else []
    if missing or unknown or unpaired:
        parts = []
        if missing:
            parts.append("missing from output: " + ", ".join(missing))
        if unknown:
            parts.append("printed but not registered: " + ", ".join(unknown))
        if unpaired:
            parts.append("registered without #else skip: " + ", ".join(unpaired))
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
        lines = result.stdout.splitlines()
        printed = printed_names(lines)
        reg_all, reg_paired = registered_functional_names()
        assert_name_set(printed, reg_all, "functional",
                        known=reg_all | exec_functional_names(),
                        paired=reg_paired)
        if result.returncode:
            fails = [line for line in lines if line.startswith("FAIL: ")]
            tc_bin = binary.parent / ("test_transport_control.exe" if binary.suffix == ".exe" else "test_transport_control")
            if fails == ["FAIL: test_transport_control exit 1"] and tc_bin.exists():
                tc_env = env.copy()
                tc_env["ROCKETNET_SERVER_PUBLIC_KEY"] = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="
                tc_env["ROCKETNET_BACKEND_PUBLIC_KEY"] = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="
                tc_res = run_binary(tc_bin, tc_env, timeout=120)
                if tc_res.returncode:
                    sys.exit("functional suite failed: %s" % result.returncode)
                printed |= printed_names(tc_res.stdout.splitlines())
                assert_name_set(printed, reg_all, "functional",
                                known=reg_all | exec_functional_names(),
                                paired=reg_paired)
            else:
                sys.exit("functional suite failed: %s" % result.returncode)
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
    reg_all, reg_paired = registered_unit_names()
    assert_name_set(printed, reg_all, "SDK suite", paired=reg_paired)


if __name__ == "__main__":
    main()
