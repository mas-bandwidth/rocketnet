#!/usr/bin/env python3
"""Run the TLA+ trace check of one layer or of every layer (proofs/tla/README-TraceCheck.md).

For each seed, bin/tla_trace writes a trace of the layer's code and each of
the layer's plant binaries writes the same walk over planted code: one
binary per plant the row names, built with that plant's define alone,
bin/tla_trace_plant_<file> for one plant and bin/tla_trace_plant_<file>--<k>
for the k-th of several. Each walk runs twice and must write the same bytes
both times. Each trace is checked by the pinned tlacheck in a run and a root
of its own: the clean trace must pass, and EVERY plant's trace must violate
TraceConsistent at the first record where it differs from the clean trace,
which must be a record where the walk says the planted effect happened. No
plant can hide another: each is checked in a binary of its own.

Before any walk, the layer's files are read and refused, by name, when:
  the Trace module is not the template: Trace == TraceLog, the ASSUME,
    TraceInit, TraceStep, TraceReject, TraceDone, TraceSpec and
    TraceConsistent must be the template's text, VarsRec must be
    [v |-> v, ...] over exactly the spec's VARIABLES (less the row's
    ghost variables), Act(a, x) must be CASE a = "X" -> X(x[1], ..., x[n])
    with every X an action of Next taking n arguments, and nothing else may
    be defined there but OpActs and the row's reach predicates;
  the layer's file defines or undefines a ROCKETNET_ macro (a plant define
    included: the build sets each from the row's plants), or (but for a
    self-contained row) includes no sdk .cpp, or one of the row's plants is
    tested by no #if of the sdk files it includes;
  the rows and the binaries disagree: a row with no layer, a layer with no
    row, a name registered twice or refused, two rows of one name, a plant
    binary that is not exactly one layer or not its row's, a plant of the
    row with no binary, a binary that is no plant of the row;
  the shard table is not a partition: a layer with no per-commit seed, a
    seed in two shards or in none, an empty shard; or the workflows' shard
    matrices or the ruleset's shard checks are not SHARD_COUNT's.

Per seed, red when: a walk fails, times out, prints no CONFIG, TRACE, ACTS,
PLANT-FIRED and PLANT-AT, or writes different output twice; the trace is not
steps + 1 records; ACTS is not the acts the trace records; a configuration
macro the binaries see differs from the library's; the cfg the walk wrote is
not SPECIFICATION TraceSpec, INVARIANT TraceConsistent and plain constants;
TLC rejects the clean trace, times out, errs, or found fewer states than the
trace has records; the clean walk shows the planted effect; the plant never
fired, changed no record, or TLC accepts it or rejects it anywhere but at
the first record it changed; each of these for every plant of the row.

Across the seeds, red when an action of Next that is not untraced, or a
reach predicate of the row, was never taken or reached in a clean trace.
Per commit (--seeds commit) the seeds are the layer's own commit seeds; with
--seeds A-B, every seed of the range.

Per commit the work is split into SHARD_COUNT shards (--shard I/N): each
layer's row puts each of its commit seeds in one shard. A layer's cross-seed
checks run in the shard of its first commit seed, its home. Nightly, a shard
runs the layers homed there over every seed of the range.

A layer is one file, tests/tla_trace_<stem>.cpp, that registers itself
with TLA_TRACE_LAYER, plus its row beside it, tests/tla_trace_<stem>.row.

  python3 tools/tla_trace_check.py --bin bin --tlacheck TLACHECK \\
      --jar /opt/tla/tla2tools.jar --layer all --seeds commit --jobs 8 --dir WORK
  python3 tools/tla_trace_check.py --table      the rows and shard lists alone
"""

import argparse
import ast
import filecmp
import hashlib
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TLA = ROOT / "proofs" / "tla"
TESTS = ROOT / "tests"

# One row per layer, in its own file beside the layer's harness file:
# tests/tla_trace_<stem>.row for tests/tla_trace_<stem>.cpp. A row is a Python
# dict literal, read with ast (never run), comments allowed. Its keys:
#   layer           the name the harness file registers (TLA_TRACE_LAYER).
#   modules         the modules TLC reads: the spec first, any module it
#                   extends or instances from proofs/tla, and its
#                   Trace<Spec>.tla last. The spec's own names are its and
#                   the extended modules'; an instanced module's stay its own.
#   untraced        Next's actions no walk takes, each with the reason.
#   ghost           (optional) the spec's variables the code does not hold
#                   (history kept for a property), each with the reason;
#                   VarsRec leaves them out and the model computes them.
#   reach           (optional) state predicates of the Trace module that
#                   some clean trace of the seed set must reach: the edges
#                   a layout layer must show (wrapped, full, empty after a
#                   wrap). Each costs one more TLC run per seed until hit.
#   steps           (optional) the walk's length for this layer (400).
#   plants          the plant defines, ROCKETNET_<X>_PLANT, on ONE line, for
#                   CMakeLists.txt reads that line to build one plant binary per
#                   plant with that define alone; each must be tested by an #if
#                   of the sdk code the layer includes, and each plant's trace
#                   is checked on every seed. Every layer has one or more; the
#                   self-contained example has none (its plant is
#                   ROCKETNET_TLA_PLANT itself).
#   header_only     (optional, true) the layer's code is a header-only library file:
#                   its plants are tested by directives of the sdk header the
#                   layer includes whole; without this key a layer must include
#                   an sdk .cpp under the plant block.
#   self_contained  (the example only) why the code under test lives in the
#                   layer's own file instead of sdk/.
#   commit          the per-commit seeds and where each runs: {seed: shard},
#                   shard in 0 .. SHARD_COUNT - 1. Per commit (--seeds commit)
#                   a layer runs exactly these seeds, and its action coverage
#                   and reach predicates must hold over them alone; nightly
#                   (--seeds A-B) it runs every seed of the range in the shard
#                   of its first commit seed, its home.
ROW_GLOB = "tla_trace_*.row"
REQUIRED_KEYS = {"layer", "modules", "untraced", "commit"}

