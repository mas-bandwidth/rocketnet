#!/usr/bin/env python3
"""Run the TLA+ trace check of one layer or of every layer (proofs/tla/README-TraceCheck.md).

For each seed, bin/tla_trace writes a trace of the layer's code and the
layer's own plant binary, bin/tla_trace_plant_<file>, writes the same walk
over the planted code. Each walk runs twice and must write the same bytes
both times. Each trace is checked by the pinned tlacheck in a run and a root
of its own: the clean trace must pass, and the planted trace must violate
TraceConsistent at the first record where it differs from the clean trace,
which must be a record where the walk says the planted effect happened.

Before any walk, the layer's files are read and refused, by name, when:
  the Trace module is not the template: Trace == TraceLog, the ASSUME,
    TraceInit, TraceStep, TraceReject, TraceDone, TraceSpec and
    TraceConsistent must be the template's text, VarsRec must be
    [v |-> v, ...] over exactly the spec's VARIABLES (less the row's
    ghost variables), Act(a, x) must be CASE a = "X" -> X(x[1], ..., x[n])
    with every X an action of Next taking n arguments, and nothing else may
    be defined there but OpActs and the row's reach predicates;
  the layer's file defines or undefines a ROCKETNET_ macro other than its
    plant's, or (but for a self-contained row) includes no sdk .cpp, or its
    plant define appears in none of the sdk files it includes;
  the table and the binaries disagree: a row with no layer, a layer with no
    row, a name registered twice or refused, two rows of one name, a plant
    binary that is not exactly one layer.

Per seed, red when: a walk fails, times out, prints no CONFIG, TRACE, ACTS,
PLANT-FIRED and PLANT-AT, or writes different output twice; the trace is not
steps + 1 records; ACTS is not the acts the trace records; a configuration
macro the binaries see differs from the library's; the cfg the walk wrote is
not SPECIFICATION TraceSpec, INVARIANT TraceConsistent and plain constants;
TLC rejects the clean trace, times out, errs, or found fewer states than the
trace has records; the clean walk shows the planted effect; the plant never
fired, changed no record, or TLC accepts it or rejects it anywhere but at
the first record it changed.

Across the seeds, red when an action of Next that is not untraced, or a
reach predicate of the row, was never taken or reached in a clean trace.

A layer is one file, tests/tla_trace_<layer>.cpp, that registers itself
with TLA_TRACE_LAYER, plus one row of LAYERS below.

  python3 tools/tla_trace_check.py --bin bin --tlacheck TLACHECK \\
      --jar /opt/tla/tla2tools.jar --layer all --seeds 1-4 --jobs 8 --dir WORK
"""

import argparse
import ast
import filecmp
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TLA = ROOT / "proofs" / "tla"
TESTS = ROOT / "tests"

# One row per layer.
#   modules         the modules TLC reads: the spec first, any module it
#                   extends from proofs/tla, and its Trace<Spec>.tla last.
#   untraced        Next's actions no walk takes, each with the reason.
#   ghost           (optional) the spec's variables the code does not hold
#                   (history kept for a property), each with the reason;
#                   VarsRec leaves them out and the model computes them.
#   reach           (optional) state predicates of the Trace module that
#                   some clean trace of the seed set must reach: the edges
#                   a layout layer must show (wrapped, full, empty after a
#                   wrap). Each costs one more TLC run per seed until hit.
#   steps           (optional) the walk's length for this layer (400).
#   self_contained  (the example only) why the code under test lives in the
#                   layer's own file instead of sdk/.
LAYERS = {
    "example": {
        "modules": ["RNTraceExample.tla", "TraceRNTraceExample.tla"],
        "untraced": {},
        "reach": ["Full"],
        "self_contained": "the template: its code is a counter written for the example, not library code",
    },
    "bank": {
        "modules": ["RNUringQueueBankLayout.tla", "TraceRNUringQueueBankLayout.tla"],
        "untraced": {"Done": "the model's end once MaxFlush flushes are spent, a stutter; the walk's length is "
                             "under MaxFlush, so it never gets there"},
        "reach": ["LastSlot"],
    },
    "gather": {
        "modules": ["RNSendGatherCall.tla", "TraceRNSendGatherCall.tla"],
        "untraced": {},
        "reach": ["SegCapEdge", "ByteCapEdge"],
        # seven steps a bank (six stages and the call): 100 calls a seed
        "steps": 700,
    },
    "fillphase": {
        "modules": ["RNFillPhase.tla", "TraceRNFillPhase.tla"],
        "untraced": {
            "MainScan": "a reversed witness's step (LookFirst: main_begin looks before it announces); the code announces first",
            "FillLoad": "a reversed witness's step (LoadStore: the lock as a load then a store); the code locks by one CAS",
            "FillStore": "a reversed witness's step (LoadStore); the code locks by one CAS",
            "Done": "the model's stutter once every call is made; the walk gives each actor as many calls as it has steps, "
                    "so it never gets there",
        },
        # each record carries the 64 fills_in lines, and a TLC run's time is the size of the log it reads: 400 steps
        # ran 35 to 53 s a run on a shared bench slot (vision, 2026-11-13) against the 100 s budget; a main call is 66
        # steps (one look a line), so a walk much shorter than this finishes none
        "steps": 240,
    },
    "sendreserve": {
        "modules": ["RNSendReserveCalls.tla", "TraceRNSendReserveCalls.tla"],
        "untraced": {},
        "ghost": {"starved": "history for NeverStarved: a refusal with a frame above the reserve; the code keeps no such word"},
        "reach": ["AtReserve", "Counted", "RoomOut", "OwnAllUsed", "ChunkTail", "SingleInBigRoom"],
    },
    "writerfinish": {
        "modules": ["RNWriterFinish.tla", "TraceRNWriterFinish.tla"],
        "untraced": {},
        # the walk's odd seeds are a staged transport (the first three predicates), its even seeds a buffered one
        # (the last three): a seed set must hold both kinds
        "reach": ["SealedAtFinish", "RefusedAtFinish", "NothingForWalk", "RingOpen", "RingQueued", "WalkCarried"],
    },
    "ackapply": {
        "modules": ["RNAckApplyPhase.tla", "TraceRNAckApplyPhase.tla"],
        "untraced": {},
        "reach": ["TakenByGameThread", "LeftForFinish", "Chained", "QueuedBehindSettled", "GatheredQueueFull",
                  "GatheredRangesFull"],
    },
    "xdp-gateway-refresh": {
        "modules": ["RNXdpGatewayRefresh.tla", "TraceRNXdpGatewayRefresh.tla"],
        "untraced": {
            "ReceiveWithoutRefresh": "enabled only under RefreshInFill, the reversed witness's rule; the code refreshes inside the receive call",
        },
        "reach": ["StagedNewMac"],
    },
    "xdp-reply-macs": {
        "modules": ["RNXdpReplyMacsLayout.tla", "TraceRNXdpReplyMacsLayout.tla"],
        "untraced": {},
        "reach": ["Wrapped", "WindowLive"],
        "steps": 250,
    },
    "xdp-replay-marks": {
        "modules": ["RNXdpReplayMarksLayout.tla", "TraceRNXdpReplayMarksLayout.tla"],
        "untraced": {
            "Commit": "the start of reset_keeping, inside the one call the walk makes; the walk takes ResetKeeping, "
                      "the call whole",
            "CompactForgetStep": "one iteration of reset_keeping's forget loop, inside the call; ResetKeeping runs it "
                                 "with the same arithmetic",
            "CompactMarkStep": "one iteration of reset_keeping's mark loop, inside the call; ResetKeeping runs it "
                               "with the same arithmetic",
        },
        "reach": ["ForgetsFull", "MarksFull", "SomeRefused", "ForgetsKept", "MarksKept"],
        # an update is up to five forgets, five marks and a commit
        "steps": 400,
    },
    "xdp-receive-ring": {
        "modules": ["RNXdpReceiveRing.tla", "TraceRNXdpReceiveRing.tla"],
        "untraced": {},
        # the staging buffer full, the RX ring full, and a refused flush
        # (even seeds: a fill ring of exactly the frames)
        "reach": ["StagingFull", "RxRingFull", "HeldBetweenPasses"],
    },
    "xdp-replay": {
        "modules": ["RNXdpReplay.tla", "TraceRNXdpReplay.tla"],
        "untraced": {},
        # a kernel mark for the filter's drop rule to read, a forget the kernel refused, a held greeting
        "reach": ["KernelMarked", "ForgetRefused", "GreetingHeld"],
    },
}

