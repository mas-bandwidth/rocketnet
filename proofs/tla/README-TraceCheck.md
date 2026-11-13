# The trace check: binding the code to a model

A model checked by TLC says the design keeps its rules. It does not say the code
implements the design. The trace check closes that gap. A seeded random walk drives
the code, and the walk writes down the model's variables after every transition.
TLC then checks that the written sequence is a behaviour of the layer's spec. A
layer that does index arithmetic or owns a state machine is not called secured
without this check.

The smallest whole layer is the example: `tests/tla_trace_example.cpp`, its model
`RNTraceExample.tla` and its trace module `TraceRNTraceExample.tla`. Copy it.

## Adding a layer `x` with spec `RNX`

1. **One file, `tests/tla_trace_x.cpp`.** It holds the walk,
   `static int trace_x( uint64_t seed, int steps, const char * out )`, and registers
   it at namespace scope with `TLA_TRACE_LAYER( "x", trace_x );`
   (`tests/tla_trace_record.h`). CMake compiles every `tests/tla_trace_*.cpp` into
   both binaries; nothing else changes in the build. Under `ROCKETNET_TLA_PLANT` the
   file includes its own code with its plant define, so `tla_trace_plant` carries the
   planted code in place of the library's:

   ```cpp
   #if defined( ROCKETNET_TLA_PLANT )
   #define ROCKETNET_X_PLANT 1
   #include "../sdk/rocketnet_x.cpp"
   #endif // #if defined( ROCKETNET_TLA_PLANT )
   ```

   The walk prints two lines: `PLANT-FIRED n`, how often the plant's condition came
   up, and the `TRACE <path>.tla records=N` line that `tla_write_trace` prints. The
   module it writes is `MCTraceXSeed<seed>`, or `MCTraceXPlantSeed<seed>` in the plant
   build.
2. **One row of `LAYERS` in `tools/tla_trace_check.py`.** The row names the modules
   TLC reads (the spec, any module it extends from `proofs/tla`, and `TraceRNX.tla`)
   and `quiet`. Set `quiet` True when the plant's condition is the planted code's own
   act, so the clean walk must print `PLANT-FIRED 0`. Set it False when the condition
   is a property of the input that the clean code meets too.
3. **`TraceRNX.tla`.** It extends `RNX` and takes the trace as the constant
   `TraceLog`; `TraceRNTraceExample.tla` is the template.
   - `TraceInit` matches record 1 against `Init`.
   - `TraceStep` takes one `Next` step whose successor is the next record.
   - `TraceReject` fires when no model step matches the next record.
   - `TraceConsistent == ~rejected` is the invariant TLC checks.
   - `TraceDone` stutters once the last record is reached, so an accepted trace never
     deadlocks.

   Compare whole records, `VarsRec' = TraceLog[ti + 1]`, never field by field.
   Record equality already requires the same fields, and it is several times cheaper
   for TLC.

`--layer all` runs every row. It also requires the rows and the harness's registered
layers (`tla_trace --list`) to be the same set, so a layer with no row is red, and so
is a row with no layer. The CI jobs run `--layer all`, so a new layer needs no
workflow edit.

## An under-logged harness fails red, never green

Every record carries every model variable and nothing else. A record that names
fewer fields, or other fields, never equals `VarsRec`, so the trace is rejected.

The harness logs the environment's choices too. TLC explores every branch of a step,
and if a record leaves a choice open (a completion's result, a fake kernel's answer),
some branch takes the other choice and is rejected at the next record. That makes
the check fail red even though the trace is a correct behaviour. For a ring that
means the fake's posted completions and each packet's status.

This is strict by design. A harness that writes too little can only fail; it can
never pass a trace it did not show.

## Controls before code

Where a layer's model is new, a self-test module (`MCTraceRNXSelf.tla`) holds two
hand-written traces:
- one that must pass;
- one with a single wrong step that must fail `TraceConsistent` at that record.

These show that the check accepts real behaviours and rejects a broken one before any
code writes a trace.

## The plant

The plant shows the check bites on real code. It is one deliberate code change that
breaks the model while the unit tests may stay green.
- With the plant in, the harness's traces must fail `TraceConsistent`.
- Without it, they pass.
- The plant must fire: a planted walk that prints `PLANT-FIRED 0` is a failure,
  because its trace proves nothing.

## Where it runs

- **Every commit:** the `tla-trace` job of `ci.yml` builds `tla_trace` and
  `tla_trace_plant` and runs `tools/tla_trace_check.py --layer all --seeds 1-4`
  through the pinned `tlacheck` and the pinned `tla2tools.jar`.
- **Nightly:** the `trace` job of `tlc.yml` runs 64 seeds that start where the day
  of the year puts them.
- **One run per trace:** each trace, clean or planted, is its own `tlacheck` run in
  a root of its own, so each gets the whole run budget.
- **Wiring:** `tools/planted_violations.tsv` plants the example's clean claim as the
  planted one, and both jobs must go red on it.
- **On failure:** TLC prints the behaviour up to the rejection. The last `ti` is the
  last record that matched, so record `ti + 1` is the first code step that is not a
  model step.
