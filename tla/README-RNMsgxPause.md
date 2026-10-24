# RNMsgxPause: the msgx pause machine

`RNMsgxPause.tla` models the pause and re-probe of the msgx pump in
`sdk/rocketnet_platform.cpp`. The modelled functions are
`rocketnet_msgx_pause` and `rocketnet_msgx_active`. Two workers share one
socket, the same arrangement as the send and receive batch paths.

`down_until` is the shared slot: 0, or the tick at which a pause expires.
Each worker keeps the value it observed and a step.

## The abstraction

- **Pause.** From `idle` or `probe`, store `now + Retry` and return to
  `idle`. This is `rocketnet_msgx_pause`: an unconditional store of
  `rocketnet_platform_time() + ROCKETNET_MSGX_RETRY_SECONDS`. The model
  does not pause a worker that is between its load and its
  compare-and-swap.
- **Expire.** The clock advances one tick, and stops at `MaxTime`. It is
  not a line of those two functions.
- **ObserveExpired.** From `idle` or `probe`, if the slot holds an expiry
  the clock has reached, remember that value and step to `hold`. This is
  the load in `rocketnet_msgx_active` once `rocketnet_platform_time()` is
  no longer before the loaded value.
- **ClearIfUnchanged.** From `hold`. If the slot still holds the observed
  value, store 0 and step to `probe`: the compare-and-swap succeeded, and
  this call is the re-probe. If it does not, the rule leaves the slot
  alone. A 0 means the other worker already cleared; any other value is a
  pause published after the load, and the worker returns to `idle` so the
  next load sees it. The C compare-and-swap updates its expected word and
  loops; the model splits that loop so the other worker can publish in
  the gap, which is the race the clear exists for.
- **Probe.** From `idle`, if the slot is 0, step to `probe`. This is
  `rocketnet_msgx_active` finding a clear pump: the batch attempt is the
  re-probe.
- **Done.** After the last tick, with no worker in `hold` and no expiry
  the clock has reached left in the slot, the run stutters. A real stall
  (a worker left in `hold`, or an expiry the clock has reached and nobody
  can load) still has no step.

`CasClearsOnlyObserved` is the rule switch. `FALSE` is the reversed
witness: the clear stores 0 even when the slot has changed.

## Properties

- `FreshPauseSurvives`: a pause published after a worker observed an older
  expiry is never cleared by that worker's clear. The history bit
  `cleared_unobserved` is set only by the reversed witness's store of 0
  over a different positive value.
- `ExpiredPauseSettled` (liveness): for each tick `t` in `1..MaxTime`, a
  state with `down_until = t` and `now >= t` is followed by a state whose
  slot is no longer `t` (cleared, or replaced by a later publish).
- `TypeOK`.

Fairness is weak fairness on `Expire`, and on each worker's
`ObserveExpired` and `ClearIfUnchanged`. `Pause` is not fair: a hard
refusal is not guaranteed. `Probe` and `Done` are not fair.

## Cases and the check against the code

| Config | Expected | What it shows |
|---|---|---|
| `MCRNMsgxPause.cfg` | pass | Two workers, `MaxTime = 4`, `Retry = 2`, compare-and-swap on. `TypeOK`, `FreshPauseSurvives` and `ExpiredPauseSettled`. |
| `MCRNMsgxPauseBrokenExpiryClearsFresh.cfg` | `FreshPauseSurvives` | `CasClearsOnlyObserved = FALSE`. The clear stores 0 with no compare. |

The reversed witness, read against the code:

- One worker loads an expiry the clock has reached (`observed` is that
  value, step `hold`). The other worker runs `Pause` and the slot becomes
  `now + Retry`, a later expiry. `ClearIfUnchanged` of the first worker
  then stores 0. The later pause is gone, and the worker that published
  it is not the one whose clear removed it.
- In the code, `rocketnet_msgx_active` passes the loaded word to
  `__atomic_compare_exchange`. A store that landed after the load fails
  the compare, the loop sees the new word, and a future pause returns
  without writing 0. `rocketnet_msgx_test_pause_expiry_race` is the same
  gap with the publish placed in the test hook immediately before the
  compare-and-swap.