STEPS = 400
# the shortest walk a row may ask for
MIN_STEPS = 100
ROW_KEYS = {"modules", "untraced", "ghost", "reach", "steps", "self_contained"}
# the rows whose code may live in the layer's own file
SELF_CONTAINED = {"example"}
WALK_TIMEOUT = 120
# each trace's TLC run budget (tlacheck run --timeout); a run past it is red
TLC_TIMEOUT = "100s"
# TLC runs of one plant trace before its missing behaviour is red (check_seed)
PLANT_ATTEMPTS = 5

HEADER = "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"
TRACE = re.compile(r"^TRACE (\S+)\.tla records=([0-9]+)$", re.M)
ACTS = re.compile(r"^ACTS((?: [A-Za-z][A-Za-z0-9]*=[0-9]+)*)$", re.M)
FIRED = re.compile(r"^PLANT-FIRED ([0-9]+)$", re.M)
PLANT_AT = re.compile(r"^PLANT-AT (-|[0-9]+(?:,[0-9]+)*)$", re.M)
CONFIG = re.compile(r"^CONFIG((?: [A-Z][A-Z0-9_]*=\S+)+)$", re.M)
LOG_ACT = re.compile(r'act \|-> "([A-Za-z][A-Za-z0-9]*)"')
DISTINCT = re.compile(r"([0-9,]+) distinct states found")
TI = re.compile(r"^/\\ ti = ([0-9]+)$", re.M)
CASE_LINE = re.compile(r"^CASE \S+ config=\S+ result=(\S+) seconds=\S+ exit=([0-9]+)", re.M)
REFUSED = re.compile(r"^REFUSED (\S+) (bad-name|registered-twice|table-full)$", re.M)
LAYER_NAME = re.compile(r"[a-z0-9-]+")

# --- the modules -------------------------------------------------------------

NAME = r"[A-Za-z_][A-Za-z0-9_]*"
DEF_HEAD = re.compile(r"^(" + NAME + r")(?:\(([^)]*)\))?\s*==", re.M)
# a top-level unit starts at column 0: a definition, a declaration, the module's end
STOP = re.compile(r"^(?:" + NAME + r"(?:\([^)]*\))?\s*==|VARIABLES?\b|CONSTANTS?\b|ASSUME\b|THEOREM\b|INSTANCE\b|LOCAL\b|"
                  r"EXTENDS\b|={4,}|-{4,})", re.M)

# the template's fixed definitions, whitespace collapsed: a Trace module
# carries these exactly, so the act binding, the init check and the whole-
# record comparison cannot be dropped in a copy
TEMPLATE = {
    "Trace": "TraceLog",
    "TraceInit": '/\\ Init /\\ ti = 1 /\\ rejected = (Trace[1].op # "init" \\/ Trace[1].act # "init" \\/ '
                 'Trace[1].args # <<>> \\/ VarsRec # Trace[1].state)',
    "TraceStep": "/\\ ~rejected /\\ ti < Len(Trace) /\\ LET r == Trace[ti + 1] IN /\\ r.op \\in DOMAIN OpActs "
                 "/\\ r.act \\in OpActs[r.op] /\\ Act(r.act, r.args) /\\ Next /\\ VarsRec' = r.state /\\ ti' = ti + 1 "
                 "/\\ UNCHANGED rejected",
    "TraceReject": "/\\ ~rejected /\\ ti < Len(Trace) /\\ ~ENABLED TraceStep /\\ rejected' = TRUE "
                   "/\\ UNCHANGED <<vars, ti>>",
    "TraceDone": "/\\ rejected \\/ ti = Len(Trace) /\\ UNCHANGED <<vars, ti, rejected>>",
    "TraceSpec": "TraceInit /\\ [][TraceStep \\/ TraceReject \\/ TraceDone]_<<vars, ti, rejected>>",
    "TraceConsistent": "~rejected",
}
TEMPLATE_ASSUME = ('/\\ Len(TraceLog) >= 2 /\\ \\A n \\in DOMAIN TraceLog : DOMAIN TraceLog[n] = '
                   '{"op", "act", "args", "state"}')
