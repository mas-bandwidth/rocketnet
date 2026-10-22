#!/usr/bin/env python3
"""Check the shared C ABI, static crypto namespace and absence of test and development code."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import check_header_exports


def run(*command):
    return subprocess.check_output(command, text=True, errors="replace")


def msvc_tool(name):
    found = shutil.which(name)
    if found:
        return found
    installer = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    root = run(str(installer), "-latest", "-products", "*", "-requires",
               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath").strip()
    tools = sorted(Path(root).glob(f"VC/Tools/MSVC/*/bin/Hostx64/x64/{name}.exe"))
    if not tools:
        raise RuntimeError(f"Cannot find {name}")
    return str(tools[-1])


def header_declared_names(header_path):
    names, seen, buffer, depth, in_dev = [], set(), None, 0, False
    for raw in Path(header_path).read_text().splitlines():
        line = raw.strip()
        if line == "#if ROCKETNET_DEV":
            in_dev = True
        if in_dev:
            if line.startswith("#endif"):
                in_dev = False
            continue
        if buffer is None:
            if line.startswith(("ROCKETNET_EXPORT ", "ROCKETNET_EXPORT_FUNC")):
                buffer, depth = line, line.count("(") - line.count(")")
        else:
            buffer += " " + line
            depth += line.count("(") - line.count(")")
        if buffer is not None and depth <= 0:
            match = re.search(r"(\w+)\s*$", buffer[:buffer.find("(")])
            if match and match.group(1) not in seen:
                seen.add(match.group(1))
                names.append(match.group(1))
            buffer, depth = None, 0
    return names


def source_check(header_path, tests_path):
    declared = header_declared_names(header_path)
    tests = Path(tests_path).read_text()
    match = re.search(r"rocketnet_test_exports\[\]\s*=\s*\{(.*?)\};", tests, flags=re.S)
    table = re.findall(r'"([^"]+)"', match.group(1)) if match else []
    seen = set()
    problems = []
    for name in table:
        if name in seen:
            problems.append(f"Duplicate in rocketnet_test_exports: {name}")
        seen.add(name)
    problems += [f"Export missing from rocketnet_test_exports: {name}" for name in declared if name not in seen]
    problems += [f"Table name not declared in sdk/rocketnet.h: {name}" for name in table if name not in set(declared)]
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"Source exports: {len(declared)} declared, table matches")
    return 0


# What a production build (ROCKETNET_DEV=0) compiles out (sdk/rocketnet_test_seams.h):
# a defined symbol naming any of these in the production archive or shared
# library is development or test code that reached production.
DEVELOPMENT_SYMBOL_PARTS = (
    "_for_test", "network_simulator", "dev_server_secret_key", "dev_backend_secret_key",
    "server_transport_dev_config", "set_test_timestamp", "client_create_testing",
    "test_stall_destroy", "test_bound_port", "create_step_driven_at_timestamp",
    "create_dev_connect_token", "write_test_connect_token",
)


def development_symbols(defined):
    return sorted(symbol for symbol in defined if any(part in symbol for part in DEVELOPMENT_SYMBOL_PARTS))


def dev_server_secret_key(source_path):
    """The published development server secret key's 32 bytes, read from its definition."""
    text = Path(source_path).read_text()
    match = re.search(r"rocketnet_dev_server_secret_key\[32\]\s*=\s*\{(.*?)\};", text, flags=re.S)
    if not match:
        raise RuntimeError(f"no rocketnet_dev_server_secret_key in {source_path}")
    key = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
    if len(key) != 32:
        raise RuntimeError(f"rocketnet_dev_server_secret_key in {source_path} is {len(key)} bytes")
    return key