# The per-commit shard count: ci.yml's tla-trace matrix, tlc.yml's trace
# matrix and the ruleset's shard names are written from it (--write-shards)
# and every run checks them against it, so adding shards is this one number
# and one --write-shards. A layer over about 40 s of a runner's check step is
# spread by seed (msgx, xdp-reply-macs). Measured (bench-nyc-5, ccacc2a0b):
# msgx 68 to 86 s a seed alone with its reach, two TLC runs of about 21 s a
# seed; xdp-reply-macs 43 to 59 s a seed; every other layer 3 to 34 s whole.
SHARD_COUNT = 9
SHARD_JOB = "TLA+ trace check (fixed seeds, shard {})"
NIGHT_JOB = "TLA+ trace check (rolling seeds, shard {})"
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"
TLC_YML = ROOT / ".github" / "workflows" / "tlc.yml"
RULESET = ROOT / ".github" / "rulesets" / "main.json"
# the matrix line of each sharded job, and the ruleset's shard entries, as --write-shards writes them
MATRIX_LINE = re.compile(r"^        shard: \[[^\]\n]*\]$", re.M)
RULESET_SHARD = re.compile(r'^ *\{"context": "TLA\+ trace check \(fixed seeds, shard [0-9]+/[0-9]+\)"[^\n]*\n', re.M)

STEPS = 400
# the shortest walk a row may ask for
MIN_STEPS = 100
ROW_KEYS = {"layer", "modules", "plants", "untraced", "ghost", "reach", "steps", "self_contained", "header_only",
            "commit"}
# a plant define, and the row line CMakeLists.txt reads the plants from (file(STRINGS REGEX), then every
# plant define on it: the line must say exactly what the row's literal says)
PLANT_NAME = re.compile(r"ROCKETNET_[A-Z0-9_]+_PLANT")
PLANTS_LINE = re.compile(r'^[ \t]*"plants":', re.M)
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
        read = {m[:-len(".tla")]: units((TLA / m).read_text()) for m in modules[:-1]}
        trace = units((TLA / modules[-1]).read_text())
    except OSError as error:
        return None, [f"a module of the row cannot be read: {error}"]
    # the spec's own names: the spec and the modules it EXTENDS, transitively. A
    # module it only instances (Fwd == INSTANCE Link WITH ...) is read by TLC, but
    # its variables, definitions and constants are the instance's, not the spec's
    order, todo = [], [modules[0][:-len(".tla")]]
    while todo:
        stem = todo.pop(0)
        if stem in order or stem not in read:
            continue
        order.append(stem)
        for _, body in read[stem].get("EXTENDS", []):
            todo += [n.strip() for n in squash(body).split(",")]
    specs = [read[s] for s in order]

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


def source_failures(layer, stem):
    """[failures] of the layer's own file, tests/tla_trace_<stem>.cpp"""
    path = TESTS / f"tla_trace_{stem}.cpp"
    if not path.exists():
        return [f"no {path.relative_to(ROOT)}"]
    failures = []
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
                if m and m.group(1).endswith("_PLANT"):
                    failures.append(f"{f.name} defines {m.group(1)}: a plant is set by the build, one plant binary "
                                    f"each, from the row's plants (tests/{path.stem}.row)")
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
    # define only to open that block, at the top level, and the block holds only the sdk
    # includes. A walk that branched on the plant could plant its own records.
    code = c_tokens(path.read_text())
    stack, included, headers = [], [], []
    for line in code.split("\n"):
        directive = DIRECTIVE.match(line)
        in_block = stack == [PLANT_BLOCK]
        if not directive:
            if in_block and line.strip():
                failures.append(f"{path.name} has code inside its plant block: it holds sdk includes only")
            continue
        word, rest = directive.group(1), " ".join(directive.group(2).split())
        if in_block and not (word == "endif" or word == "define" and re.match(r"ROCKETNET_\w+_PLANT\b", rest) or word == "include" and re.match(r'"\.\./sdk/[^"]+\.cpp"', rest)):
            failures.append(f"{path.name} has #{word} {rest} inside its plant block: it holds sdk includes only")
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
            # a library header the layer includes whole (a header-only layout): its plants are
            # tested by directives of the header itself
            h = re.match(r'"\.\./sdk/([^"]+\.h)"', rest)
            if h and stack == [] and layer.get("header_only"):
                headers.append(h.group(1))
    if layer.get("header_only") and not headers:
        failures.append(f"{path.name} is a header_only layer and includes no sdk header: the plant must be in the library's code")
        return failures
    if not layer.get("header_only") and not included:
        failures.append(f"{path.name} includes no sdk .cpp under ROCKETNET_TLA_PLANT: the plant must be in the library's code")
        return failures
    sdk_code = ""
    for f in included + headers:
        if not (ROOT / "sdk" / f).exists():
            failures.append(f"{path.name} includes sdk/{f}, which does not exist")
        else:
            sdk_code += c_tokens((ROOT / "sdk" / f).read_text())
    # EVERY plant is tested by a directive of the sdk code, not named in a comment: one untested
    # plant is a binary of the clean code, whatever its siblings do
    for plant in layer.get("plants", []):
        if not re.search(r"^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif)\b[^\n]*\b" + plant + r"\b", sdk_code, re.M):
            failures.append(f"plant {plant} is tested by no #if in the sdk files {path.name} includes")
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
    if kind.startswith("plant") and exit_code == 0:
        return f"{kind} trace accepted by TLC"
    return f"{kind} trace: TLC exit {exit_code}"