TEMPLATE_VARIABLES = "ti, rejected"


def strip_comments(text):
    text = re.sub(r"\(\*.*?\*\)", " ", text, flags=re.S)
    return re.sub(r"\\\*[^\n]*", "", text)


def squash(text):
    return " ".join(text.split())


def units(text):
    """{name: (params or None, body)} of the definitions, plus ("ASSUME", n) and ("VARIABLES", n) blocks,
    each running to the next unit that starts at column 0"""
    code = strip_comments(text)
    found = {}
    starts = [m.start() for m in STOP.finditer(code)] + [len(code)]
    for i, start in enumerate(starts[:-1]):
        chunk = code[start:starts[i + 1]]
        head = DEF_HEAD.match(chunk)
        if head:
            name = head.group(1)
            found.setdefault(name, []).append((head.group(2), chunk[head.end():]))
            continue
        word = re.match(r"(VARIABLES?|CONSTANTS?|ASSUME|EXTENDS)\b", chunk)
        if word:
            key = {"VARIABLE": "VARIABLES", "CONSTANT": "CONSTANTS"}.get(word.group(1), word.group(1))
            found.setdefault(key, []).append((None, chunk[word.end():]))
    return found


def arity(params):
    return 0 if not params or not params.strip() else len(params.split(","))


