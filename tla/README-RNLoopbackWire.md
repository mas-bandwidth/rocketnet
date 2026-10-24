# RNLoopbackWire

The loopback transport's wire (`sdk/rocketnet_driver_loopback.cpp`) between
caller threads. In a multiplayer game over loopback the client and the server
usually run on different threads, so nothing a send does may touch the
receiver's state beyond its inbox, and the transport runs no thread of its own.

Every endpoint owns an inbox of `ROCKETNET_LOOPBACK_QUEUE_PACKETS` one-datagram
slots with the free and ready queues of `sdk/rocketnet_buffer_handoff.h`. Each
slot has one owner: the free queue, one sender (reserved, being written), the
ready queue (filled) or the receiver (taken, being read). Only indices move, in
one exchange under the inbox mutex; the datagram is written and read outside it.

| step | thread | code |
|---|---|---|
| Pin, Verify | sender | `loopback_route`, `loopback_pin` |
| Reserve | sender | `rocketnet_buffer_handoff_reserve` in `loopback_deliver` |
| Fill | sender | the write and the release store of `filled` in `loopback_deliver` |
| Take, Copy, Release | receiver | `loopback_take` |
| Destroy, Free | receiver | `loopback_destroy` |
| Leave | sender | the sender's own destroy, which leaves its datagrams in the inbox |

A send reserves its slot at the tail of the ready queue, so the queue keeps
send order with any number of senders, and marks it filled before it returns
(`SentIsVisible`); the receiver takes only the filled prefix. A message a client
sends in `send_packets` is therefore taken by the server's next update: the same
frame when the client's frame runs first (`examples/game_loop`).

The instance is two senders, two slots, a batch of two and two datagrams per
sender. The passing control checks every invariant and `FilledRead` under weak
fairness. The records are in `RUNS.tsv` (group `rnloopbackwire`).

## Abstractions

- One item stands for one datagram; a slot's bytes are its `content`.
- The inbox mutex is one atomic action per exchange; the filled flag's release
  store and acquire load are the `filled` variable.
- The table mutex and the route cache are folded into Pin and Verify: what
  matters is that the pin precedes the liveness check.
- A dead receiver drops the datagram at Verify, as a send to a destroyed
  endpoint does (counted as a send failure in the code).

## Reversed witnesses

Each restores one defect and fails its named property.

- `MCRNLoopbackWireBrokenFillLate.cfg`, `SentIsVisible`: a send returns with its
  slot reserved and not filled, so the receiver's next update cannot take it.
  This is the frame the I/O-thread hand-off cost before: the datagram left a
  millisecond later, after the server's update.
- `MCRNLoopbackWireBrokenCheckBeforePin.cfg`, `NoUseAfterFree`: the sender reads
  liveness, the receiver destroys and frees, then the sender pins and writes.
  `loopback_route` pins before it reads `live`.
- `MCRNLoopbackWireBrokenReleaseBeforeCopy.cfg`, `NoOverlap`: the receiver frees
  its batch before reading it and a sender reserves one of those slots.
  `loopback_take` releases only after the copy.
- `MCRNLoopbackWireBrokenTakeUnfilled.cfg`, `InOrder`: the receiver takes a
  reserved slot whose write has not finished and reads a stale datagram.
  `loopback_take` stops at the first slot whose `filled` is not set.
- `MCRNLoopbackWireBrokenNoDestroyWait.cfg`, `NoUseAfterFree`: destroy frees the
  inbox with a sender's pin held. `loopback_destroy` waits for `pins` to reach 0.
- `MCRNLoopbackWireBrokenDropWithSender.cfg`, `NoLoss`: the sender's destroy
  discards its datagrams in flight, as an inbox kept with the sender would.
  The inbox lives with the receiver.
