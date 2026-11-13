#!/usr/bin/env python3
"""Run the TLA+ trace check of one layer or of every layer (proofs/tla/README-TraceCheck.md).

For each seed, bin/tla_trace writes a trace of the layer's code and the
layer's own plant binary, bin/tla_trace_plant_<file>, writes the same walk
over the planted code. Each walk runs twice and must write the same bytes
both times. Each trace is checked by the pinned tlacheck in a run and a root
of its own: the clean trace must pass, the planted trace must violate
TraceConsistent, at a record where the walk says the planted effect happened.

What makes a seed set red, each named in its TRACE-CHECK FAIL line:
  the harness: a walk that fails, times out, prints no TRACE, ACTS,
    PLANT-FIRED and PLANT-AT lines, or writes different output twice;
    a step layer whose trace is not steps + 1 records;
  the clean trace: rejected by TLC, a TLC timeout or error, or a clean walk
    that reports the planted effect;
  the plant: a walk whose planted effect never happened, a planted trace TLC
    accepts, or a rejection at a record where the plant did not fire;
  the layer: an action of the spec's Next (or a feature a case layer names)
    that no seed reached; a VarsRec that does not name exactly the spec's
    vars; a row with no registered layer, a registered layer with no row, a
    name registered twice, a plant binary that is not one layer.

A layer is one file, tests/tla_trace_<layer>.cpp, that registers itself
with TLA_TRACE_LAYER, plus one row of LAYERS below.

  python3 tools/tla_trace_check.py --bin build/bin --tlacheck TLACHECK \\
      --jar /opt/tla/tla2tools.jar --layer all --seeds 1-4 --jobs 8 --dir WORK
"""

import argparse
import filecmp
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TLA = ROOT / "proofs" / "tla"

# One row per layer.
#   modules  the modules TLC reads: the spec first, any module it extends
#            from proofs/tla, and its Trace<Spec>.tla.
#   kind     "steps": one record per step of the walk, [op, act, state];
#            the trace is steps + 1 records, every action of the spec's Next
#            not in untraced is reached across the seeds, and the Trace
#            module's VarsRec names exactly the spec's vars.
#            "cases": one record per independent case of a pure function;
#            every name in features is reached across the seeds.
#   untraced (steps) Next's actions no walk takes, each with the reason.
#   features (cases) the input and output shapes the walk must have shown.
LAYERS = {
    "example": {
        "modules": ["RNTraceExample.tla", "TraceRNTraceExample.tla"],
        "kind": "steps",
        "untraced": {},
    },
    "xdp-receive-ring": {
        "modules": ["RNXdpReceiveRing.tla", "TraceRNXdpReceiveRing.tla"],
        "kind": "steps",
        "untraced": {},
    },
    "xdp-replay-marks": {
        "modules": ["RNXdpReplayMarksLayout.tla", "TraceRNXdpReplayMarksLayout.tla"],
        "kind": "steps",
        "untraced": {},
    },
    "xdp-reply-macs": {
        "modules": ["RNXdpReplyMacsLayout.tla", "TraceRNXdpReplyMacsLayout.tla"],
        "kind": "steps",
        "untraced": {},
    },
    "xdp-gateway-refresh": {
        "modules": ["RNXdpGatewayRefresh.tla", "TraceRNXdpGatewayRefresh.tla"],
        "kind": "steps",
        "untraced": {
            "ReceiveWithoutRefresh": "enabled only under RefreshInFill, the reversed witness's rule; the code refreshes inside the receive call",
        },
    },
    "xdp-replay": {
        "modules": ["RNXdpReplay.tla", "TraceRNXdpReplay.tla"],
        "kind": "steps",
        "untraced": {
            "Seqs": "not an action: the set of sequences Next's Forge ranges over",
        },
    },
    # tests/tla_trace_iocp.cpp registers both IOCP layers: each plant
    # includes the transport's one .cpp, so they share a file
    "iocp-admit": {
        "modules": ["RNIocpReceiveAdmit.tla", "TraceRNIocpReceiveAdmit.tla"],
        "quiet": True,
    },
    "iocp-lanes": {
        "modules": ["RNIocpSendLanes.tla", "TraceRNIocpSendLanes.tla"],
        "quiet": True,
    },
    "bank": {
        "modules": ["RNUringQueueBankLayout.tla", "TraceRNUringQueueBankLayout.tla"],
        "quiet": True,
    },
    "gather": {
        "modules": ["RNSendGatherLayout.tla", "TraceRNSendGatherLayout.tla"],
        "quiet": False,
    },
}