def module_facts(name, layer):
    """(facts dict, [failures]) of the layer's modules"""
    failures = []
    modules = layer["modules"]
    if not modules[-1].startswith("Trace") or any(m.startswith("Trace") for m in modules[:-1]):
        return None, ["the row's modules must end with its one Trace module"]
    try:
        specs = [units((TLA / m).read_text()) for m in modules[:-1]]
        trace = units((TLA / modules[-1]).read_text())
    except OSError as error:
        return None, [f"a module of the row cannot be read: {error}"]

    defined = {}
    bodies = {}
    variables = []
    constants = []
    for spec in specs:
        for key, entries in spec.items():
            if key in ("VARIABLES", "CONSTANTS"):
                for _, body in entries:
                    names = [re.sub(r"\(.*", "", v).strip() for v in body.split(",") if v.strip()]
                    (variables if key == "VARIABLES" else constants).extend(names)
            elif key not in ("ASSUME", "EXTENDS"):
                defined[key] = entries[0][0]
                bodies[key] = squash(entries[0][1])
    if "Next" not in defined:
        return None, [f"{modules[0]} has no Next"]
    nxt = squash(specs[0].get("Next", [(None, "")])[0][1]) if "Next" in specs[0] else ""
    for spec in specs[1:]:
        if "Next" in spec:
            nxt = squash(spec["Next"][0][1])
    # Next's actions: the definitions it names that change state (a prime or
    # UNCHANGED in their own text); a guard Next names beside an action is
    # not an action, and an Act arm for it would let any change through
    actions = {n for n in re.findall(r"\b(" + NAME + r")\b", nxt)
               if n in defined and n != "Next" and ("'" in bodies[n] or "UNCHANGED" in bodies[n])}

    # vars is the spec's VARIABLES, and VarsRec is [v |-> v, ...] over them
    ghost = set(layer.get("ghost", {}))
    vars_body = None
    for spec in specs:
        if "vars" in spec:
            vars_body = squash(spec["vars"][0][1])
    if vars_body is None or not re.fullmatch(r"<<\s*" + NAME + r"(?:\s*,\s*" + NAME + r")*\s*>>", vars_body):
        failures.append("the spec has no vars == <<v, ...>>")
    elif set(re.findall(NAME, vars_body)) != set(variables):
        failures.append(f"the spec's vars {sorted(set(re.findall(NAME, vars_body)))} are not its VARIABLES {sorted(variables)}")
    for g in sorted(ghost - set(variables)):
        failures.append(f"ghost {g} is not a variable of the spec")
    # a ghost is never compared, so it must be one no step reads: outside
    # its own priming and UNCHANGED, no definition Next reaches names it
    closure, todo = set(), ["Next"]
    while todo:
        d = todo.pop()
        if d in closure or d not in bodies:
            continue
        closure.add(d)
        todo += [n for n in re.findall(r"\b(" + NAME + r")\b", bodies[d]) if n in bodies]
    for g in sorted(ghost & set(variables)):
        for d in sorted(closure):
            text = re.sub(r"UNCHANGED\s*(?:<<[^>]*>>|" + NAME + r")", " ", bodies[d])
            if re.search(r"\b" + re.escape(g) + r"\b(?!')", text):
                failures.append(f"ghost {g} is read by {d}, which Next reaches: a ghost no step reads is all a ghost may be")
                break
    rec = squash(trace.get("VarsRec", [(None, "")])[0][1])
    pairs = re.findall(r"(" + NAME + r")\s*\|->\s*(" + NAME + r")", rec)
    shape = r"\[\s*" + NAME + r"\s*\|->\s*" + NAME + r"(?:\s*,\s*" + NAME + r"\s*\|->\s*" + NAME + r")*\s*\]"
    if not re.fullmatch(shape, rec) or any(k != v for k, v in pairs):
        failures.append("VarsRec must be [v |-> v, ...]: each field the variable of its name, nothing computed")
    fields = {k for k, _ in pairs}
    if fields != set(variables) - ghost:
        failures.append(f"VarsRec names {sorted(fields)}, the spec's variables less its ghosts are {sorted(set(variables) - ghost)}")

    # the template's fixed text
    for key, body in TEMPLATE.items():
        if key not in trace:
            failures.append(f"the Trace module has no {key}")
        elif squash(trace[key][0][1]) != body:
            failures.append(f"the Trace module's {key} is not the template's: copy it from TraceRNTraceExample.tla")
    assumes = [squash(b) for _, b in trace.get("ASSUME", [])]
    if assumes != [TEMPLATE_ASSUME]:
        failures.append("the Trace module's ASSUME is not the template's")
    if [squash(b) for _, b in trace.get("VARIABLES", [])] != [TEMPLATE_VARIABLES]:
        failures.append("the Trace module declares variables other than ti, rejected")
    extends = [squash(b) for _, b in trace.get("EXTENDS", [])]
    stem = modules[0][:-len(".tla")]
    if len(extends) != 1 or not re.fullmatch(re.escape(stem) + r", Sequences(?:, " + NAME + r")*", extends[0]):
        failures.append(f"the Trace module must EXTENDS {stem}, Sequences")
    if [squash(b) for _, b in trace.get("CONSTANTS", [])] != ["TraceLog"]:
        failures.append("the Trace module declares constants other than TraceLog")
    allowed = set(TEMPLATE) | {"VarsRec", "OpActs", "Act", "ASSUME", "VARIABLES", "CONSTANTS", "EXTENDS"} | set(layer.get("reach", []))
    for extra in sorted(set(trace) - allowed):
        failures.append(f"the Trace module defines {extra}: only the template, VarsRec, OpActs, Act and the row's reach predicates")
    for p in layer.get("reach", []):
        if p not in trace:
            failures.append(f"reach predicate {p} is not defined in the Trace module")
            continue
        body = squash(trace[p][0][1])
        if trace[p][0][0] is not None or "==" in body or "'" in body or \
                re.search(r"\b(?:CONSTANTS?|VARIABLES?|INSTANCE|LOCAL|RECURSIVE|THEOREM|AXIOM|ASSUME|ENABLED|TRUE)\b", body) or \
                not any(re.search(r"\b" + re.escape(v) + r"\b", body) for v in variables):
            failures.append(f"reach predicate {p} must be a plain state predicate over the spec's variables")
    for key, entries in trace.items():
        if len(entries) > 1 and key not in ("ASSUME",):
            failures.append(f"the Trace module has two {key}")

    # OpActs == [op |-> {"X", ...}, ...]: a constant table and nothing else
    op_acts = squash(trace.get("OpActs", [(None, "")])[0][1])
    entry = NAME + r' \|-> \{"' + NAME + r'"(?:, "' + NAME + r'")*\}'
    if not re.fullmatch(r"\[" + entry + r"(?:, " + entry + r")*\]", op_acts):
        failures.append('OpActs must be [op |-> {"X", ...}, ...] and nothing else')

    # Act(a, x) == CASE a = "X" -> X(x[1], ..., x[n]) [] ...
    act = trace.get("Act")
    if not act or squash(act[0][0] or "") != "a, x":
        failures.append("the Trace module has no Act(a, x)")
    else:
        body = squash(act[0][1])
        arm = r'a = "(' + NAME + r')" -> (' + NAME + r')(?:\(([^()]*)\))?'
        if not re.fullmatch(r"CASE " + arm + r"(?: \[\] " + arm + r")*", body):
            failures.append('Act must be CASE a = "X" -> X(x[1], ..., x[n]) [] ... and nothing else')
        for named, taken, argtext in re.findall(arm, body):
            want = ", ".join(f"x[{i}]" for i in range(1, arity(defined.get(taken)) + 1))
            if named != taken:
                failures.append(f'Act maps "{named}" to {taken}: an arm must take the action it names')
            elif named not in actions:
                failures.append(f"Act names {named}, which is not an action of the spec's Next")
            elif squash(argtext or "") != want:
                failures.append(f'Act\'s arm "{named}" must pass {taken}({want})')
    for u in sorted(set(layer.get("untraced", {})) - actions):
        failures.append(f"untraced names {u}, which is not an action of the spec's Next")
    return {"actions": actions, "constants": set(constants), "trace_stem": modules[-1][:-len(".tla")]}, failures


def c_tokens(text):
    """C++ source as the preprocessor's lines see it: line splices joined, comments gone, %: as #"""
    text = text.replace("\\\n", "")
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', lambda m: m.group(0) if "include" in text[max(0, m.start() - 12):m.start()] else '""', text)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text.replace("%:", "#")


DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(\w+)[ \t]*(.*)$", re.M)


PLANT_BLOCK = "if defined( ROCKETNET_TLA_PLANT )"
PLANT_OPENS = {PLANT_BLOCK, "if defined(ROCKETNET_TLA_PLANT)", "ifdef ROCKETNET_TLA_PLANT"}