def files_holding(needle, paths):
    return [str(path) for path in paths if needle in Path(path).read_bytes()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources-only", action="store_true", help="stop after the source check")
    parser.add_argument("--header", type=Path, default=Path(__file__).parent.parent / "sdk" / "rocketnet.h")
    parser.add_argument("--tests", type=Path, default=Path(__file__).parent.parent / "sdk" / "rocketnet_tests.cpp")
    parser.add_argument("--connect-token-source", type=Path,
                        default=Path(__file__).parent.parent / "sdk" / "rocketnet_connect_token.cpp")
    parser.add_argument("shared", nargs="?", type=Path)
    parser.add_argument("archive", nargs="?", type=Path)
    args = parser.parse_args()
    if check_header_exports.check(args.header, Path(__file__).with_name("exports.txt")) != 0:
        return 1
    if source_check(args.header, args.tests) != 0:
        return 1
    if args.sources_only:
        return 0
    if args.shared is None or args.archive is None:
        parser.error("the following arguments are required: shared archive")
    expected = {line.strip() for line in Path(__file__).with_name("exports.txt").read_text().splitlines()
                if line.strip() and not line.startswith("#")}
    if sys.platform == "win32":
        output = run(msvc_tool("dumpbin"), "/nologo", "/exports", str(args.shared))
        actual = set(re.findall(r"^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)", output, re.M))
        members = run(msvc_tool("lib"), "/nologo", "/list", str(args.archive))
        symbols = run(msvc_tool("dumpbin"), "/nologo", "/symbols", str(args.archive))
        defined = {match[1] for line in symbols.splitlines() if "UNDEF" not in line
                   for match in [re.search(r"(?:External|WeakExternal)\s+\|\s+(\S+)", line)] if match}
    elif sys.platform == "darwin":
        output = run("nm", "-gU", str(args.shared))
        actual = {line.split()[-1].removeprefix("_") for line in output.splitlines() if line.split()}
        members = run("ar", "-t", str(args.archive))
        symbols = run("nm", "-gU", str(args.archive))
        defined = {line.split()[-1] for line in symbols.splitlines()
                   if line.split() and not line.endswith(":")}
    else:
        output = run("nm", "-D", "--defined-only", str(args.shared))
        actual = {line.split()[-1].split("@")[0] for line in output.splitlines() if line.split()}
        members = run("ar", "-t", str(args.archive))
        symbols = run("nm", "-g", "--defined-only", str(args.archive))
        defined = {line.split()[-1] for line in symbols.splitlines()
                   if line.split() and not line.endswith(":")}
    crypto = re.compile(r"^(?:crypto_|sodium_|randombytes(?:_|$)|blake2b(?:_|$)|"
                        r"fe25519_|ge25519_|sc25519_|ristretto255_|chacha20_dolbeau_|poly1305_sse2_)")
    unprefixed = {symbol for symbol in defined if crypto.match(symbol.lstrip("_"))}
    missing, extra = expected - actual, actual - expected
    development = development_symbols(defined | actual)
    key_holders = files_holding(dev_server_secret_key(args.connect_token_source), [args.shared, args.archive])
    if missing or extra or unprefixed or "rocketnet_tests" in members or development or key_holders:
        if development:
            print("Development or test code in the production library (build with -DROCKETNET_DEV=OFF):",
                  ", ".join(development), file=sys.stderr)
        if key_holders:
            print("The development server secret key bytes are in:", ", ".join(key_holders), file=sys.stderr)
        if missing:
            print("Missing exports:", ", ".join(sorted(missing)), file=sys.stderr)
        if extra:
            print("Unexpected exports:", ", ".join(sorted(extra)), file=sys.stderr)
        if unprefixed:
            print("Unprefixed crypto definitions in production archive:",
                  ", ".join(sorted(unprefixed)), file=sys.stderr)
        if "rocketnet_tests" in members:
            print("Test suite is present in the production archive", file=sys.stderr)
        return 1
    print(f"C ABI: {len(actual)} exports match; static crypto namespace is private; production archive contains no test suite, "
          "no development or test code and no development secret key")
    return 0


if __name__ == "__main__":
    sys.exit(main())
