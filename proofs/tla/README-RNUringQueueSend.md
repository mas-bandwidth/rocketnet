# RNUringQueueSend: one queue's send banks, its worker and the kernel

`RNUringQueueSend.tla` models layer L1 of the io_uring per-queue send
(`sdk/rocketnet_uring_queue_send.cpp` over `sdk/rocketnet_uring_send_queue.cpp`). It
covers:
- **The fill:** `rocketnet_uring_send_queues_start`, `rocketnet_uring_send_queues_finish`
  and `rocketnet_uring_send_queues_abort` stage or abort claims in the queue's producer
  bank (`Stage`, `StageFull`).
- **Main's send_packets:** `rocketnet_uring_queue_send_publish` publishes the producer to
  the ready FIFO and turns to a free bank (`rocketnet_uring_send_queues_turn_to`) when one
  can replace it (`Publish`); otherwise the producer is kept for the next flush
  (`PublishKept`).
- **The handoff's lock:** the worker takes it (`WorkerLocks`, `WorkerUnlocks`) in
  `rocketnet_uring_queue_send_claim` and `rocketnet_uring_queue_send_retire`. Main only
  tries it (LockRule "try") in `rocketnet_uring_queue_send_publish` and
  `rocketnet_uring_queue_send_idle`: a publish that finds it held keeps the producer for
  the next flush, and the idle check says not idle, so main never waits on the worker
  (`MainNeverWaits`). LockRule "wait" is the code before the fix: main blocks on the lock
  (`PublishWaits`, `PublishResumes`).
- **The worker:** `rocketnet_uring_queue_send_claim` takes the oldest ready bank
  (`WorkerClaim`) and `rocketnet_uring_queue_send_next` hands out its staged slots as runs
  (`WorkerSubmit`). A run is the earliest staged slots of one destination, at most
  `MaxSeg` of them, sent as one SENDMSG with one completion. The worker reaps completions
  in posted order via `rocketnet_uring_queue_send_complete` (`WorkerReap`) and retires the
  bank via `rocketnet_uring_queue_send_retire` (`WorkerRetire`, or `WorkerRetireRefused`
  when `unsubmitted != 0` or `pending != 0`) only when nothing in it is staged or in a run
  and none of its runs is pending.
- **The kernel:** it completes each run as ok, err, or eagain, with at most
  `MaxEagain` eagains in all (`KernelComplete`).

The queues share nothing but main's loop, so one queue is the machine. Related models:
- `RNUringQueueBankLayout` covers the claim arithmetic across queues.
- `RNBankFifoLayout` (the loop-initialised exchange of three, `MCRNBankFifoLayoutLoopFour.cfg`
  and kin) covers the exchange's index arrays.
- `RNSegmentRefusal` covers the refused-segment resend. Here that resend is the same
  ownership step as an eagain: the slots go back.

`TraceRNUringQueueSend.tla` binds the code to this model (README-TraceCheck.md).

Status: the model, its trace module and the code are written; the trace check binds
the code to the model (tools/tla_trace_check.py, layer queue-send).

## The small instance (MCRNUringQueueSend.tla)

- 3 banks of 2 slots.
- 2 destinations; runs of up to 2 slots.
- 3 claims per fill, so a bank refuses one.
- 3 flushes, so every bank is reused (2 flushes for the liveness cfgs).
- 1 eagain.

## Results (amd-3, pinned jar 936a2620..., tlacheck at nova-tools 0c97d595, 2 workers, 2026-11-13)

| cfg | expected | result | states (distinct) |
|---|---|---|---|
| MCRNUringQueueSend.cfg | pass | pass | 24,602,144 (4,336,670), 62.4 s, under MCSafetyView |
| MCRNUringQueueSendLive.cfg | pass (Drains) | pass | 8,039,760 (1,475,334), 70.9 s; BUDGETS.tsv declares 200 s (84.3 s on bench-xdp-8) |
| MCRNUringQueueSendBrokenRetireAtSubmit.cfg | NoWriteWhileKernelOwned | violated | 299,007 (95,202) |
| MCRNUringQueueSendBrokenEagainLost.cfg | Accounted | violated | 4,705 (2,262) |
| MCRNUringQueueSendBrokenPendingStuck.cfg | PendingMatches | violated | see RUNS.tsv |
| MCRNUringQueueSendBrokenRetireKeepsHeld.cfg | BanksPartition | violated | see RUNS.tsv |
| MCRNUringQueueSendBrokenEagainStuck.cfg | Drains | violated (stuttering) | 3,193,565 (661,518), LockRule "none" |
| MCRNUringQueueSendBrokenMainWaits.cfg | MainNeverWaits | violated | 67 (42) |
| MCTraceRNUringQueueSendSelf.cfg | pass | pass | 9 (8): all 8 records accepted |
| MCTraceRNUringQueueSendSelfBrokenRetire.cfg | TraceConsistent | violated: ti = 5, record 6 rejected | 6 (6) |

The trace check's arguments (the slot and packet id start returned, the bank a
claim took, the runs a submit made, the count a reap returned) are action
arguments, and `pids`, the server's id counter, is a variable. `pids` counts the
aborted and refused starts the other variables forget, so the control runs under
`MCSafetyView`, every variable but `pids`: no guard but the pid binding reads it
and no invariant but TypeOK names it. The view gives the control exactly the
distinct states it had before `pids` (2,147,440); without it the control times out
at 110 s. The liveness cases run without a view.

## The witnesses, as TLC printed them

Each was checked by hand against the broken rule.

- **RetireAtSubmit.**
  1. Stage into bank 0, then Publish: bank 0 is ready and bank 1 is the producer.
  2. Stage, then Publish: bank 1 is ready and bank 2 is the producer.
  3. Stage. The worker takes bank 0, submits its run, and retires bank 0 with the run
     still in the kernel. Free is now <<0>>.
  4. Publish: bank 2 is ready and bank 0 is the producer.
  5. Stage writes slot 0 of bank 0, which the kernel owns.
- **EagainLost.** Stage twice, Publish, the worker takes the bank and submits a run,
  the kernel answers eagain, and the reap marks the run done with no count.
- **PendingStuck.** Stage, Publish, the worker takes the bank and submits a run, the
  kernel answers eagain, and the reap hands the slots back without decrementing `pend`:
  `PendingMatches` fails at once.
- **RetireKeepsHeld.** Stage (aborted or completed), Publish, the worker claims the bank
  and retires it under RetireRule "dup", appending it to `free` while leaving `worker`
  set: `BanksPartition` fails at once.
- **EagainStuck.** The worker submits, the kernel answers eagain, and the reap hands
  the slots back but leaves the run pending (`PendingMatches` is omitted from this
  temporal cfg so TLC reaches the liveness check). The resubmitted run is completed ok and
  reaped, but pending stays at 1. The bank never retires, and the behaviour stutters
  with the worker holding it.
- **MainWaits.** Stage, the worker takes the handoff mutex to claim, and main's
  publish blocks on it (LockRule "wait"): MainNeverWaits fails at state 4.
- **SelfBrokenRetire.** Records 1-5 match: init, stage, publish, worker claim, submit.
  Record 6 retires the bank while its run is pending, which no model step does, so
  TraceReject fires with ti = 5.

## Records

The cases are in CASES.tsv and COVERAGE.tsv; RUNS.tsv holds the run in the table
above, merged through the pinned tlacheck.
