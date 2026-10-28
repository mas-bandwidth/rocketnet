#!/usr/bin/env python3
"""Verify the DEV=1-only test-scaffolding boundary across headers, build trees, artifacts, and negative controls."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import check_exports


# The per-feature test switches this repository used to have. ROCKETNET_DEV is
# the one switch now; none of these may come back.
RETIRED_SWITCHES = (
    "ROCKETNET_TEST_SEAMS",
    "ROCKETNET_ACK_VISIT_TEST_HOOKS",
    "ROCKETNET_ACK_COST_TEST_HOOKS",
    "ROCKETNET_ACK_STATS_TEST_HOOKS",
    "ROCKETNET_BLOCK_COST_TEST_HOOKS",
    "ROCKETNET_LINK_SIMULATOR_TEST_HOOKS",
    "ROCKETNET_MESSAGE_COPY_TEST_HOOKS",
    "ROCKETNET_SECRET_WIPE_TEST_HOOKS",
    "ROCKETNET_URING_REAP_TEST_HOOKS",
    "ROCKETNET_RECEIVE_TEST_HOOKS",
    "ROCKETNET_PORTABLE_TX_TEST_HOOKS",
    "ROCKETNET_RIO_OVERFLOW_TEST_HOOKS",
    "ROCKETNET_XDP_STAGING_TEST_HOOKS",
    "ROCKETNET_ENVIRONMENT_TEST_HOOKS",
    "ROCKETNET_SEND_ERROR_TEST_HOOKS",
    "ROCKETNET_LOOPBACK_TEST_HOOKS",
    "ROCKETNET_MSGX_TEST_HOOKS",
    "ROCKETNET_WINDOWS_LIFECYCLE_TRACK",
    "ROCKETNET_IOCP_TESTS",
    "ROCKETNET_RIO_TESTS",
    "ROCKETNET_XDP_TESTS",
    "ROCKETNET_TEST_CLOCK_WRAP",
    "ROCKETNET_PLATFORM_SEND_SEAM",
    "ROCKETNET_SPIKE_TRACKING",
    "ROCKETNET_ENABLE_MEMORY_CHECKS",
    "rocketnet_test_hooks",
)

# Where a retired switch would be read or defined.
SWITCH_SCAN_DIRS = ("sdk", "tests", "functional", "fuzz", "examples", "proofs", "oss-fuzz", ".github", "cmake")
SWITCH_SCAN_FILES = ("CMakeLists.txt", "Makefile", "client.c", "server.c", "soak.c", "load.c")
SWITCH_SCAN_SUFFIXES = (".h", ".hpp", ".c", ".cpp", ".inc", ".txt", ".cmake", ".yml", ".yaml", ".sh", ".py", ".sexp", "Makefile")

FORBIDDEN_PUBLIC_HEADER_TOKENS = (
    "_for_test",
    "rocketnet_write_connect_token_deterministic",
    "rocketnet_box_seal_with_ephemeral",
    "rocketnet_reset_inventory_matches",
    "rocketnet_reset_member_t",
    "ROCKETNET_RESET_MEMBER",
    "rocketnet_network_simulator",
)

FORBIDDEN_DEV0_SYMBOLS = (
    "rocketnet_write_connect_token_deterministic",
    "rocketnet_box_seal_with_ephemeral",
    "rocketnet_reset_inventory_matches",
    "rocketnet_client_connection_reset_inventory_matches_for_test",
    "rocketnet_client_transport_connection_reset_inventory_matches_for_test",
    "rocketnet_server_connection_reset_inventory_matches_for_test",
    "rocketnet_dev_session_sequence_start",
    "rocketnet_dev_server_secret_key",
    "rocketnet_dev_backend_secret_key",
    "g_ack_visit_count",
    "rn_message_payload_copy_count",
)


def retired_switch_problems(repo_root: Path, path: Path) -> list[str]:
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return []
    found = []
    for name in RETIRED_SWITCHES:
        if re.search(rf"\b{name}\b", text):
            found.append(f"{path.relative_to(repo_root)} names retired switch {name} (ROCKETNET_DEV is the one switch)")
    return found


def check_sources(repo_root: Path) -> list[str]:
    problems = []
    public_header = (repo_root / "sdk" / "rocketnet.h").read_text()
    for token in FORBIDDEN_PUBLIC_HEADER_TOKENS:
        if token in public_header:
            problems.append(f"sdk/rocketnet.h exposes test-scaffolding token: {token}")

    for scan_dir in SWITCH_SCAN_DIRS:
        base = repo_root / scan_dir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or not (path.suffix in SWITCH_SCAN_SUFFIXES or path.name == "Makefile"):
                continue
            problems += retired_switch_problems(repo_root, path)
    for name in SWITCH_SCAN_FILES:
        path = repo_root / name
        if path.is_file():
            problems += retired_switch_problems(repo_root, path)

    token_header = (repo_root / "sdk" / "rocketnet_connect_token.h").read_text()
    if "#if ROCKETNET_DEV" not in token_header or "rocketnet_write_connect_token_deterministic" not in token_header:
        problems.append("sdk/rocketnet_connect_token.h missing #if ROCKETNET_DEV guard for deterministic token writer")

    token_source = (repo_root / "sdk" / "rocketnet_connect_token.cpp").read_text()
    for name in ("rocketnet_box_seal_with_ephemeral", "rocketnet_write_connect_token_deterministic"):
        if name not in token_source:
            problems.append(f"sdk/rocketnet_connect_token.cpp missing {name}")

    reset_header = (repo_root / "sdk" / "rocketnet_connection_reset.h").read_text()
    if "#if ROCKETNET_DEV" not in reset_header:
        problems.append("sdk/rocketnet_connection_reset.h missing #if ROCKETNET_DEV guard for reset inventory walker")
    for prod_fn in (
        "rocketnet_client_connection_reset(",
        "rocketnet_client_protocol_wipe_secrets(",
        "rocketnet_server_connection_reset(",
    ):
        if prod_fn not in reset_header:
            problems.append(f"sdk/rocketnet_connection_reset.h missing production declaration {prod_fn}")

    doc_path = repo_root / "docs" / "TEST-SCAFFOLDING.md"
    if doc_path.is_file():
        for cited in sorted(set(re.findall(r"(?:sdk|tests|tools)/[A-Za-z0-9_./-]+\.(?:h|cpp|c|py|txt)", doc_path.read_text()))):
            if not (repo_root / cited).exists():
                problems.append(f"docs/TEST-SCAFFOLDING.md cites missing path: {cited}")
    if not doc_path.is_file():
        problems.append("docs/TEST-SCAFFOLDING.md is missing")
    else:
        doc_text = doc_path.read_text()
        for sym in (
            "ROCKETNET_DEV",
            "rocketnet_write_connect_token_deterministic",
            "rocketnet_box_seal_with_ephemeral",
            "rocketnet_reset_inventory_matches",
        ):
            if sym not in doc_text:
                problems.append(f"docs/TEST-SCAFFOLDING.md does not document {sym}")

    return problems


def check_build_dir(build_dir: Path) -> list[str]:
    problems = []
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        return [f"Missing CMakeCache.txt in {build_dir}"]
    cache_text = cache_path.read_text(errors="replace")
    if "ROCKETNET_DEV:BOOL=OFF" not in cache_text and "ROCKETNET_DEV:UNINITIALIZED=OFF" not in cache_text:
        problems.append(f"{cache_path} was not configured with ROCKETNET_DEV=OFF")

    compile_commands_path = build_dir / "compile_commands.json"
    if compile_commands_path.is_file():
        entries = json.loads(compile_commands_path.read_text(errors="replace"))
        for entry in entries:
            command = entry.get("command", "") or " ".join(entry.get("arguments", []))
            output = entry.get("output", "")
            # Production libraries, test support, and unit test binary must compile with ROCKETNET_DEV=0 and no retired switch.
            if any(
                part in output
                for part in (
                    "CMakeFiles/rocketnet.dir/",
                    "CMakeFiles/rocketnet_shared.dir/",
                    "CMakeFiles/rocketnet_test_support.dir/",
                    "CMakeFiles/test.dir/",
                )
            ):
                if re.search(r"(^|\s)[-/]DROCKETNET_DEV=1(\s|$)", command):
                    problems.append(f"DEV=0 target {output} compiled with ROCKETNET_DEV=1")
                for macro in RETIRED_SWITCHES:
                    if re.search(rf"(^|\s)[-/]D{macro}(=|\s|$)", command):
                        problems.append(f"DEV=0 target {output} compiled with retired switch {macro}")
    return problems


def default_cxx_compiler() -> str:
    if sys.platform == "win32":
        try:
            return check_exports.msvc_tool("cl")
        except Exception:
            pass
    for candidate in (os.environ.get("CXX"), "c++", "g++", "clang++", "cl"):
        if candidate:
            found = shutil.which(candidate)
            if found:
                return found
    raise RuntimeError("No C++ compiler found for negative-control checks")


def msvc_env() -> dict[str, str]:
    env = os.environ.copy()
    if sys.platform != "win32" or "INCLUDE" in env:
        return env
    installer = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not installer.is_file():
        return env
    root = check_exports.run(
        str(installer),
        "-latest",
        "-products",
        "*",
        "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property",
        "installationPath",
    ).strip()
    vc_includes = sorted(Path(root).glob("VC/Tools/MSVC/*/include"))
    kits_root = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10/Include"
    kit_versions = sorted(kits_root.glob("10.*")) if kits_root.is_dir() else []
    includes = []
    if vc_includes:
        includes.append(str(vc_includes[-1]))
    if kit_versions:
        latest_kit = kit_versions[-1]
        for sub in ("ucrt", "shared", "um"):
            if (latest_kit / sub).is_dir():
                includes.append(str(latest_kit / sub))
    if includes:
        env["INCLUDE"] = ";".join(includes)
    return env


def compile_snippet(cxx: str, repo_root: Path, code: str, extra_defines: list[str]) -> subprocess.CompletedProcess:
    sdk_dir = repo_root / "sdk"
    sodium_dir = repo_root / "sodium"
    is_msvc = Path(cxx).stem.lower().startswith("cl") and not Path(cxx).stem.lower().startswith("clang")
    with tempfile.TemporaryDirectory() as tmpdir:
        src = Path(tmpdir) / "red_control.cpp"
        src.write_text(code)
        if is_msvc:
            cmd = [cxx, "/nologo", "/Zs", "/WX", f"/I{sdk_dir}", f"/I{sodium_dir}"]
            cmd += [f"/D{d}" for d in extra_defines] + [str(src)]
            return subprocess.run(cmd, capture_output=True, text=True, env=msvc_env())
        else:
            cmd = [cxx, "-std=c++11", "-fsyntax-only", "-Werror", f"-I{sdk_dir}", f"-I{sodium_dir}"]
            cmd += [f"-D{d}" for d in extra_defines] + [str(src)]
            return subprocess.run(cmd, capture_output=True, text=True)


def run_red_controls(repo_root: Path, cxx: str | None) -> list[str]:
    problems = []
    compiler = cxx or default_cxx_compiler()

    # 1. Detector self-test on forbidden symbols.
    flagged = set(check_exports.development_symbols(set(FORBIDDEN_DEV0_SYMBOLS)))
    missing_flagged = set(FORBIDDEN_DEV0_SYMBOLS) - flagged
    if missing_flagged:
        problems.append(f"check_exports.development_symbols missed forbidden symbols: {sorted(missing_flagged)}")

    # 2. Positive baseline: sdk/rocketnet_test_seams.h, sdk/rocketnet_connect_token.h,
    # and sdk/rocketnet_connection_reset.h compile cleanly under ROCKETNET_DEV=0.
    baseline_code = (
        '#include "rocketnet_test_seams.h"\n'
        '#include "rocketnet_connect_token.h"\n'
        '#include "rocketnet_connection_reset.h"\n'
        "int red_baseline(void) { return 0; }\n"
    )
    res = compile_snippet(compiler, repo_root, baseline_code, ["ROCKETNET_DEV=0"])
    if res.returncode != 0:
        problems.append(f"Baseline DEV=0 header compile failed unexpectedly:\n{res.stdout}\n{res.stderr}")
        return problems

    # 3. The one remaining seam with a public name is undeclared when ROCKETNET_DEV=0.
    res = compile_snippet(
        compiler,
        repo_root,
        '#include "rocketnet_test_seams.h"\nvoid * p = (void *)&rocketnet_dev_session_sequence_start;\n',
        ["ROCKETNET_DEV=0"],
    )
    if res.returncode == 0:
        problems.append("Red control failed: rocketnet_dev_session_sequence_start remained declared under ROCKETNET_DEV=0")

    # 4. Deterministic token helper and reset-inventory walker must be undeclared when ROCKETNET_DEV=0.
    for symbol, header in (
        ("rocketnet_write_connect_token_deterministic", "rocketnet_connect_token.h"),
        ("rocketnet_reset_inventory_matches", "rocketnet_connection_reset.h"),
        ("rocketnet_client_connection_reset_inventory_matches_for_test", "rocketnet_connection_reset.h"),
        ("rocketnet_client_transport_connection_reset_inventory_matches_for_test", "rocketnet_connection_reset.h"),
        ("rocketnet_server_connection_reset_inventory_matches_for_test", "rocketnet_connection_reset.h"),
    ):
        snippet = f'#include "{header}"\nvoid * p = (void *)&{symbol};\n'
        res = compile_snippet(compiler, repo_root, snippet, ["ROCKETNET_DEV=0"])
        if res.returncode == 0:
            problems.append(f"Red control failed: {symbol} remained declared under ROCKETNET_DEV=0 in {header}")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--build", type=Path, help="DEV=0 CMake build directory to inspect")
    parser.add_argument("--shared", type=Path, help="DEV=0 shared library to inspect")
    parser.add_argument("--archive", type=Path, help="DEV=0 static archive to inspect")
    parser.add_argument("--import-library", type=Path, help="Windows DEV=0 import library to inspect")
    parser.add_argument("--red-controls", action="store_true", help="Run compiler and CMake negative-control checks")
    parser.add_argument("--cxx", type=str, help="C++ compiler to use for --red-controls")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    problems = check_sources(repo_root)

    if args.build is not None:
        problems += check_build_dir(args.build.resolve())

    if args.shared is not None or args.archive is not None:
        if args.shared is None or args.archive is None:
            parser.error("--shared and --archive must be supplied together")
        cmd = [sys.executable, str(repo_root / "tools" / "check_exports.py")]
        if args.import_library is not None:
            cmd += ["--import-library", str(args.import_library)]
        cmd += [str(args.shared), str(args.archive)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            problems.append(f"check_exports.py failed:\n{res.stdout}\n{res.stderr}")
        else:
            print(res.stdout.strip())

    if args.red_controls:
        problems += run_red_controls(repo_root, args.cxx)

    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        return 1

    summary = [f"Source & header boundary clean ({len(RETIRED_SWITCHES)} retired switches absent)"]
    if args.build is not None:
        summary.append(f"build dir {args.build} compiles with ROCKETNET_DEV=0 and no retired switch")
    if args.red_controls:
        summary.append("1 seam + 5 symbol negative controls refused as expected")
    print("; ".join(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
