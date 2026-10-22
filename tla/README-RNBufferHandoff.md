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
| receive | I/O thread (datagrams) | update | 1 | `rocketnet_client_io_step`, `rocketnet_client_io_expose`, `rocketnet_client_io_return`; BASE: `rocketnet_client_receive_packets`, `rocketnet_client_update_expose_legacy_receive` |

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
