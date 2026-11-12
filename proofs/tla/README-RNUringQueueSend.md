# RNUringQueueSend: one queue's send banks, its worker and the kernel

`RNUringQueueSend.tla` models layer L1 of the io_uring per-queue send. It covers:
- **The fill:** it stages into the queue's producer bank.
- **Main's send_packets:** it publishes the producer to the ready FIFO when a free
  bank can replace it; otherwise the producer is kept for the next flush.
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

Status: the model and its trace module are written and checked; the code (L1) is not
yet built.

## The small instance (MCRNUringQueueSend.tla)

- 3 banks of 2 slots.
- 2 destinations; runs of up to 2 slots.
- 3 claims per fill, so a bank refuses one.
- 3 flushes, so every bank is reused (2 flushes for the liveness cfgs).
- 1 eagain.

## Results (captain, pinned jar 936a2620..., TLC 2.19, 2026-11-12)

| cfg | expected | result | states (distinct) |
|---|---|---|---|
| MCRNUringQueueSend.cfg | pass | pass | 9,037,998 (2,147,440), depth 40, 52 s |
| MCRNUringQueueSendLive.cfg | pass (Drains) | pass | 1,024,152 (258,610), 14 s |
| MCRNUringQueueSendBrokenRetireAtSubmit.cfg | NoWriteWhileKernelOwned | violated | 156,334 (51,131) |
| MCRNUringQueueSendBrokenEagainLost.cfg | Accounted | violated | 4,077 (2,130) |
| MCRNUringQueueSendBrokenEagainStuck.cfg | Drains | violated (stuttering) | 125,641 (41,413) |
| MCTraceRNUringQueueSendSelf.cfg | pass | pass | 9 (8): all 8 records accepted |
| MCTraceRNUringQueueSendSelfBrokenRetire.cfg | TraceConsistent | violated: ti = 5, record 6 rejected | 6 (6) |

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

These cases are not yet in CASES.tsv, RUNS.tsv or COVERAGE.tsv. Their rows are added
with the code they bind, through the pinned tlacheck, when L0 and L1 are built.
