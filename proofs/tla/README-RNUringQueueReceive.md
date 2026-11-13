# RNUringQueueReceive: one queue's receive, its worker and main's drain

The io_uring server receives per queue. Each queue's worker thread owns its ring, and
`sdk/rocketnet_uring_receive_queue.cpp` is what the worker runs on it:
- **The arm:** a multishot RECVMSG on a provided buffer ring.
- **Each completion:** the transport's checks, then the stateless prefilter, then the
  bank. Each datagram is either banked or counted by name.
- **The buffer:** it goes back to the ring once every datagram in it is banked or
  counted.
- **The rearm:** when the multishot ends (a completion without
  `IORING_CQE_F_MORE`), the queue needs an arm again.
- **Main:** it only drains the bank. It adds no thread and no mutex: the handoff is
  the tail, head and exposed indexes, through atomics.

The queues share nothing, so one queue is the machine. Two models cover it:
- `RNUringQueueReceive.tla` is the design. It has the network, the kernel's socket
  queue and multishot, the worker, main and the stop. The worker's slot write and its
  publish of tail are separate steps, so an order that publishes first can be shown
  wrong.
- `RNUringQueueReceiveLayout.tla` is the code's layout, one step per call the code
  makes. It covers:
  - the bank's uint64 indexes and slot `tail % S`;
  - the worker's cached head, loaded again only when the cache says the bank is full;
  - the drain's release of the drain before;
  - the provided ring's 16-bit counters and the mask `c & (B - 1)`;
  - the GRO split.

`TraceRNUringQueueReceiveLayout.tla` binds the code to the layout model
(README-TraceCheck.md). `tests/tla_trace_uring_receive.cpp` walks the real module with
fake completions and writes one record per call (tla_step): the call, what it gave
back, the action its own result names, and every variable after it. The fake kernel's
take is KernelReceive(bid) with the buffer test_take returned; a completion is
Complete(n) with the datagrams cqe returned it banked; a drain is Drain(n) with drain's
count; an arm is Arm when arm returned true. Its plant,
`ROCKETNET_URING_RECEIVE_QUEUE_PLANT`, is the ReleaseAtTake witness in the code: the
drain stores head as the end of what it takes. The plant build (`ROCKETNET_TLA_PLANT`)
includes the module's .cpp with that define set. The walk reports the plant where it
shows (PLANT-AT): a drain that took datagrams and moved head past the drain before's
end, which the clean code never does. The plant trace must be rejected at the first of
those records.

## The claims

The abstract model (MCRNUringQueueReceive.tla) checks:
- `NoOverwrite`: the worker never writes a slot main has not finished.
- `NoTornRead`: no slot main took is written before main is done with it.
- `InOrder`: main reads exactly the datagrams banked, in order.
- `Accounted`: every datagram the socket delivered is in a completion, banked,
  refused by name, or counted BANK_FULL.
- `BuffersKept`: the provided buffers are conserved.
- `SocketCounted`: the socket's own drops (the SO_RXQ_OVFL stamp) are counted as
  DROPPED_SOCKET, exactly to the last stamp seen.
- `ArmedAgrees`.
- The temporal property `Rearms`, under LiveSpec: an ended multishot is armed again
  while the queue runs.

A full bank drops by name and never blocks: no step waits on main.

The layout model (MCRNUringQueueReceiveLayout.tla) checks `InOrder`, `NoTornRead`,
`FullOnlyWhenFull`, `NoDoubleTake`, `Accounted` and `BuffersKept`.

## The small instances

- **Abstract:** 2 slots, 2 buffers, socket queue of 1, 4 arrivals, drains of up
  to 2.
- **Layout:** 3 slots, 4 buffers, ring counters wrapping at 8, 4 receives of up to 2
  datagrams, drains of up to 2.
- **Trace walk:** 3 slots, 4 buffers, the kernel's 65536 wrap, up to 40 receives in at
  200 steps (the row's steps). Each record carries every datagram banked and delivered
  so far, so TLC's time grows faster than the cube of the walk's length: on amd-2, 201
  records took 5 s and 401 took 116 s. 200 steps stays well inside the 110 s budget,
  and more seeds give the coverage.

## The reversed witnesses

Each witness changes one rule switch and must break one claim.

| cfg | switch | must violate |
|---|---|---|
| MCRNUringQueueReceiveBrokenFullAtCapacity.cfg | FullRule "le" | NoOverwrite |
| MCRNUringQueueReceiveBrokenPublishFirst.cfg | PublishRule "before" | InOrder |
| MCRNUringQueueReceiveBrokenReleaseAtTake.cfg | ReleaseRule "take" | NoTornRead |
| MCRNUringQueueReceiveBrokenFullUncounted.cfg | FullCount "silent" | Accounted |
| MCRNUringQueueReceiveBrokenRearmKept.cfg | RearmRule "keep" | Rearms |
| MCRNUringQueueReceiveLayoutBrokenStaleHead.cfg | RoomRule "stale" | FullOnlyWhenFull |
| MCRNUringQueueReceiveLayoutBrokenSlotPlusOne.cfg | SlotRule "plus1" | InOrder |
| MCRNUringQueueReceiveLayoutBrokenReleaseAtTake.cfg | ReleaseRule "take" | NoTornRead |
| MCRNUringQueueReceiveLayoutBrokenNarrowCounter.cfg | CounterWrap 4 | InOrder |
| MCRNUringQueueReceiveLayoutBrokenRingNotPow2.cfg | B = 3 | NoDoubleTake |
| MCTraceRNUringQueueReceiveLayoutSelfBrokenRelease.cfg | BadLog: head = 1 at record 6 | TraceConsistent |
| MCTraceRNUringQueueReceiveLayoutSelfBrokenShort.cfg | ShortLog: record 3 leaves out torn | TraceConsistent |

`MCTraceRNUringQueueReceiveLayoutSelf.cfg` must accept every record of GoodLog.

## Results

The layout model and the trace self controls, on bench-nyc-5 (pinned jar 936a2620...,
TLC 2.19, 2 workers, 2026-11-13). The layout's actions take what the code's calls
return as arguments (KernelReceive(bid), Complete(n), Drain(n)); each argument is
fixed by the state, so the control's distinct states are as before (378,694) and
only the generated states grow.

| cfg | result | states (distinct) |
|---|---|---|
| MCRNUringQueueReceiveLayout.cfg | pass | 2,305,441 (378,694), 11.8 s |
| MCRNUringQueueReceiveLayoutBrokenStaleHead.cfg | FullOnlyWhenFull violated | 2,040,586 (382,422) |
| MCRNUringQueueReceiveLayoutBrokenSlotPlusOne.cfg | InOrder violated | 91,087 (30,379) |
| MCRNUringQueueReceiveLayoutBrokenReleaseAtTake.cfg | NoTornRead violated | 1,227,033 (246,098) |
| MCRNUringQueueReceiveLayoutBrokenNarrowCounter.cfg | InOrder violated | 1,696,993 (276,342) |
| MCRNUringQueueReceiveLayoutBrokenRingNotPow2.cfg | NoDoubleTake violated | 6,055 (2,888) |
| MCTraceRNUringQueueReceiveLayoutSelf.cfg | pass: all 7 records accepted | 13 (7) |
| MCTraceRNUringQueueReceiveLayoutSelfBrokenRelease.cfg | TraceConsistent violated | 9 (6) |
| MCTraceRNUringQueueReceiveLayoutSelfBrokenShort.cfg | TraceConsistent violated | 6 (3) |

Records: RUNS.tsv holds this run, merged through the pinned tlacheck.