def source_failures(layer, plant_binary):
    """[failures] of the layer's own file, found from its plant binary's name"""
    stem = plant_binary[len("tla_trace_plant_"):]
    if stem.endswith(".exe"):
        stem = stem[:-4]
    path = TESTS / f"tla_trace_{stem}.cpp"
    if not path.exists():
        return [f"no {path.relative_to(ROOT)} for {plant_binary}"]
    failures = []
    plants = set()
    # the layer's file and every header of tests/ it includes, transitively
    seen, todo = set(), [path]
    while todo:
        f = todo.pop()
        if f in seen or not f.exists():
            continue
        seen.add(f)
        code = c_tokens(f.read_text())
        for word, rest in DIRECTIVE.findall(code):
            if word in ("define", "undef"):
                m = re.match(r"(ROCKETNET_[A-Za-z0-9_]+)", rest)
                if m and m.group(1).endswith("_PLANT") and f == path:
                    plants.add(m.group(1))
                elif m and not re.fullmatch(r"ROCKETNET_TESTS_[A-Z0-9_]+_H", m.group(1)):
                    failures.append(f"{f.name} defines {m.group(1)}: the trace binaries take the library's configuration, never their own")
            elif word == "include":
                m = re.match(r'"([^"]+)"', rest)
                if m and not m.group(1).startswith("../"):
                    todo.append(f.parent / m.group(1))
    if "self_contained" in layer:
        return failures
    # every sdk .cpp include sits in a top-level #if defined( ROCKETNET_TLA_PLANT ): the clean binary
    # traces the library itself, the plant binary the planted copy of the same file
    # The plant lives in the library's code alone: the file tests ROCKETNET_TLA_PLANT or a plant
    # define only to open that block, at the top level, and the block holds only the plant
    # defines and the sdk includes. A walk that branched on the plant could plant its own records.
    code = c_tokens(path.read_text())
    stack, included = [], []
    for line in code.split("\n"):
        directive = DIRECTIVE.match(line)
        in_block = stack == [PLANT_BLOCK]
        if not directive:
            if in_block and line.strip():
                failures.append(f"{path.name} has code inside its plant block: it holds plant defines and sdk includes only")
            continue
        word, rest = directive.group(1), " ".join(directive.group(2).split())
        if in_block and not (word == "endif" or word == "define" and re.match(r"ROCKETNET_\w+_PLANT\b", rest) or word == "include" and re.match(r'"\.\./sdk/[^"]+\.cpp"', rest)):
            failures.append(f"{path.name} has #{word} {rest} inside its plant block: it holds plant defines and sdk includes only")
        if word in ("if", "ifdef", "ifndef", "elif") and "_PLANT" in rest:
            if f"{word} {rest}" not in PLANT_OPENS or stack:
                failures.append(f"{path.name} tests the plant in #{word} {rest}: only the top-level block that includes the sdk may")
        if word in ("if", "ifdef", "ifndef"):
            stack.append(PLANT_BLOCK if f"{word} {rest}" in PLANT_OPENS else f"{word} {rest}")
        elif word in ("elif", "else"):
            stack[-1:] = [f"{word} {rest}"]
        elif word == "endif":
            stack[-1:] = []
        elif word == "include":
            m = re.match(r'"\.\./sdk/([^"]+\.cpp)"', rest)
            if m:
                if stack != [PLANT_BLOCK]:
                    failures.append(f"{path.name} includes sdk/{m.group(1)} outside a top-level #if defined( ROCKETNET_TLA_PLANT )")
                included.append(m.group(1))
    if not included:
        failures.append(f"{path.name} includes no sdk .cpp under ROCKETNET_TLA_PLANT: the plant must be in the library's code")
        return failures
    sdk_code = ""
    for f in included:
        if not (ROOT / "sdk" / f).exists():
            failures.append(f"{path.name} includes sdk/{f}, which does not exist")
        else:
            sdk_code += c_tokens((ROOT / "sdk" / f).read_text())
    # the plant define is tested by a directive of the sdk code, not named in a comment
    tested = any(re.search(r"^[ \t]*#[ \t]*(?:if|ifdef|elif)\b[^\n]*\b" + p + r"\b", sdk_code, re.M) for p in plants)
    if not tested:
        failures.append(f"{path.name}'s plant define is tested by no #if in the sdk files it includes")
    return failures


# --- one walk, one TLC run ---------------------------------------------------

CFG_LINE = re.compile(r'(?:\\\*.*|SPECIFICATION TraceSpec|CONSTANTS|INVARIANT TraceConsistent|PROPERTIES(?: ' + NAME + r')+|'
                      r'    TraceLog <- Log|    ' + NAME + r' = (?:-?[0-9]+|"[^"]*"|' + NAME + r'|\{[^{}<]*\})|)')


def cfg_failures(path, constants):
    text = path.read_text().splitlines()
    bad = [line for line in text if not CFG_LINE.fullmatch(line)]
    if bad:
        return [f"the walk's cfg has a line that is not a plain constant or the template's: {bad[0]!r}"]
    # an assignment names a declared CONSTANT of the spec: TLC would also
    # take `Def = value` as an override of a definition
    for line in text:
        m = re.fullmatch(r"    (" + NAME + r") = .*", line)
        if m and m.group(1) not in constants:
            return [f"the walk's cfg assigns {m.group(1)}, which is not a CONSTANT of the spec"]
    for need in ("SPECIFICATION TraceSpec", "INVARIANT TraceConsistent", "    TraceLog <- Log"):
        if need not in text:
            return [f"the walk's cfg lacks {need.strip()}"]
    return []


def mc_failures(path, trace_stem):
    """the walk's module is the writer's form and nothing more: its header, EXTENDS <Trace module>, TLC, no
    definitions, and Log"""
    lines = path.read_text().splitlines()
    name = path.stem
    want = [f"---- MODULE {name} ----", None, f"EXTENDS {trace_stem}, TLC", "", "", "Log == <<"]
    if len(lines) < 9 or any(w is not None and lines[i] != w for i, w in enumerate(want)) or \
            not lines[1].startswith("\\* written by tests/tla_trace.cpp: ") or lines[-3:] != [">>", "", "===="]:
        return [f"the walk's module {name} is not the writer's form: EXTENDS {trace_stem}, TLC, no definitions, then Log"]
    return []


def records(path):
    """the record lines of a trace module's Log, in order"""
    lines, inside = [], False
    for line in path.read_text().splitlines():
        if line == "Log == <<":
            inside = True
        elif line == ">>":
            inside = False
        elif inside:
            lines.append(line.strip().rstrip(","))
    return lines


