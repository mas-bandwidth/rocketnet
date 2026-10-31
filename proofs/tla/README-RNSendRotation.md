# RNSendRotation

The server's send loop over the endpoints' write passes, against the portable
transport's per-flush send capacity, with the loop's own arithmetic.
[RNOutgoingBuild](README-RNOutgoingBuild.md) models one connection's build and
`RNServerSendFair.tla` the rotation abstractly (`K` clients served per update,
the cursor moved by `K`); this model carries the rounds, the capacity test,
the frame starts and aborts, and the cursor arithmetic of the code, across
several connections whose windows stall and whose budgets refuse.

## What it guards

Capacity spent on frames that carry nothing. If a connection whose window
is stalled, or whose reliable packet the budget refuses, answers `ORDERED`
from `rn_endpoint_next_packet_bytes`, the object starts a transport frame, the
write finds nothing to send, and the frame is aborted. If the pass then ends
and the loop's next round reopens it, it does the same again. The portable
transport takes a slot of the update's capacity at each start
(`payload_ring.num_packets`, `rocketnet_server_transport_start_packet` in
`sdk/rocketnet_server_transport.cpp`) and an abort does not give it back
(`rocketnet_server_transport_abort_packet_unchecked` clears only the slot's
bytes); the count resets only when the transport sends
(`rocketnet_server_transport_send_packets_impl`). So stalled or refused
connections would spend the capacity the connections with work need, once
per round.

## State

| Model | Code |
|---|---|
| `conn[c].phase` | `endpoint->write_phase`: `RN_ENDPOINT_WRITE_IDLE`, `_RELIABLE`, `_UNRELIABLE`, `_DONE` |
| `conn[c].ready` | new reliable messages queued (`ordered.channels.ready_head != RN_DESCRIPTOR_NONE`) |
| `conn[c].resend` | a resend due (`rn_endpoint_fifo_due`) |
| `conn[c].unr` | queued unreliable records (`unreliable_count`) |
| `conn[c].stalled` | `ordered.send.stalled` |
| `conn[c].pays` | the send budget pays this flush's packets |
| `conn[c].waiting` | `budget.reliable_waiting` |
| `used` | `payload_ring.num_packets`, frames started this flush |
| `start`, `off`, `progress` | the loop's `start`, `offset` and `progress` |
| `cursor` | `object->send_next_connection` |

Work, the stall and the budget change only between flushes (`Env`): nothing
else runs while `send_packets` does. One visit of a connection is one action,
since `rn_message_connection_write` runs to its return with nothing
interleaved. `packets_per_update` is taken as never reached.

## Actions and the code they mirror

- **`BeginFlush`**: `rn_message_object_send_packets`
  (`sdk/rocketnet_message_server.cpp`): `rn_endpoint_begin_flush` for every
  connection (`sdk/rocketnet_endpoint.cpp`, a `DONE` pass reopens), then
  `start = send_next_connection % num_connections`.
- **`Visit`**: one offset of a round of `rn_message_object_send_packets`: the
  capacity test (`rocketnet_server_transport_send_capacity_available`, `goto
  flush` when spent), `rn_message_connection_write`, the cursor set to
  `( i + 1 ) % num_connections`, and the next round while any connection
  progressed. Inside the visit, `ConnWrite` is
  `rn_message_connection_write`: `rn_endpoint_next_packet_bytes` (endpoint.cpp:
  an idle pass opens its reliable phase and clears `reliable_waiting` through
  `rocketnet_send_budget_begin_update`; `rn_endpoint_ordered_due`, gated on
  the stall; `rn_endpoint_end_reliable`; the pass ends `DONE`),
  `start_packet` (a NULL start is `rn_endpoint_packet_failed` with nothing
  written), `rn_endpoint_write_packet` (endpoint.cpp: an ordered packet the
  budget refuses or with nothing to number writes nothing and ends the
  reliable path; an unreliable packet the budget refuses drops every queued
  record and ends the pass), the abort or the finish.
- **`BeginUpdate`**: `rn_endpoint_begin_update` (endpoint.cpp): the pass
  reopens.
- **`Env(c)`**: the environment between flushes.

## Invariants

- **`NoAbortWhilePaying`**: no frame is aborted for a connection whose budget
  pays: a stalled window with nothing to resend asks for no frame.
- **`AbortsBounded`**: a connection aborts at most one frame a flush (one
  budget refusal ends its pass until the next flush).
- **`ServedWhenCapacity`**: a flush ends with its capacity spent or with no
  connection holding work the code would write.
- **`BoundedWait`**: a connection with writable work at every flush is served
  within `N` flushes. Each flush's first visit has capacity and moves the
  cursor past one connection, and the cursor moves past a connection only by
  visiting it with capacity, when a connection with work is served.
  `RNServerSendFair` states the tighter `ceil(N / K)` for the abstract
  rotation.

## The instances

- `MCRNSendRotation.cfg`: two connections, three frames a flush, three
  flushes, windows that stall and budgets that refuse. 379,396 distinct
  states, depth 21.
- `MCRNSendRotationFair.cfg`: three connections, one frame a flush, five
  flushes, no stall and no refusal, so a connection starved for three flushes
  would show. 1,221,453 distinct states, depth 26.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample was checked against the C by hand.

1. **`MCRNSendRotationBrokenReopen.cfg`** (`KeepDone = FALSE`, an ended pass
   that reopens within the flush: the end of `rn_endpoint_next_packet_bytes`
   sets `IDLE`). TLC, three connections and four frames: connection 0 has a
   ready message and a budget that refuses it; connection 2 has an unreliable record. Round one:
   connection 0 plans `ORDERED`, a frame is started (`used = 1`),
   `rn_ordered_write` is refused and sets `reliable_waiting`, the frame is
   aborted, the pass ends; connection 2 writes its record, so the round
   progressed. Round two: connection 0's pass is `IDLE` again, so
   `rn_endpoint_next_packet_bytes` restarts the budget phase (clearing
   `reliable_waiting`) and plans the same refused packet: a second abort
   (`AbortsBounded`), `used = 3` for one written frame.
2. **`MCRNSendRotationBrokenStallDue.cfg`** (`StallGate = FALSE`, ready messages
   of a stalled window counted as due: `rn_endpoint_ordered_due` answers yes to
   `ready_head != RN_DESCRIPTOR_NONE` without `!ordered->send.stalled`). TLC:
   a connection whose budget pays has a ready message and a stalled window;
   it plans `ORDERED`, a frame is started, `rn_ordered_write` numbers nothing
   (`rn_window_send_first_write` refuses while stalled), the frame is aborted
   (`NoAbortWhilePaying`).
3. **`MCRNSendRotationBrokenCursorAtServed.cfg`** (`CursorNext = FALSE`:
   `send_next_connection = i`). TLC, three connections and one frame a flush:
   connection 0 has three frames of work and connection 2 one. Each flush
   starts at 0, serves connection 0, finds the capacity spent at the next
   connection and leaves the cursor at 0; connection 2 waits three flushes
   (`BoundedWait`). Against the C: with `send_next_connection = i` in
   `rn_message_object_send_packets` the `goto flush` at the capacity test
   leaves the cursor on the connection just served, and the next
   `send_packets` starts there again.
