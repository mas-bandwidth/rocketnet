# RNBufferHandoff

The N-buffer handoff of `sdk/rocketnet_buffer_handoff.h`, as the client
transport runs it in both directions (api/BUFFER-HANDOFF-DECISION-20261021.md).
Every buffer has one owner: the producer, the ready queue, the consumer or the
free queue. Only indices move, inside a mutex held for the exchange alone.
The game thread only try-acquires the mutex; the I/O thread acquires it and
waits on a level-triggered wake.

| direction | producer | consumer | MaxReady | code |
|---|---|---|---|---|
| send | game thread (payload rings) | I/O thread | 2 | `rocketnet_client_io_hand_off`, `rocketnet_client_io_step`, `rocketnet_client_io_stop` |
| receive | I/O thread (datagrams) | update | 1 | `rocketnet_client_io_step`, `rocketnet_client_io_expose`, `rocketnet_client_io_return` |

The instance is three buffers of two items and five items. Both controls pass
with every rule on (send 7,464 states, 3,920 distinct; receive 10,792 states,
5,256 distinct). The records are in `RUNS.tsv` (group `rnbufferhandoff`).

## Abstractions

- One data item stands for one packet or datagram; `Cap` stands for a ring's
  256 slots or a receive buffer's 1024 records.
- `WorkerExit` reads the ready queue directly; the code observes it empty under
  the mutex after the quit load, and drains every ready ring in the one step
  before it exits.
- `WorkerLock` and `WorkerExit` are strongly fair: a blocked acquirer gets a
  mutex that is free infinitely often, and the code's exit check follows the
  exchange that found the queue empty. Main's try-lock is strongly fair: update
  runs every frame. Stop is never forced.
- The wait's guard timeout (1 ms) is not modelled: a lost wake is a liveness
  failure here, where the code would only lose a millisecond.

## Reversed witnesses

Each restores one defect and fails its named property. The traces below are
TLC's, checked against the code by hand.

- `MCRNBufferHandoffBrokenMainBlocks.cfg`, `MainNeverWaits`, 3 states: the I/O
  thread takes the mutex, then update finds it held and waits. In the code,
  `rocketnet_client_io_receive_exchange` calls
  `rocketnet_platform_mutex_try_acquire` and returns false; replacing it with
  `rocketnet_platform_mutex_acquire` is this defect.
  `test_client_io_hand_off_never_waits_for_the_mutex` holds the mutex and
  checks that hand-off returns LATE and expose returns NULL.
- `MCRNBufferHandoffBrokenWorkUnderLock.cfg`, `NoWorkUnderLock`, 3 states: the
  game thread takes the mutex and writes a packet before releasing it. In the
  code no copy, clear, send or wake sits between acquire and release in
  `rocketnet_client_io_hand_off`, `rocketnet_client_io_receive_exchange`,
  `rocketnet_client_io_step` or the BASE pair; ring clears
  (`num_packets = 0`) and the wake follow the release.
- `MCRNBufferHandoffBrokenWriteAfterPublish.cfg`, `ViewStable`, 8 states: the
  I/O thread publishes buffer 0, update takes it, then the I/O thread writes
  into it. In the code the I/O thread writes only `io->buffer[io->receive_fill]`,
  and `receive_fill` changes only to the index the publish returns.
- `MCRNBufferHandoffBrokenSkipWake.cfg`, `ArrivedDelivered` (temporal), 28
  states: the I/O thread holds item 5 in its buffer with one buffer ready, so
  its publish is owed and it waits; update takes the ready buffer and clears
  the owed flag without waking it; the I/O thread waits forever. In the code
  `rocketnet_buffer_handoff_owed` is read in the same locked exchange and the
  wake follows the release; `test_client_io_step_full_buffer_waits_on_wake_only`
  and `test_client_io_step_receive_triple_buffer` check the one wake.
- `MCRNBufferHandoffBrokenReleaseBeforeSent.cfg`, `FreeIsEmpty`, 10 states:
  the I/O thread frees a ring it took before sending it. In the code the ring
  the I/O thread holds (`send_ring`) is released only at its next exchange,
  after `rocketnet_client_io_send_ring` returned.
- `MCRNBufferHandoffBrokenDropFinal.cfg`, `ExitSentAll`, 4 states: main writes
  a packet, stops, and the I/O thread exits without the Filling ring stop
  handed over. In the code `rocketnet_client_io_stop` records `send_final`
  before the quit store, and the exit branch of `rocketnet_client_io_step`
  sends it; `test_client_io_stop_hands_over_the_filling_ring` checks all three
  rings arrive in order.

## BASE batched update refinement

`RNBufferHandoffBase.tla` models BASE separately because update may own several
buffers at once. `MCRNBufferHandoffBase.cfg` checks four buffers (`MaxReady=N-1`),
a producer that always owns one buffer, and the maximal oldest READY prefix
that fits a 1024-packet update. Each model item represents 256 packets, with
two items per buffer and five produced items. This finite quotient includes
multiple buffers drained in one update and a queued buffer that exceeds the
remaining budget; it does not enumerate all possible packet counts.

`Expose` returns any deferred cleared buffers and takes the maximal fitting
prefix under the mutex. `Process` reads and clears outside the main mutex.
`ReleaseOnly` returns the batch without taking another buffer. A failed
`MainTry` advances directly to the end of the update, retaining ownership;
the next successful expose releases those deferred buffers. Producer writes
and publication may interleave between these phases. The model checks safety,
including FIFO delivery, stable consumer views, exact ownership, empty free
buffers, the budget, maximal draining and absence of a main wait state. It
makes no fairness or bounded-latency claim about eventual successful try-locks.
The existing threaded receive model retains its wake/liveness checks.

The `rnbufferhandoffbase` records use the same pinned runner and TLC JAR as CI.
Reversed controls restore one defect each: `BaseBrokenSingleTake` leaves a
second fitting READY buffer behind (`MaximalDrain`); `BaseBrokenBudget` takes
three buffers containing five items, exceeding 1024 (`BudgetBound`);
`BaseBrokenDeferred` forgets a consumer-owned buffer when its return try-lock
fails (`OneOwner`). Each must produce the named invariant counterexample.

Measured on Linux with the CI-pinned runner `0c97d5958bf2b3cb1ad24e6e1b58f5b04e70347a`:
control 3,211 generated / 1,825 distinct states; all ten invariants pass.
The single-take witness is 9 states (buffer 1 remains READY after buffer 0
was taken with only 256 packets charged). The budget witness is 14 states
(three buffers, 1,280 packets). The deferred-return witness is 10 states
(buffer 0 was consumed, the worker holds the mutex, and forgetting the held
index leaves that buffer ownerless). Logs are retained on the allocated
Linux bench under `~/p3-stella-handoff/tlc-base-3/`; measured records are in
`RUNS.tsv`. `tlacheck groups --stale` returned `[]` after integration.

## IOCP stop drain

The Windows server transport follows `MainStop` and `WorkerExit`: stop transfers
its final producer, and TX sends every published bank before that producer.
`DiscardOnStop` restores the defect that clears these buffers after quit without
appending their datagrams to `consumed`. The reversed configuration
`MCRNBufferHandoffBrokenDiscardOnStop.cfg` checks `ExitSentAll`; existing
configurations keep this switch FALSE. The native
`test_iocp_transport_stop_sends_published_burst` holds TX, publishes ten loopback
datagrams, stops, releases TX and checks all ten arrive with no failed sends.
Completion workers wait for TX exit before cancelling receives individually,
so cancellation cannot abort the final sends. Kernel completion and cancellation
are outside this buffer ownership model.