def check_seed(args, name, layer, plants, seed, facts):
    """(name, seed, log, failures, clean report, plant reruns) of one seed: the clean walk, and the walk of
    each plant binary in plants ([(binary, plant define or None)]), each plant checked on its own"""
    log, failures = [], []
    base = args.dir / f"{name}-{seed}"
    if base.exists():
        shutil.rmtree(base)
    steps = args.steps or layer.get("steps", STEPS)
    # (kind, its directory, binary, how a failure names it): one plant keeps the plain "plant"; of
    # several, each failure names its plant
    kinds = [("clean", "clean", "tla_trace", "clean")]
    for k, (binary, plant) in enumerate(plants, 1):
        if len(plants) == 1:
            kinds.append(("plant", "plant", binary, "plant"))
        else:
            kinds.append(("plant", f"plant-{k}", binary, f"plant {k}/{len(plants)} {plant} ({binary})"))

    reports = {}
    for kind, sub, binary, said_kind in kinds:
        first = walk(args, binary, name, seed, steps, base / sub / "walk", log)
        second = walk(args, binary, name, seed, steps, base / sub / "again", log)
        if first is None or second is None:
            said = [line for chunk in log for line in chunk.splitlines() if line.startswith("tla_trace: ")]
            failures.append(f"{said_kind} harness failed, timed out or printed no CONFIG, TRACE, ACTS, PLANT-FIRED and PLANT-AT lines"
                            + (" (it said: " + "; ".join(line[len("tla_trace: "):] for line in said[:3]) + ")" if said else ""))
            return name, seed, log, failures, None, 0
        if not same_walk(first, second, base / sub / "walk", base / sub / "again"):
            failures.append(f"{said_kind} walk is nondeterministic: two runs of one seed differ")
            return name, seed, log, failures, None, 0
        if first["records"] != steps + 1 or len(first["lines"]) != steps + 1:
            failures.append(f"{said_kind} trace has {first['records']} records, not steps + 1 = {steps + 1}")
        if first["acts"] != first["logged"]:
            failures.append(f"{said_kind} walk's ACTS {first['acts']} are not the acts its trace records {first['logged']}")
        failures += [f"{said_kind}: {f}" for f in cfg_failures(base / sub / "walk" / (first["module"] + ".cfg"), facts["constants"])]
        failures += [f"{said_kind}: {f}" for f in mc_failures(base / sub / "walk" / (first["module"] + ".tla"), facts["trace_stem"])]
        reports[sub] = first
        if kind == "clean":
            log.append(f"TRACE-SHA256 {name} {seed} {trace_sha(base / sub / 'walk', first)}\n")

    clean = reports["clean"]
    for macro, value in clean["config"].items():
        if args.library_config.get(macro, "undef") != value:
            failures.append(f"the trace binaries see {macro}={value}, the library is built with "
                            f"{macro}={args.library_config.get(macro, 'undef')}")
    if clean["fired"] != 0:
        failures.append(f"clean code shows the planted effect {clean['fired']} times")
    # per plant: the first record its code changed (the walk is the same, so the two traces agree until
    # the plant acts), which must be a record where the walk says the plant fired
    changed = {}
    for kind, sub, binary, said_kind in kinds[1:]:
        # one plant's messages are the plain ones; of several, each names its plant
        the = "the plant" if said_kind == "plant" else said_kind
        report = reports[sub]
        if report["config"] != clean["config"]:
            failures.append(f"{binary}'s CONFIG {report['config']} is not the clean one's {clean['config']}")
        if report["fired"] == 0:
            failures.append(f"{the} never fired: its trace proves nothing")
        clean_lines, plant_lines = clean["lines"], report["lines"]
        first = next((i + 1 for i, (c, p) in enumerate(zip(clean_lines, plant_lines)) if c != p), None)
        if first is None and len(clean_lines) == len(plant_lines):
            failures.append(f"{the} changed no record: its trace is the clean one")
        elif first is None:
            first = min(len(clean_lines), len(plant_lines)) + 1
        if first is not None and first not in report["at"]:
            failures.append(f"{the} first changed record {first}, where the walk does not say the plant fired")
        changed[sub] = first
    if failures:
        return name, seed, log, failures, clean, 0

    expected = {"clean": "pass\t-", "plant": "invariant\tTraceConsistent"}
    with ThreadPoolExecutor(max_workers=len(kinds)) as pool:
        runs = {sub: pool.submit(tlc, args, name, base / sub, reports[sub]["module"], expected[kind], layer, log)
                for kind, sub, _, _ in kinds}
        results = {sub: runs[sub].result() for sub in runs}

    clean_exit, clean_log = results["clean"]
    if clean_exit != 0:
        failures.append(tlc_failure("clean", clean_exit))
    else:
        # TLC walked every record: an accepted trace has a state for each
        # record (ti = 1 .. N), so fewer distinct states means TLC stopped
        # short of the end
        found = DISTINCT.findall(clean_log)
        distinct = int(found[-1].replace(",", "")) if found else -1
        if distinct < clean["records"]:
            failures.append(f"clean trace: TLC found {distinct} distinct states for {clean['records']} records")
    reruns = 0
    for kind, sub, binary, said_kind in kinds[1:]:
        the = "plant" if said_kind == "plant" else said_kind
        plant_exit, plant_log = results[sub]
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
            plant_exit, plant_log = tlc(args, name, base / f"{sub}-attempt-{attempts}", reports[sub]["module"],
                                        expected["plant"], layer, log, source=base / sub / "walk")
        if plant_exit == 12 and not TI.search(plant_log):
            missing += 1
        if attempts > 1:
            log.append(f"PLANT-ATTEMPTS {attempts}: TLC printed no behaviour for the {the}'s violation on {missing} of them\n")
        reruns += attempts - 1
        if plant_exit != 12:
            failures.append(tlc_failure(the, plant_exit))
        else:
            # the last matched record is ti, so the record that is not a model
            # step is ti + 1: it must be the first record the plant changed
            values = TI.findall(plant_log)
            if not values:
                failures.append(f"{the} rejection: no ti in TLC's counterexample after {attempts} attempts")
            elif int(values[-1]) + 1 != changed[sub]:
                failures.append(f"{the} trace rejected at record {int(values[-1]) + 1}, not at record {changed[sub]}, "
                                f"the first the plant changed")
    return name, seed, log, failures, clean, reruns


