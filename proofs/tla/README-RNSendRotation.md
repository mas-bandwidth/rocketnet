# RNSendRotation

The server's send loop over the endpoints' write passes, against the portable
transport's per-flush send capacity, with the loop's own arithmetic.
[RNOutgoingBuild](README-RNOutgoingBuild.md) models one connection's build and
`RNServerSendFair.tla` the same rounds at the loop's index arithmetic against
the transport's capacity (`send_writers`, `count`, `kept`, `previous`, the
stored cursors, the send queues), each visit taken whole; this model carries
the rounds and the writer list, the capacity test, the frame starts, and the
cursor arithmetic of the code with each connection's write pass, across
several connections whose windows stall, whose budgets refuse and whose
ordered paths may be vain.

## What it guards

Capacity spent on frames that carry nothing, and a pass whose cost grows with
one client's work.

The portable transport takes a slot of the update's capacity at each frame
start (`payload_ring.num_packets`, `rocketnet_server_transport_start_packet` in
`sdk/rocketnet_server_transport.cpp`) and an abort does not give it back
(`rocketnet_server_transport_abort_packet_unchecked` clears only the slot's
bytes); the count resets only when the transport sends
(`rocketnet_server_transport_send_packets_impl`). No transport returns an
aborted reservation. So a frame started for a packet that is then not written
spends capacity the other connections need ([MESSAGES.md m1.8](../../standard/MESSAGES.md#m1.8)).
Two refusals can come after a frame is started unless they are decided
first: the window cannot number the ready message (a stall, or an ack that
clears the stall without making the room the message needs), and the send
budget refuses the packet. The code decides the window's before the frame
(`rn_endpoint_ordered_due` asks `rn_window_send_would_number`, the test the
walk's first write applies, `RNWindow` `WouldNumber`), and the budget's by
writing the packet into a stack payload first while the bucket holds less
than one maximum packet (`rn_endpoint_write_may_be_refused`), starting a frame
only when bytes came out ([m9.4](../../standard/MESSAGES.md#m9.4)).

One abort the due test cannot rule out stays. A connection that has ended
(exhausted, a channel error, a reliable message past its timeout) still has
its ordered work answered due by `rn_endpoint_ordered_due`, which does not
ask whether the connection ended, so it plans an ordered packet; but
`rn_ordered_write` returns 0 for an ended connection before anything else,
so `rn_message_connection_write` aborts the started frame and
`rn_endpoint_write_packet` ends the reliable path
(`rn_endpoint_end_reliable`). The model calls such a connection vain. The
reliable path does not reopen within the flush, so a vain write costs at
most one slot per connection per flush (`AbortsBounded`), and no refusal the
code decides before a frame ever costs one (`NoRefusalAbort`). It costs one
again at each later flush until the owner kicks the connection.
`rn_endpoint_fifo_due` also answers yes after 64 acknowledged steps at a
FIFO's head ("a long acked prefix"), which would be the same shape for a live
connection; the resend FIFOs hold no acknowledged number (an ack unlinks its
number before marking it, and the walk asserts it, `rocketnet_channel.cpp`),
so that branch never takes a step, and the model leaves it out.

The loop runs rounds of one frame per connection while any connection wrote.
A connection that writes nothing in a round has ended its pass until the next
flush, so after the first round only the connections that wrote in the round
before (the writer list) are visited, in the same cyclic order: a flush costs
`num_connections` visits plus one per frame, not `num_connections` per round.
The cursor is set as a walk over every connection would set it, so the
rotation is the one the full walk gave.

## State

| Model | Code |
|---|---|
| `conn[c].phase` | `endpoint->write_phase`: `RN_ENDPOINT_WRITE_IDLE`, `_RELIABLE`, `_UNRELIABLE`, `_DONE` |
| `conn[c].ready` | new reliable messages queued (`ordered.channels.ready_head != RN_DESCRIPTOR_NONE`) |
| `conn[c].resend` | a resend due (`rn_endpoint_fifo_due`) |
| `conn[c].unr` | queued unreliable records (`unreliable_count`) |
| `conn[c].stalled` | `ordered.send.stalled` |
| `conn[c].fits` | `rn_window_send_would_number` for the ready message |
| `conn[c].pays` | the send budget pays this flush's packets |
| `conn[c].waiting` | `budget.reliable_waiting` |
| `conn[c].vain` | the connection has ended (`rn_region_exhausted`, `channels.error`, `rn_window_ack_check_timeout`) with ordered work the due test sees: its ordered write returns 0 |
| `used` | `payload_ring.num_packets`, frames started this flush |
| `start`, `off` | the loop's `start` and `offset` |
| `later`, `listed`, `wrote` | a round after the first; the writer list (`send_writers`, `count`); this round's writers (`kept`) |
| `cursor` | `object->send_next_connection` |
| `visits`, `aborts` | the development seam's `slot_visits` and `frames_aborted` (`rn_send_pass_cost_read`) |
| `badAborts` | ghost: the aborts not for a vain ordered write |
| `fullCursor`, `skipOk` | ghosts: the cursor a walk over every connection sets, and whether every slot the list skipped would have written nothing |

Work, the window and the budget change only between flushes (`Env`): nothing
else runs while `send_packets` does. One visit of a connection is one action,
since `rn_message_connection_write` runs to its return with nothing
interleaved. `packets_per_update` is taken as never reached. `MayBeRefused`
is the budget's refusal itself: the code writes first in a superset of those
cases (any bucket below one maximum packet), which changes where the bytes
are written and nothing the model observes.

## Actions and the code they mirror

- **`BeginFlush`**: `rn_message_object_send_packets`
  (`sdk/rocketnet_message_server.cpp`): `rn_endpoint_begin_flush` for every
  connection (`sdk/rocketnet_endpoint.cpp`, a `DONE` pass reopens), then
  `start = send_next_connection % num_connections`.
- **`Visit`**: one offset of a round of `rn_message_object_send_packets`. In
  the first round every connection is visited; in a later round a connection
  off the writer list is not (the model walks the offset to compare with the
  full walk). A visit is the capacity test
  (`rocketnet_server_transport_send_capacity_available`, `goto flush` when
  spent, with the cursor the loop held before this visit: in a later round the
  one stored with the connection in the list),
  `rn_message_connection_write`, and the cursor set to
  `( i + 1 ) % num_connections`; after the round, another over this round's
  writers, or the end with the cursor past the last connection. Inside the
  visit, `ConnWrite` is `rn_message_connection_write`:
  `rn_endpoint_next_packet_bytes` (an idle pass opens its reliable phase and
  clears `reliable_waiting`; `rn_window_send_stall_unless_numbered` marks the
  stall when the window cannot number the ready message;
  `rn_endpoint_ordered_due`, gated on the stall and on `would_number`;
  `rn_endpoint_end_reliable`; the pass ends `DONE`), then
  `rn_endpoint_write_may_be_refused` (`MayBeRefused`): the packet is written
  first and a refusal takes no frame, or the frame is started (a NULL start is
  `rn_endpoint_packet_failed` with nothing written) and the packet written in
  place, then the finish; a vain ordered write returns 0 into the frame, which
  is aborted, and the reliable path ends.
- **`BeginUpdate`**: `rn_endpoint_begin_update` (endpoint.cpp): the pass
  reopens.
- **`Env(c)`**: the environment between flushes.

## Invariants

- **`NoRefusalAbort`**: no frame of the capacity is started and aborted in
  the send pass for a refusal the code decides before a frame (the window's,
  the budget's): the only abort is a vain ordered write's.
- **`AbortsBounded`**: at most one frame per connection per flush is aborted.
- **`UnlistedPassEnded`**: in a later round every connection off the writer
  list has ended its pass (the list's premise).
- **`SkippedWritesNothing`**: every slot a later round skips is one a walk
  over every connection would have found nothing to write in.
- **`CursorAsFullWalk`**: between flushes the cursor is the one a walk over
  every connection in every round leaves.
- **`VisitsBounded`**: a flush visits at most `N` slots plus one per frame
  started.
- **`ServedWhenCapacity`**: a flush ends with its capacity spent or with no
  connection holding work the code would write.
- **`BoundedWait`**: a connection with writable work at every flush is served
  within `N` flushes. Each flush's first visit has capacity and moves the
  cursor past one connection, and the cursor moves past a connection only by
  visiting it with capacity, when a connection with work is served.
  `RNServerSendFair` states the tighter `ceil(N / K)` for the abstract
  rotation.

## The instances

Measured on the Linux bench, two workers:

| Case | Connections, frames a flush, flushes | Environment | Distinct states |
|---|---|---|---:|
| `MCRNSendRotation.cfg` | 2, 3, 3 | stalls, refusals | 281,591 |
| `MCRNSendRotationShortAck.cfg` | 2, 3, 3 | stalls, acks without room | 180,872 |
| `MCRNSendRotationRounds.cfg` | 3, 4, 1 | stalls, refusals | 822,853 |
| `MCRNSendRotationRoundsRotate.cfg` | 3, 2, 3 | none | 310,317 |
| `MCRNSendRotationFair.cfg` | 3, 1, 5 | none | 1,149,208 |

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample was checked against the C by hand.

1. **`MCRNSendRotationBrokenReopen.cfg`** (`KeepDone = FALSE`, an ended pass
   that reopens within the flush: the end of `rn_endpoint_next_packet_bytes`
   sets `IDLE`). TLC, three connections and four frames: connections 0 and 1
   have nothing to send and their passes end `IDLE`; connection 2 writes its
   unreliable record, so a second round runs over the writer list `{2}` with
   connections 0 and 1 off it and their passes open (`UnlistedPassEnded`).
   Against the C: with `IDLE` at the pass's end, the next
   `rn_endpoint_next_packet_bytes` restarts the pass, so a connection the list
   skips could still have written.
2. **`MCRNSendRotationBrokenStallDue.cfg`** (`StallGate = FALSE`, ready messages
   of a stalled window counted as due: `rn_endpoint_ordered_due` answers yes to
   `ready_head != RN_DESCRIPTOR_NONE` without `!ordered->send.stalled`). TLC:
   a connection whose budget pays has a ready message and a stalled window;
   it plans `ORDERED`, a frame is started, `rn_ordered_write` numbers nothing
   (`rn_window_send_first_write` refuses while stalled), the frame is aborted
   (`NoRefusalAbort`).
3. **`MCRNSendRotationBrokenShortAck.cfg`** (`FitsGate = FALSE`: the due test
   does not ask `rn_window_send_would_number`, as on v1.1.1). TLC, two
   connections: connection 0 has a ready message the window cannot number
   and no stall (an ack moved `U'` by less than the message needs); it plans
   `ORDERED`, a frame is started, the first write refuses and marks the stall,
   the frame is aborted (`NoRefusalAbort`). Against the C at v1.1.1: the partial
   ack clears `stalled` in `rn_window_send_mark_acked`, `rn_endpoint_ordered_due`
   answers yes on `ready_head`, and `rn_window_send_first_write_block` refuses
   (`test_endpoint_partial_ack_stall_requests_no_frame`).
4. **`MCRNSendRotationBrokenBudgetFrame.cfg`** (`WriteFirst = FALSE`: the frame
   is started before the budget decides, as on v1.1.1). TLC, two connections:
   connection 0 has one unreliable record and a budget that refuses it; a
   frame is started, `rn_endpoint_write_packet` drops the record and writes
   nothing, the frame is aborted (`NoRefusalAbort`). Against the C at v1.1.1:
   `rn_message_connection_write` starts the frame, the unreliable write is
   refused by `rocketnet_send_budget_admit` and returns 0, and
   `abort_packet` keeps the slot
   (`test_server_send_cap_unreliable_over_budget_takes_no_slot`).
5. **`MCRNSendRotationBrokenCutCursor.cfg`** (`CutCursor = FALSE`: at a capacity
   cut in a later round the cursor is left where the last writer set it).
   TLC, three connections and four frames: round one, connections 0 and 1
   write and 2 writes nothing; round two over `{0, 1}` writes two more
   frames and ends with the capacity spent; round three's first visit, of
   connection 0, finds no capacity. The full walk visited connection 2 in
   round two and left the cursor at 0; the broken cut leaves it at 2
   (`CursorAsFullWalk`). Against the C: the stored cursor of connection 0 is
   `-1`, past the last connected connection of round one (2), so the cut sets
   `send_next_connection = 0`.
6. **`MCRNSendRotationBrokenCursorAtServed.cfg`** (`CursorNext = FALSE`:
   `send_next_connection = i`). TLC, three connections and one frame a flush:
   connection 0 has three frames of work and connection 2 one. Each flush
   starts at 0, serves connection 0, finds the capacity spent at the next
   connection and leaves the cursor at 0; connection 2 waits three flushes
   (`BoundedWait`). Against the C: with `send_next_connection = i` in
   `rn_message_object_send_packets` the `goto flush` at the capacity test
   leaves the cursor on the connection just served, and the next
   `send_packets` starts there again.

## Revision 1.1: a dark client takes no more than its turn (RNServerSendFair)

Standard rule: MESSAGES.md m1.8 (per-client scope). `RNServerSendFair.tla`
gains the constant `Dark`, the clients whose path is dark or that never
acknowledge, so they always have resends due, and the invariant
`OtherClientUnaffected`: every client outside `Dark` is still served within
`ceil(N / K)` updates. The rule is the one-frame-per-connection-per-round
loop of `rn_message_object_send_packets` with the cursor at `i + 1`: a busy
or dark connection cannot take K turns. `Mode = "darkhogs"` is the reversed
witness, a per-client allowance that lets a dark client with resends due take
the whole cap each update. The four existing configurations set `Dark = {}`
and their records are unchanged.

| Case | Expected | Exit | Distinct |
|---|---|---:|---:|
| `MCRNServerSendFairDark.cfg` (N 2, K 1, `Dark = {2}`) | pass | 0 | 9 |
| `MCRNServerSendFairBrokenDarkHogs.cfg` | `OtherClientUnaffected` | 12 | 3 |

Witness trace, read by hand (3 states): N = 2, K = 1, client 2 dark. Each
`Tick` serves only client 2; at update 2 client 1 has waited `Window = 2`
updates and the invariant fails. Against the C: the inner loop of
`rn_message_object_send_packets` writes one frame for a connection, moves
`send_next_connection` past it and goes on to the next connection before
any connection gets a second frame, so client 1 is served at the next update.