HEADER = "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"
TRACE = re.compile(r"^TRACE (\S+)\.tla records=([0-9]+)$", re.M)
ACTS = re.compile(r"^ACTS((?: [A-Za-z][A-Za-z0-9]*=[0-9]+)*)$", re.M)
FIRED = re.compile(r"^PLANT-FIRED ([0-9]+)$", re.M)
PLANT_AT = re.compile(r"^PLANT-AT (-|[0-9]+(?:,[0-9]+)*)$", re.M)
CASE_LINE = re.compile(r"^CASE \S+ config=\S+ result=(\S+) seconds=\S+ exit=([0-9]+)", re.M)
WALK_TIMEOUT = 120


def seeds(text):
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", text)
    if not match or int(match.group(1)) > int(match.group(2)):
        raise argparse.ArgumentTypeError(f"seeds must be A-B with A <= B: {text}")
    return range(int(match.group(1)), int(match.group(2)) + 1)


def definition(text, name):
    """The text of `name ==` up to the next blank line or top-level definition."""
    match = re.search(r"^" + re.escape(name) + r"\s*==(.*?)(?=^\S|\Z)", text, re.M | re.S)
    return match.group(1) if match else None


def spec_facts(layer):
    """(vars, Next's actions, VarsRec's fields) read from the layer's modules, or a failure."""
    spec = (TLA / layer["modules"][0]).read_text()
    trace_name = [m for m in layer["modules"] if m.startswith("Trace")]
    if len(trace_name) != 1:
        return None, "the row names no Trace module, or more than one"
    trace = (TLA / trace_name[0]).read_text()
    nxt = definition(spec, "Next")
    if nxt is None:
        return None, f"{layer['modules'][0]} has no Next"
    # Next's actions: the names in it the spec defines (constants and sets it
    # names are not definitions)
    defined = set(re.findall(r"^([A-Z][A-Za-z0-9]*)(?:\([^)]*\))?\s*==", spec, re.M))
    actions = set(re.findall(r"\b([A-Z][A-Za-z0-9]*)\b", nxt)) & defined
    if layer["kind"] != "steps":
        return (None, actions, None), None
    vars_text = definition(spec, "vars")
    rec_text = definition(trace, "VarsRec")
    if vars_text is None or rec_text is None:
        return None, "the spec has no vars or the Trace module no VarsRec"
    names = set(re.findall(r"\b([a-z][A-Za-z0-9]*)\b", vars_text))
    fields = set(re.findall(r"\b([A-Za-z][A-Za-z0-9]*)\s*\|->", rec_text))
    return (names, actions, fields), None


def walk(args, binary, name, seed, out, log):
    """(report dict, or None) of one walk into out"""
    out.mkdir(parents=True)
    try:
        result = subprocess.run([str(args.bin / binary), "--layer", name, "--seed", str(seed), "--steps", str(args.steps),
                                 "--out", str(out)], capture_output=True, text=True, timeout=WALK_TIMEOUT)
    except subprocess.TimeoutExpired:
        log.append(f"{binary}: no end within {WALK_TIMEOUT} s\n")
        return None
    log.append(result.stdout)
    log.append(result.stderr)
    traced, acts, fired, at = (TRACE.search(result.stdout), ACTS.search(result.stdout),
                               FIRED.search(result.stdout), PLANT_AT.search(result.stdout))
    if result.returncode != 0 or not (traced and acts and fired and at):
        return None
    counts = {}
    for item in acts.group(1).split():
        key, value = item.split("=")
        counts[key] = int(value)
    return {
        "module": Path(traced.group(1)).name,
        "records": int(traced.group(2)),
        "acts": counts,
        "fired": int(fired.group(1)),
        "at": set() if at.group(1) == "-" else {int(x) for x in at.group(1).split(",")},
        "stdout": result.stdout.replace(str(out), "<out>"),
    }