def coverage_walk(args, name, layer, seed):
    """(clean walk report or None, log, failures) of one seed walked for its home shard's coverage and reach
    only: walked twice, and held to check_seed's checks before any TLC run (the same bytes twice, steps + 1
    records, ACTS the acts the trace records), so what is counted here is the walk TLC checks in the seed's
    own shard; the trace's sha256 is printed both there and here (TRACE-SHA256), for a reader to compare"""
    log, failures = [], []
    base = args.dir / f"{name}-{seed}"
    if base.exists():
        shutil.rmtree(base)
    steps = args.steps or layer.get("steps", STEPS)
    first = walk(args, "tla_trace", name, seed, steps, base / "clean" / "walk", log)
    second = walk(args, "tla_trace", name, seed, steps, base / "clean" / "again", log)
    if first is None or second is None:
        return None, log, ["the coverage walk failed, or wrote no trace"]
    if not same_walk(first, second, base / "clean" / "walk", base / "clean" / "again"):
        failures.append("the coverage walk is nondeterministic: two runs of one seed differ")
    if first["records"] != steps + 1 or len(first["lines"]) != steps + 1:
        failures.append(f"the coverage walk's trace has {first['records']} records, not steps + 1 = {steps + 1}")
    if first["acts"] != first["logged"]:
        failures.append(f"the coverage walk's ACTS {first['acts']} are not the acts its trace records {first['logged']}")
    log.append(f"TRACE-SHA256 {name} {seed} {trace_sha(base / 'clean' / 'walk', first)} (coverage walk)\n")
    return (None if failures else first), log, failures


def trace_sha(walk_dir, report):
    """sha256 of a walk's trace module"""
    return hashlib.sha256((walk_dir / (report["module"] + ".tla")).read_bytes()).hexdigest()


def reached(args, name, layer, predicate, log, seeds):
    """True when some seed's accepted clean trace reaches the predicate: a TLC run of the trace with the
    invariant ~predicate must find it violated"""
    for seed in seeds:
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


def read_row(path):
    """(row dict or None, [failure]) of one row file: one dict literal of constant keys, no key twice at any
    level. A dict literal keeps only the last of two equal keys, so a second "steps", or a commit seed given
    two shards, would hide the first silently: both are refused here, from the syntax tree."""
    where = f"tests/{path.name}"
    try:
        tree = ast.parse(path.read_text(), filename=where, mode="eval")
    except (OSError, SyntaxError, ValueError) as error:
        return None, [f"{where}: not a Python literal: {error}"]
    if not isinstance(tree.body, ast.Dict):
        return None, [f"{where}: a row is one dict literal"]
    failures = []
    commit = None
    for key, value in zip(tree.body.keys, tree.body.values):
        if isinstance(key, ast.Constant) and key.value == "commit":
            commit = value
    layer = next((v.value for k, v in zip(tree.body.keys, tree.body.values)
                  if isinstance(k, ast.Constant) and k.value == "layer" and isinstance(v, ast.Constant)), path.name)
    for node in ast.walk(tree.body):
        if not isinstance(node, ast.Dict):
            continue
        seen = {}
        for key, value in zip(node.keys, node.values):
            if not isinstance(key, ast.Constant):
                failures.append(f"{where}: a key that is not a constant")
                continue
            if key.value in seen and node is commit:
                shards = [seen[key.value], value]
                said = " and ".join(f"{s.value}/{SHARD_COUNT}" if isinstance(s, ast.Constant) else "?" for s in shards)
                failures.append(f"SHARDS: {layer}:{key.value} is listed twice, in shards {said}")
            elif key.value in seen:
                failures.append(f"{where}: key {key.value!r} twice in one dict: the second hides the first")
            seen[key.value] = value
    try:
        row = ast.literal_eval(tree.body)
    except ValueError as error:
        return None, failures + [f"{where}: not a literal: {error}"]
    # the build reads the plants from the text, the check from the literal: they must agree
    if isinstance(row, dict) and "plants" in row:
        lines = [line for line in path.read_text().splitlines() if PLANTS_LINE.match(line)]
        built = PLANT_NAME.findall(lines[0]) if len(lines) == 1 else None
        if built is None or built != row["plants"]:
            failures.append(f"{where}: \"plants\" is not one line naming exactly the row's plants, in order: "
                            f"CMakeLists.txt builds a plant binary for each define on that line")
    return row, failures