def walk(args, binary, name, seed, steps, out, log):
    """(report dict, or None) of one walk into out"""
    out.mkdir(parents=True)
    try:
        result = subprocess.run([str(args.bin / binary), "--layer", name, "--seed", str(seed), "--steps", str(steps),
                                 "--out", str(out)], capture_output=True, text=True, timeout=WALK_TIMEOUT)
    except subprocess.TimeoutExpired:
        log.append(f"{binary}: no end within {WALK_TIMEOUT} s\n")
        return None
    log.append(result.stdout)
    log.append(result.stderr)
    traced, acts, fired, at, config = (TRACE.search(result.stdout), ACTS.search(result.stdout), FIRED.search(result.stdout),
                                       PLANT_AT.search(result.stdout), CONFIG.search(result.stdout))
    if result.returncode != 0 or not (traced and acts and fired and at and config):
        return None
    module = Path(traced.group(1)).name
    trace_file = out / (module + ".tla")
    if not trace_file.exists() or not (out / (module + ".cfg")).exists():
        return None
    if sorted(p.name for p in out.iterdir()) != sorted([module + ".cfg", module + ".tla"]):
        log.append(f"{binary}: the walk wrote files other than {module}.tla and {module}.cfg\n")
        return None
    lines = records(trace_file)
    logged = {}
    for line in lines[1:]:
        found = LOG_ACT.match(line[line.index("act |-> "):] if "act |-> " in line else "")
        act = found.group(1) if found else "?"
        logged[act] = logged.get(act, 0) + 1
    counts = {}
    for item in acts.group(1).split():
        key, value = item.split("=")
        counts[key] = int(value)
    return {
        "module": module,
        "logged": logged,
        "records": int(traced.group(2)),
        "lines": lines,
        "acts": counts,
        "fired": int(fired.group(1)),
        "at": set() if at.group(1) == "-" else {int(x) for x in at.group(1).split(",")},
        "config": dict(item.split("=", 1) for item in config.group(1).split()),
        "stdout": result.stdout.replace(str(out), "<out>"),
    }


def same_walk(first, second, a, b):
    if first["stdout"] != second["stdout"]:
        return False
    for path in a.iterdir():
        if not filecmp.cmp(path, b / path.name, shallow=False):
            return False
    return True


def tlc(args, name, kind_dir, module, expected, layer, log, source=None):
    """(exit of the TLC case, its log text) of one module in a root of its own"""
    tla = kind_dir / "root" / "tla"
    tla.mkdir(parents=True)
    for m in layer["modules"]:
        shutil.copy(TLA / m, tla / m)
    for path in (source or kind_dir / "walk").iterdir():
        if path.suffix in (".tla", ".cfg"):
            shutil.copy(path, tla / path.name)
    (tla / "CASES.tsv").write_text(HEADER + f"{module}.cfg\t{module}.tla\t{expected}\tcheck\ttrace{name.replace('-', '')}\trequired\t-\n")
    result = subprocess.run([args.tlacheck, "run", "--root", str(kind_dir / "root"), "--dir", str(kind_dir / "run"),
                             "--jar", args.jar, "--timeout", args.tlc_timeout], capture_output=True, text=True)
    log.append(result.stdout)
    log.append(result.stderr)
    # tlacheck prints a passing case on stdout and a failing one on stderr
    case = CASE_LINE.search(result.stdout + "\n" + result.stderr)
    tlc_log = kind_dir / "run" / f"{module}.cfg.log"
    return (int(case.group(2)) if case else -1), (tlc_log.read_text() if tlc_log.exists() else "")


def tlc_failure(kind, exit_code):
    if exit_code == -1:
        return f"{kind} trace: tlacheck printed no CASE line"
    if exit_code == 124:
        return f"{kind} trace: TLC timeout (the run budget)"
    if kind == "clean" and exit_code == 12:
        return "clean trace rejected by TLC (TraceConsistent)"
    if kind == "plant" and exit_code == 0:
        return "plant trace accepted by TLC"
    return f"{kind} trace: TLC exit {exit_code}"


