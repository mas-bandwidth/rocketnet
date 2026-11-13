# The trace check: binding the code to a model

A model checked by TLC says the design keeps its rules. It does not say the code
implements the design. The trace check closes that gap. A seeded random walk drives
the code and writes down, after every call:
- which operation it made, and with which arguments;
- which model action the code's own result says it took;
- the model's variables.

TLC then checks that the written sequence is a behaviour of the layer's spec, one named
action with its arguments at a time. A layer that does index arithmetic or owns a state
machine is not called secured without this check.

The smallest whole layer is the example: `tests/tla_trace_example.cpp`, its model
`RNTraceExample.tla` and its trace module `TraceRNTraceExample.tla`. Copy it. The
checker holds every layer to the example's shape: what a copy may change is listed
below, and the rest is compared with the template and refused when it differs.

## What it can and cannot see

The walk interleaves whole calls: one call of the code per step, run to its end on one
thread. It sees every order of calls the walk makes, every refusal, and every index the
code computes. It cannot see anything that happens inside one call while another thread
runs: a race inside a call, a torn read, a memory-order fault, a lost wake. Those belong
to TSan (the `tsan` jobs) and to the layer's own concurrency model. A layer whose
correctness rests on them is not secured by this check alone.

For a completion-driven layer (a ring, a worker), the walk is the kernel too. It posts
each fake completion as a step of its own, with its result in the record: completions in
any order, early or late, failed or short. A completion the fake never posts is never
checked. The walk runs each model action atomically, so the model's actions must be at
least as fine as the code's real interleaving points (a submit, a completion, a drain).
Where the model joins two of them into one action, the check cannot show the code is
right between them. For the same reason, `Next` names its actions one by one: a `Next`
that is one big `Step(x)` gives the act binding nothing to bind.

## What the check trusts

The walk is trusted code, read by a person or a reader, never proven by the checker:
- that the state it logs is read from the library's own structures, not a shadow copy
  the walk keeps;
- that `act` comes from the call's result;
- that the constants it writes are the code's own;
- that it records every call it makes, and makes the same calls in both builds;
- that every value a call gives back (a popped item, a slot index, a count freed) is
  an argument of its action, so TLC checks it: a value the record leaves out is never
  checked, and a wrong item popped from a ring with the right head passes;
- that it calls the library's code, not a copy of it left in the test file;
- that it narrows no value before `tla_int` (`tla_int( (int) seq64 )` hides the high
  bits from the range check: pass the 64-bit value, or `tla_int_split`).

The plant proves part of this: it changes library code, and TLC must reject the trace
exactly where the change first shows. The checker keeps the plant in the library: the
layer file may test `ROCKETNET_TLA_PLANT` only to open the one top-level block holding
its plant defines and sdk includes, and the writer names the plant trace itself, so the
walk never branches on the plant. A walk that logs a shadow, or reads the wrong
variable, fails the plant on the variable the plant changes, and nowhere else. A layer's
review reads its walk against these points.

The checker reads the sources with regular expressions, not with TLC's or the
compiler's parser. It is a guard against mistakes, not against an author set on
deceiving it: nested `(* *)` comments, a comment opener inside a TLA+ string, a
helper module `INSTANCE`d with its own `Next`, or a configuration macro without the
`ROCKETNET_` prefix can make it read something other than what TLC or the compiler
reads. A layer's review reads the Trace module and the layer file whole.

The trace is checked against `TraceConsistent` only. The spec's own invariants are
checked by the spec's model, at the model's sizes, not on the trace at the code's
sizes. Enforcing this, the copy of library code and the per-source configuration are
roadmap items `trace-spec-invariants`, `trace-library-copy` and
`trace-config-per-source` in `docs/roadmap.sexp`.

## Sizes

The walk checks the sizes the code really has. A layout bug at a wrap of a 4096-slot
ring shows only if the walk wraps it: give the row `steps` long enough and `reach`
predicates for the edges (below), so a walk that never gets there is red.

## Adding a layer `x` with spec `RNX`

