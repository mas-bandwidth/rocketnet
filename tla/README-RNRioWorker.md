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
  2. The worker posts handed phases in order and the finished fill phase,
     commits deferred sends, and retries a refused commit.
  3. It closes the socket, which completes every posted request, and drains
     both queues.
  4. Only then does it set `exited`, which `stopped` reports.
  5. Destroy joins the worker and frees the registered region.

## The abstraction

- **Receive.** `posted` is the slices posted and `retry` the slices whose
  re-post was refused. `rcq` is the receive completion queue: a datagram
  ordinal, or 0 for a cancelled slice. `Arrive` completes a posted slice.
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
- **Worker.** `wpc` steps through `WTop`, then:
  - `WTake`, under the mutex, which marks the phase posted;
  - `WPost` and `WCommit`;
  - `WRetire`, which retries a refused commit;
  - `WDequeue`, which is bounded by the fill bank's room under the rule;
  - `WPublish`, under the mutex, which publishes the fill bank and returns
    drained phases idle.

  With no progress it goes to `WArm` and then `WWait`. The stop path is
  `WStop`, then `WClose` (only once committed), then `WDrain`.
- **Main.** Main's steps are:
  - `Stage`, a fill thread;
  - `SendPackets` and `ReceivePackets`, each a successful try;
  - `TryFail`, a contended try, enabled at any time, which retains
    everything main owns;
  - `Stop`, `Destroy`, and `Poll` (stopped).

## Properties

- `ExclusiveOwnership`: every bank appears exactly once among the fill bank,
  `ready`, `exposed` and `free`.
- `ExposedStable`: the exposed banks hold exactly what main was shown.
- `ExactDelivery`: until stop, `consumed`, then the ready banks, then the
  fill bank, then the queued completions are exactly `accepted`.
- `FillPhaseOwnedByMain`: until stop, the fill phase is `fill`. No send of
  it is outstanding, the worker does not hold it, and it is not handed.
- `SlicesAccounted`: until close, every slice is posted, queued or waiting
  for a re-post.
- `NoSendDuplicated`.
- `StopSendsHanded`: at worker exit no handed phase was silently dropped.
- `StopSendsFinished`: every staged send, including the finished fill phase, has retired at exit.
- `FreedOnlyWhenDrained`: the region is freed only when the kernel owns no
  request.
- `EveryDatagramConsumed`, `EveryStagedSent` and `StopCompletes` (`stopping`
  leads to `freed`) are the liveness properties. They are checked with no
  wait timeout for an idle worker. The waits that are really timed in the
  code are while sends are outstanding, a refused commit, or a refused
  re-post; `WWait` lets those pass.

## Cases and the checks against the code

| Config | Expected | What it shows |
|---|---|---|
| `MCRNRioWorker.cfg` | pass | Safety, both directions: three datagrams and two staged sends over three phases, every invariant. |
| `MCRNRioWorkerReceiveLive.cfg` | pass | No sends, three datagrams: invariants, `EveryDatagramConsumed`, `StopCompletes`. |
| `MCRNRioWorkerSendLive.cfg` | pass | No datagrams, two sends: invariants, `EveryStagedSent`, `StopCompletes`. |
| `MCRNRioWorkerTwoBanks.cfg` | pass | Two banks, three datagrams: the fill bank fills while main holds the other bank (backpressure); invariants and receive liveness. |
| `MCRNRioWorkerBrokenSwitchBeforeIdle.cfg` | `FillPhaseOwnedByMain` | `SwitchOnIdle = FALSE` |
| `MCRNRioWorkerBrokenUnboundedDequeue.cfg` | `ExactDelivery` | `BoundedDequeue = FALSE` |
| `MCRNRioWorkerBrokenExitBeforeDrain.cfg` | `FreedOnlyWhenDrained` | `DrainBeforeExit = FALSE` |
| `MCRNRioWorkerBrokenDropHanded.cfg` | `StopSendsHanded` | `DropHanded = TRUE` keeps the old stop discard. |
| `MCRNRioWorkerBrokenNoWake.cfg` | `EveryDatagramConsumed` | `WakeOnReturn = FALSE` |

The liveness controls are split by direction so each run fits the 110 s
budget at two TLC workers.

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
    `room = ROCKETNET_RIO_RECEIVES - num_packets` and asks
    `RIODequeueCompletion` for at most that many. The overflow branch in
    `rocketnet_rio_complete_receive` is unreachable.
- **ExitBeforeDrain.**
  - In the trace, stop arrives while phase 0 is in hand. The worker posts
    and commits send 1, closes the socket and reports stopped at once. Send
    1 is still kernel owned when destroy frees the region.
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