def load_rows(tests=None):
    """({layer: row}, {layer: stem}, [failure]) of every row file and harness file in tests/: each harness file
    tests/tla_trace_<stem>.cpp has its row tests/tla_trace_<stem>.row and each row its harness file, and no two
    rows name one layer"""
    tests = tests or TESTS
    failures, rows, stems, where = [], {}, {}, {}
    harness = {p.name[len("tla_trace_"):-len(".cpp")] for p in tests.glob("tla_trace_*.cpp")}
    files = {p.name[len("tla_trace_"):-len(".row")]: p for p in tests.glob(ROW_GLOB)}
    for stem in sorted(harness - set(files)):
        failures.append(f"tests/tla_trace_{stem}.cpp has no row: every layer file has its row tests/tla_trace_{stem}.row")
    for stem in sorted(set(files) - harness):
        failures.append(f"tests/tla_trace_{stem}.row is a row with no layer file tests/tla_trace_{stem}.cpp")
    for stem, path in sorted(files.items()):
        row, row_failures = read_row(path)
        failures += row_failures
        if row is None:
            continue
        name = row.get("layer")
        if not isinstance(name, str):
            failures.append(f"tests/{path.name}: no \"layer\", the name its harness file registers")
            continue
        if name in where:
            failures.append(f"layer {name}: two rows, tests/{where[name]} and tests/{path.name}")
            continue
        where[name] = path.name
        rows[name] = row
        stems[name] = stem
    return rows, stems, failures


def row_failures(name, row, steps_given):
    """[failure] of one row's own keys and values"""
    failures = []
    if not LAYER_NAME.fullmatch(name):
        failures.append(f"layer {name}: a name is lower-case letters, digits and '-' only")
    for key in sorted(set(row) - ROW_KEYS):
        failures.append(f"layer {name}: no row key {key}")
    for key in sorted(REQUIRED_KEYS - set(row)):
        failures.append(f"layer {name}: the row has no {key}")
    if not isinstance(row.get("modules", []), list) or not all(isinstance(m, str) for m in row.get("modules", [])):
        failures.append(f"layer {name}: modules is a list of file names")
    for key in ("untraced", "ghost"):
        if not isinstance(row.get(key, {}), dict):
            failures.append(f"layer {name}: {key} is a dict of name: reason")
    if not isinstance(row.get("reach", []), list):
        failures.append(f"layer {name}: reach is a list of predicate names")
    if "self_contained" in row and name not in SELF_CONTAINED:
        failures.append(f"layer {name}: only the example is self-contained; a layer traces sdk code")
    plants = row.get("plants")
    if "self_contained" in row:
        if plants is not None:
            failures.append(f"layer {name}: a self-contained row has no plants: its plant is ROCKETNET_TLA_PLANT")
    elif plants is None:
        failures.append(f"layer {name}: the row has no plants: every layer names its plant defines, one plant "
                        "binary each")
    elif not isinstance(plants, list) or not plants or not all(isinstance(p, str) and PLANT_NAME.fullmatch(p)
                                                                for p in plants):
        failures.append(f"layer {name}: plants is a non-empty list of ROCKETNET_<X>_PLANT defines")
    elif len(set(plants)) != len(plants):
        failures.append(f"layer {name}: a plant named twice in plants")
    steps = row.get("steps", STEPS)
    if not isinstance(steps, int):
        failures.append(f"layer {name}: steps is a whole number")
    elif not steps_given and steps < MIN_STEPS:
        failures.append(f"layer {name}: steps {steps} is under {MIN_STEPS}")
    return failures


def plant_binaries(stem, row):
    """[(binary name, plant define or None)] the build makes for a layer (CMakeLists.txt): one per plant, the
    bare name for one plant, --<k> for the k-th of several; one of ROCKETNET_TLA_PLANT alone for a row
    without plants"""
    plants = row.get("plants") if isinstance(row.get("plants"), list) else []
    if len(plants) <= 1:
        return [(f"tla_trace_plant_{stem}", plants[0] if plants else None)]
    return [(f"tla_trace_plant_{stem}--{k}", plant) for k, plant in enumerate(plants, 1)]


def commit_seeds(row):
    """the row's per-commit seeds, sorted (the well-formed ones)"""
    commit = row.get("commit")
    if not isinstance(commit, dict):
        return []
    return sorted(s for s in commit if isinstance(s, int) and not isinstance(s, bool) and s >= 1)


def shard_table(rows, count):
    """([[(layer, seed), ...] per shard], {layer: home shard}, [failure]): the rows' commit maps as shards,
    and every way they are not a partition of the per-commit pairs into count shards: a layer with no commit
    seed, a seed that is not a positive whole number, a shard outside 0 .. count - 1, an empty shard. (A seed
    given two shards is refused by read_row, which sees the literal.)"""
    failures, expanded, homes = [], [[] for _ in range(count)], {}
    for name in sorted(rows):
        commit = rows[name].get("commit")
        if not isinstance(commit, dict) or not commit:
            failures.append(f"{name} is listed in no shard: its row has no commit seed, so it would never be "
                            "checked per commit")
            continue
        for seed, shard in sorted(commit.items(), key=lambda item: str(item[0])):
            if isinstance(seed, bool) or not isinstance(seed, int) or seed < 1:
                failures.append(f"{name}: commit seed {seed!r} is not a positive whole number")
                continue
            if isinstance(shard, bool) or not isinstance(shard, int) or not 0 <= shard < count:
                failures.append(f"{name}:{seed} is listed in no shard: shard {shard!r} is not one of the {count} "
                                f"shards 0 to {count - 1}, so it would never be checked per commit")
                continue
            expanded[shard].append((name, seed))
        seeds = commit_seeds(rows[name])
        if seeds and isinstance(commit[seeds[0]], int) and 0 <= commit[seeds[0]] < count:
            homes[name] = commit[seeds[0]]
    for i, pairs in enumerate(expanded):
        if not pairs:
            failures.append(f"shard {i}/{count} holds no (layer, seed) pair")
    return expanded, homes, failures