1. **One file, `tests/tla_trace_x.cpp`.** It holds the walk,
   `static int trace_x( uint64_t seed, int steps, const char * out )`, and registers it
   at namespace scope with `TLA_TRACE_LAYER( "x", trace_x );` (`tests/tla_trace_record.h`).
   - **Names.** A layer name is lower-case letters, digits and `-`. The module the walk
     writes is `MC` followed by letters and digits (`MCTraceXSeed<seed>`, or
     `MCTraceXPlantSeed<seed>` in the plant build: the walk passes `"MCTraceX"` and the
     seed to `tla_write_trace`, which adds the rest). tlacheck refuses any other name,
     and the writer refuses it first.
   - **Build.** `tla_trace` takes every `tests/tla_trace_*.cpp`. Each file also gets its
     own plant binary, `tla_trace_plant_<file>`, holding that file alone, compiled with
     `ROCKETNET_TLA_PLANT` and with the library's own private definitions, include
     directories and options. Under `ROCKETNET_TLA_PLANT` the file includes the sdk code
     it traces with its plant define, so the plant binary carries the planted library
     code in place of the library's:

     ```cpp
     #if defined( ROCKETNET_TLA_PLANT )
     #define ROCKETNET_X_PLANT 1
     #include "../sdk/rocketnet_x.cpp"
     #endif // #if defined( ROCKETNET_TLA_PLANT )
     ```

     The checker requires that include, in a top-level `#if defined(
     ROCKETNET_TLA_PLANT )` that holds only `ROCKETNET_..._PLANT` defines and sdk
     includes, refuses any other test of the plant in the file, and requires an `#if`
     of the sdk file to test the `ROCKETNET_..._PLANT` define. The file defines or undefines no other
     `ROCKETNET_` macro: the trace binaries take the library's configuration, never
     their own. The walk calls the library's code, never a copy of it in the test file.
     One layer's plant never reaches another layer's trace, and two layers may include
     the same sdk file.
   - **Records.** The walk writes one record per step with
     `tla_step( op, act, args, state )`:
     - `op`: the call it made, a lower-case identifier (letters, digits, `_`).
     - `act`: the model action the call took, read from its return value or
       out-parameters only. Never derive it by comparing the state before and after:
       then the state picks the action, and a call that did one thing but landed where
       another action allows would pass.
     - `args`: a `tla_sequence` of the action's parameters in the order the model
       declares them, `"<<>>"` when it has none. What the call gives back is a
       parameter too: the example's `Drain(n)` takes the count the drain returned,
       and the model requires `n = count`. A ghost never stands for a returned value.
     - `state`: every model variable after the call (a ghost variable aside, below).

     Record 1 is `tla_step( "init", "init", "<<>>", state )`.
   - **Constants.** The constants the walk writes into the cfg are plain values
     (`Limit = 3`), taken from the code's own definitions, never typed again in the
     walk: a model given a looser limit than the code would pass a bug.
   - **Report.** The walk prints a `tla_report_t`:
     - `ACTS`: each action taken, with a count. It must equal the acts the trace
       records; coverage is counted from the traces TLC accepted, not from this line.
     - `PLANT-FIRED n`: how often the planted EFFECT happened: a value the clean code
       can never produce, such as a place taken past the limit, not the input that
       could provoke it. The clean code's count must be 0.
     - `PLANT-AT`: the records at which the effect showed.
   - **Values.** TLC's integers are 32-bit. `tla_int` refuses anything outside them and
     the trace is not written; a 64-bit counter goes through `tla_int_split` and the
     model compares 4-tuples. `tla_string`, `tla_sequence` and `tla_function` (empty is
     `<<>>`; a key twice is an error, never merged) write the rest.
2. **One row of `LAYERS` in `tools/tla_trace_check.py`.**
   - `modules`: the spec first, any module it extends from `proofs/tla`, then
     `TraceRNX.tla` last.
   - `untraced`: any action of `Next` no walk takes, with the reason.
   - `ghost` (optional): variables of the spec the code does not hold (a history kept
     for a property), with the reason. `VarsRec` leaves them out and the model computes
     them. A ghost is never compared, so no step may read it: the checker refuses a
     ghost that a definition `Next` reaches names other than primed or in `UNCHANGED`.
   - `reach` (optional): state predicates of the Trace module that some clean trace of
     the seed set must reach. A layout layer lists its edges here: wrapped, full, empty
     after a wrap. Each costs one more TLC run per seed until a seed reaches it.
   - `steps` (optional): this layer's walk length, when 400 steps cannot reach its
     edges; never under 100.
   - `self_contained`: the example's alone; any other row with it is refused.
