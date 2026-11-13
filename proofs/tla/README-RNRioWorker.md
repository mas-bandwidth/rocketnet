# RNRioWorker: the RIO server transport's I/O worker

`RNRioWorker.tla` models the Windows Registered I/O server transport in
`sdk/rocketnet_server_rio_windows.cpp` with its background I/O worker. It
supersedes `RNRioDriver.tla` for the threading and the handoffs. That module
keeps its claims about slots and slices, and its README says which of its
paths changed.

RIO request and completion queues are unsynchronized. Sends and receives
share the socket's request queue (the `RIOCreateRequestQueue` remarks). So
one worker owns the request queue and both completion queues, and makes
every RIO call after create.

- **Main side.** Main makes no RIO call and never waits. It exchanges bank
  indices and send-phase states with the worker under one try of a short
  mutex.
- **Receive.** The worker owns:
  - its fill bank;
  - the dequeue of the receive completion queue;
  - the re-post of each slice.

  It publishes a non-empty fill bank onto a ready FIFO in exchange for a
  free bank. `rocketnet_server_rio_receive_packets` returns the banks it
  exposed last call and exposes the ready banks that fit the process view.
  The views stay valid until the next call. With every bank full, the
  worker dequeues nothing: completions wait in the queue and their slices
  stay unposted. That is backpressure; nothing is dropped.
- **Send.**
  - There are three send phases, as every other transport's send handoff
    has three banks. The fill threads stage into the open phase, as before.
  - `rocketnet_server_rio_send_packets` hands the fill phase to the worker
    and opens the next phase, only when the worker has returned that phase
    idle, meaning every send of it has been retired. Otherwise the fill
    phase stays open, the fill threads keep appending, and a later flush
    hands it. A busy worker delays sends by a flush; it never makes the fill
    threads refuse, except when the open phase is full.
  - The worker posts with `RIO_MSG_DEFER`, commits, retires send
    completions and returns a drained phase idle.
- **Idle wait.** When idle, the worker arms `RIONotify`:
  - on the receive queue, only if its fill bank has room;
  - on the send queue, if sends are outstanding.

  It then waits on that event and on main's wake. A `RIONotify` on a
  non-empty queue sets the event at once, per the documented rule.
