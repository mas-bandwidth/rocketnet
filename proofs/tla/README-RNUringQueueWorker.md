# RNUringQueueWorker: one queue's worker thread, its ring and the stop

`RNUringQueueWorker.tla` models layer L3 of the io_uring per-queue send: the thread
that owns one queue's ring and does all of that queue's kernel-facing work. The code is
`sdk/rocketnet_uring_queue_worker.cpp`. Each branch of `rocketnet_uring_queue_worker_step`
is one action of the model, named in a comment beside it.

- **Main:**
  - `Publish` hands a bank to the worker (including disconnect batches while stopping).
  - `BeginStop` is the driver's `rocketnet_server_uring_begin_shutdown` over
    `rocketnet_uring_queue_worker_begin_shutdown` (ShutdownRule "begin", called when
    `ServerStopSpread.tla` enters `Stop`): the server is stopping, the worker's shutdown
    flag is set and the worker is woken. `ShutdownDrains` discharges `ServerStopSpread`'s
    premise that `send_idle` (`rocketnet_server_uring_send_idle`: no bank ready or held)
    eventually holds even on a permanently full socket, after which `ServerStopSpread`'s
    `RequestIoStop` asks `Stop` (`rocketnet_server_uring_stop`).
  - `Stop` (`rocketnet_uring_queue_worker_stop`) sets shutdown and the quit flag. An
    un-started `rocketnet_server_uring_free` asks it directly without `BeginStop`, so the
    worker's rules hold for a stop whatever waits.
  - Each signals the queue's eventfd (`Signal`): an armed or cancelling wake read
    becomes posted, otherwise the count stays in the eventfd for the next read.
- **The kernel:**
  - It answers each run in the kernel ok (`KernelOk`), full (`KernelAgain`: -EAGAIN,
    -EINTR, -ENOBUFS or -ENOMEM), or with a segment refusal (`KernelRefuse`, at most
    `MaxRefuse`).
  - Full answers are bounded by `MaxAgain` before the server is stopping. From then on
    they are unbounded: that is the socket that stays full.
  - It fires the retry timer (`KernelTimer`, a 1 ms timeout), with weak fairness: time
    passes whatever the socket does.
  - It ends the multishot receive, on a cancel (`KernelRecvEnd`) or on its own, refused
    (`KernelRecvEndsItself`: no buffer free, or a kernel that refuses the receive; at most
    `MaxRecvEnd`, with no fairness).
  - It posts a datagram for the armed receive (`KernelRecvData`, which keeps it armed),
    with no fairness and no bound: the network may send nothing, or keep sending after the
    stop.
  - It fires the receive's retry timer (`KernelRecvTimer`, 1 ms), with weak fairness.
  - It completes the wake read, or ends it on a cancel.
  - It wakes a sleeping worker only when a completion is ready.
- **The worker:**
  - It observes the quit flag, reaps one completion a step, retires a bank whose runs
    are all answered, and claims the next ready bank.
  - It submits runs, up to `K` in the kernel. It splits a run to a refused destination
    into plain singles, and drops a run the socket cannot name.
  - A full answer hands the run back and blocks submits until the retry timer fires
    (`ArmTimer`, `ReapTimer`) or, before shutdown begins, main's wake comes. Never a
    POLLOUT poll: a socket reports room while the device queue behind it refuses
    (-ENOBUFS), and a poll for it would fire again and again. This is the gso worker's
    wait.
  - Once shutdown has begun, the full answer that starts a wait counts toward
    `MaxRetry` (one a wait, not each run the burst refused). A run done or a new bank
    claimed starts the count again. Once `MaxRetry` waits have begun it abandons what
    waits, at the start of the last wait (not after its timer), so the queue turns idle
    however full the socket stays.
  - It re-arms a receive that ended on its own and arms the wake read until it sees the
    stop; then it cancels both.
  - Fairness beyond the step as a whole, each from the code: seeing the stop (`Observe`,
    the first branch of every step); the step's branches past the reaps (`PastReaps`)
    and the cancels (a pass reaps at most `ROCKETNET_URING_WORKER_CQE_BATCH` completions
    and then goes on to the rest of the step); each reap (the completion queue is a
    FIFO, reaped oldest first). So datagrams that keep coming never hold off the stop, a
    claim, a submit or another completion.
  - A refused receive (RecvRule "backoff") is re-armed at once the first time. From the
    second refused end in a row, with no datagram between, the arm waits for the
    receive's own retry timer (`ArmRecvTimer`, `ReapRecvTimer`), so a kernel that refuses
    every arm costs one arm a millisecond, never a pegged core. A datagram starts the
    count again.
  - It exits only when nothing of its own is in the kernel (its timer included) and no
    bank is held.