def same_walk(first, second, a, b):
    if first["stdout"] != second["stdout"]:
        return False
    for path in a.iterdir():
        if not filecmp.cmp(path, b / path.name, shallow=False):
            return False
    return True


def tlc(args, name, kind_dir, module, expected, layer, log):
    """(exit of the TLC case, its log text) of one trace in a root of its own"""
    tla = kind_dir / "root" / "tla"
    tla.mkdir(parents=True)
    for m in layer["modules"]:
        shutil.copy(TLA / m, tla / m)
    for suffix in (".tla", ".cfg"):
        shutil.copy(kind_dir / "walk" / (module + suffix), tla / (module + suffix))
    (tla / "CASES.tsv").write_text(HEADER + f"{module}.cfg\t{module}.tla\t{expected}\tcheck\ttrace{name.replace('-', '')}\trequired\t-\n")
    result = subprocess.run([args.tlacheck, "run", "--root", str(kind_dir / "root"), "--dir", str(kind_dir / "run"),
                             "--jar", args.jar], capture_output=True, text=True)
    log.append(result.stdout)
    log.append(result.stderr)
    case = CASE_LINE.search(result.stdout)
    tlc_log = kind_dir / "run" / f"{module}.cfg.log"
    return (int(case.group(2)) if case else -1), (tlc_log.read_text() if tlc_log.exists() else "")


def tlc_failure(kind, exit_code):
    if exit_code == 124:
        return f"{kind} trace: TLC timeout (the run budget)"
    if kind == "clean" and exit_code == 12:
        return "clean trace rejected by TLC (TraceConsistent)"
    if kind == "plant" and exit_code == 0:
        return "plant trace accepted by TLC"
    return f"{kind} trace: TLC exit {exit_code}"


def check_seed(args, name, layer, plant_binary, seed):
    """(name, seed, log, failures, clean report)"""
    log, failures = [], []
    base = args.dir / f"{name}-{seed}"
    if base.exists():
        shutil.rmtree(base)

    reports = {}
    for kind, binary in (("clean", "tla_trace"), ("plant", plant_binary)):
        first = walk(args, binary, name, seed, base / kind / "walk", log)
        second = walk(args, binary, name, seed, base / kind / "again", log)
        if first is None or second is None:
            failures.append(f"{kind} harness failed, timed out or printed no TRACE, ACTS, PLANT-FIRED and PLANT-AT lines")
            return name, seed, log, failures, None
        if not same_walk(first, second, base / kind / "walk", base / kind / "again"):
            failures.append(f"{kind} walk is nondeterministic: two runs of one seed differ")
            return name, seed, log, failures, None
        if layer["kind"] == "steps" and first["records"] != args.steps + 1:
            failures.append(f"{kind} trace has {first['records']} records, not steps + 1 = {args.steps + 1}")
        if first["records"] < 2:
            failures.append(f"{kind} trace has fewer than 2 records")
        reports[kind] = first

    if reports["clean"]["fired"] != 0:
        failures.append(f"clean code shows the planted effect {reports['clean']['fired']} times")
    if reports["plant"]["fired"] == 0:
        failures.append("the plant never fired: its trace proves nothing")
    if failures:
        return name, seed, log, failures, reports["clean"]

    expected = {"clean": "pass\t-", "plant": "invariant\tTraceConsistent"}
    with ThreadPoolExecutor(max_workers=2) as pool:
        runs = {kind: pool.submit(tlc, args, name, base / kind, reports[kind]["module"], expected[kind], layer, log)
                for kind in ("clean", "plant")}
        results = {kind: runs[kind].result() for kind in runs}

    clean_exit, _ = results["clean"]
    if clean_exit != 0:
        failures.append(tlc_failure("clean", clean_exit))
    plant_exit, plant_log = results["plant"]
    if plant_exit != 12:
        failures.append(tlc_failure("plant", plant_exit))
    else:
        # the rejection: a step layer's last matched record is ti, so the
        # record that is not a model step is ti + 1; a case layer's is ci
        field = "ti" if layer["kind"] == "steps" else "ci"
        values = re.findall(r"^/\\ " + field + r" = ([0-9]+)$", plant_log, re.M)
        if not values:
            failures.append(f"plant rejection: no {field} in TLC's counterexample")
        else:
            record = int(values[-1]) + (1 if layer["kind"] == "steps" else 0)
            if record not in reports["plant"]["at"]:
                failures.append(f"plant trace rejected at record {record}, where the plant did not fire")
    return name, seed, log, failures, reports["clean"]


