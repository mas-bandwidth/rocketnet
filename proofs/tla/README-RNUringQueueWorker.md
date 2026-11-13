# RNUringQueueWorker: one queue's worker thread, its ring and the stop

`RNUringQueueWorker.tla` models layer L3 of the io_uring per-queue send: the thread
that owns one queue's ring and does all of that queue's kernel-facing work. The code is
`sdk/rocketnet_uring_queue_worker.cpp`. Each branch of `rocketnet_uring_queue_worker_step`
is one action of the model, named in a comment beside it.

- **Main:** `Publish` hands a bank to the worker and `Stop` sets the quit flag. Both
  signal the queue's eventfd (`Signal`): an armed or cancelling wake read becomes
  posted, otherwise the count stays in the eventfd for the next read.
- **The kernel:**
  - It answers each run in the kernel ok (`KernelOk`), with -EAGAIN (`KernelAgain`),
    or with a segment refusal (`KernelRefuse`, at most `MaxRefuse`).
  - -EAGAIN is bounded by `MaxAgain` before the stop. After the stop it is unbounded:
    that is the socket that stays full.
  - It fires the POLLOUT poll, or ends it on a cancel (`KernelPoll`). Its weak
    fairness before the stop is the socket draining. After the stop the only poll
    left is one being cancelled.
  - It ends the multishot receive, on a cancel (`KernelRecvEnd`) or on its own
    (`KernelRecvEndsItself`, at most `MaxRecvEnd`, with no fairness).
  - It completes the wake read, or ends it on a cancel.
  - It wakes a sleeping worker only when a completion is ready.
- **The worker:**
  - It observes the quit flag, reaps one completion a step, retires a bank whose runs
    are all answered, and claims the next ready bank.
  - It submits runs, up to `K` in the kernel. It splits a run to a refused destination
    into plain singles, and drops a run the socket cannot name.
  - Before the stop it arms the poll after -EAGAIN and blocks submits until the poll
    fires. It also re-arms a receive that ended on its own, and arms the wake read.
  - Once it sees the stop, nothing blocks. It cancels the receive, the wake read and
    any poll, and retries a run -EAGAIN hands back at once. After `MaxRetry` such
    answers it abandons what waits.
  - It exits only when nothing of its own is in the kernel and no bank is held.
- **Sleep** is enabled only when no other worker step is. With WakeRule "armed" that
  includes `ArmWake`, so a publish or a stop always reaches a sleeping worker. The code
  meets this by never sleeping after a step the full submission queue had no room for:
  that pass says continue instead, and the submit that follows frees the room.

`Runs == u + k + cOk + cAgain + cRef` counts one bank's runs: waiting, in the kernel
and answered. `RunsBounded` keeps it at most `R`. The bank's slot arithmetic is not
here: `RNUringQueueSend` and `RNUringQueueBankLayout` cover it.

## The trace check

`TraceRNUringQueueWorker.tla` binds the code to this model (README-TraceCheck.md).
`tests/tla_trace_queue_worker.cpp` walks the real worker against a fake kernel. Every
completion is its own step, in any order: ok, -EAGAIN, refused, failed, a receive that
ends on its own, a poll that fires, a wake read.

- Each record is read off the fake kernel (what it holds and what it has posted) and off
  the handoff, never off the worker's own flags. A worker that miscounts what it put in
  the kernel therefore shows as a step the model does not take.
- After the recorded steps the walk stops main and drives the worker and the kernel,
  unrecorded, until the worker exits. A worker that is stuck with nothing of the
  kernel's left to come, or that runs past the step bound, fails the walk.
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

- `TypeOK`, `RunsBounded`, `SeenOnlyAfterQuit`.
- `ExitDrained`: the worker exits only with no run, poll, receive or wake read in the
  kernel, no completion unreaped, and no bank held or ready.
- `BlockedOnlyBeforeStop`: once the stop is seen nothing blocks, so no poll can hold
  the stop.
- `StopFinishes` (liveness): after a stop the worker exits.
- `Drains` (liveness): before the stop, what main publishes is submitted.

## The small instance (MCRNUringQueueWorker.tla)

- 2 banks published (`U`), each of at most 2 runs (`R`).
- 2 runs in the kernel at once (`K`).
- 1 -EAGAIN before the stop (`MaxAgain`).
- 2 -EAGAIN answers after the stop before the worker abandons (`MaxRetry`).
- 1 segment refusal (`MaxRefuse`).
- 1 receive that ends on its own (`MaxRecvEnd`).

The trace instance (`tests/tla_trace_queue_worker.cpp`) has 3 slots (`R` = 3), 2
-EAGAIN answers before the stop and 3 destinations, one of which the socket cannot name.

## Cases (CASES.tsv, group rnuringqueueworker)

| cfg | expected |
|---|---|
| MCRNUringQueueWorker.cfg | pass |
| MCRNUringQueueWorkerLive.cfg | pass (StopFinishes, Drains) |
| MCRNUringQueueWorkerBrokenExitEarly.cfg | ExitDrained violated: the witness "exit before drain" |
| MCRNUringQueueWorkerBrokenRetryUnbounded.cfg | StopFinishes violated: after the stop, -EAGAIN is retried forever on a socket that stays full |
| MCRNUringQueueWorkerBrokenWakeSkip.cfg | StopFinishes violated: the worker sleeps with no wake read armed |

Status: the model, its trace module and the code are written. The TLC results and the
witnesses, each checked by hand against the code, go here when the pinned tlacheck run
on the bench lands.
