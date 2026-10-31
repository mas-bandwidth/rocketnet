# RNSendRotation

The server's send loop over the endpoints' write passes, against the portable
transport's per-flush send capacity, with the loop's own arithmetic.
[RNOutgoingBuild](README-RNOutgoingBuild.md) models one connection's build and
`RNServerSendFair.tla` the rotation abstractly (`K` clients served per update,
the cursor moved by `K`); this model carries the rounds, the capacity test,
the frame starts and aborts, and the cursor arithmetic of the code, across
several connections whose windows stall and whose budgets refuse.

## What it guards

Commit bba252cb fixed this: a connection whose window was stalled, or whose
reliable packet the budget refused, answered `ORDERED` from
`rn_endpoint_next_packet_bytes`; the object started a transport frame, the
write found nothing to send, and the frame was aborted. The pass then ended,
and the loop's next round reopened it and did the same again. The portable
transport takes a slot of the update's capacity at each start
(`payload_ring.num_packets`, `sdk/rocketnet_server_transport.cpp` 2889) and an
abort does not give it back (2953 clears only the slot's bytes); the count
resets only when the transport sends (3460). So stalled or refused
connections spent the capacity the connections with work needed, once per
round.

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

Line numbers are at this change.

- **`BeginFlush`**: `rn_message_object_send_packets`
  (`sdk/rocketnet_message_server.cpp` 1312): `rn_endpoint_begin_flush` for every
  connection (1321; `sdk/rocketnet_endpoint.cpp` 1207, a `DONE` pass reopens),
  then `start = send_next_connection % num_connections` (1333).
- **`Visit`**: one offset of a round (1339-1359): the capacity test
  (`rocketnet_server_transport_send_capacity_available`, 1349-1351, `goto
  flush` when spent), `rn_message_connection_write` (1269), the cursor set to
  `( i + 1 ) % num_connections` (1357), and the next round while any
  connection progressed (1359). Inside the visit, `ConnWrite` is
  `rn_message_connection_write`: `rn_endpoint_next_packet_bytes` (endpoint.cpp
  1124: an idle pass opens its reliable phase and clears `reliable_waiting`
  through `rocketnet_send_budget_begin_update`, 1141; `rn_endpoint_ordered_due`,
  978, gated on the stall; `rn_endpoint_end_reliable`, 1040; the pass ends
  `DONE`, 1200), `start_packet` (a NULL start is `rn_endpoint_packet_failed`
  with nothing written, 1290 and endpoint.cpp 1489), `rn_endpoint_write_packet`
  (1254: an ordered packet the budget refuses or with nothing to number writes
  nothing and ends the reliable path; an unreliable packet the budget refuses
  drops every queued record and ends the pass), the abort (1297) or the
  finish.
- **`BeginUpdate`**: `rn_endpoint_begin_update` (endpoint.cpp 930): the pass
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

1. **`MCRNSendRotationBrokenReopen.cfg`** (`KeepDone = FALSE`, the code before
   bba252cb: the end of `rn_endpoint_next_packet_bytes` set `IDLE`). TLC, three
   connections and four frames: connection 0 has a ready message and a budget
   that refuses it; connection 2 has an unreliable record. Round one:
   connection 0 plans `ORDERED`, a frame is started (`used = 1`),
   `rn_ordered_write` is refused and sets `reliable_waiting`, the frame is
   aborted, the pass ends; connection 2 writes its record, so the round
   progressed. Round two: connection 0's pass is `IDLE` again, so
   `rn_endpoint_next_packet_bytes` restarts the budget phase (clearing
   `reliable_waiting`) and plans the same refused packet: a second abort
   (`AbortsBounded`), `used = 3` for one written frame.
2. **`MCRNSendRotationBrokenStallDue.cfg`** (`StallGate = FALSE`, the code
   before bba252cb: `rn_endpoint_ordered_due` answered yes to
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
   (`BoundedWait`). Against the C: with `send_next_connection = i` at 1357 the
   `goto flush` at 1351 leaves the cursor on the connection just served, and
   the next `send_packets` starts there again.
