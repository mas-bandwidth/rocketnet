# RNUringQueueSend: one queue's send banks, its worker and the kernel

`RNUringQueueSend.tla` models layer L1 of the io_uring per-queue send. It covers:
- **The fill:** it stages into the queue's producer bank.
- **Main's send_packets:** it publishes the producer to the ready FIFO when a free
  bank can replace it; otherwise the producer is kept for the next flush.
- **The handoff's lock:** the worker takes it (`WorkerLocks`, `WorkerUnlocks`) to
  claim and retire. Main only tries it (LockRule "try"): a publish that finds it held
  keeps the producer for the next flush, and the idle check says not idle, so main
  never waits on the worker (`MainNeverWaits`). LockRule "wait" is the code before
  the fix: main blocks on the lock (`PublishWaits`, `PublishResumes`).
- **The worker:** it takes the oldest ready bank and submits its staged slots as runs.
  A run is the earliest staged slots of one destination, at most `MaxSeg` of them,
  sent as one SENDMSG with one completion. The worker reaps completions in posted
  order and retires the bank only when nothing in it is staged or in a run and none of
  its runs is pending.
- **The kernel:** it completes each run as ok, err, or eagain, with at most
  `MaxEagain` eagains in all.

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

## Results (bench-nyc-5, pinned jar 936a2620..., TLC 2.19, 2 workers, 2026-11-13)

| cfg | expected | result | states (distinct) |
|---|---|---|---|
| MCRNUringQueueSend.cfg | pass | pass | 10,820,982 (2,147,440), 54.5 s, under MCSafetyView |
| MCRNUringQueueSendLive.cfg | pass (Drains) | pass | 3,225,151 (671,550), 59.7 s |
| MCRNUringQueueSendBrokenRetireAtSubmit.cfg | NoWriteWhileKernelOwned | violated | 164,961 (58,664) |
| MCRNUringQueueSendBrokenEagainLost.cfg | Accounted | violated | 2,871 (1,531) |
| MCRNUringQueueSendBrokenEagainStuck.cfg | Drains | violated (stuttering) | 3,193,565 (661,518) |
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
- **EagainStuck.** The worker submits, the kernel answers eagain, and the reap hands
  the slots back but leaves the run pending. The resubmitted run is completed ok and
  reaped, but pending stays at 1. The bank never retires, and the behaviour stutters
  with the worker holding it.
- **SelfBrokenRetire.** Records 1-5 match: init, stage, publish, worker claim, submit.
  Record 6 retires the bank while its run is pending, which no model step does, so
  TraceReject fires with ti = 5.

## Records

The cases are in CASES.tsv and COVERAGE.tsv; RUNS.tsv holds the run in the table
above, merged through the pinned tlacheck.
