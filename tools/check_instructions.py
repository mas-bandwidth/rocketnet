#!/usr/bin/env python3
"""
Deterministic instruction-count gate for RocketNet client_frame and server_tick.

Runs benchmarks/instruction_count under valgrind --tool=cachegrind for each region
(client_frame, server_tick), parses the collected Ir count from the cachegrind
output file with the Python standard library, and compares against
benchmarks/instruction_baseline.json.

Exit codes:
  0: PASS (all regions <= +2.0% over baseline; prints NOTE if < -2.0% under)
  1: FAIL (any region > +2.0% over baseline, build_type mismatch, or runner error)
  2: NOT COMPARABLE (toolchain_id, compiler_id or valgrind_version differs from
     baseline; outputs 'not comparable: <local> vs <pinned> verdict=FAIL' and
     names the pinned toolchain to measure with)

The toolchain_id tells the pinned build (tools/build_pinned_toolchain.sh, which
writes <prefix>/toolchain-id naming both versions and the script's hash) apart
from a host or distro build that reports the same compiler_id. --toolchain
<prefix> measures with that prefix's valgrind and records its id; without it
the id is 'unpinned', which no pinned baseline accepts.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from typing import Any, Callable, Dict, List, Optional, Tuple

from nightly_timeout import execution_timeout

REGIONS: Tuple[str, ...] = ("client_frame", "server_tick")
UNPINNED_TOOLCHAIN_ID: str = "unpinned"
DEFAULT_THRESHOLD_PCT: float = 2.0
DEFAULT_BASELINE_PATH: str = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "benchmarks",
    "instruction_baseline.json",
)


def parse_cachegrind_ir(content: str) -> int:
    """
    Parse the total Ir (instruction read) count from a cachegrind.out file.
    Raises ValueError if events: or summary: is missing or malformed.
    """
    if not content:
        raise ValueError("empty cachegrind output")
    ir_index: Optional[int] = None
    for raw_line in content.splitlines():
        line = raw_line.strip()
        if line.startswith("events:"):
            events = line[len("events:") :].strip().split()
            if "Ir" not in events:
                raise ValueError(f"cachegrind events line has no Ir column: {line!r}")
            ir_index = events.index("Ir")
        elif line.startswith("summary:"):
            if ir_index is None:
                ir_index = 0
            parts = line[len("summary:") :].strip().split()
            if ir_index >= len(parts):
                raise ValueError(f"cachegrind summary line has too few columns: {line!r}")
            try:
                ir_val = int(parts[ir_index])
            except ValueError as exc:
                raise ValueError(f"invalid Ir integer in cachegrind summary: {line!r}") from exc
            if ir_val < 0:
                raise ValueError(f"negative Ir count in cachegrind summary: {line!r}")
            return ir_val
    raise ValueError("missing summary: line in cachegrind output")


def parse_scenario_stdout(stdout: str) -> Dict[str, Any]:
    """
    Parse the metadata line printed by benchmarks/instruction_count:
      instruction_count region=<r> units=<n> [mode=<m>] compiler_id=<c> build_type=<b>
    """
    if not stdout:
        raise ValueError("empty stdout from instruction_count")
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line.startswith("instruction_count "):
            continue
        kv: Dict[str, Any] = {}
        for token in line.split()[1:]:
            if "=" not in token:
                continue
            k, v = token.split("=", 1)
            kv[k] = v
        if "region" not in kv or "units" not in kv:
            raise ValueError(f"missing region or units in instruction_count line: {line!r}")
        try:
            kv["units"] = int(kv["units"])
        except ValueError as exc:
            raise ValueError(f"invalid units in instruction_count line: {line!r}") from exc
        if kv["units"] <= 0:
            raise ValueError(f"non-positive units in instruction_count line: {line!r}")
        return kv
    raise ValueError("missing instruction_count metadata line in stdout")


def get_valgrind_version(valgrind: str = "valgrind") -> str:
    proc = subprocess.run(
        [valgrind, "--version"],
        capture_output=True,
        text=True,
        timeout=execution_timeout(30),
    )
    if proc.returncode != 0:
        raise RuntimeError(f"valgrind --version exited {proc.returncode}: {proc.stderr.strip()}")
    version = proc.stdout.strip().splitlines()[0].strip()
    if not version:
        raise RuntimeError("empty output from valgrind --version")
    return version


def get_git_head_commit() -> str:
    proc = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        timeout=execution_timeout(30),
    )
    if proc.returncode != 0:
        return "unknown"
    return proc.stdout.strip() or "unknown"


def check_toolchain_comparable(
    baseline: Dict[str, Any],
    compiler_id: str,
    valgrind_version: str,
    toolchain_id: str,
) -> Optional[str]:
    """
    Return 'not comparable: <local> vs <pinned>' when toolchain_id, compiler_id
    or valgrind_version differs from the pinned baseline, or None when
    comparable. A baseline with no toolchain_id matches no toolchain.
    """
    pinned_toolchain = str(baseline.get("toolchain_id", ""))
    pinned_compiler = str(baseline.get("compiler_id", ""))
    pinned_valgrind = str(baseline.get("valgrind_version", ""))
    if (
        not pinned_toolchain
        or toolchain_id != pinned_toolchain
        or compiler_id != pinned_compiler
        or valgrind_version != pinned_valgrind
    ):
        local = f"{toolchain_id}/{compiler_id}/{valgrind_version}"
        pinned = f"{pinned_toolchain or '(none)'}/{pinned_compiler}/{pinned_valgrind}"
        return f"not comparable: {local} vs {pinned}"
    return None


def evaluate_instructions(
    baseline: Dict[str, Any],
    measured: Dict[str, Dict[str, Any]],
    compiler_id: str,
    valgrind_version: str,
    build_type: str,
    toolchain_id: str,
    threshold_pct: Optional[float] = None,
) -> Tuple[int, str]:
    """
    Compare measured region instruction counts against the loaded baseline.
    Returns (exit_code, report_text).
    """
    lines: List[str] = []

    mismatches: List[str] = []
    for field, actual in (
        ("toolchain_id", toolchain_id),
        ("compiler_id", compiler_id),
        ("valgrind_version", valgrind_version),
        ("build_type", build_type),
    ):
        expected = str(baseline.get(field, ""))
        if expected != actual:
            mismatches.append(f"{field} mismatch (baseline={expected!r} actual={actual!r})")

    if mismatches:
        mismatch_str = "; ".join(mismatches)
        lines.append(f"toolchain mismatch: {mismatch_str} verdict=FAIL")
        return 1, "\n".join(lines)

    limit_pct = float(
        threshold_pct
        if threshold_pct is not None
        else baseline.get("threshold_pct", DEFAULT_THRESHOLD_PCT)
    )
    baseline_regions = baseline.get("regions", {})
    any_fail = False

    modes = [
        str(measured.get(r, {}).get("mode", ""))
        for r in REGIONS
        if measured.get(r, {}).get("mode")
    ]
    if modes:
        lines.append(f"isolation mode={modes[0]}")

    for region in REGIONS:
        if region not in measured:
            lines.append(f"instructions region={region} verdict=FAIL (missing measured region)")
            any_fail = True
            continue
        if region not in baseline_regions:
            lines.append(f"instructions region={region} verdict=FAIL (missing baseline region)")
            any_fail = True
            continue

        ir = int(measured[region]["ir"])
        units = int(measured[region]["units"])
        if units <= 0:
            lines.append(f"instructions region={region} verdict=FAIL (invalid units={units})")
            any_fail = True
            continue
        per_unit = ir / float(units)

        base_entry = baseline_regions[region]
        base_ir = int(base_entry["ir"])
        if base_ir <= 0:
            lines.append(f"instructions region={region} verdict=FAIL (invalid baseline={base_ir})")
            any_fail = True
            continue

        delta_pct = ((ir - base_ir) / float(base_ir)) * 100.0
        passed = delta_pct <= limit_pct
        verdict = "PASS" if passed else "FAIL"
        if not passed:
            any_fail = True

        lines.append(
            f"instructions region={region} ir={ir} per_unit={per_unit:.2f} "
            f"baseline={base_ir} delta_pct={delta_pct:+.2f}% verdict={verdict}"
        )
        if delta_pct < -limit_pct:
            lines.append(
                f"NOTE: region={region} is {abs(delta_pct):.2f}% under baseline "
                f"(>{limit_pct:.2f}% improvement); consider --repin --reason <text> if intentional"
            )

    return (1 if any_fail else 0), "\n".join(lines)


def write_repin_baseline(
    baseline_path: str,
    measured: Dict[str, Dict[str, Any]],
    compiler_id: str,
    valgrind_version: str,
    build_type: str,
    toolchain_id: str,
    commit: str,
    reason: str,
    threshold_pct: float = DEFAULT_THRESHOLD_PCT,
) -> Dict[str, Any]:
    """
    Write benchmarks/instruction_baseline.json with measured counts and provenance.
    Raises ValueError if reason is empty.
    """
    clean_reason = (reason or "").strip()
    if not clean_reason:
        raise ValueError("--repin requires a non-empty --reason <text>")
    if not toolchain_id or toolchain_id == UNPINNED_TOOLCHAIN_ID:
        raise ValueError("--repin requires --toolchain <prefix> from tools/build_pinned_toolchain.sh")

    regions_out: Dict[str, Dict[str, Any]] = {}
    for region in REGIONS:
        if region not in measured:
            raise ValueError(f"missing measured region {region!r} for repin")
        ir = int(measured[region]["ir"])
        units = int(measured[region]["units"])
        regions_out[region] = {
            "ir": ir,
            "units": units,
            "per_unit": round(ir / float(units), 2),
        }

    payload: Dict[str, Any] = {
        "toolchain_id": toolchain_id,
        "compiler_id": compiler_id,
        "valgrind_version": valgrind_version,
        "build_type": build_type,
        "commit": commit,
        "reason": clean_reason,
        "threshold_pct": threshold_pct,
        "regions": regions_out,
    }
    os.makedirs(os.path.dirname(os.path.abspath(baseline_path)), exist_ok=True)
    with open(baseline_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=False)
        f.write("\n")
    return payload


def run_cachegrind_region(binary: str, region: str, valgrind: str = "valgrind") -> Dict[str, Any]:
    """
    Run valgrind --tool=cachegrind on `binary <region>` and return parsed counts
    and toolchain metadata.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        out_file = os.path.join(tmpdir, f"cachegrind.{region}.out")
        cmd = [
            valgrind,
            "--tool=cachegrind",
            "--instr-at-start=no",
            "--cache-sim=no",
            "--branch-sim=no",
            f"--cachegrind-out-file={out_file}",
            binary,
            region,
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=execution_timeout(120))
        if proc.returncode != 0:
            err = (proc.stderr or proc.stdout or "").strip()
            raise RuntimeError(f"cachegrind {region} exited {proc.returncode}: {err}")
        meta = parse_scenario_stdout(proc.stdout)
        with open(out_file, "r", encoding="utf-8") as f:
            cg_content = f.read()
        ir = parse_cachegrind_ir(cg_content)

        mode = str(meta.get("mode", "client_requests"))
        if mode == "subtract_setup":
            full_out = os.path.join(tmpdir, f"cachegrind.{region}.full.out")
            setup_out = os.path.join(tmpdir, f"cachegrind.{region}.setup.out")
            for tag, extra_args, target_out in (
                ("full", [region], full_out),
                ("setup", [region, "setup"], setup_out),
            ):
                sub_cmd = [
                    valgrind,
                    "--tool=cachegrind",
                    "--cache-sim=no",
                    "--branch-sim=no",
                    f"--cachegrind-out-file={target_out}",
                    binary,
                    *extra_args,
                ]
                sub_proc = subprocess.run(sub_cmd, capture_output=True, text=True, timeout=execution_timeout(120))
                if sub_proc.returncode != 0:
                    err = (sub_proc.stderr or sub_proc.stdout or "").strip()
                    raise RuntimeError(f"cachegrind {region} ({tag}) exited {sub_proc.returncode}: {err}")
            with open(full_out, "r", encoding="utf-8") as f:
                full_ir = parse_cachegrind_ir(f.read())
            with open(setup_out, "r", encoding="utf-8") as f:
                setup_ir = parse_cachegrind_ir(f.read())
            ir = max(0, full_ir - setup_ir)

        return {
            "region": region,
            "ir": ir,
            "units": int(meta["units"]),
            "mode": mode,
            "compiler_id": str(meta.get("compiler_id", "unknown")),
            "build_type": str(meta.get("build_type", "unknown")),
        }


