# RNClientReceiveQuit

**Shutdown scope:** The request/completion API is modeled in
[RNClientIoShutdown](README-RNClientIoShutdown.md). That ownership model is not
a proved refinement of this one.

The legacy client receive thread's quit-and-wake handshake with stop and
destroy. This is the thread `create` starts when the I/O thread does not run:
the granted transport has no wait and wake (custom BASE drivers, and the
built-in portable client on platforms without `ROCKETNET_PLATFORM_HAS_WAKE`),
or a development link simulator is active. The I/O thread is `RNClientIo`'s.

- **The thread** (`rocketnet_client_thread_function`,
  `sdk/rocketnet_client_transport.cpp`). `Check` is the acquire load of
  `quit` at the top of every pass. The body drains the socket
  (`rocketnet_client_receive_packets`, which never blocks) and then calls
  `rocketnet_platform_thread_wait( thread, 0.001 )`: `WaitBegin`. `WaitWoken`
  is that wait ending on the wake, `WaitTimesOut` on its timeout.
- **The wait and the wake, per platform** (`sdk/rocketnet_platform.cpp`).
  - linux: the wait locks `wake_mutex` and, while `woken` is false, waits on
    `wake_cond` until a CLOCK_MONOTONIC deadline; the wake sets `woken` under
    the same mutex and broadcasts. A spurious wakeup re-checks `woken` and
    waits again: `Spurious` changes nothing on linux.
  - mac and iOS: the wait returns at once when `woken` is set (acquire), else
    waits on a mach semaphore until the timeout; the wake stores `woken` with
    release and then signals the semaphore. A wake between the check and the
    semaphore wait is kept by the semaphore's count (`sem`). The wait ignores
    `semaphore_timedwait`'s result, so an aborted wait returns early.
  - windows: the wait is `rocketnet_platform_sleep`, which always returns; the
    wake is a no-op. `HasWake` is false.
  - `woken` is never cleared, so every wait after the wake returns at once.
- **Stop and destroy.** `StopQuit` is the release store of `quit` in the
  legacy branch of `rocketnet_client_transport_stop`, and `StopWake` the
  `rocketnet_client_thread_wake` that follows it.
  `JoinBegin` and `JoinReturns` are `rocketnet_platform_thread_join` in
  `rocketnet_client_thread_release`, which returns only once the thread
  function has returned, and `DestroyFree` the `thread_destroy` and the frees of
  the transport that follow. `StopQuitLate` exists only for the defect
  `WakeBeforeQuit`.

`WaitTimeout` says what the model may assume of the wait's timeout: `"fair"`
(it always fires: Windows' sleep), `"unreliable"` (it may never fire, so linux
and mac must finish destroy on the wake alone; no fairness on `WaitTimesOut`),
or `"none"`.

## Invariants and properties

- **`WokenSeesQuit`**: a wait the wake ended is followed by a quit check that
  ends the loop; the wake is never spent on a pass that still reads `quit`
  false.
- **`DestroyCompletes`**: once stop has begun, destroy completes, under weak
  fairness of the thread's `Check`, `WaitBegin` and `WaitWoken`, of the steps
  after stop's first, and of `WaitTimesOut` only when `WaitTimeout = "fair"`
  (`SpecFair`).
- **`TypeOK`**.

That the join returns only after the thread has exited is the platform join's
contract (`JoinReturns` is enabled only at `th = "exited"`), so the module
states no invariant for it.

## Cases

The model is finite as written (one thread, one stop, one wake), so there are
no size constants. The measured records are in `proofs/tla/RUNS.tsv` (group
`rnclientreceivequit`).

| Configuration | Platform | Timeout | Expected |
|---|---|---|---|
| `MCRNClientReceiveQuit.cfg` | linux | unreliable | pass |
| `MCRNClientReceiveQuitMac.cfg` | mac | unreliable | pass |
| `MCRNClientReceiveQuitWindows.cfg` | windows | fair | pass |
| `MCRNClientReceiveQuitBrokenWakeBeforeQuit.cfg` | linux | unreliable | fails `WokenSeesQuit` |
| `MCRNClientReceiveQuitBrokenNoWakeNoTimeout.cfg` | linux | none | fails `DestroyCompletes` |
| `MCRNClientReceiveQuitBrokenNoWake.cfg` | linux | unreliable | fails `DestroyCompletes` |

## The witnesses, checked against the code by hand

**`WakeBeforeQuit` (WokenSeesQuit).** The thread checks `quit` false and
enters the drain; the defect's stop wakes first (`woken` set) with `quit`
still false; the next wait finds `woken` set and returns at once with `quit`
false. In the code `quit.store` precedes `rocketnet_client_thread_wake` on the
same thread, and the wake's mutex (on mac, its release store paired with the
wait's acquire load) orders the two, so a wait the wake ends is followed by a
check that reads `quit` true.

**`NoWake` alone with `WaitTimeout = "unreliable"` (DestroyCompletes).**
This changes only `NoWake` from the healthy Linux configuration. A timeout
remains enabled, but `SpecFair` does not require it to occur in this
environment. A thread already waiting when stop makes no wake can therefore
stutter while destroy waits in the join, violating `DestroyCompletes`.
This witness does not assume a fair timeout or remove an enabled timeout.

The older two-switch configuration below is retained as historical evidence
for the stricter environment with no timeout transition.

**`NoWake` with `WaitTimeout = "none"` (DestroyCompletes).** The thread checks
`quit` false and blocks in its wait; stop stores `quit` and makes no wake;
destroy enters the join; then stuttering: the thread never leaves its wait,
so the join never returns. Before stop made the wake, this was the code on
every platform whose timed wait can fail to time out (the macOS hang this
model was first written for). `test_client_legacy_stop_wakes_the_receive_thread`
(`tests/test_unit_client_memory_plan.cpp`) checks that stop on the legacy path
makes the wake.

## Not modelled

The I/O thread branch (`RNClientIo`); create's failure path (no thread to
join); and the contents of the drain, which `RNClientIo` and the receive tests
cover.
