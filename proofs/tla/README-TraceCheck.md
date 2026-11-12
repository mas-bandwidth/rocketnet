# The trace check: binding the code to a model

A model checked by TLC says the design keeps its rules. It does not say the code
implements the design. The trace check closes that gap. A seeded random walk drives
the code, and the walk writes down the model's variables after every transition.
TLC then checks that the written sequence is a behaviour of the layer's spec. A
layer that does index arithmetic or owns a state machine is not called secured
without this check. The first layer to use it is the io_uring per-queue send (L1,
`RNUringQueueSend`). Every later layer follows the same shape.

## The parts, for a layer with spec `RNX`

1. **The harness.** It is a unit-suite test, so it uses no real time and no sockets.
   - It drives the code under test with a seeded random walk.
   - The kernel side is a fake that the walk also drives. For io_uring that is the
     transport's fake completion source. Completions arrive in any legal order, and
     some come back as `-EAGAIN`, failures or short sends.
   - Every time the code takes a step that the model names as an action, the harness
     appends one record of the model's variables.
2. **The trace module, `TraceRNX.tla`.** It extends `RNX`; `TraceRNUringQueueSend.tla`
   is the template.
   - It takes the trace as the constant `TraceLog`.
   - `TraceInit` matches record 1 against `Init`.
   - `TraceStep` takes one `Next` step whose successor matches the next record.
   - `TraceReject` fires when no model step matches the next record.
   - `TraceConsistent == ~rejected` is the invariant TLC checks.
   - `TraceDone` stutters once the last record is reached, so an accepted trace
     never deadlocks.
   - `TraceNext == TraceStep \/ TraceReject \/ TraceDone` is the module's own
     next-state action (it cannot define `Next` again), and the per-action
     citation check follows it (README.md); the harness cites `TraceStep` and
     `TraceReject`, and the row declares `TraceDone` an environment action.
   - Compare the whole record, `VarsRec' = TraceLog[ti + 1]`, not field by
     field: TLC evaluates the harness's `Log` at every reference to `TraceLog`,
     so a comparison per field costs a pass over the trace per field. For
     `TraceRNGsoQueueBankLayout` that was 40 s against 6 s on a 171-record trace.
3. **The record format.** The harness writes `TraceRNX_<seed>.tla`, which defines
   `TraceLog` as a sequence of TLA records, one field per variable, named as
   `VarsRec` names them. It also writes a cfg that substitutes that definition for
   `TraceLog`.

## Every record carries every variable

TLC explores every branch of a step. If a record leaves a choice open, such as a
completion's result, some branch takes the other choice and is rejected at the next
record. That makes the check fail red even though the trace is a correct behaviour.

So the harness logs the environment's choices too. For `RNUringQueueSend` that means
the fake's posted completions (`runs`, `cq`) and the packets' statuses.

This is strict by design: an under-logged trace fails red, never green.

## Controls before code

Each trace module has a self-test module (`MCTraceRNUringQueueSendSelf.tla`) with two
hand-written traces:
- one that must pass;
- one with a single wrong step (here, a bank retired before its last completion) that
  must fail `TraceConsistent` at that record.

These two checks show that the check accepts real behaviours and rejects a broken one
before any code writes a trace.

## The plant

The plant shows the check bites on real code. It is one deliberate code change that
breaks the model while the unit tests may stay green: for L1, recycling a bank before
its last completion.
- With the plant in, the harness's traces must fail `TraceConsistent`.
- Without it, they pass.

The plant is kept as a build flag of the harness, and CI runs it as a reversed witness
next to the model's own witnesses.

## Where it runs

- **Every commit:** CI runs the harness at a fixed set of seeds, then runs TLC
  (pinned `tlacheck`, pinned `tla2tools.jar`) on each trace and on the plant. This
  sits beside the witnesses.
- **Nightly:** a rolling range of seeds.
- **On failure:** TLC prints the behaviour up to the rejection. The last `ti` is the
  last record that matched, so record `ti + 1` is the first code step that is not a
  model step.