- **Stop.**
  1. Stop sets `stopping` and signals the wake.
  2. Each stop pass (`rocketnet_rio_stop_pass`) posts handed phases in
     order and the finished fill phase, and commits deferred sends.
  3. It retires send completions and keeps the socket open while any
     committed send is in flight. `closesocket` cancels a send still in
     flight, which would retire it as `SEND_FAILED` and lose the shutdown
     disconnect burst.
  4. It closes the socket, which cancels every posted receive, and dequeues
     each queue in calls of at most `ROCKETNET_RIO_DEQUEUE_BATCH` (the
     model's `DrainBatch`), calling again only after a full batch. The
     cancelled receives reach the completion queue asynchronously after
     `closesocket` returns, so a pass may find none, some or all of them.
  5. A refused commit, a refused `closesocket`, a send still in flight or a
     cancellation not yet dequeued fails the pass, and the pass runs again. The worker retries without
     bound. On create's failure path, before the worker exists, the pass
     runs inline (`InlineStop`). A failed pass that dequeued a receive
     completion made progress and is not counted; after
     `ROCKETNET_RIO_DRAIN_WAITS + 1` failed passes that dequeued none, the
     transport leaks instead of freeing a region the kernel may still
     write (`CountStalledOnly`). That can follow a `closesocket` that
     succeeded, when the cancellations arrive after the last wait.
  6. Only after a pass succeeds does the worker set `exited`, which
     `stopped` reports. Destroy joins the worker and frees the registered
     region.

## The abstraction

- **Receive.** `posted` is the slices posted and `retry` the slices whose
  re-post was refused. `retired` counts completed slices that stop deliberately
  leaves unposted. `rcq` is the receive completion queue: a datagram
  ordinal, or 0 for a cancelled slice. `Arrive` completes a posted slice.
  After a successful close, `posted` is the receives whose cancellation
  has not arrived yet, and the environment action `CancelArrives` moves
  one at a time to `rcq` as a 0.
  At the model's action boundaries, the source's `posted_receives` corresponds
  to `posted + Len(rcq)`: the source decrements that count at dequeue, while
  the model separates posted requests from queued completions.
  `ArriveDropped` stands for a kernel drop when no slice is posted; that is
  an OS count, outside the claim. `accepted` is every datagram that
  completed a slice, in order.
- **Banks.** `data[b]` holds the datagrams in bank `b`. `fill` is the
  worker's bank, and `free`, `ready` and `exposed` are FIFOs.
  - `view` is what main exposed, and `consumed` is everything main
    processed.
  - The code's free list is a stack. Which free bank the worker takes does
    not matter to any property.
- **Send.**
  - `staged[p]` holds the packets staged into phase `p`, for three phases.
  - `state[p]` is one of `fill`, `ready`, `posted` or `idle`.
  - `sready` is the queue of handed phases, oldest first.
  - `pend` is posted but uncommitted.
  - `infl` is committed and kernel owned.
  - `scq` is the send completion queue, and `done` the retired sends.
  - `cancelled` is the sends that completed after the close, which the
    kernel cancelled: `SendComplete` on a closed socket completes a send
    failed.
- **Worker.** `wpc` steps through `WTop`, then:
  - `WTake`, under the mutex, which marks the phase posted;
  - `WPost` and `WCommit`; `WPost` lets the kernel refuse any of the
    phase's posts (`RIOSendEx` returns false in `rocketnet_rio_post_phase`):
    a refused packet joins `failed`, counted `SEND_FAILED`, its slot free
    with no completion. The rule switch `CountRefusedPost` (an operator)
    is FALSE in `MCRNRioWorkerBrokenUncountedPost.cfg`;
  - `WRetire`, which retries a refused commit;
  - `WDequeue`, bounded by the fill bank's room under the rule, which
    summarizes the retry loop and receive completions, including a stop
    interleaving as described below;
  - `WPublish`, under the mutex, which publishes the fill bank and returns
    drained phases idle.

  With no progress it goes to `WArm` and then `WWait`.
- **Stop pass.** `WStop` takes the handed phases and the finished fill
  phase through `WPost` and `WCommit`. `WClose` is
  `rocketnet_rio_close_socket`: it commits deferred sends (a refusal fails
  the pass), fails the pass while a committed send is in flight under the
  rule (after retiring the send completions available), and closes the
  socket, which may be refused and fail the pass. `WDrainSend` and then
  `WDrainReceive` are one `RIODequeueCompletion` call each into the
  fixed `results[ROCKETNET_RIO_DEQUEUE_BATCH]` array: at most `DrainBatch`
  completions, and another call only when the call came back full, as the
  code's `count < ROCKETNET_RIO_DEQUEUE_BATCH` break does, so a queue of
  exactly one batch takes a second call that returns 0. After the last call
  the pass exits only when no send is outstanding and no receive is posted
  or queued; otherwise it fails. A failed pass returns to `WStop`. The
  worker retries it without bound. With `InlineStop`, `stopwaits` is the
  create drain's `stalled` count: the failed passes that dequeued no receive
  completion, so `posted_receives` did not fall during them
  (`CountStalledOnly`; `WDrainReceive` remembers a full batch in its place
  `"drainrp"`). A pass may start while at most `StopDrainWaits` have
  stalled, and after `StopDrainWaits + 1` `WLeak` leaks the transport.
  `stoprefused` records whether the last stop commit or `closesocket` was
  refused. The ghost `countedbusy` records that a counted failed pass had
  dequeued a receive completion, which the code's stalled count never does.
  The close itself queues no cancellation: `CancelArrives` delivers them
  later, so after a close the drain may stall.
- **Receive stop interleaving.** The source checks `stopping` once before
  its whole retry loop, then separately for each completion after copying
  the datagram. `WDequeue` remains an aggregate action. `stopDuring` summarizes
  a `Stop` inside that action and sets the same stopping flag and wake.
  `attemptRetry` selects whether the initial check permits the whole old
  retry list; `repost` is the completion prefix whose checks precede stop.
  The remaining completion suffix enters `retired`. When stop was already
  set on entry, no retry is attempted and every dequeued completion retires.
  This count abstraction exposes the resulting ownership states without
  claiming instruction-level interleavings inside the loop or a separate
  local completion-array model. It adds no fairness condition.
  A retry already in the list stays unposted and accounted through stop;
  it is not kernel-owned and does not block close or free. `WDrainReceive`
  moves queued receive completions into `retired`, including close
  cancellations.
- **Main.** Main's steps are:
  - `Stage`, a fill thread;
  - `SendPackets` and `ReceivePackets`, each a successful try;
  - `TryFail`, a contended try, enabled at any time, which retains
    everything main owns;
  - `Stop`, `Destroy`, and `Poll` (stopped).

Left out: a refused address write at `rocketnet_server_rio_start_packet`
and a refused `rocketnet_server_rio_finish_packet` also lose a packet
counted `SEND_FAILED`, before it is staged; the model's `Stage` is only a
packet that became postable, so those losses are outside `sent`.

## Properties

- `ExclusiveOwnership`: every bank appears exactly once among the fill bank,
  `ready`, `exposed` and `free`.
- `ExposedStable`: the exposed banks hold exactly what main was shown.
- `ExactDelivery`: until stop, `consumed`, then the ready banks, then the
  fill bank, then the queued completions are exactly `accepted`.
- `FillPhaseOwnedByMain`: until stop, the fill phase is `fill`. No send of
  it is outstanding, the worker does not hold it, and it is not handed.
- `SlicesAccounted`: every slice is posted, queued, waiting for a re-post
  or retired, including after close and drain. No slice is retired while
  running, so a refused re-post cannot shrink the running receive depth.
- `NoSendDuplicated`: no send retired twice, and none both retired and
  refused.
- `StopSendsHanded`: at worker exit no handed phase was silently dropped.
- `StopSendsFinished`: every staged send, including the finished fill
  phase, has retired or was counted refused at its post at exit, and the
  close cancelled none of them. A refused `RIOSendEx` loses its packet,
  counted `SEND_FAILED` (`failed`); `MCRNRioWorkerReachRefusedPost.cfg`
  reaches an exit with one (`NotReachedRefusedPostAtExit`, expected to
  fail).
- `FreedOnlyWhenDrained`: the region is freed only when the kernel owns no
  request and every completion has been dequeued.
- `LeakOnlyAfterStalls`: create's bounded inline drain leaks only after
  `StopDrainWaits + 1` stalled passes, none of which dequeued a receive
  completion (no counted pass has `countedbusy`), and only while a receive
  is posted, a completion queued or a send kernel owned. This is what
  `rocketnet_rio_create_failed_drain` guarantees. It does not promise a
  drain after a successful `closesocket`: the cancellations may arrive after
  the last stalled pass, and `MCRNRioWorkerReachLateCancelLeak.cfg` reaches
  that leak (`NotReachedLateCancelLeak`, expected to fail). The leak is the
  safe direction: memory the kernel may still write is never freed.
- `NoGarbageBanked`: no datagram the prefilter refuses (`Garbage`) is ever
  in a bank; the worker counts it in its lane's drop tally and re-posts the
  slice. `ExactDelivery` and `EveryDatagramConsumed` range over the
  datagrams that pass.
- `EveryDatagramConsumed` and `EveryStagedSent` are the running liveness
  properties; a staged send is sent, or its post refused and counted.
  They are checked with no wait timeout for an idle worker.
  The waits that are really timed in the code are while sends are
  outstanding, a refused commit, or a refused re-post; `WWait` lets those
  pass. While running, a refused commit is retried until it succeeds
  (strong fairness on `WCommit` and `WRetire` before stop).
- No fairness covers the stop pass's commit or `closesocket`: either may
  be refused forever.
  - `StopCompletes`, on create's inline path: `stopping` leads to `freed`,
    or to `wpc = "leaked"` once the bounded drain gives up.
  - `StopCompletesUnlessRefused`, on the worker's path, which retries
    without bound as the code does: unless the kernel refuses every later
    stop commit or `closesocket`, `stopping` leads to `freed`.

## Cases and the checks against the code

| Config | Expected | Distinct states | Seconds | What it shows |
|---|---|---|---|---|
| `MCRNRioWorker.cfg` | pass | 2616318 | 37 | Safety, both directions: three datagrams and two staged sends over three phases, every invariant. |
| `MCRNRioWorkerThreeSends.cfg` | pass | 69750 | 2 | The exact three-send constants of SwitchBeforeIdle with its guard restored and every safety invariant; the rotation can return to an occupied phase. |
| `MCRNRioWorkerReceiveLive.cfg` | pass | 20685 | 25 | No sends, three datagrams: invariants, `EveryDatagramConsumed`, `StopCompletes`. |
| `MCRNRioWorkerSendLive.cfg` | pass | 9597 | 8 | No datagrams, two sends: invariants, `EveryStagedSent`, `StopCompletes`. |
| `MCRNRioWorkerTwoBanks.cfg` | pass | 20539 | 19 | Two banks, three datagrams: the fill bank fills while main holds the other bank (backpressure); invariants and receive liveness. |
| `MCRNRioWorkerBrokenSwitchBeforeIdle.cfg` | `FillPhaseOwnedByMain` | 609 | 0 | `SwitchOnIdle = FALSE` |
| `MCRNRioWorkerBrokenUnboundedDequeue.cfg` | `ExactDelivery` | 7226 | 1 | `BoundedDequeue = FALSE` |
| `MCRNRioWorkerBrokenExitBeforeDrain.cfg` | `FreedOnlyWhenDrained` | 189 | 0 | `DrainBeforeExit = FALSE` |
| `MCRNRioWorkerBrokenDropHanded.cfg` | `StopSendsHanded` | 413 | 1 | `DropHanded = TRUE` keeps the old stop discard. |
| `MCRNRioWorkerBrokenNoWake.cfg` | `EveryDatagramConsumed` | 16661 | 4 | `WakeOnReturn = FALSE` |
| `MCRNRioWorkerBrokenCloseCancelsSends.cfg` | `StopSendsFinished` | 212 | 0 | `DrainSendsBeforeClose = FALSE` keeps the stop pass that closes with sends in flight. |
| `MCRNRioWorkerInlineStop.cfg` | pass | 468 | 1 | Create's failure path: five posted slices, one datagram, `DrainBatch = 2`, `StopDrainWaits = 1`, every `closesocket` refusable, cancellations late; invariants, `LeakOnlyAfterStalls`, `StopCompletes`. |
| `MCRNRioWorkerBrokenUnboundedRetry.cfg` | `StopCompletes` | 204 | 1 | `BoundedStopDrain = FALSE`: the old create loop retries forever. |
| `MCRNRioWorkerBrokenOneBatchPerPass.cfg` | `LeakOnlyAfterStalls` | 119 | 0 | `DrainLoop = FALSE` and `CountStalledOnly = FALSE`: one batch per queue per pass, and every failed pass counted. Passes that make progress use up the bound. |
| `MCRNRioWorkerInlineStopBatchPerPass.cfg` | pass | 420 | 1 | That witness with `CountStalledOnly = TRUE`: the passes that dequeue a batch are not counted, the drain finishes, and it leaks only after stalled passes; invariants, `LeakOnlyAfterStalls`, `StopCompletes`. |
| `MCRNRioWorkerReachLateCancelLeak.cfg` | `NotReachedLateCancelLeak` | 175 | 1 | Reachability: the constants of `InlineStop`; the drain leaks after a `closesocket` that succeeded, its cancellations not yet arrived. |
| `MCRNRioWorkerReachRefusedPost.cfg` | `NotReachedRefusedPostAtExit` | 213 | 1 | Reachability: one send; the stop burst's post is refused, counted, and the worker exits. |
| `MCRNRioWorkerBrokenUncountedPost.cfg` | `StopSendsFinished` | 240 | 0 | `CountRefusedPost <- FALSE`: a refused post is lost uncounted. |
| `MCRNRioWorkerGarbage.cfg` | pass | 18830 | 29 | Two banks, four datagrams, two of them garbage (`Garbage = {2, 3}`): the prefilter refuses them in the worker before banking; invariants with `NoGarbageBanked`, receive liveness over the datagrams that pass. |
| `MCRNRioWorkerBrokenBankGarbage.cfg` | `NoGarbageBanked` | 151 | 0 | `PrefilterInWorker <- FALSE`: the old shape, filtering on main after the bank; a garbage datagram is banked and takes room. Checked against `rocketnet_rio_complete_receive`, which calls `rocketnet_receive_prefilter` before it copies into the bank. |
| `MCRNRioWorkerBrokenTakeBeforeRetire.cfg` | `DrainedReturnedAtPost` | 3568 | 1 | `RetireFirst <- FALSE`: the pass takes the ready phase before it retires completions, so it posts while a phase whose sends all completed is still held as posted; main cannot reuse it and refuses starts. Checked against `rocketnet_rio_worker_pass`, which commits, dequeues the send completions and returns the drained phases to idle before it takes. |

The liveness controls are split by direction so each run fits the 110 s
budget at two TLC workers. The distinct states and seconds are the
2026-11-11 run on amd-2 (one shard per case, two workers, four cases at a
time), after `CancelArrives` and the refused send post; the longest case
was `MCRNRioWorker.cfg` at 35.9 s, up from 8.3 s with the refusal subsets.

The stalled-pass rule (`CountStalledOnly`) was added after that run, and all
fifteen cases were run again on a Linux bench with the same pinned runner
and jar; `RUNS.tsv` holds those records.

On 2026-11-10 the bounded stop-drain model ran all fourteen required cases
on amd-2 (native Linux) with the pinned `tlacheck` built from the commit
`.github/workflows/tlc.yml` names and the pinned TLC jar, one shard per case.
All six healthy cases passed; the eight witnesses produced exactly the
properties listed above. The longest case was 26.8 s, within the
110 s budget. `RUNS.tsv` records the inputs and results of that run. The
existing configurations keep their constants and expected properties and
add `DrainBatch = 2`, `StopDrainWaits = 2`, `InlineStop = FALSE`,
`BoundedStopDrain = TRUE` and `DrainLoop = TRUE`; the two receive and send
liveness controls check `StopCompletesUnlessRefused`. These bounded
count-model checks do not establish an instruction-level refinement of the
worker or qualify native Windows timing.

Each reversed witness was read against the code by hand (2026-10-22):

- **SwitchBeforeIdle** (three staged sends, so the rotation returns to
  phase 0).
  - In the trace, phases 0 and 1 are handed and phase 2 is staged. The
    worker takes phase 0 (`posted`). The flush then hands phase 2 and
    reopens phase 0 to the fill threads while the worker is about to post
    its frames.
  - In the code, `rocketnet_server_rio_send_packets` hands and opens `next`
    only if `phase_state[next] == ROCKETNET_RIO_PHASE_IDLE`. That state is
    set only in `rocketnet_rio_worker_pass`, under the mutex, once
    `outstanding[p] == 0`.
- **UnboundedDequeue.**
  - In the trace, there are two banks. Main holds none yet, bank 1 is
    ready, and the fill bank holds datagrams 2 and 3. An unbounded dequeue
    takes datagram 4 with no room and drops it. This is the old
    caller-driven `RECEIVE_DROPPED_OVERFLOW` path.
  - In the code, `rocketnet_rio_dequeue_receive` starts from
    `room = rio->receives - num_packets` and asks
    `RIODequeueCompletion` for at most that many. The overflow branch in
    `rocketnet_rio_complete_receive` is unreachable.
- **ExitBeforeDrain.**
  - In the trace, stop arrives with no send staged. The worker closes the
    socket, which cancels both posted slices, and reports stopped at once,
    before either cancellation has reached the receive queue. Destroy
    frees the region with both receives still kernel owned (`posted = 2`,
    re-checked 2026-11-11 after `CancelArrives`).
  - In the code, `rocketnet_rio_stop_pass` returns true only when both
    `outstanding` counts and `posted_receives` are zero. The worker sets
    `exited` only after that, and `rocketnet_server_rio_stopped` returns
    `exited`.
- **NoWake.**
  - In the trace, there are two banks. Main exposes bank 1. The worker's
    fill bank holds datagram 2, and its publish is refused (`free` empty).
    It arms the receive notify and waits. Main returns bank 1 without a
    signal. No datagram arrives, so datagram 2 is never published.
  - In the code, `rocketnet_server_rio_receive_packets` computes
    `wake = free_count == 0 && exposed_count > 0` under the mutex and calls
    `SetEvent( rio->wake )` after the release.

- **CloseCancelsSends** (one send).
  - In the trace, one packet is staged into the fill phase and stop
    arrives. The stop pass posts and commits it, then closes the socket
    while it is in flight. It completes cancelled, the drain retires it, and
    the worker exits with the burst lost.
  - In the code, `rocketnet_rio_close_socket` returns false, leaving the
    socket open, while `rocketnet_rio_outstanding_sends` is above zero after
    `rocketnet_rio_dequeue_send` retires the completions available. The
    worker's stop loop runs the pass again after a 1 ms wait. A cancelled
    completion has a nonzero status, which `rocketnet_rio_retire_send`
    counts as `SEND_FAILED`.

- **UnboundedRetry** (create's failure path, checked 2026-11-10).
  - In the trace, stop is set with five slices posted and a datagram
    arrives. The pass is refused at `closesocket` and returns false; the
    next pass is refused again, forever. Nothing stops and nothing leaks.
  - In the code before rn-rio-stop-drain-bound, the failure path ran
    `while ( !rocketnet_server_rio_stopped( rio ) ) rocketnet_platform_sleep( 0.001 );`,
    and `rocketnet_rio_close_socket` returns false when
    `rocketnet_rio_close_call` fails. `rocketnet_rio_create_failed_drain`
    gives up after `ROCKETNET_RIO_DRAIN_WAITS + 1` passes that dequeued no
    receive completion, then create logs and returns NULL without
    `rocketnet_server_rio_destroy`.
- **OneBatchPerPass** (checked 2026-11-10; re-checked 2026-11-11 after
  `CancelArrives`).
  - In the trace, one datagram arrives (posted 4, the datagram queued). The
    first pass is refused at `closesocket` (counted, stalled). The second
    closes the socket; no cancellation has arrived yet, and it dequeues the
    datagram's completion and fails with four receives still posted. Under
    the old count both passes are counted although the second lowered
    `posted_receives`, so `countedbusy` is set, the bound of two is spent,
    and the transport leaks.
  - In the code before rn-rio-stop-drain-bound, each queue had "exactly one
    available batch" per pass. The landed pass loops while a call returns
    a full `ROCKETNET_RIO_DEQUEUE_BATCH`, so the second pass drains two, two
    and one and stops. The drain as first bounded counted every failed
    pass; `rocketnet_rio_create_failed_drain` now compares `posted_receives`
    before and after each pass and counts only a pass that did not lower it,
    so even a pass that takes one batch is free
    (`MCRNRioWorkerInlineStopBatchPerPass.cfg`). In the code the second
    pass reads `posted` = 5 before it and 4 after, so `stalled` stays 1.
- **ReachLateCancelLeak** (checked 2026-11-11).
  - In the trace, the first pass is refused at `closesocket` (stalled 1).
    The second closes the socket, finds both queues empty because none of
    the five cancellations has arrived, and fails (stalled 2). The bound
    is spent and the drain leaks with `stoprefused` FALSE.
  - In the code, `rocketnet_rio_create_failed_drain` reads
    `posted_receives` (5), runs `rocketnet_rio_stop_pass`, whose
    `rocketnet_rio_close_socket` succeeds and sets `rio->socket =
    INVALID_SOCKET`; the receive dequeue returns 0 and the pass returns
    false with `posted_receives` still 5, so `++stalled`. Each further
    pass waits in `rocketnet_rio_create_drain_wait` at most
    `ROCKETNET_RIO_DRAIN_NOTIFY_MILLISECONDS`; after
    `ROCKETNET_RIO_DRAIN_WAITS + 1` such passes it returns false and create
    leaks the transport. The model's `StopDrainWaits = 1` is the code's 10.

- **UncountedPost** (checked 2026-11-11).
  - In the trace, one packet is staged and stop arrives. The stop pass
    takes the fill phase; `WPost` has the kernel refuse its one post, and
    with `CountRefusedPost` FALSE it is recorded nowhere. Commit, close,
    the slice's cancellation arrives, both drains, and the worker exits
    with `sent = 1` and the packet neither in `done` nor in `failed`.
  - In the code, `rocketnet_rio_post_phase` takes the `else` branch of the
    `RIOSendEx` call (also reached through
    `ROCKETNET_RIO_TEST_FAULT_SEND_POST`): it counts
    `ROCKETNET_COUNTER_RIO_SEND_FAILED` and sets `slot_bytes[index] = 0`;
    `outstanding[phase]` grows only by the accepted posts, so the pass
    does not wait for a completion that never comes.
- **ReachRefusedPost** (checked 2026-11-11): the same steps with the rule
  on; the refused post is in `failed` at exit, which the code counts as
  above.

The worker's stop pass has no deadline: like the IOCP transport, which
keeps its socket open until `pending` is zero, it waits for the kernel to
complete every committed UDP send, and it retries a refused commit or
`closesocket`. Only create's inline drain is bounded.

The unit case `test_rio_transport_worker_handoff`
(`test_unit_rio_overflow_seam.cpp`, through
`rocketnet_server_rio_test_worker_handoff`) drives the real worker pass and
main calls on one thread against a fake kernel, with no socket, no worker
thread and no clock. It covers these cases:

- backpressure with every bank full;
- a failed main try that retains its exposed banks;
- the wake on a return to an empty free list;
- the send-phase handoff that hands and opens the next phase only when it
  is idle, and otherwise keeps the fill phase open with nothing refused.

The unit case `test_rio_transport_stop_drains_sends` (the same file, through
`rocketnet_server_rio_test_stop_drains_sends`) steps the real stop pass
against a fake kernel whose `closesocket` cancels every send in flight. The
stop pass keeps the socket open across two passes while the burst is in
flight, and closes it only after the burst completes. Every packet counts
`PACKETS_SENT` and none `SEND_FAILED`.
