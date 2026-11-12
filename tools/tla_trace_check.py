#!/usr/bin/env python3
"""Run the TLA+ trace check of one layer or of every layer (proofs/tla/README-TraceCheck.md).

For each seed, bin/tla_trace writes a trace of the layer's code and
bin/tla_trace_plant writes the same walk over the planted code. Each trace
goes into a private root of its own with the layer's modules and a CASES.tsv:
the clean trace must pass, the planted trace must violate TraceConsistent.
The pinned tlacheck runs each as its own run, so each has the whole run
budget; --jobs runs go at a time. A planted walk that never fired its plant
is a failure: its trace would prove nothing.

A layer is one file, tests/tla_trace_<layer>.cpp, that registers itself with
TLA_TRACE_LAYER, plus one row of LAYERS below. --layer all runs every row,
and requires the rows and the harness's registered layers (tla_trace --list)
to be the same set: a layer with no row, or a row with no layer, is red.

  python3 tools/tla_trace_check.py --bin build/bin --tlacheck TLACHECK \\
      --jar /opt/tla/tla2tools.jar --layer all --seeds 1-4 --jobs 4 --dir WORK
"""

import argparse
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TLA = ROOT / "proofs" / "tla"

# One row per layer: the modules TLC reads (the spec, the modules it extends
# from proofs/tla, and Trace<Spec>.tla), and quiet: True when the plant's
# condition is the planted code's own act, so the clean walk must print
# PLANT-FIRED 0; False when the condition is the input's (a case the plant
# would get wrong), which the clean walk meets too.
LAYERS = {
    "example": {
        "modules": ["RNTraceExample.tla", "TraceRNTraceExample.tla"],
        "quiet": True,
    },
    "fillphase": {
        "modules": ["RNFillPhase.tla", "TraceRNFillPhase.tla"],
        "quiet": False,
    },
}

HEADER = "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"
FIRED = re.compile(r"^PLANT-FIRED ([0-9]+)$", re.M)
TRACE = re.compile(r"^TRACE (\S+)\.tla records=([0-9]+)$", re.M)


def seeds(text):
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", text)
    if not match or int(match.group(1)) > int(match.group(2)):
        raise argparse.ArgumentTypeError(f"seeds must be A-B with A <= B: {text}")
    return range(int(match.group(1)), int(match.group(2)) + 1)


def walk(args, name, seed, binary, tla, log):
    """(module name or None, plant fired count or None) of one walk into tla"""
    result = subprocess.run([str(args.bin / binary), "--layer", name, "--seed", str(seed), "--out", str(tla)],
                            capture_output=True, text=True)
    log.append(result.stdout)
    log.append(result.stderr)
    traced = TRACE.search(result.stdout)
    fired = FIRED.search(result.stdout)
    if result.returncode != 0 or not traced or not fired:
        return None, None
    return Path(traced.group(1)).name, int(fired.group(1))


def check(args, name, layer, seed, kind, module, log):
    """tlacheck one trace in a root of its own: None, or the failure"""
    work = args.dir / f"{name}-{seed}-{kind}"
    tla = work / "root" / "tla"
    expected = "pass\t-" if kind == "clean" else "invariant\tTraceConsistent"
    rows = HEADER + f"{module}.cfg\t{module}.tla\t{expected}\tcheck\ttrace{name}\trequired\t-\n"
    (tla / "CASES.tsv").write_text(rows)
    result = subprocess.run([args.tlacheck, "run", "--root", str(work / "root"), "--dir", str(work / "run"), "--jar", args.jar],
                            capture_output=True, text=True)
    log.append(result.stdout)
    log.append(result.stderr)
    if result.returncode != 0:
        return f"{kind} trace: tlacheck exit {result.returncode}"
    return None


def prepare(args, name, layer, seed, kind):
    work = args.dir / f"{name}-{seed}-{kind}"
    if work.exists():
        shutil.rmtree(work)
    tla = work / "root" / "tla"
    tla.mkdir(parents=True)
    for module in layer["modules"]:
        shutil.copy(TLA / module, tla / module)
    return tla


def check_seed(args, name, layer, seed):
    """(name, seed, log, failure or None)"""
    log = []
    clean_tla = prepare(args, name, layer, seed, "clean")
    plant_tla = prepare(args, name, layer, seed, "plant")

    clean, clean_fired = walk(args, name, seed, "tla_trace", clean_tla, log)
    plant, plant_fired = walk(args, name, seed, "tla_trace_plant", plant_tla, log)
    if clean is None or plant is None:
        return name, seed, log, "the harness failed or printed no TRACE and PLANT-FIRED line"
    if plant_fired == 0:
        return name, seed, log, "the plant never fired"
    if layer["quiet"] and clean_fired != 0:
        return name, seed, log, "the clean code met the plant's condition"

    with ThreadPoolExecutor(max_workers=2) as pool:
        failures = list(pool.map(lambda job: check(args, name, layer, seed, job[0], job[1], log),
                                 [("clean", clean), ("plant", plant)]))
    failures = [failure for failure in failures if failure]
    return name, seed, log, "; ".join(failures) if failures else None


def registered(args):
    result = subprocess.run([str(args.bin / "tla_trace"), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return set(result.stdout.split())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--tlacheck", required=True)
    parser.add_argument("--jar", required=True)
    parser.add_argument("--layer", required=True, choices=sorted(LAYERS) + ["all"])
    parser.add_argument("--seeds", required=True, type=seeds)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--dir", required=True, type=Path)
    args = parser.parse_args()

    failed = []
    names = [args.layer]
    if args.layer == "all":
        names = sorted(LAYERS)
        found = registered(args)
        if found is None:
            failed.append("tla_trace --list failed")
        else:
            for name in sorted(found - set(LAYERS)):
                failed.append(f"layer {name}: registered by the harness, no row in LAYERS")
            for name in sorted(set(LAYERS) - found):
                failed.append(f"layer {name}: a row in LAYERS, not registered by the harness")

    jobs = [(name, seed) for name in names for seed in args.seeds]
    # each seed runs two tlacheck runs at once
    with ThreadPoolExecutor(max_workers=max(1, args.jobs // 2)) as pool:
        results = list(pool.map(lambda job: check_seed(args, job[0], LAYERS[job[0]], job[1]), jobs))

    for name, seed, log, failure in results:
        print(f"== layer {name} seed {seed}")
        sys.stdout.write("".join(log))
        if failure:
            failed.append(f"layer {name} seed {seed}: {failure}")

    for line in failed:
        print(f"TRACE-CHECK FAIL {line}")
    if failed:
        return 1
    print(f"TRACE-CHECK OK layers={','.join(names)} seeds={args.seeds.start}-{args.seeds.stop - 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