def check_seed(args, name, layer, plant_binary, seed, facts):
    """(name, seed, log, failures, clean report)"""
    log, failures = [], []
    base = args.dir / f"{name}-{seed}"
    if base.exists():
        shutil.rmtree(base)
    steps = args.steps or layer.get("steps", STEPS)

    reports = {}
    for kind, binary in (("clean", "tla_trace"), ("plant", plant_binary)):
        first = walk(args, binary, name, seed, steps, base / kind / "walk", log)
        second = walk(args, binary, name, seed, steps, base / kind / "again", log)
        if first is None or second is None:
            said = [line for chunk in log for line in chunk.splitlines() if line.startswith("tla_trace: ")]
            failures.append(f"{kind} harness failed, timed out or printed no CONFIG, TRACE, ACTS, PLANT-FIRED and PLANT-AT lines"
                            + (" (it said: " + "; ".join(line[len("tla_trace: "):] for line in said[:3]) + ")" if said else ""))
            return name, seed, log, failures, None
        if not same_walk(first, second, base / kind / "walk", base / kind / "again"):
            failures.append(f"{kind} walk is nondeterministic: two runs of one seed differ")
            return name, seed, log, failures, None
        if first["records"] != steps + 1 or len(first["lines"]) != steps + 1:
            failures.append(f"{kind} trace has {first['records']} records, not steps + 1 = {steps + 1}")
        if first["acts"] != first["logged"]:
            failures.append(f"{kind} walk's ACTS {first['acts']} are not the acts its trace records {first['logged']}")
        failures += [f"{kind}: {f}" for f in cfg_failures(base / kind / "walk" / (first["module"] + ".cfg"), facts["constants"])]
        failures += [f"{kind}: {f}" for f in mc_failures(base / kind / "walk" / (first["module"] + ".tla"), facts["trace_stem"])]
        reports[kind] = first

    if reports["clean"]["config"] != reports["plant"]["config"]:
        failures.append(f"the plant binary's CONFIG {reports['plant']['config']} is not the clean one's {reports['clean']['config']}")
    for macro, value in reports["clean"]["config"].items():
        if args.library_config.get(macro, "undef") != value:
            failures.append(f"the trace binaries see {macro}={value}, the library is built with "
                            f"{macro}={args.library_config.get(macro, 'undef')}")
    if reports["clean"]["fired"] != 0:
        failures.append(f"clean code shows the planted effect {reports['clean']['fired']} times")
    if reports["plant"]["fired"] == 0:
        failures.append("the plant never fired: its trace proves nothing")
    # the first record the planted code changed: the walk is the same, so
    # the two traces agree until the plant acts
    clean_lines, plant_lines = reports["clean"]["lines"], reports["plant"]["lines"]
    changed = next((i + 1 for i, (c, p) in enumerate(zip(clean_lines, plant_lines)) if c != p), None)
    if changed is None and len(clean_lines) == len(plant_lines):
        failures.append("the plant changed no record: its trace is the clean one")
    elif changed is None:
        changed = min(len(clean_lines), len(plant_lines)) + 1
    if changed is not None and changed not in reports["plant"]["at"]:
        failures.append(f"the plant first changed record {changed}, where the walk does not say the plant fired")
    if failures:
        return name, seed, log, failures, reports["clean"]

    expected = {"clean": "pass\t-", "plant": "invariant\tTraceConsistent"}
    with ThreadPoolExecutor(max_workers=2) as pool:
        runs = {kind: pool.submit(tlc, args, name, base / kind, reports[kind]["module"], expected[kind], layer, log)
                for kind in ("clean", "plant")}
        results = {kind: runs[kind].result() for kind in runs}

    clean_exit, clean_log = results["clean"]
    if clean_exit != 0:
        failures.append(tlc_failure("clean", clean_exit))
    else:
        # TLC walked every record: an accepted trace has a state for each
        # record (ti = 1 .. N), so fewer distinct states means TLC stopped
        # short of the end
        found = DISTINCT.findall(clean_log)
        distinct = int(found[-1].replace(",", "")) if found else -1
        if distinct < reports["clean"]["records"]:
            failures.append(f"clean trace: TLC found {distinct} distinct states for {reports['clean']['records']} records")
    plant_exit, plant_log = results["plant"]
    # TLC 2.19 sometimes reports the violation (exit 12) and prints no
    # behaviour at all: on byte-identical inputs, with -fp and -seed pinned,
    # one worker and a fresh metadir each run, the same plant printed it in
    # 5, 6 and 7 of 10 runs (hetzner1, 2026-11-13). A run without a behaviour
    # is run again, up to PLANT_ATTEMPTS in all; every attempt past the first
    # is printed, so the flake stays visible, and a plant that never prints
    # one stays red
    attempts = 1
    missing = 0
    while plant_exit == 12 and not TI.search(plant_log) and attempts < PLANT_ATTEMPTS:
        missing += 1
        attempts += 1
        plant_exit, plant_log = tlc(args, name, base / f"plant-attempt-{attempts}", reports["plant"]["module"],
                                    expected["plant"], layer, log, source=base / "plant" / "walk")
    if plant_exit == 12 and not TI.search(plant_log):
        missing += 1
    if attempts > 1:
        log.append(f"PLANT-ATTEMPTS {attempts}: TLC printed no behaviour for the plant's violation on {missing} of them\n")
    if plant_exit != 12:
        failures.append(tlc_failure("plant", plant_exit))
    else:
        # the last matched record is ti, so the record that is not a model
        # step is ti + 1: it must be the first record the plant changed
        values = TI.findall(plant_log)
        if not values:
            failures.append(f"plant rejection: no ti in TLC's counterexample after {attempts} attempts")
        elif int(values[-1]) + 1 != changed:
            failures.append(f"plant trace rejected at record {int(values[-1]) + 1}, not at record {changed}, "
                            "the first the plant changed")
    return name, seed, log, failures, reports["clean"]


def reached(args, name, layer, predicate, log):
    """True when some seed's accepted clean trace reaches the predicate: a TLC run of the trace with the
    invariant ~predicate must find it violated"""
    for seed in args.seeds:
        walk_dir = args.dir / f"{name}-{seed}" / "clean" / "walk"
        traced = list(walk_dir.glob("MC*.tla")) if walk_dir.exists() else []
        if len(traced) != 1:
            continue
        module = traced[0].stem
        work = args.dir / f"{name}-{seed}" / f"reach-{predicate}"
        if work.exists():
            shutil.rmtree(work)
        src = work / "src"
        src.mkdir(parents=True)
        shutil.copy(traced[0], src / traced[0].name)
        reach = f"MCReach{predicate.replace('_', '')}{module[2:]}"
        (src / f"{reach}.tla").write_text(f"---- MODULE {reach} ----\nEXTENDS {module}\n\nNotReached == ~({predicate})\n\n====\n")
        cfg = (walk_dir / f"{module}.cfg").read_text().replace("INVARIANT TraceConsistent", "INVARIANT NotReached")
        (src / f"{reach}.cfg").write_text(cfg)
        code, text = tlc(args, name, work, reach, "invariant\tNotReached", layer, log, source=src)
        if code == 12 and "Invariant NotReached is violated" in text:
            return True
    return False


# --- the table ---------------------------------------------------------------

def library_config(path):
    """The library's compile definitions as {NAME: value}, from the file CMake writes beside tla_trace."""
    if not path.exists():
        return None
    config = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            name, _, value = line.partition("=")
            config[name] = value if value else "1"
    return config


