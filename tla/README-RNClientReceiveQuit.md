# RNClientReceiveQuit

The client receive thread's quit-and-wake handshake with destroy. This is the
thread `rocketnet_client_transport_create_internal` in
`sdk/rocketnet_client_transport.cpp` starts when the granted driver has no
wait and wake (`rocketnet_client_transport_uses_io_thread` false); the I/O
thread of the other branch is `RNClientIo`'s. Destroy must finish on the wake
alone, because the receive thread's 1 ms wait is not relied on to time out:
a wait that never times out and is never woken leaves the join blocked
forever.

- **The thread** (`rocketnet_client_thread_function` in
  `sdk/rocketnet_client_transport.cpp`). `Check` is the acquire load of
  `quit` at the top of every pass: true ends the loop, false enters the body.
  The body drains the socket (`rocketnet_client_receive_packets`, which
  never blocks: it breaks when the buffer is full or the socket is empty)
  and then calls `rocketnet_platform_thread_wait( 0.001 )`: `WaitBegin`.
- **The wait and the wake, per platform** (`rocketnet_platform_thread_wait`
  and `rocketnet_platform_thread_wake` in `sdk/rocketnet_platform.cpp`).
  - linux: the wait locks `wake_mutex` and, while `woken` is false, waits on
    `wake_cond` until a CLOCK_MONOTONIC deadline (`pthread_cond_timedwait`);
    the wake sets `woken` under the same mutex and broadcasts
    (`pthread_cond_broadcast`). The check and the block are one step under
    the mutex, so `WaitBegin` returns at once (`th = "woken"`) when `woken`
    is set and blocks otherwise; `WaitWoken` is the broadcast ending the
    wait. A spurious wakeup re-checks `woken` and waits again (the `while`
    around the timed wait): `Spurious` changes nothing on linux.
  - mac and iOS: the wait returns at once when `woken` is set (acquire), else
    waits on a mach semaphore until the timeout (`semaphore_timedwait`); the
    wake stores `woken` with release and then signals the semaphore
    (`semaphore_signal`). A wake between the check and the semaphore wait is
    kept by the semaphore's count (`sem`), so `WaitWoken` reads `sem`, not
    `woken`. The wait ignores `semaphore_timedwait`'s result, so an aborted
    wait returns early: `Spurious` goes back to the check.
  - windows: the wait is `rocketnet_platform_sleep`, which always returns;
    the wake is a no-op. `HasWake` is false.
  - `woken` starts false at `rocketnet_platform_thread_create` and is never
    cleared, so every wait after the wake returns at once.
- **Destroy** (the receive-thread branch of
  `rocketnet_client_transport_destroy` in
  `sdk/rocketnet_client_transport.cpp`). `DestroyQuit` is the release store
  of `quit`, `DestroyWake` the `rocketnet_platform_thread_wake`, `JoinBegin`
  and `JoinReturns` the `rocketnet_platform_thread_join` (`pthread_join` on
  linux and mac, `WaitForSingleObject` on windows), which returns only once
  the thread function has returned, and `DestroyFree` the
  `rocketnet_platform_thread_destroy` that frees the handle and the frees of
  the transport the thread reads that follow. `DestroyQuitLate` exists only
  for the defect `WakeBeforeQuit`.

`WaitTimeout` says what the model may assume of the wait's timeout: `"fair"`
(it always fires: Windows' sleep), `"unreliable"` (it may never fire,
so linux and mac must finish destroy on the wake alone; no fairness on
`WaitTimesOut`), or `"none"`. A lost wake is a timeout that never
fires; a spurious wake is `Spurious`.

## Invariants and properties

- **`NoJoinBeforeExit`**: the join has returned, and the handle and the
  transport are freed, only once the thread has exited.
- **`NoUseAfterJoin`** (action property): once the join has returned the
  thread takes no step and no wake writes `woken` or the semaphore of the
  handle `thread_destroy` frees.
