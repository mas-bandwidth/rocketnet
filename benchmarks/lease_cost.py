#!/usr/bin/env python3
"""Build matched isolated lease-cost variants and run valid-call comparisons (POSIX)."""
import argparse
import hashlib
import io
import itertools
import json
import math
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--samples", type=int, default=5)
parser.add_argument("--batches", type=int, default=300)
parser.add_argument("--work", type=Path, default=Path("build/lease-cost-run"))
args = parser.parse_args()
if args.samples < 1 or args.batches < 1:
    parser.error("samples and batches must be positive")
root = Path(__file__).resolve().parents[1]
work = args.work.resolve()
# Refuse reuse so a prior control, CMake cache or stray file cannot enter a result.
work.mkdir(parents=True, exist_ok=False)
if platform.system() not in ("Darwin", "Linux"):
    parser.error("the build controller supports macOS and Linux")
env = {k: v for k, v in os.environ.items() if not k.startswith("ROCKETNET_")}
revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
archived = subprocess.check_output(["git", "archive", revision], cwd=root)
compiler = shlex.split(os.environ.get("CXX", "c++"))
libs = [] # The archived default SDK contains its pinned sodium implementation.
libs += ["-framework", "CoreFoundation", "-framework", "SystemConfiguration"] if platform.system() == "Darwin" else ["-lbpf", "-lxdp", "-pthread"]
results = {"revision": revision, "platform": platform.platform(),
           "compiler": subprocess.check_output(compiler + ["--version"], text=True).splitlines()[0],
           "sodium": json.loads((root / "sodium/manifest.json").read_text())["upstream_version"] + " vendored",
           "samples": args.samples, "batches": args.batches, "status": "running", "results": []}
for variant in ("checked", "control"):
    source = work / variant
    source.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archived)) as archive:
        archive.extractall(source, filter="data")
    header = source / "sdk/rocketnet_packet_lease.h"
    if variant == "control":
        text = header.read_text()
        start = text.index("        if ( !epoch || used.fetch_add")
        end = text.index("\n    bool take(", start)
        # Diagnostic only: preserve storage and all other SDK code. No checked
        # library build uses this header, and this is not a supported API mode.
        header.write_text(text[:start] + """        (void) context;
        return true;
    }

    bool resolve( const rocketnet_packet_context_t & supplied, rocketnet_packet_context_t & trusted, uint64_t & backend_id, bool consume )
    {
        (void) consume;
        trusted = supplied;
        backend_id = supplied.packet_id;
        return supplied.frame != NULL;
    }
""" + text[end:])
    results[variant + "_header_sha256"] = hashlib.sha256(header.read_bytes()).hexdigest()
    build = source / "build"
    with (work / (variant + "-build.log")).open("w") as log:
        subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                        "-DROCKETNET_BUILD_TESTS=OFF", "-DROCKETNET_BUILD_TOOLS=OFF", "-DROCKETNET_BUILD_EXAMPLES=OFF"],
                       stdout=log, stderr=subprocess.STDOUT, check=True, env=env)
        subprocess.run(["cmake", "--build", str(build), "--target", "rocketnet", "--parallel", "4"],
                       stdout=log, stderr=subprocess.STDOUT, check=True, env=env)
    subprocess.run(compiler + ["-O3", "-DNDEBUG", "-std=c++11", "-ffp-contract=off", "-I", str(source / "sdk"),
                              str(source / "benchmarks/lease_cost.cpp"), str(build / "librocketnet.a"), *libs,
                              "-o", str(work / (variant + "-bench"))], check=True, env=env)

def run(variant, endpoint, operation, size, threads, sample, plant=False):
    command = [str(work / (variant + "-bench")), endpoint, operation, str(size), str(threads), str(args.batches)]
    if plant:
        command.append("plant")
    completed = subprocess.run(command, env=env, text=True, capture_output=True, timeout=120)
    if completed.returncode:
        raise RuntimeError(f"benchmark failed: {command}\n{completed.stdout}\n{completed.stderr}")
    row = json.loads(completed.stdout)
    if not math.isfinite(row["ns_per_packet"]) or row["ns_per_packet"] <= 0:
        raise RuntimeError(f"invalid timing result: {row}")
    row.update(variant=variant, sample=sample)
    results["results"].append(row)
    (work / "results.json").write_text(json.dumps(results, indent=2) + "\n")

for sample in range(args.samples):
    for endpoint, operation, size, threads in itertools.product(
            ("server", "client"), ("finish", "abort", "access"), (64, 1192, 4096), (1, 4)):
        order = ("checked", "control") if sample % 2 == 0 else ("control", "checked")
        for variant in order:
            run(variant, endpoint, operation, size, threads, sample)
    print(f"paired sample {sample + 1}/{args.samples}", flush=True)
for variant in ("checked", "control"):
    run(variant, "server", "finish", 64, 1, "plant", plant=True)
    values = [r["ns_per_packet"] for r in results["results"] if r["variant"] == variant
              and r["endpoint"] == "server" and r["operation"] == "finish"
              and r["payload_bytes"] == 64 and r["threads"] == 1 and not r["plant"]]
    if results["results"][-1]["ns_per_packet"] <= 2 * max(values):
        raise RuntimeError(f"{variant}: planted workload did not clearly move timing")
results["status"] = "complete"
(work / "results.json").write_text(json.dumps(results, indent=2) + "\n")
print(work / "results.json")