def shard_names(count):
    return [f"{i}/{count}" for i in range(count)]


def job_block(text, job):
    """(start, end) of a job's block in a workflow's text, or None"""
    found = re.search(r"^  " + re.escape(job) + r":\n", text, re.M)
    if not found:
        return None
    after = re.search(r"^  [A-Za-z0-9_-]+:\n", text[found.end():], re.M)
    return found.start(), found.end() + after.start() if after else len(text)


def matrix_line(count):
    return "        shard: [" + ", ".join(f"'{s}'" for s in shard_names(count)) + "]"


def workflow_failures(count, ci=None, tlc_yml=None, ruleset=None):
    """[failure]: the sharded jobs' matrices and the ruleset's shard checks against the shard count"""
    failures = []
    for path, job in ((ci or CI_YML, "tla-trace"), (tlc_yml or TLC_YML, "trace")):
        text = path.read_text()
        block = job_block(text, job)
        lines = MATRIX_LINE.findall(text[block[0]:block[1]]) if block else []
        if lines != [matrix_line(count)]:
            failures.append(f"{path.name} job {job}: its matrix is not the {count} shards of SHARD_COUNT "
                            f"({lines[0].strip() if lines else 'no shard matrix'}): run tools/tla_trace_check.py --write-shards")
    text = (ruleset or RULESET).read_text()
    named = re.findall(r'"TLA\+ trace check \(fixed seeds, shard ([0-9]+/[0-9]+)\)"', text)
    if named != shard_names(count):
        failures.append(f"{(ruleset or RULESET).name}: requires the trace shards {named}, not the {count} of SHARD_COUNT: "
                        "run tools/tla_trace_check.py --write-shards")
    return failures


def write_shards(count, ci=None, tlc_yml=None, ruleset=None):
    """write the sharded jobs' matrices and the ruleset's shard checks from the shard count"""
    for path, job in ((ci or CI_YML, "tla-trace"), (tlc_yml or TLC_YML, "trace")):
        text = path.read_text()
        block = job_block(text, job)
        if not block or len(MATRIX_LINE.findall(text[block[0]:block[1]])) != 1:
            raise SystemExit(f"{path.name} job {job}: no one `shard: [...]` matrix line to write")
        part = MATRIX_LINE.sub(matrix_line(count), text[block[0]:block[1]])
        path.write_text(text[:block[0]] + part + text[block[1]:])
    path = ruleset or RULESET
    text = path.read_text()
    found = list(RULESET_SHARD.finditer(text))
    if not found or any(a.end() != b.start() for a, b in zip(found, found[1:])):
        raise SystemExit(f"{path.name}: the trace shard checks are not one run of lines to write")
    first = found[0].group(0)
    indent = first[:len(first) - len(first.lstrip())]
    entries = "".join(indent + '{"context": "' + SHARD_JOB.format(s) + '", "integration_id": 15368},\n'
                      for s in shard_names(count))
    path.write_text(text[:found[0].start()] + entries + text[found[-1].end():])


def table_failures(rows, stems, steps_given=False, count=None):
    """[failure] of the rows and the shard table, before any build"""
    count = count or SHARD_COUNT
    failed = []
    for name in sorted(rows):
        failed += row_failures(name, rows[name], steps_given)
    _, _, shard_failures = shard_table(rows, count)
    failed += [f"SHARDS: {f}" for f in shard_failures]
    return failed


def span_text(seeds):
    seeds = list(seeds)
    if len(seeds) == 1:
        return f"seed {seeds[0]}"
    if seeds and seeds == list(range(seeds[0], seeds[-1] + 1)):
        return f"seeds {seeds[0]}-{seeds[-1]}"
    return "seeds " + ",".join(str(s) for s in seeds)