- **Sleep** is enabled only when no other worker step is. With WakeRule "armed" that
  includes `ArmWake`, so a publish or a stop always reaches a sleeping worker. At the
  model's grain each SQE-taking action acquires its SQE atomically; in the code, when a
  full submission queue refuses `rocketnet_uring_queue_worker_sqe`, `worker->starved` is
  set, the step returns `STEP_NONE` (a stuttering step on the model variables), and
  `rocketnet_uring_queue_worker_pass` clears `starved` and returns `CONTINUE` so
  `rocketnet_uring_queue_worker_run` submits the prepared SQEs with `wait = false`
  instead of entering `SLEEP`.

`Runs == u + k + cOk + cAgain + cRef` counts one bank's runs: waiting, in the kernel
and answered. `RunsBounded` keeps it at most `R`. The bank's slot arithmetic is not
here: `RNUringQueueSend` and `RNUringQueueBankLayout` cover it.

## The trace check

`TraceRNUringQueueWorker.tla` binds the code to this model (README-TraceCheck.md).
`tests/tla_trace_queue_worker.cpp` walks the real worker against a fake kernel. Every
completion is its own step, in any order: ok, each of the four full answers, refused,
failed, a datagram, a receive that ends on its own (-ENOBUFS or -EINVAL, more often
than a datagram comes), either retry timer firing, a wake read. A trace that never
has the receive waiting on its timer asleep (`RecvWaits`) fails the check.

- Each record reads the worker's struct (`shutdown`, `quit`, `seen`, `blocked`, `agains`,
  `recv_refused`, `recv_waiting`, and the armed and cancelled flags), the send and receive
  queues (`ready_count`, `held`, `unsubmitted`, `armed`), and the fake kernel (in-flight
  runs and posted completions), together with the walk's own history for fields the
  production struct does not keep (`stopping`, `st`, `handed`, `wakeCount`, `againsBefore`,
  `refusals`, `recvEnds`, `recvQuick`). A worker that miscounts what it put in the kernel
  therefore shows as a step the model does not take.
