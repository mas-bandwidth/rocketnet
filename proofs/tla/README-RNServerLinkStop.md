# Server stop with packets held in the link simulator

`RNServerLinkStop.tla` models the server's stop while the development link
simulator holds packets (`sdk/rocketnet_server_transport.cpp`:
`rocketnet_server_link_drain` at shutdown, called from
`rocketnet_server_transport_stop_step`, and the step's `link_empty` gate on
`rocketnet_server_transport_io_stop`). The disconnect bursts themselves are
`ServerStopSpread.tla`'s; this module is the simulator's copies, which the I/O
stop also waits on.

At the stop the simulator holds copies in slot order, each bound for the send
queue its destination hashes to. `DrainOne` is one iteration of the stop's
drain at the shutdown clock: a copy whose own queue has room is started and
staged; a copy whose queue has no room is held back while anything staged or in
flight could still free room (the drain ends for this step); once the transport
is idle no copy for that queue can ever leave, so it is discarded and counted
as `SEND_RING_FULL`. `Flush`, `Complete(q)` and `Jam(q)` are the staging, the
transport finishing a frame, and a queue losing its room for good, as in
`ServerStopSpread.tla`. `RequestIoStop` needs the simulator empty, nothing
staged and the transport idle.

Invariants: `TypeOK`, `CopiesAccounted` (every copy held at the stop is still
held, sent or discarded, once), `WithinBank`, `OnlyJammedQueuesDrop`,
`NoDropWhileRoomCanReturn` and `LinkEmptyAtIoStop`; liveness `StopFinishes`.

| Config | Defect | Expected |
|---|---|---|
| `MCRNServerLinkStop.cfg` | none, two queues | pass |
| `MCRNServerLinkStopOneQueue.cfg` | none, one queue | pass |
| `MCRNServerLinkStopBrokenFewest.cfg` | `fewest` | `StopFinishes` violated |
| `MCRNServerLinkStopBrokenNoDiscard.cfg` | `nodiscard` | `StopFinishes` violated |
| `MCRNServerLinkStopBrokenEarlyDiscard.cfg` | `earlydiscard` | `NoDropWhileRoomCanReturn` violated |

## The witnesses against the code, by hand

- `fewest` is the drain before the fix: `if ( rocketnet_server_transport_io_send_room( transport ) <= 0 ) break;`
  at the top of every iteration, `io_send_room` being the transport's
  `send_room`, the fewest free frames of any queue, and no discard. The trace:
  a copy is held for queue 0, queue 1 jams (`Jam(1)`), and the drain never
  takes another copy, so the simulator holds it, `link_empty` stays false and
  `rocketnet_server_transport_io_stop` is never called: the stop waits until
  destroy's bound. The unit test
  `test_server_stop_finishes_with_held_link_packets_past_a_jammed_queue`
  (`tests/test_unit_server_send_queues.cpp`) is this trace on the fake staged
  transport of three queues, red on the code before the fix (`stopped` never
  true) and green after.
- `nodiscard` is a drain that judges each copy by its own queue but never
  discards: a copy for a jammed queue at the front is held back on every step,
  so the stop never finishes either. The fixed code discards it once
  `rocketnet_server_transport_io_send_idle` is true and the queue's room, read
  again after it, is still 0.
- `earlydiscard` discards a copy whose queue has no room without asking
  `send_idle`: the trace is a copy for queue 0 while queue 0's bank is all in
  flight and not jammed; the discard loses a copy whose queue would have had
  room once the frames finished. The fixed code holds it back
  (`rocketnet_network_simulator_deliver_packets` puts it back in the simulator,
  due at once) and ends the step's drain.

## What is abstracted

One drain iteration is one step; the stop step's other work (the bursts, the
flush before the drain) is `ServerStopSpread.tla`'s. The simulator's slot
order is a sequence, and a copy held back stays at its front; the code puts
it back in whatever slot the simulator gives it, which may change which copy
the next step takes first, but not what happens to any copy: one whose queue
has room leaves, one whose queue has none waits for send_idle. Loss, latency and
duplicates were rolled when the copies were sent and are not rolled again
(`deliver_packets` rolls nothing). The drain at a running update (not the
stop) is unchanged and still goes on only while every queue has room.