3. **`TraceRNX.tla`.** Copy `TraceRNTraceExample.tla` and change only these:
   - `EXTENDS RNX, Sequences` (more standard modules may follow).
   - `VarsRec == [v |-> v, ...]` over exactly the spec's `VARIABLES`, less the row's
     ghosts. Each field is the variable of its name; nothing is computed (a length in
     place of a ring would compare nothing of its contents).
   - `OpActs == [op |-> {"Action", ...}, ...]`: the actions each operation may be.
   - `Act(a, x) == CASE a = "A" -> A(x[1], ..., x[n]) [] ...`: one arm per action of
     `Next` the walk takes, each the action it names with the record's arguments in
     order.
   - The row's `reach` predicates: plain state predicates over the spec's variables
     (`Full == count = Limit`), no `TRUE`, no definitions or declarations inside.

   `TraceStep` requires the step to be the recorded `Act` AND a `Next` step, so the ranges
   and guards `Next` puts around an action (`\E k \in 1..MaxClaim : Claim(k)`) hold for
   the recorded arguments. An action is a definition `Next` names that changes state
   (a prime or `UNCHANGED` in its own text); a guard `Next` names beside an action is
   not one, and `Act` may not name it. `Next` names each action, not a `Step` made of
   several.

   Everything else is the template's text, and the checker compares it:
   `Trace == TraceLog`, the `ASSUME`, `VARIABLES ti, rejected`, `TraceInit`,
   `TraceStep`, `TraceReject`, `TraceDone`, `TraceSpec` and `TraceConsistent`.
   - `Trace == TraceLog` is there for cost. TLC evaluates a definition of no arguments
     once, but rebuilds the substituted constant at every access. Measured on the
     example at 3200 records: 68 s through `TraceLog`, 3 s through `Trace`; the cost
     grew faster than the square of the length.
   - Never quantify over the log in a state formula: every state would pay for the
     whole log. Shape checks over the log are the template's `ASSUME`, checked once.

## What makes it red

`tools/tla_trace_check.py` is red, with the reason named, when any of these hold.

- **The layer's files**, before any walk:
  - the Trace module differs from the template outside `EXTENDS`, `VarsRec`, `OpActs`,
    `Act` and the reach predicates, or defines anything else;
  - `VarsRec` is not `[v |-> v, ...]` over exactly the spec's `VARIABLES` less ghosts,
    or the spec's `vars` is not its `VARIABLES`;
  - an `Act` arm takes another action than it names, an action not in `Next`, a
    definition that changes no state, or other arguments than `x[1], ..., x[n]`, or
    `Act` holds anything but its arms;
  - a ghost is read by a step, or a reach predicate is not a plain state predicate;
  - `untraced` or `ghost` names something the spec does not have;
  - the layer's file, or a header of `tests/` it includes, defines or undefines a
    `ROCKETNET_` macro other than its plant's (comments, line splices and `%:` are
    read as the preprocessor reads them); it includes no sdk `.cpp`, or one outside a
    top-level `#if defined( ROCKETNET_TLA_PLANT )`; that block holds anything but plant
    defines and sdk includes, or the file tests the plant anywhere else; or no `#if` of
    those sdk files tests its plant define (the example alone is self-contained, and
    its row says why).
- **The table:** a registered layer with no row, or a row with no layer; a name
  registered twice (`REFUSED <name> registered-twice`), a bad name, or two rows of one
  name; a row key the table does not know, `self_contained` on a row but the
  example's, or `steps` under 100; a plant binary that is not exactly one layer.
- **Each walk:**
  - it fails, times out (120 s), or prints no `CONFIG`, `TRACE`, `ACTS`,
    `PLANT-FIRED` and `PLANT-AT`;
  - two runs of one seed write different bytes (a walk that reads a clock or an
    address is caught here);
  - the trace is not `steps + 1` records, or `ACTS` differs from the acts it records;
  - the cfg it wrote is anything but `SPECIFICATION TraceSpec`, `INVARIANT
    TraceConsistent`, `TraceLog <- Log` and plain values for the spec's declared
    `CONSTANT`s (TLC would take `Name = value` for a definition as an override);
  - the module it wrote is not the writer's form: `EXTENDS Trace<Spec>, TLC`, no
    definitions, then `Log`; or the walk wrote any other file;
  - the binaries see a different value of a configuration macro (`ROCKETNET_IO_URING`,
    `ROCKETNET_XDP`, `ROCKETNET_HISTORY_PACKETS`, ...) than the library is built with
    (`tla_trace_config.txt`, written by CMake from `rocketnet_dev`), or the plant
    binary's differ from the clean one's.