def main():
    rows, stems, load_failures = load_rows()
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path)
    parser.add_argument("--tlacheck")
    parser.add_argument("--jar")
    parser.add_argument("--layer", choices=sorted(rows) + ["all"])
    parser.add_argument("--seeds", type=seeds,
                        help="commit: each layer's own per-commit seeds (its row's commit); A-B: every seed of the range")
    parser.add_argument("--steps", type=int, default=0, help="every layer's walk length (default: the row's, else 400)")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--dir", type=Path)
    parser.add_argument("--tlc-timeout", default=TLC_TIMEOUT)
    parser.add_argument("--shard", default="0/1",
                        help=f"I/N: 0/1 runs the layers asked for whole; I/{SHARD_COUNT} runs shard I: with --seeds commit, "
                             f"the (layer, seed) pairs the rows put there; with --seeds A-B, the layers whose first commit "
                             f"seed is there, every seed of the range")
    parser.add_argument("--table", action="store_true",
                        help="check the rows, the shard table and the workflows' shard lists only (no build)")
    parser.add_argument("--stems", metavar="I/N",
                        help="print the harness file stems shard I runs, ';'-joined (CMake's ROCKETNET_TLA_TRACE_ONLY)")
    parser.add_argument("--shard-names", action="store_true", help="print the per-commit shard jobs' names")
    parser.add_argument("--write-shards", action="store_true",
                        help="write ci.yml's and tlc.yml's shard matrices and the ruleset's shard checks from SHARD_COUNT")
    args = parser.parse_args()

    if args.shard_names:
        print("\n".join(SHARD_JOB.format(s) for s in shard_names(SHARD_COUNT)))
        return 0
    if args.write_shards:
        write_shards(SHARD_COUNT)
        return 0
    failed = list(load_failures) + table_failures(rows, stems, args.steps != 0) + workflow_failures(SHARD_COUNT)
    expanded, homes_of, _ = shard_table(rows, SHARD_COUNT)
    if args.stems:
        shard = re.fullmatch(r"([0-9]+)/([1-9][0-9]*)", args.stems)
        if not shard or int(shard.group(2)) != SHARD_COUNT or int(shard.group(1)) >= SHARD_COUNT:
            parser.error(f"--stems must be I/{SHARD_COUNT} with I < {SHARD_COUNT}: {args.stems}")
        if failed:
            for line in failed:
                print(f"TRACE-CHECK FAIL {line}", file=sys.stderr)
            return 1
        print(";".join(sorted({stems[n] for n, _ in expanded[int(shard.group(1))]})))
        return 0
    if args.table:
        for line in failed:
            print(f"TRACE-CHECK FAIL {line}")
        if failed:
            return 1
        print(f"TRACE-TABLE OK layers={len(rows)} shards={SHARD_COUNT} "
              f"pairs={sum(len(p) for p in expanded)}")
        return 0
    for need in ("bin", "tlacheck", "jar", "layer", "seeds", "dir"):
        if getattr(args, need) is None:
            parser.error(f"--{need} is required")
    args.library_config = library_config(args.bin / "tla_trace_config.txt")

    names = sorted(rows) if args.layer == "all" else [args.layer]
    shard = re.fullmatch(r"([0-9]+)/([1-9][0-9]*)", args.shard)
    if not shard or int(shard.group(1)) >= int(shard.group(2)):
        parser.error(f"--shard must be I/N with I < N: {args.shard}")
    index, count = int(shard.group(1)), int(shard.group(2))
    commit = args.seeds == "commit"
    if count == 1:
        pairs = [(n, seed) for n in names for seed in (commit_seeds(rows[n]) if commit else args.seeds)]
    elif count == SHARD_COUNT:
        if args.layer != "all":
            parser.error(f"--shard {args.shard} is a shard of the rows' table: it needs --layer all")
        if commit:
            pairs = expanded[index]
        else:
            pairs = [(n, seed) for n in names if homes_of.get(n) == index for seed in args.seeds]
    else:
        parser.error(f"--shard {args.shard}: the rows hold {SHARD_COUNT} shards (SHARD_COUNT); use I/{SHARD_COUNT}, "
                     "or 0/1 for every layer asked for")
    # an empty shard checks nothing: never TRACE-CHECK OK
    if not pairs and not failed:
        parser.error(f"--shard {args.shard} holds no (layer, seed) pair, so nothing would be checked")
    names = sorted({n for n, _ in pairs})
    # the seeds a layer's action coverage and reach predicates are held over: per commit its commit seeds,
    # wherever their TLC runs are, in the shard of its first (its home); else the run's whole range
    cover = {n: (commit_seeds(rows[n]) if commit else list(args.seeds)) for n in names}
    homes = [n for n in names if cover[n] and (n, cover[n][0]) in pairs]
    if args.library_config is None:
        failed.append(f"no {args.bin / 'tla_trace_config.txt'}: the library's definitions are not known")

    # the harness and the plant binaries against the rows: a shard's build may hold only its own layers
    # (CMake's ROCKETNET_TLA_TRACE_ONLY); a run of every layer needs every row's layer registered
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
        for n in sorted(set(registered) - set(rows)):
            failed.append(f"layer {n}: registered by the harness, no row (tests/tla_trace_<file>.row)")
        if args.layer == "all" and count == 1:
            for n in sorted(set(rows) - set(registered)):
                failed.append(f"layer {n}: a row, not registered by the harness")
        else:
            for n in sorted(set(names) - set(registered)):
                failed.append(f"layer {n}: a row this run needs, not registered by the harness (a build of other layers: "
                              f"ROCKETNET_TLA_TRACE_ONLY)")
    layer_of_stem = {stem: n for n, stem in stems.items()}
    # every plant binary built: the layer it holds, and that it is a plant binary its row names
    wanted = {name_: (n, plant) for n in rows for name_, plant in plant_binaries(stems[n], rows[n])}
    for path in sorted(args.bin.glob("tla_trace_plant_*")):
        if path.suffix not in ("", ".exe"):
            continue
        binary = path.stem if path.suffix else path.name
        layers, plant_refusals = listed(path)
        for n, reason in plant_refusals:
            failed.append(f"{path.name}: layer {n} refused at registration: {reason}")
        if layers is None or len(layers) != 1:
            failed.append(f"{path.name}: a plant binary must hold exactly one layer, it lists {layers}")
            continue
        stem = binary[len("tla_trace_plant_"):].split("--")[0]
        if stem in layer_of_stem and layer_of_stem[stem] != layers[0]:
            failed.append(f"{path.name} holds layer {layers[0]}, its row tests/tla_trace_{stem}.row names "
                          f"{layer_of_stem[stem]}")
            continue
        if binary not in wanted:
            failed.append(f"{path.name}: layer {layers[0]}'s row names no plant for it (its plants: one binary "
                          "tla_trace_plant_<file>, or tla_trace_plant_<file>--<k> for the k-th of several)")
            continue
        plants.setdefault(layers[0], []).append(binary)

    facts = {}
    for n in names:
        needed = plant_binaries(stems[n], rows[n])
        missing = [f"{b} ({p or 'ROCKETNET_TLA_PLANT'})" for b, p in needed if b not in plants.get(n, [])]
        if missing:
            failed.append(f"layer {n}: no plant binary {', '.join(missing)}: every plant of the row is built and "
                          "checked on its own")
            continue
        plants[n] = needed
        # a row without its modules or with values of the wrong kind is already red above
        if not REQUIRED_KEYS <= set(rows[n]) or not isinstance(rows[n]["modules"], list) or not rows[n]["modules"] \
                or not all(isinstance(rows[n].get(k, {}), dict) for k in ("untraced", "ghost")) \
                or not isinstance(rows[n].get("reach", []), list):
            continue
        found, failures = module_facts(n, rows[n])
        failures += source_failures(rows[n], stems[n])
        for failure in failures:
            failed.append(f"layer {n}: {failure}")
        if found:
            facts[n] = found

    if failed:
        for line in failed:
            print(f"TRACE-CHECK FAIL {line}")
        return 1

    taken = {n: {} for n in names}
    # plant TLC runs past each seed's first, over the whole run: a layer whose
    # plant needs reruns on every seed shows in the summary line, not only in
    # each seed's own PLANT-ATTEMPTS line
    reruns = 0
    # each seed runs its two TLC runs at once
    with ThreadPoolExecutor(max_workers=max(1, args.jobs // 2)) as pool:
        futures = [pool.submit(check_seed, args, n, rows[n], plants[n], seed, facts[n]) for n, seed in pairs]
        for future in as_completed(futures):
            n, seed, log, failures, clean, reran = future.result()
            reruns += reran
            print(f"== layer {n} seed {seed}")
            sys.stdout.write("".join(log))
            sys.stdout.flush()
            for failure in failures:
                failed.append(f"layer {n} seed {seed}: {failure}")
            if clean and not failures:
                # actions are counted from the traces TLC accepted, never
                # from the walk's own report
                for act, count_ in clean["logged"].items():
                    taken[n][act] = taken[n].get(act, 0) + count_

    # a home layer's seeds that other shards check: the clean walk alone, into
    # the directory check_seed uses, so their actions are counted here and the
    # reach predicates can try them; TLC accepts each in its own shard, and the
    # summary job is green only when every shard is
    elsewhere = [(n, seed) for n in homes for seed in cover[n] if (n, seed) not in pairs]
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        walked = list(pool.map(lambda p: coverage_walk(args, p[0], rows[p[0]], p[1]), elsewhere))
    for (n, seed), (report, log, failures) in zip(elsewhere, walked):
        sys.stdout.write("".join(log))
        for failure in failures:
            failed.append(f"layer {n} seed {seed}: {failure}")
        if report is None:
            continue
        for act, count_ in report["logged"].items():
            taken[n][act] = taken[n].get(act, 0) + count_

    def unmet(n):
        """how a cross-seed check of layer n that failed is told: its seeds, and per commit what to change"""
        said = span_text(cover[n])
        if commit:
            said += (f" (its per-commit seeds: the row's commit, tests/tla_trace_{stems[n]}.row; a layer's checks "
                     "per commit hold over those seeds alone, so give it the seeds this needs)")
        return said

    for n in homes:
        required = facts[n]["actions"] - set(rows[n]["untraced"])
        for act in sorted(required):
            if taken[n].get(act, 0) == 0:
                failed.append(f"layer {n}: {act} never reached in an accepted clean trace of {unmet(n)}")
        for act in sorted(set(taken[n]) - facts[n]["actions"]):
            failed.append(f"layer {n}: the trace records {act}, which is not an action of the spec's Next")
    # each reach predicate tries the seeds in turn, the predicates at once
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        checks = [(n, p, []) for n in homes for p in rows[n].get("reach", [])]
        hits = list(pool.map(lambda c: reached(args, c[0], rows[c[0]], c[1], c[2], cover[c[0]]), checks))
    for (n, predicate, log), hit in zip(checks, hits):
        if not hit:
            sys.stdout.write("".join(log))
            failed.append(f"layer {n}: reach predicate {predicate} never reached in a clean trace of {unmet(n)}")

    for line in failed:
        print(f"TRACE-CHECK FAIL {line}")
    if failed:
        print(f"TRACE-CHECK FAILED reruns={reruns}")
        return 1
    seeds_said = "commit" if commit else f"{args.seeds.start}-{args.seeds.stop - 1}"
    print(f"TRACE-CHECK OK layers={','.join(names)} seeds={seeds_said} "
          f"pairs={len(pairs)} homes={','.join(homes)} reruns={reruns}")
    return 0


def seeds(text):
    if text == "commit":
        return "commit"
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", text)
    if not match or int(match.group(1)) > int(match.group(2)):
        raise ValueError(f"seeds must be commit, or FIRST-LAST with FIRST <= LAST: {text}")
    return range(int(match.group(1)), int(match.group(2)) + 1)


if __name__ == "__main__":
    sys.exit(main())
