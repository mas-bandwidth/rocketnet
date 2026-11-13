# RNUpdateClock

`RNUpdateClock.tla` models the update clock the client transport, the server
transport and the message object keep: the caller's raw time beside the
clamped time the owner hands out
(`rocketnet_update_clock_advance` in `sdk/rocketnet_client_transport.cpp`,
declared in `sdk/rocketnet_internal.h`).
The model is the clock only; it says nothing about the packets, the channels
or the slots.

## Abstraction

- **The two times.** `raw` is the caller's clock as it was last sampled;
  `clamped` is the time the owner hands out and every deadline is measured
  against. They are equal until the caller's clock steps backwards.
- **The offset.** `offset` is how far the caller's clock fell behind. It is
  exactly 0 until the first backwards step and never returns to 0, so it is
  the state that says a backwards step has happened.
- **ForwardStep(d).** A forward raw step of `d`: while the caller's clock is
  still behind the clamped one, the clamped time advances by exactly `d`;
  once the caller's clock has caught up, the clamped time is the caller's
  time. While `offset` is 0 the clamped time is the caller's time, bit for
  bit, so a caller that reads an exact time still reads it.
- **BackwardStep(t).** A backwards raw step: the clamped time stands (the
  last good time), `raw` and `offset` record the step, and the misuse is
  counted. This is the case BASE could not resume from: BASE returned the
  last good time until the caller's clock passed that value again, so the
  keep-alive, the send-rate gate, the reliable resends and the dead-connection
  timeout all stood still for the whole step.
- **NonFiniteStep.** A NaN, infinite or negative sample is refused before
  the clock reads it: it changes neither value and counts one misuse. Every
  negative sample is one, not only the first, so raw times are never
  negative and BackwardStep's target is drawn from `0..raw-1`.
- **Done.** The step budget is spent; the model stops.
- **Exact time.** The model's times are bounded naturals and its sums are
  exact. The code's are doubles: the resumed time `last_good + step` rounds
  (a small step beside a large last good time can add less than the step,
  or nothing), and for finite inputs near the top of the double range it can
  overflow to +infinity and stay there, since every later sample is behind
  it. `ResumeAdvances` and `ClampedNeverDecreases` describe the code only
  where that sum is exact and finite. A guard on the sum belongs to the
  code.

## Properties

- `TypeOK` holds the ranges.
- `ClampedNeverDecreases`: no sample moves the clamped time back.
- `ResumeAdvances`: after a backwards step, each later forward raw step of
  `d` advances the clamped time by exactly `d` while the caller's clock is
  still behind; the step that catches up with the clamped time makes the
  clock the caller's clock again.
- `NonFiniteChangesNothing`: a non-finite sample leaves both values alone.

## Cases

- `proofs/tla/MCRNUpdateClock.cfg` passes: the control at `MaxTime = 4`,
  `MaxSteps = 4`, with `Broken = {}`.
- `proofs/tla/MCRNUpdateClockBrokenStall.cfg` violates `ResumeAdvances`: the
  reversed witness for BASE's rule, the clock that stalls until the
  caller's clock catches up.

The model bounds the caller's clock to `MaxTime` and the run to `MaxSteps`
samples; it does not model the transports, the channels or the deadlines that
read the clamped time.