- **The clean trace:** TLC rejects it; times out (`--tlc-timeout`, 100 s) or errs on
  it; or found fewer distinct states than the trace has records, so it never reached
  the end. The clean walk reports the planted effect.
- **The plant:** the effect never happened; the planted trace is the clean one; the
  first record it changed is not one the walk reports in `PLANT-AT`; TLC accepts it;
  or TLC rejects it anywhere but at that first changed record. The walk is the same in
  both builds, so the two traces agree until the planted code acts, and the rejection
  must land exactly where it does: the plant is the cause.
- **Across the seeds:** an action of `Next` that is not `untraced`, or a `reach`
  predicate, that no accepted clean trace took or reached.

`tools/planted_violations.tsv` plants these on the example layer, each red with its
reason. The sdk-include rules are planted on the example with its `self_contained`
key removed; the example has no sdk code of its own, so the rules for a layer that
includes sdk code correctly are exercised by the first such layer's own plant.

## An under-logged harness fails red, never green

Every record carries every model variable and nothing else, plus the call, its
arguments and its action. A state that names fewer or other fields never equals
`VarsRec`, and the checker compares `VarsRec` with the spec's `VARIABLES`.

The harness logs the environment's choices too: a completion's result, a fake kernel's
answer. TLC explores every branch of a step, and a record that leaves a choice open is
rejected on the branch it did not take.

## Controls before code

Where a layer's model is new, a self-test module (`MCTraceRNXSelf.tla`) holds two
hand-written traces:
- one that must pass;
- one with a single wrong step that must fail `TraceConsistent` at that record.

## Where it runs, and what it costs

- **Every commit:** the `tla-trace` job of `ci.yml` builds `tla_trace_all` (the harness
  and every plant) and runs `--layer all --seeds 1-4 --jobs 8 --shard I/9` in nine parallel shard jobs
  (a summary job carries the required name "TLA+ trace check (fixed seeds)").
- **Nightly:** the `trace` job of `tlc.yml` runs 64 seeds that start where the day of
  the year puts them.
- **Cost:** see "Measured cost" below.
- **On failure:** TLC prints the behaviour up to the rejection. The last `ti` is the
  last record that matched, so record `ti + 1` is the first code step that is not the
  model step it names.
- **Summary line:** `TRACE-CHECK OK layers=... seeds=... reruns=N` (or `TRACE-CHECK
  FAILED reruns=N` after the failures). `reruns` counts the plant TLC runs past each
  seed's first over the whole run (a plant whose violation TLC reports without
  printing a behaviour is run again, up to five times; each seed's log says
  `PLANT-ATTEMPTS` when it was).
- **A shard with no layer is an error:** `--shard I/N` with I at or past the layer
  count exits 2 and checks nothing; it never prints `TRACE-CHECK OK`.

## Measured cost

The per-commit `tla-trace` job, measured on bench-nyc-6 at load 30 to 70 (2026-11-12,
b774962a1, seeds 1-4, `--jobs 8`):

| part | seconds |
|---|---|
| cold build of `tla_trace_all` (no compiler cache) | 9 to 13 |
| tlacheck clone and build (empty Go cache 12 to 14) | 15 to 17 |
| the check, 1 example-sized layer | 4 |
| the check, 8 example-sized layers | 21 |
| the check, 16 example-sized layers | 41 |

The check grows about 2.6 s per example-sized layer: each costs four seeds of two
walks twice over, two TLC runs and its reach runs, each TLC run about 2.5 s of JVM
start. With build and tlacheck at about 30 s, the 2-minute budget holds about 30
example-sized layers; a real layer's longer walk and larger state cost more, measured
when its row lands. When the check step passes 60 s, CI splits it with `--shard I/N`
into parallel jobs over the same build, and the layers of one shard are the sorted
list's every Nth.

## MIGRATION: a layer written against v1 (dc60a6b09 or 0c583957e)

