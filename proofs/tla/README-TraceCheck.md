# The trace check: binding the code to a model

A model checked by TLC says the design keeps its rules. It does not say the code
implements the design. The trace check closes that gap. A seeded random walk drives
the code and writes down, after every call, which operation it made, which model
action the code's own result says it took, and the model's variables. TLC then checks
that the written sequence is a behaviour of the layer's spec, one named action at a
time. A layer that does index arithmetic or owns a state machine is not called
secured without this check.

The smallest whole layer is the example: `tests/tla_trace_example.cpp`, its model
`RNTraceExample.tla` and its trace module `TraceRNTraceExample.tla`. Copy it.

## What it can and cannot see

The walk interleaves whole calls: one call of the code per step, run to its end on
one thread. It sees every order of calls the walk can make, every refusal, and every
index the code computes. It cannot see a race inside one call, a torn read, or a
memory-order fault. Those belong to TSan and to the layer's own concurrency model.

For a completion-driven layer (a ring, a worker), the walk is the kernel too. The
walk posts each fake completion as a step of its own, with its result in the record:
completions in any order, early or late, failed or short. A completion the fake never
posts is never checked.

## Adding a layer `x` with spec `RNX`

1. **One file, `tests/tla_trace_x.cpp`.** It holds the walk,
   `static int trace_x( uint64_t seed, int steps, const char * out )`, and registers it
   at namespace scope with `TLA_TRACE_LAYER( "x", trace_x );` (`tests/tla_trace_record.h`).
   - **Names.** A layer name is lower-case letters, digits and `-`. The module the walk
     writes is `MC` followed by letters and digits (`MCTraceXSeed<seed>`, or
     `MCTraceXPlantSeed<seed>` in the plant build). tlacheck refuses any other name,
     and the writer refuses it first, with a message that says so.
   - **Build.** `tla_trace` takes every `tests/tla_trace_*.cpp`. Each file also gets
     its own plant binary, `tla_trace_plant_<file>`, holding that file alone, compiled
     with `ROCKETNET_TLA_PLANT` and with the library's own private definitions,
     include directories and options. Under `ROCKETNET_TLA_PLANT` the file includes its
     own code with its plant define, so the plant binary carries the planted code in
     place of the library's:

     ```cpp
     #if defined( ROCKETNET_TLA_PLANT )
     #define ROCKETNET_X_PLANT 1
     #include "../sdk/rocketnet_x.cpp"
     #endif // #if defined( ROCKETNET_TLA_PLANT )
     ```

     One layer's plant never reaches another layer's trace. Two layers may include the
     same sdk file.
   - **Records.** A step layer writes one record per step with
     `tla_step( op, act, state )`: the operation it called, the model action the code's
     result says it took, and every model variable after it. Record 1 is
     `tla_step( "init", "init", state )`.
   - **Report.** The walk prints a `tla_report_t`:
     - `ACTS`: each action taken, with a count.
     - `PLANT-FIRED n`: how often the planted EFFECT happened. This is a fact about
       what the code did, such as a place taken past the limit, not about the input
       that could provoke it. The clean code's count must be 0.
     - `PLANT-AT`: the records at which the effect happened.
   - **Values.** TLC's integers are 32-bit. `tla_int` refuses anything outside them
     and the trace is not written; a 64-bit counter goes through `tla_int_split`.
     `tla_string`, `tla_sequence` and `tla_function` (empty is `<<>>`) write the rest.
2. **One row of `LAYERS` in `tools/tla_trace_check.py`.**
   - `modules`: the spec first, then `TraceRNX.tla`.
   - `kind`:
     - `"steps"`: the walk writes records over time.
     - `"cases"`: a pure function checked case by case. The Trace module starts the
       model from each case's input and compares its output; the row lists the
       `features` (input and output shapes) the walk must show.
   - `untraced`: any action of `Next` no walk takes, with the reason.
3. **`TraceRNX.tla`.** It extends `RNX`; `TraceRNTraceExample.tla` is the template.
   - `OpActs` maps each operation to the actions it may be.
   - `Act(a)` maps each action name to its formula.
   - `TraceStep` allows only the recorded action, and requires
     `VarsRec' = TraceLog[ti + 1].state`, which compares whole records.
   - Never quantify over `TraceLog` in a state formula. Every state would pay for the
     whole log, and a 150-record log went from 2 s to over 110 s. Shape checks over the
     log are `ASSUME`s, checked once.

## What makes it red

`tools/tla_trace_check.py --layer all` is red, with the reason named, when any of these
hold:

- **Harness:**
  - a walk fails, times out, or prints no `TRACE`, `ACTS`, `PLANT-FIRED` and `PLANT-AT`;
  - two runs of one seed write different bytes (a walk that reads a clock or an address
    is caught here);
  - a step trace is not `steps + 1` records.
- **Clean trace:**
  - TLC rejects it;
  - TLC times out or errs on it;
  - the clean walk reports the planted effect.
- **Plant:**
  - the effect never happened;
  - TLC accepts the planted trace;
  - TLC rejects it at a record where the effect did not happen. The plant must be the
    cause.
- **Layer:**
  - an action of `Next` (or a row's feature) no seed reached;
  - a `VarsRec` that does not name exactly the spec's `vars`;
  - a registered layer with no row, or a row with no layer;
  - a name registered twice;
  - a plant binary that is not exactly one layer.

Each of these is planted in `tools/planted_violations.tsv` on the example layer, and
the check must go red on it with that reason.

## An under-logged harness fails red, never green

Every record carries every model variable and nothing else, plus the action. A state
that names fewer or other fields never equals `VarsRec`. The checker also compares
`VarsRec` with the spec's `vars`, so a variable named in neither is caught.

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
  and every plant) and runs `--layer all --seeds 1-4 --jobs 8`.
- **Nightly:** the `trace` job of `tlc.yml` runs 64 seeds that start where the day of
  the year puts them.
- **Cost:** each seed of each layer is four walks and two TLC runs, each in a root of
  its own. One TLC run is a JVM of about 2 GB. `--jobs 8` runs four seeds at once,
  which is eight JVMs. With the clean and planted runs of a small layer at 5-20 s each,
  the per-commit job is about `layers × 4 seeds × 20 s / 4` of TLC, plus the build:
  - 1 minute for 3 layers;
  - over the 2-minute per-commit bound at about 6 layers.

  Past that, the per-commit job takes seed 1 only per layer and the nightly takes the
  rest. The measured times are in the records of the job.
- **On failure:** TLC prints the behaviour up to the rejection. The last `ti` is the
  last record that matched, so record `ti + 1` is the first code step that is not the
  model step it names.