- Late in the walk main begins the server's stop, mostly while a wait is on, and stops
  the worker once the queue is idle; it may also stop the worker at once with no begin
  (the transport's free).
- An even seed's walk is patient: it begins the stop halfway and never stops the
  worker at once in the last third, so its records reach the shutdown-only tail (the
  bound reached with the bank held and no stop yet, then the abandon). It still
  stops the worker shortly before its end, so the plant shows in its records. A
  trace set without that state (`BoundBeforeQuit`) fails the check.
- After the recorded steps, a server that is stopping and has not stopped the worker is
  driven, unrecorded and with no publish, until the queue is idle (ShutdownDrains).
  Then the walk stops main and drives the worker and the kernel until the worker exits.
  A worker that is stuck with nothing of the kernel's left to come, or that runs past
  the step bound, fails the walk.
- The plant (`ROCKETNET_URING_QUEUE_WORKER_PLANT`) exits at the first step after the stop
  is seen: the code's form of ExitRule "early". The handoff's own plant acts only
  in the queue-send walk, so it cannot stand in for this one.
- Two parts of the code are not traced:
  - `rocketnet_uring_queue_worker_pass` and `run`: the starved-continue rule, the CQE
    batch (`step` with reap false) and the submit. The unit suite covers them in
    `tests/test_unit_uring_queue_worker.cpp`.
  - The segmentation fallback past the refusal threshold, which needs more refusals
    than this instance has. The unit suite covers it too.

## Properties

- `TypeOK`, `RunsBounded`, `SeenOnlyAfterQuit` (quit comes with shutdown).
- `ExitDrained`: the worker exits only with no run, timer, receive or wake read in the
  kernel, no completion unreaped, and no bank held or ready.
- `BlockedHasTimer`: a blocked worker never sleeps without its timer in the kernel, so
  every wait ends.
- `ShutdownDrains` (liveness): once the server is stopping, the queue turns idle (or
  the worker has exited), however full the socket stays.
- `StopFinishes` (liveness): after a stop the worker exits, datagrams still coming.
- `RecvNoSpin`: never two arms after a refused end with neither the receive's timer nor
  a datagram between (`recvQuick`, history the trace walk keeps beside the code).
- `RecvWaitHasTimer`: a receive waiting to be re-armed never sleeps without its timer in
  the kernel.
- `RecvRearms` (liveness, `MCRNUringQueueWorkerLiveRecv.cfg`): a refused receive is
  armed again before the stop.
- `Drains` (liveness): before the stop, what main publishes is submitted.

## The small instance (MCRNUringQueueWorker.tla)

- 2 banks published (`U`), each of at most 2 runs (`R`).
- 2 runs in the kernel at once (`K`).
- 1 full answer before the server is stopping (`MaxAgain`).
- 2 waits begun after shutdown, with no run done, and the worker abandons (`MaxRetry`).
- 1 segment refusal (`MaxRefuse`).
- 2 receives that end on their own (`MaxRecvEnd`; none in the liveness control and the
  three temporal witnesses, whose datagrams still come, so that they fit the budget; 3 in
  `MCRNUringQueueWorkerLiveRecv.cfg`, which has 1 bank and no full answers or refusals so
  that 3 fits too).

The trace instance (`tests/tla_trace_queue_worker.cpp`) has 3 slots (`R` = 3), 2
full answers before the server is stopping and 3 destinations, one of which the socket
cannot name.

## Cases (CASES.tsv, group rnuringqueueworker)

Results (amd-3, pinned jar 936a2620..., tlacheck at nova-tools 0c97d595, 2 workers,
2026-11-13):

| cfg | expected | result | states (distinct) |
|---|---|---|---|
| MCRNUringQueueWorker.cfg | pass | pass | 13,109,144 (2,197,174), 18.3 s |
| MCRNUringQueueWorkerLive.cfg | pass (ShutdownDrains, StopFinishes, Drains) | pass | 1,125,427 (199,180), 42.5 s |
| MCRNUringQueueWorkerLiveRecv.cfg | pass (StopFinishes, RecvRearms) | pass | 1,179,230 (215,238), 30.5 s |
| MCRNUringQueueWorkerBrokenExitEarly.cfg | ExitDrained violated: the witness "exit before drain" | violated | 168 (87) |
| MCRNUringQueueWorkerBrokenRetryUnbounded.cfg | StopFinishes violated: a socket that stays full is retried forever | violated (temporal) | 1,053,469 (190,580) |
| MCRNUringQueueWorkerBrokenWakeSkip.cfg | StopFinishes violated: the worker sleeps with no wake read armed | violated (temporal) | 1,155,568 (206,360) |
| MCRNUringQueueWorkerBrokenShutdownNone.cfg | ShutdownDrains violated: the witness "stop never completes", no begin_shutdown (the transport before the fix) | violated (temporal) | 1,228,476 (214,426) |
| MCRNUringQueueWorkerBrokenRecvSpin.cfg | RecvNoSpin violated: a refused receive re-armed at once every time (RecvRule "immediate", the code before the fix) | violated | 6,282 (1,864) |

Each witness was checked by hand against its broken rule. RetryUnbounded: after
the stop, submit, the kernel answers full, the timer fires, submit again, for
ever. WakeSkip: the worker sleeps with no wake read, main stops, and nothing
wakes it. ShutdownNone: with no begin, the held bank on a full socket is
retried for ever and never abandoned. RecvSpin: two refused ends, each armed
again at once with no timer and no datagram between.

Status: the model, its trace module and the code are written; the trace check binds
the code to the model (tools/tla_trace_check.py, layer queue-worker). RUNS.tsv holds
the run above.
