#!/usr/bin/env python3
"""Run the TLA+ trace check of a layer (proofs/tla/README-TraceCheck.md).

For each seed, bin/tla_trace writes a trace of the layer's code and
bin/tla_trace_plant writes the same walk over the planted code. The traces go
into a private root with the layer's modules and a CASES.tsv that declares
the clean trace pass and the planted trace a TraceConsistent violation; the
pinned tlacheck runs them, one run per seed, --jobs seeds at a time. A
planted walk that never fired its plant is a failure: its trace would prove
nothing.

  python3 tools/tla_trace_check.py --bin build/bin --tlacheck TLACHECK \\
      --jar /opt/tla/tla2tools.jar --layer bank --seeds 1-8 --jobs 4 --dir WORK
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

# each layer: the module names the harness writes, the modules TLC reads, and
# the line that says how often the plant's condition happened
LAYERS = {
    "bank": {
        "clean": "MCTraceBankSeed",
        "plant": "MCTraceBankPlantSeed",
        "modules": ["RNUringQueueBankLayout.tla", "TraceRNUringQueueBankLayout.tla"],
        "fired": re.compile(r"^STALE-TURNS ([0-9]+)$", re.M),
        "quiet": True,
    },
    "gather": {
        "clean": "MCTraceGatherSeed",
        "plant": "MCTraceGatherPlantSeed",
        "modules": ["RNSendGatherLayout.tla", "TraceRNSendGatherLayout.tla"],
        "fired": re.compile(r"^GATHER-REORDERABLE ([0-9]+)$", re.M),
        "quiet": False,
    },
}

HEADER = "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"


def seeds(text):
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", text)
    if not match or int(match.group(1)) > int(match.group(2)):
        raise argparse.ArgumentTypeError(f"seeds must be A-B with A <= B: {text}")
    return range(int(match.group(1)), int(match.group(2)) + 1)


def check_seed(args, layer, seed):
    """(seed, log, failure or None)"""
    log = []

    def run(argv):
        result = subprocess.run(argv, capture_output=True, text=True)
        log.append(result.stdout)
        log.append(result.stderr)
        return result

    work = args.dir / f"{args.layer}-{seed}"
    if work.exists():
        shutil.rmtree(work)
    tla = work / "root" / "tla"
    tla.mkdir(parents=True)
    for module in layer["modules"]:
        shutil.copy(TLA / module, tla / module)

    clean = run([str(args.bin / "tla_trace"), "--layer", args.layer, "--seed", str(seed), "--out", str(tla)])
    plant = run([str(args.bin / "tla_trace_plant"), "--layer", args.layer, "--seed", str(seed), "--out", str(tla)])
    if clean.returncode != 0 or plant.returncode != 0:
        return seed, log, "the harness failed"

    fired = layer["fired"].search(plant.stdout)
    if not fired or int(fired.group(1)) == 0:
        return seed, log, "the plant never fired"
    if layer["quiet"]:
        # the plant's condition is the planted code's own act: the clean code never meets it
        quiet = layer["fired"].search(clean.stdout)
        if not quiet or int(quiet.group(1)) != 0:
            return seed, log, "the clean code met the plant's condition"

    group = f"trace{args.layer}"
    rows = HEADER
    rows += f"{layer['clean']}{seed}.cfg\t{layer['clean']}{seed}.tla\tpass\t-\tcheck\t{group}\trequired\t-\n"
    rows += f"{layer['plant']}{seed}.cfg\t{layer['plant']}{seed}.tla\tinvariant\tTraceConsistent\tcheck\t{group}\trequired\t-\n"
    (tla / "CASES.tsv").write_text(rows)

    checked = run([args.tlacheck, "run", "--root", str(work / "root"), "--dir", str(work / "run"), "--jar", args.jar])
    if checked.returncode != 0:
        return seed, log, f"tlacheck exit {checked.returncode}"
    return seed, log, None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--tlacheck", required=True)
    parser.add_argument("--jar", required=True)
    parser.add_argument("--layer", required=True, choices=sorted(LAYERS))
    parser.add_argument("--seeds", required=True, type=seeds)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--dir", required=True, type=Path)
    args = parser.parse_args()

    layer = LAYERS[args.layer]
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda seed: check_seed(args, layer, seed), args.seeds))

    failed = []
    for seed, log, failure in results:
        print(f"== seed {seed}")
        sys.stdout.write("".join(log))
        if failure:
            failed.append(f"seed {seed}: {failure}")

    for line in failed:
        print(f"TRACE-CHECK FAIL {line}")
    if failed:
        return 1
    print(f"TRACE-CHECK OK layer={args.layer} seeds={args.seeds.start}-{args.seeds.stop - 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
