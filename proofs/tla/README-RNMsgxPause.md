# Shared msgx pause expiry

`RNMsgxPause.tla` models the atomic `msgx_down_until` used by
`rocketnet_msgx_active` and `rocketnet_msgx_pause` in
`sdk/rocketnet_platform.cpp`. TX and RX may hold different observations of
one socket's expiry. This is separate from the receive-bank ownership in
`RNPortableRx`.

The instance has two workers, time 0 through 3 and retry length 2. A worker
probes when the pause is absent, then either completes its batch or publishes
a new pause after refusal. `Expire` advances monotone time. `ObserveExpired`
loads an expired deadline; `ClearIfUnchanged` clears that deadline only if it
still matches. A failed compare-and-swap reloads the actual deadline and
rechecks whether it is zero, still live, or expired, as the C++ loop does.
`PollPaused` represents an inactive call during a live pause.

`FreshPauseSurvives` checks a history flag set only when a clear actually
erases a deadline newer than the clearing worker's observation. It also
protects a newer deadline that expires while the old worker is delayed.
Positive retry and monotone time make a pause published after an expired
observation strictly newer; same-tick publications before expiry need no
unique generation. No ordering of unrelated socket state is modeled: the
source atomics are relaxed.

`ExpiredClearedOrReplaced` says every currently expired value eventually
changes. The passing configuration assumes weak fairness for time advance,
probe completion, eligible probing, expired observation and the compare-and-swap
step. It does not assume that probes fail or that the environment publishes
another pause. Expiries beyond the bounded clock are outside this liveness
claim. This finite instance is not a claim about clock rollback or unbounded
execution.

The broken configuration changes only `CompareBeforeClear` to FALSE. Its
unconditional zero store must violate `FreshPauseSurvives`: after one worker
observes an old expired value, a delayed probe publishes a new pause and the
old worker erases it. Actual run counts and the witness trace are obtained
through the pinned runner and recorded in `RUNS.tsv`; no runtime behavior or
unit program changes are needed.

## Measured witness

The passing run checks 151 distinct states (452 generated), including the
liveness property. The broken run exits 12 on `FreshPauseSurvives` after
111 distinct states (249 generated). Its nine-state trace has both workers
start probes at time 0; TX publishes expiry 2; time advances to 2; TX
observes expiry 2; RX publishes expiry 4; TX's unconditional clear erases
4. The source compare-and-swap instead reloads 4 into TX's local observation
and returns inactive while time is 2. This directly matches the hand-built
`rocketnet_msgx_test_pause_expiry_race` ordering. The measurements are in
`RUNS.tsv`; the broken trace was inspected against the source loop.