def listed(binary_path):
    """(layer names, [(name, reason) refused at registration]); names None when --list failed"""
    try:
        result = subprocess.run([str(binary_path), "--list"], capture_output=True, text=True, timeout=WALK_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, []
    refusals = REFUSED.findall(result.stdout)
    if result.returncode != 0:
        return None, refusals
    return result.stdout.split(), refusals


def table_rows():
    """The keys of the LAYERS literal in this file, in order, duplicates kept: a dict literal keeps
    only the last of two equal keys, so a second row of one name would hide the first silently."""
    tree = ast.parse(Path(__file__).read_text())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "LAYERS" for t in node.targets):
            return [k.value for k in node.value.keys if isinstance(k, ast.Constant)]
    return []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--tlacheck", required=True)
    parser.add_argument("--jar", required=True)
    parser.add_argument("--layer", required=True, choices=sorted(LAYERS) + ["all"])
    parser.add_argument("--seeds", required=True, type=seeds)
    parser.add_argument("--steps", type=int, default=0, help="every layer's walk length (default: the row's, else 400)")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--dir", required=True, type=Path)
    parser.add_argument("--tlc-timeout", default=TLC_TIMEOUT)
    parser.add_argument("--shard", default="0/1", help="I/N: run the layers whose place in the sorted list is I mod N")
    args = parser.parse_args()
    args.library_config = library_config(args.bin / "tla_trace_config.txt")

    failed = []
    names = sorted(LAYERS) if args.layer == "all" else [args.layer]
    shard = re.fullmatch(r"([0-9]+)/([1-9][0-9]*)", args.shard)
    if not shard or int(shard.group(1)) >= int(shard.group(2)):
        parser.error(f"--shard must be I/N with I < N: {args.shard}")
    if not names:
        parser.error(f"--shard {args.shard} holds no layer: nothing would be checked")
    names = names[int(shard.group(1))::int(shard.group(2))]
    if args.library_config is None:
        failed.append(f"no {args.bin / 'tla_trace_config.txt'}: the library's definitions are not known")

    rows = table_rows()
    for n in sorted({r for r in rows if rows.count(r) > 1}):
        failed.append(f"layer {n}: two rows in LAYERS")
    for n in rows:
        if not LAYER_NAME.fullmatch(n):
            failed.append(f"layer {n}: a name is lower-case letters, digits and '-' only")
    for n, layer in sorted(LAYERS.items()):
        for key in sorted(set(layer) - ROW_KEYS):
            failed.append(f"layer {n}: no row key {key}")
        if "self_contained" in layer and n not in SELF_CONTAINED:
            failed.append(f"layer {n}: only the example is self-contained; a layer traces sdk code")
        if args.steps == 0 and layer.get("steps", STEPS) < MIN_STEPS:
            failed.append(f"layer {n}: steps {layer['steps']} is under {MIN_STEPS}")

    # the harness and the plant binaries against the table
    registered, refusals = listed(args.bin / "tla_trace")
    plants = {}
    for n, reason in refusals:
        failed.append(f"layer {n}: refused at registration: {reason}")
    if registered is None:
        if not refusals:
            failed.append("tla_trace --list failed")
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
        layers, plant_refusals = listed(path)
        for n, reason in plant_refusals:
            failed.append(f"{path.name}: layer {n} refused at registration: {reason}")
        if layers is None or len(layers) != 1:
            failed.append(f"{path.name}: a plant binary must hold exactly one layer, it lists {layers}")
            continue
        if layers[0] in plants:
            failed.append(f"layer {layers[0]}: two plant binaries")
        plants[layers[0]] = path.name

    facts = {}
    for n in names:
        if n not in plants:
            failed.append(f"layer {n}: no plant binary tla_trace_plant_<file> holds it")
            continue
        found, failures = module_facts(n, LAYERS[n])
        failures += source_failures(LAYERS[n], plants[n])
        for failure in failures:
            failed.append(f"layer {n}: {failure}")
        if found:
            facts[n] = found

    if failed:
        for line in failed:
            print(f"TRACE-CHECK FAIL {line}")
        return 1

    taken = {n: {} for n in names}
    jobs = [(n, seed) for n in names for seed in args.seeds]
    # each seed runs its two TLC runs at once
    with ThreadPoolExecutor(max_workers=max(1, args.jobs // 2)) as pool:
        futures = [pool.submit(check_seed, args, n, LAYERS[n], plants[n], seed, facts[n]) for n, seed in jobs]
        for future in as_completed(futures):
            n, seed, log, failures, clean = future.result()
            print(f"== layer {n} seed {seed}")
            sys.stdout.write("".join(log))
            sys.stdout.flush()
            for failure in failures:
                failed.append(f"layer {n} seed {seed}: {failure}")
            if clean and not failures:
                # actions are counted from the traces TLC accepted, never
                # from the walk's own report
                for act, count in clean["logged"].items():
                    taken[n][act] = taken[n].get(act, 0) + count

    span = f"seeds {args.seeds.start}-{args.seeds.stop - 1}"
    for n in names:
        required = facts[n]["actions"] - set(LAYERS[n]["untraced"])
        for act in sorted(required):
            if taken[n].get(act, 0) == 0:
                failed.append(f"layer {n}: {act} never reached in an accepted clean trace of {span}")
        for act in sorted(set(taken[n]) - facts[n]["actions"]):
            failed.append(f"layer {n}: the trace records {act}, which is not an action of the spec's Next")
    # each reach predicate tries the seeds in turn, the predicates at once
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        checks = [(n, p, []) for n in names for p in LAYERS[n].get("reach", [])]
        hits = list(pool.map(lambda c: reached(args, c[0], LAYERS[c[0]], c[1], c[2]), checks))
    for (n, predicate, log), hit in zip(checks, hits):
        if not hit:
            sys.stdout.write("".join(log))
            failed.append(f"layer {n}: reach predicate {predicate} never reached in a clean trace of {span}")

    for line in failed:
        print(f"TRACE-CHECK FAIL {line}")
    if failed:
        return 1
    print(f"TRACE-CHECK OK layers={','.join(names)} seeds={args.seeds.start}-{args.seeds.stop - 1}")
    return 0


def seeds(text):
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", text)
    if not match or int(match.group(1)) > int(match.group(2)):
        raise ValueError(f"seeds must be FIRST-LAST with FIRST <= LAST: {text}")
    return range(int(match.group(1)), int(match.group(2)) + 1)


if __name__ == "__main__":
    sys.exit(main())
