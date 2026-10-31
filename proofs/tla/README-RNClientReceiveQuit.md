# RNClientReceiveQuit

**Shutdown scope:** The request/completion API is modeled in
[RNClientIoShutdown](README-RNClientIoShutdown.md). That ownership model is not
a proved refinement of this one.

The legacy client receive thread's quit handshake with stop and destroy. This
is the thread `create` starts when the granted transport has no wait and wake
(custom BASE drivers, and the built-in portable client on platforms without
`ROCKETNET_PLATFORM_HAS_WAKE`); the I/O thread of the other branch is
`RNClientIo`'s.

- **The thread** (`rocketnet_client_thread_function`,
  `sdk/rocketnet_client_transport.cpp`). `Check` is the acquire load of
  `quit` at the top of every pass: true ends the loop, false enters the
  body. The body drains the socket (`rocketnet_client_receive_packets`,
  which never blocks) and then calls
  `rocketnet_platform_thread_wait( thread, 0.001 )`: `WaitBegin`.
  `WaitTimesOut` is that wait returning on its timeout.
- **The wait, per platform** (`sdk/rocketnet_platform.cpp`). linux waits on
  a condition variable until a CLOCK_MONOTONIC deadline and re-waits after a
  spurious wakeup (`Spurious` changes nothing); mac waits on a mach semaphore
  until the timeout and ignores the result, so an aborted wait returns early
  (`Spurious` goes back to the check); windows sleeps, which always returns.
- **Stop and destroy.** `StopQuit` is the release store of `quit` in the
  legacy branch of `rocketnet_client_transport_stop`. Stop makes no wake:
  nothing on this path calls `rocketnet_platform_thread_wake`, so the thread
  leaves its wait only when the wait returns by itself. `JoinBegin` and
  `JoinReturns` are `rocketnet_platform_thread_join` in
  `rocketnet_client_thread_release`, which returns only once the thread
  function has returned, and `DestroyFree` the `thread_destroy` and the frees
  of the transport that follow.

`WaitTimeout` says what the model may assume of the wait's timeout: `"fair"`
(it always fires) or `"unreliable"` (it may never fire; no fairness on
`WaitTimesOut`). The code relies on `"fair"`.

## Properties

- **`DestroyCompletes`**: once stop has run, destroy completes, under weak
  fairness of the thread's `Check` and `WaitBegin`, of destroy's steps, and of
  `WaitTimesOut` only when `WaitTimeout = "fair"` (`SpecFair`). Stop has no
  fairness: the game need never call it.
- **`TypeOK`**: every variable has its declared type.

That the join returns only after the thread has exited, and that the thread
takes no step after it, is the platform join's contract (`JoinReturns` is
enabled only at `th = "exited"`), so the module states no invariant for it.

## Cases

The model is finite as written (one thread, one stop, one destroy), so there
are no size constants. The measured records are in `proofs/tla/RUNS.tsv`
(group `rnclientreceivequit`).

| Configuration | Platform | Timeout | Expected |
|---|---|---|---|
| `MCRNClientReceiveQuit.cfg` | linux | fair | pass |
| `MCRNClientReceiveQuitMac.cfg` | mac | fair | pass |
| `MCRNClientReceiveQuitWindows.cfg` | windows | fair | pass |
| `MCRNClientReceiveQuitBrokenTimeoutNeverFires.cfg` | mac | unreliable | fails `DestroyCompletes` |

## The witness, checked against the code by hand

**`WaitTimeout = "unreliable"` (DestroyCompletes).** TLC's trace: the thread
checks `quit` false, drains and blocks in its 1 ms wait; stop stores `quit`
with release and makes no wake; destroy enters the join; then a lasso in
which the wait never returns. In the code, `rocketnet_client_transport_stop`'s
legacy branch stores `quit` and returns, and `rocketnet_client_thread_release`
calls `rocketnet_platform_thread_join`, which waits for a thread that is
inside `rocketnet_platform_thread_wait` and comes out only on the timeout. On
a platform whose timed wait always returns (Windows' sleep, and the POSIX
waits when their timeout fires) destroy completes within one wait; nothing
else bounds it.

## Not modelled

The I/O thread branch (`RNClientIo`); create's failure path (no thread to
join); and the contents of the drain, which `RNClientIo` and the receive tests
cover.