def listed(binary_path):
    try:
        result = subprocess.run([str(binary_path), "--list"], capture_output=True, text=True, timeout=WALK_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.split()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--tlacheck", required=True)
    parser.add_argument("--jar", required=True)
    parser.add_argument("--layer", required=True, choices=sorted(LAYERS) + ["all"])
    parser.add_argument("--seeds", required=True, type=seeds)
    parser.add_argument("--steps", type=int, default=400)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--dir", required=True, type=Path)
    args = parser.parse_args()

    failed = []
    names = sorted(LAYERS) if args.layer == "all" else [args.layer]

    # the harness and the plant binaries against the table
    registered = listed(args.bin / "tla_trace")
    plants = {}
    if registered is None:
        failed.append("tla_trace --list failed: a layer was refused (a bad or second name) or the harness is broken")
    else:
        for dup in sorted({n for n in registered if registered.count(n) > 1}):
            failed.append(f"layer {dup}: registered twice")
        if args.layer == "all":
            for n in sorted(set(registered) - set(LAYERS)):
                failed.append(f"layer {n}: registered by the harness, no row in LAYERS")
            for n in sorted(set(LAYERS) - set(registered)):
                failed.append(f"layer {n}: a row in LAYERS, not registered by the harness")
    for path in sorted(args.bin.glob("tla_trace_plant_*")):
        if path.suffix not in ("", ".exe"):
            continue
        layers = listed(path)
        if layers is None or len(layers) != 1:
            failed.append(f"{path.name}: a plant binary must hold exactly one layer, it lists {layers}")
            continue
        if layers[0] in plants:
            failed.append(f"layer {layers[0]}: two plant binaries")
        plants[layers[0]] = path.name
    for n in names:
        if n not in plants:
            failed.append(f"layer {n}: no plant binary tla_trace_plant_<file> holds it")

    facts = {}
    for n in names:
        found, failure = spec_facts(LAYERS[n])
        if failure:
            failed.append(f"layer {n}: {failure}")
            continue
        facts[n] = found
        spec_vars, _, fields = found
        if LAYERS[n]["kind"] == "steps" and spec_vars != fields:
            failed.append(f"layer {n}: VarsRec names {sorted(fields)}, the spec's vars are {sorted(spec_vars)}")

    if failed:
        for line in failed:
            print(f"TRACE-CHECK FAIL {line}")
        return 1

    reached = {n: {} for n in names}
    jobs = [(n, seed) for n in names for seed in args.seeds]
    # each seed runs its two TLC runs at once
    with ThreadPoolExecutor(max_workers=max(1, args.jobs // 2)) as pool:
        futures = [pool.submit(check_seed, args, n, LAYERS[n], plants[n], seed) for n, seed in jobs]
        for future in as_completed(futures):
            n, seed, log, failures, clean = future.result()
            print(f"== layer {n} seed {seed}")
            sys.stdout.write("".join(log))
            sys.stdout.flush()
            for failure in failures:
                failed.append(f"layer {n} seed {seed}: {failure}")
            if clean:
                for act, count in clean["acts"].items():
                    reached[n][act] = reached[n].get(act, 0) + count

    for n in names:
        layer = LAYERS[n]
        if layer["kind"] == "steps":
            required = facts[n][1] - set(layer["untraced"])
        else:
            required = set(layer["features"])
        for act in sorted(required):
            if reached[n].get(act, 0) == 0:
                failed.append(f"layer {n}: {act} never reached in seeds {args.seeds.start}-{args.seeds.stop - 1}")
        for act in sorted(set(reached[n]) - required - set(layer.get("untraced", {}))):
            failed.append(f"layer {n}: the walk reports {act}, which is not an action of the spec's Next or a feature of the row")

    for line in failed:
        print(f"TRACE-CHECK FAIL {line}")
    if failed:
        return 1
    print(f"TRACE-CHECK OK layers={','.join(names)} seeds={args.seeds.start}-{args.seeds.stop - 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