def resolve_binary_path(path: str) -> str:
    if os.path.isfile(path) and os.access(path, os.X_OK):
        return path
    fallback = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "bin",
        os.path.basename(path),
    )
    if os.path.isfile(fallback) and os.access(fallback, os.X_OK):
        return fallback
    return path


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run cachegrind instruction gate on benchmarks/instruction_count."
    )
    parser.add_argument(
        "--bin",
        default="./bin/instruction_count",
        help="Path to the instruction_count binary (default: ./bin/instruction_count).",
    )
    parser.add_argument(
        "--baseline",
        default=DEFAULT_BASELINE_PATH,
        help="Path to instruction_baseline.json (default: benchmarks/instruction_baseline.json).",
    )
    parser.add_argument(
        "--toolchain",
        default=None,
        help="Prefix built by tools/build_pinned_toolchain.sh: measure with its valgrind "
        "and record its toolchain-id. Without it, valgrind is taken from PATH and the "
        "toolchain is 'unpinned', which no pinned baseline accepts.",
    )
    parser.add_argument(
        "--repin",
        action="store_true",
        help="Overwrite benchmarks/instruction_baseline.json with the newly measured counts.",
    )
    parser.add_argument(
        "--reason",
        default=None,
        help="Required reason string when --repin is set.",
    )
    args = parser.parse_args(argv)

    if args.repin and not (args.reason and args.reason.strip()):
        print("error: --repin requires --reason <text>", file=sys.stderr)
        return 1

    binary = resolve_binary_path(args.bin)
    if not (os.path.isfile(binary) and os.access(binary, os.X_OK)):
        print(f"error: instruction_count binary not found or not executable: {args.bin}", file=sys.stderr)
        return 1

    valgrind = "valgrind"
    toolchain_id = UNPINNED_TOOLCHAIN_ID
    if args.toolchain:
        prefix = os.path.abspath(args.toolchain)
        valgrind = os.path.join(prefix, "bin", "valgrind")
        os.environ["VALGRIND_LIB"] = os.path.join(prefix, "libexec", "valgrind")
        try:
            with open(os.path.join(prefix, "toolchain-id"), "r", encoding="utf-8") as f:
                toolchain_id = f.read().strip()
        except OSError as exc:
            print(f"error: {prefix} is not a pinned toolchain prefix: {exc} verdict=FAIL", file=sys.stderr)
            return 1

    try:
        valgrind_version = get_valgrind_version(valgrind)
        measured: Dict[str, Dict[str, Any]] = {}
        compiler_id = "unknown"
        build_type = "unknown"
        for region in REGIONS:
            res = run_cachegrind_region(binary, region, valgrind)
            measured[region] = res
            compiler_id = res["compiler_id"]
            build_type = res["build_type"]
    except Exception as exc:
        print(f"error: {exc} verdict=FAIL", file=sys.stderr)
        return 1

    if args.repin:
        commit = get_git_head_commit()
        try:
            baseline = write_repin_baseline(
                baseline_path=args.baseline,
                measured=measured,
                compiler_id=compiler_id,
                valgrind_version=valgrind_version,
                build_type=build_type,
                toolchain_id=toolchain_id,
                commit=commit,
                reason=args.reason or "",
            )
        except Exception as exc:
            print(f"error: {exc} verdict=FAIL", file=sys.stderr)
            return 1
    else:
        if not os.path.isfile(args.baseline):
            print(f"error: baseline file missing: {args.baseline} verdict=FAIL", file=sys.stderr)
            return 1
        with open(args.baseline, "r", encoding="utf-8") as f:
            baseline = json.load(f)

    not_comparable = check_toolchain_comparable(
        baseline=baseline,
        compiler_id=compiler_id,
        valgrind_version=valgrind_version,
        toolchain_id=toolchain_id,
    )
    if not_comparable is not None:
        print(f"{not_comparable} verdict=FAIL")
        print(
            f"the baseline is measured with toolchain {baseline.get('toolchain_id')} "
            f"({baseline.get('compiler_id')}, {baseline.get('valgrind_version')}); build it "
            "with tools/build_pinned_toolchain.sh, compile with its gcc and pass --toolchain <prefix>"
        )
        return 2

    code, report = evaluate_instructions(
        baseline=baseline,
        measured=measured,
        compiler_id=compiler_id,
        valgrind_version=valgrind_version,
        build_type=build_type,
        toolchain_id=toolchain_id,
    )
    print(report)
    return code


if __name__ == "__main__":
    sys.exit(main())