- **`WokenSeesQuit`**: a wait the wake ended is followed by a quit check that
  ends the loop; the wake is never spent on a pass that still reads `quit`
  false. This is the ordering rule of `rocketnet_platform_thread_wake`'s
  contract in `sdk/rocketnet_platform.h`: a thread that polls a quit flag
  between waits stores the flag first, then wakes, then joins.
- **`DestroyCompletes`**: once destroy has begun it completes, under weak
  fairness of the thread's `Check`, `WaitBegin` and `WaitWoken`, of
  destroy's steps after its first, and of `WaitTimesOut` only when
  `WaitTimeout = "fair"` (`SpecFair`). Destroy's first step has no
  fairness: the game need never call it.
- **`TypeOK`**: every variable has its declared type.

## Runs

The model is finite as written (one thread, one destroy, one wake), so there
are no size constants. Raw TLC 2.19 output, one worker, on a Linux bench:

| Configuration | Platform | Timeout | Result |
|---|---|---|---|
| `MCRNClientReceiveQuit.cfg` | linux | unreliable | No error; 34 generated, 19 distinct |
| `MCRNClientReceiveQuitMac.cfg` | mac | unreliable | No error; 43 generated, 25 distinct |
| `MCRNClientReceiveQuitWindows.cfg` | windows | fair | No error; 27 generated, 17 distinct |
| `MCRNClientReceiveQuitBrokenWakeBeforeQuit.cfg` | linux | unreliable | exit 12, `Invariant WokenSeesQuit is violated` (4 states) |
| `MCRNClientReceiveQuitBrokenNoWakeNoTimeout.cfg` | linux | none | exit 13, `Temporal properties were violated` (`DestroyCompletes`, 6 states then stuttering) |

The measured records are in `tla/RUNS.tsv` (group `rnclientreceivequit`).

Unrecorded mutations, run once to see that the controls do not pass for the
wrong reason: linux and mac with the wake made a no-op but the timeout
`"unreliable"` fail `DestroyCompletes` (so the POSIX controls finish on the
wake, not on the timeout), Windows with the timeout `"unreliable"` fails it
(so the Windows control finishes on the sleep), and linux with no wake and a
`"fair"` timeout passes (a destroy with no wake finishes only on a platform
whose timeout fires).

## The witnesses, checked against the code by hand

**`WakeBeforeQuit` (WokenSeesQuit).** TLC's trace: (1) the thread at the
check; (2) `Check` reads `quit` false and enters the drain; (3) the
defect's destroy wakes first: `woken` set, `quit` still false; (4)
`WaitBegin`: the wait finds `woken` set under the mutex and returns at once,
with `quit` false. The thread goes round again on a spent wake. In
`rocketnet_client_transport_destroy` the `quit` store precedes
`rocketnet_platform_thread_wake` on the same thread and the wake's mutex (or, on mac, its release store paired with
the wait's acquire load) orders the two, so a wait the wake ends is followed
by a check that reads `quit` true. With `woken` sticky the defect costs
passes, not a hang; on a wake consumed by the wait it would be a lost wake
and the thread would sleep until a timeout that, on macOS, may not come.

**`NoWake` with `WaitTimeout = "none"` (DestroyCompletes).** TLC's trace:
the thread checks `quit` false and blocks in its wait; destroy stores
`quit`, its wake does nothing, and it enters the join; then stuttering: the
thread never leaves its wait, so the join never returns: a wait whose
timeout does not fire and no wake to end it. The code has the wake on linux
and mac (`rocketnet_platform_thread_wake`), and on Windows a sleep that
returns (`rocketnet_platform_sleep`).

## Not modelled

The I/O thread branch of destroy (`RNClientIo`); create's failure path (no
thread to join); `rocketnet_platform_thread_self` being NULL on a thread
`rocketnet_platform_thread_create` did not start, which never holds for this
thread because the shim sets it before the thread function runs; and the contents of the drain, which `RNClientIo` and the
receive tests cover.
