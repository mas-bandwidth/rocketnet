# RNMsgxPause: the macOS msgx pump pause and re-probe

`RNMsgxPause.tla` models the pause and re-probe of the macOS msgx batch pump
in `sdk/rocketnet_platform.cpp`. The modelled functions are:

- `rocketnet_msgx_pause`, which a refused `sendmsg_x` or `recvmsg_x` (an
  errno outside the weather sets) runs;
- `rocketnet_msgx_active`, which every batch call runs first.

One socket holds one shared word, `msgx_down_until`: 0 while the batch pump
is live, or the time a pause expires. The server's TX and RX workers run
batch calls on the same socket at once, so both race on that word.

## The abstraction

- **Time.** `now` counts ticks up to `MaxTime`. `Expire` advances it one
  tick; it is the only environment action. `Retry` stands for
  `ROCKETNET_MSGX_RETRY_SECONDS`.
- **The word.** `down` is `msgx_down_until`.
- **Workers.** `Workers` is the TX and RX worker. Each one steps through:
  - `Load`: the first load in `rocketnet_msgx_active`. A 0 goes straight to
    the batch call.
  - `StayPaused`: a pause that has not expired. `rocketnet_msgx_active`
    returns false and the caller takes the per-packet path.
  - `ObserveExpired`: the expiry test passed. This is where the test hook
    `rocketnet_msgx_before_expiry_clear` runs, before the swap.
  - `ClearIfUnchanged`: the compare-and-swap. It clears the word only while
    it still holds the value this worker loaded. A failed swap reloads the
    word and tests it again.
  - `Probe`: the batch call after a true return (the re-probe, after a
    clear) succeeds, or is refused for weather, which never pauses.
  - `Pause`: the batch call is refused and `rocketnet_msgx_pause` stores
    `now + Retry` unconditionally.
- **Ghost.** `removed` is the value the word held when the last clear ran.
  It is not in the code; it lets the invariant name what a clear removed.

`StoreZero` is the rule switch. It is FALSE in the control. TRUE replaces
the swap with `ClearUnconditional`, a plain store of 0.

## Properties

- `FreshPauseSurvives`: no clear removes a pause that has not expired
  (`removed <= now`). A pause the other worker publishes after this worker
  observed an older expiry expires after `now`, so this worker's clear
  never removes it.
- `ClearOnlyExpired`: a worker about to clear holds a value that had
  expired when it looked.
- `TypeOK`.
- `ExpiredPauseResolved` (liveness): an expired pause is eventually cleared
  or replaced by a fresh one. It holds under weak fairness on each worker's
  steps.

Two pauses stored at the same instant carry the same expiry. The
compare-and-swap compares values and cannot tell them apart. It need not:
clearing either removes an expired value. A first cut of the model numbered
each pause and asked that a clear remove only the pause its worker
observed. TLC refuted it with exactly that case: TX and then RX paused at
tick 0, and TX's clear at tick 2 removed RX's pause, which had the same
expiry and had expired too. The
code does the same, and nothing is lost by it, so the rule is stated on
expiry values.

## Cases and the checks against the code

| Config | Expected | What it shows |
|---|---|---|
| `MCRNMsgxPause.cfg` | pass | Two workers, four ticks, a retry of two. Every invariant and `ExpiredPauseResolved` under `SpecFair`. |
| `MCRNMsgxPauseBrokenExpiryClearsFresh.cfg` | `FreshPauseSurvives` | `StoreZero = TRUE` |

The reversed witness was checked against the code by hand (2026-10-24):

- **ExpiryClearsFresh.**
  - In the trace, TX pauses at tick 0 (expiry 2) while RX's batch call is
    still in flight. At tick 2, TX loads 2 and sees it expired. RX's call is
    then refused and pauses again (expiry 4). TX stores 0 and removes the
    fresh pause at tick 2, so the pump goes live two ticks early.
  - In the code, `rocketnet_msgx_pause` stores `now + retry` with a plain
    atomic store, as `Pause` does. `rocketnet_msgx_active` clears with
    `__atomic_compare_exchange` against the value it loaded, so TX's swap
    fails on 4, reloads it and returns false. That is `ClearIfUnchanged`.
    `rocketnet_msgx_test_pause_expiry_race` drives this interleaving: its
    hook publishes a fresh pause between the expiry test and the swap.
