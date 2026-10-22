# RNClientReceiveQuit

**Shutdown scope:** This model retains its earlier synchronous shutdown
abstraction and exact historical evidence. The approved request/completion API
is separately modeled in [RNClientIoShutdown](README-RNClientIoShutdown.md).
That additive ownership model is not a proved refinement of this one; existing
data-path/session checks do not establish the new shutdown implementation.

The client receive thread's quit-and-wake handshake with destroy, as
rocketnet-client-destroy-joins-receive-thread built it (the fix for a macOS
hang in destroy: the receive thread's 1 ms wait sometimes never timed out,
so the join never returned). This is the thread `create` starts when the
granted transport has no wait and wake
(`sdk/rocketnet_client_transport.cpp:973`); the I/O thread of the other
branch is `RNClientIo`'s.

The line numbers below are those of the code the model was written against
(sprint/rocketnet-proof at f3160398).

- **The thread** (`rocketnet_client_thread_function`,
  `sdk/rocketnet_client_transport.cpp:2088-2100`). `Check` is the acquire
  load of `quit` at the top of every pass (2094): true ends the loop, false
  enters the body. The body drains the socket (2096,
  `rocketnet_client_receive_packets`, which never blocks: it breaks when the
  buffer is full, 2047, or the socket is empty, 2052) and then calls
  `rocketnet_platform_thread_wait( 0.001 )` (2098): `WaitBegin`.
- **The wait and the wake, per platform** (`sdk/rocketnet_platform.cpp`).
  - linux: the wait locks `wake_mutex` (1124) and, while `woken` is false,
    waits on `wake_cond` until a CLOCK_MONOTONIC deadline (1125-1128); the
    wake sets `woken` under the same mutex and broadcasts (1136-1139). The
    check and the block are one step under the mutex, so `WaitBegin` returns
    at once (`th = "woken"`) when `woken` is set and blocks otherwise;
    `WaitWoken` is the broadcast ending the wait. A spurious wakeup
    re-checks `woken` and waits again (the `while` at 1125): `Spurious`
    changes nothing on linux.
  - mac and iOS: the wait returns at once when `woken` is set (acquire,
    2785-2786), else waits on a mach semaphore until the timeout (2796); the
    wake stores `woken` with release and then signals the semaphore
    (2802-2803). A wake between the check and the semaphore wait is kept by
    the semaphore's count (`sem`), so `WaitWoken` reads `sem`, not `woken`.
    The wait ignores `semaphore_timedwait`'s result, so an aborted wait
    returns early: `Spurious` goes back to the check.
  - windows: the wait is `rocketnet_platform_sleep` (3284-3287), which
    always returns; the wake is a no-op (3140-3144). `HasWake` is false.
  - `woken` starts false at `thread_create` (1064, 2742) and is never
    cleared, so every wait after the wake returns at once.
- **Destroy** (`sdk/rocketnet_client_transport.cpp:1035-1068`).
  `DestroyQuit` is the release store of `quit` (1041), `DestroyWake` the
  `rocketnet_platform_thread_wake` (1042), `JoinBegin` and `JoinReturns` the
  `rocketnet_platform_thread_join` (1043; `pthread_join` at 1087 and 2764,
  `WaitForSingleObject` at 3128), which returns only once the thread
  function has returned, and `DestroyFree` the `thread_destroy` that frees
  the handle (1044; 1093-1095, 2770-2771, 3134-3135) and the frees of the
  transport the thread reads that follow (1047-1068). `DestroyQuitLate`
  exists only for the defect `WakeBeforeQuit`.

`WaitTimeout` says what the model may assume of the wait's timeout: `"fair"`
(it always fires: Windows' sleep), `"unreliable"` (it may never fire, the
macOS hang, so linux and mac must finish destroy on the wake alone; no
fairness on `WaitTimesOut`), or `"none"`. A lost wake is a timeout that never
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
  contract (`sdk/rocketnet_platform.h:582-584`: store the flag, then wake,
  then join).
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
`"fair"` timeout passes (the pre-fix base design on a platform whose timeout
fires).

## The witnesses, checked against the code by hand

**`WakeBeforeQuit` (WokenSeesQuit).** TLC's trace: (1) the thread at the
check; (2) `Check` reads `quit` false and enters the drain (2094, 2096);
(3) the defect's destroy wakes first: `woken` set (1137), `quit` still false;
(4) `WaitBegin`: the wait finds `woken` set under the mutex (1125) and
returns at once, with `quit` false. The thread goes round again on a spent
wake. In the code `quit.store` (1041) precedes `thread_wake` (1042) on the
same thread and the wake's mutex (or, on mac, its release store paired with
the wait's acquire load) orders the two, so a wait the wake ends is followed
by a check that reads `quit` true. With `woken` sticky the defect costs
passes, not a hang; on a wake consumed by the wait it would be a lost wake
and the thread would sleep until a timeout that, on macOS, may not come.

**`NoWake` with `WaitTimeout = "none"` (DestroyCompletes).** TLC's trace:
the thread checks `quit` false and blocks in its wait (2094, 2098); destroy
stores `quit` (1041), its wake does nothing, and it enters the join (1043);
then stuttering: the thread never leaves its wait, so the join never
returns. This is the macOS hang the fix removed: a wait whose timeout did
not fire and no wake to end it. The code has the wake on linux
(1133-1140) and mac (2799-2804), and on Windows a sleep that returns
(3284-3287).

## Not modelled

The I/O thread branch of destroy (992-1034, `RNClientIo`); create's failure
path (974-981, no thread to join); `rocketnet_platform_thread_self` being NULL
on a thread `thread_create` did not start (1100-1105, 2776-2781), which never
holds for this thread because the shim sets it before the thread function
runs (1014, 2707); and the contents of the drain, which `RNClientIo` and the
receive tests cover.