v1's record was the state alone, and a step passed if any action of `Next` could
produce it.

First, the base. Migrate against v2 on `v1.2/trace-check-infra`, which sits on main at
v1.1.8's release (bff9bb18b). Your branch must carry main's checkout fix before any CI
run: v1's branch point still has `filter: tree:0` in the workflows, and that damages
the runners' clones. Rebase or merge onto `v1.2/trace-check-infra`, and check that
`grep -n 'filter: tree:0' .github/workflows/*.yml` finds only a comment.

A v1 layer then changes in exactly these places:

1. **`tests/tla_trace_<layer>.cpp`, records.** Every `records.push_back( record )`
   becomes `records.push_back( tla_step( op, act, args, record ) )`:
   - `op`: a lower-case identifier for the call the walk made (`"claim"`,
     `"post_completion"`).
   - `act`: the model action that call took, read from its return value or
     out-parameters (`ok ? "Claim" : "Refuse"`), never from the state's change.
   - `args`: `tla_sequence( { tla_int( a ), ... } )` of the action's parameters in the
     order the model declares them, `"<<>>"` for none. A value the call returns (an
     item popped, a slot taken, a count freed) is a parameter of its action too; add
     it to the model's action if v1's model left it out.
   - The first record is `tla_step( "init", "init", "<<>>", state )`.
2. **`tests/tla_trace_<layer>.cpp`, report.** Replace `printf( "PLANT-FIRED %d\n", n )`
   with a `tla_report_t report`: `report.act( act )` beside each `tla_step`, and
   `report.planted( (int) records.size() )` right after pushing the record where the
   planted EFFECT shows (a value the clean code can never produce), not where its input
   condition arises. Call `report.print()` before `tla_write_trace`, which now takes the
   module stem and the seed, `tla_write_trace( out, "MCTraceX", seed, ... )`, and names
   the plant trace itself: delete the walk's `#if defined( ROCKETNET_TLA_PLANT )` around
   the name, and every other test of the plant outside the sdk include block. TLC must reject
   the planted trace at the first record that differs from the clean trace, and that
   record must be in `PLANT-AT`; if the plant changes a record before the one you
   meant, narrow the plant.
3. **`tests/tla_trace_<layer>.cpp`, the plant and the code.** The file includes the sdk
   `.cpp` under `ROCKETNET_TLA_PLANT`, with a `ROCKETNET_<X>_PLANT` define that the sdk
   file tests. Delete any other `#define` or `#undef` of a `ROCKETNET_` macro, and any
   copy of library code in the test file: the walk calls the library.
4. **`tests/tla_trace_<layer>.cpp`, values.** A value that can pass 2^31 - 1 goes
   through `tla_int_split` (the model compares 4-tuples); `tla_int` now refuses it and
   no trace is written, so never cast to `int` before it. An empty `tla_function` is
   now `<<>>`, and a key twice is refused. Constants in the cfg
   are plain values taken from the code's own definitions.
5. **`proofs/tla/Trace<Spec>.tla`.** Copy the v2 `TraceRNTraceExample.tla`, then set
   `EXTENDS <Spec>, Sequences`, your `VarsRec` in the form `[v |-> v, ...]` over every
   variable (a ghost aside), `OpActs`, `Act(a, x)` with one arm per action, and your
   reach predicates. Change nothing else: the checker compares the rest with the
   template. If your v1 `VarsRec` computed a field (a length, a sum), log the variable
   itself instead. If the spec's `vars` leaves a variable out, fix the spec.
6. **`tools/tla_trace_check.py`, your row.** `{"modules": [spec, ..., "Trace<Spec>.tla"],
   "untraced": {"Action": "why no walk takes it"}}`, plus `ghost`, `reach` and `steps`
   (100 or more) where they apply. Drop `kind`, `quiet` and `features`. A layout layer lists its
   edges in `reach` and sets `steps` so a walk reaches them.
7. **CI.** Nothing: `--layer all` picks the row up. Build `tla_trace_all`.

Run it before you push, on a bench with the pinned jar:
`cmake --build build --target tla_trace_all && python3 tools/tla_trace_check.py --bin bin
--tlacheck TLACHECK --jar /opt/tla/tla2tools.jar --layer <yours> --seeds 1-4 --jobs 8
--dir WORK`.
