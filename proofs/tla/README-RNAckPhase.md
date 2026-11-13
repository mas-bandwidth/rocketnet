# RNAckPhase: the server's ack phase

`RNAckPhase.tla` models the split server update of SERVER-UPDATE-ACK.md P3:
`rocketnet_server_update_begin`, the per-client `rocketnet_server_update_acks`
calls from the game's threads, and `rocketnet_server_update_end`. It checks the
split update against the serial update it replaces, the one where each ack
block is applied in the receive loop as it arrives.

## The phase and the model

The receive loop defers each session packet's ack block, with its client and
connection tag, in arrival order. When the entries or the block store run out,
the update spills: everything deferred so far is applied and gathered on main,
and the rest of the update applies its blocks as they come. A reset of a client
inside the loop applies that client's deferred blocks before the endpoint is
reset. Then the ack phase opens. Any thread may apply a client's blocks, one
thread per client per phase. The message layer takes a settled client's ids
in that same call. After the join, `update_end` gathers: it applies every client
nobody called, copies every entry's ids into the public batch in arrival order
and adds the per-client counts to the counters. Then the sweep makes its sends,
and the message layer's drain takes the batch's ids of every client the ack
calls did not take.

An endpoint is its history since its last reset: the arrivals applied to it and
the sends recorded on it, in the order they landed, plus its epoch (its resets).
`process_block` is a function of the endpoint and the block. So the ids a block
surfaces are named by the client, the epoch and the history it was applied on,
and two worlds that apply the same block on the same history surface the same
ids. A block's counts (unknown sequences, a full batch) are k, 0 or 1. The
serial reference runs beside the split world in the same state.

| Code | Model |
| --- | --- |
| `rocketnet_server_acks_begin_update` | `Begin` |
| `rocketnet_server_acks_defer`, the receive site in `rocketnet_server_protocol.cpp` | `Defer` |
| the receive site when `defer` refuses: `rocketnet_server_protocol_ack_gather`, then `process_block` inline | `Spill` |
| the receive site after a spill (the acks are DIRECT) | `Inline` |
| the connection response's first keep-alive, the receive loop's one session send | `FirstKeepAlive` |
| `rocketnet_server_acks_reset_slot`: `apply_client`, then the endpoint reset | `ResetFlush` |
| `rocketnet_server_transport_update_begin`: `open_ack_phase` | `OpenAcks` |
| `rocketnet_server_transport_update_acks`: the fill phase's ack scope (one thread per client) | `AckStart` |
| `rocketnet_server_acks_apply_client`: one block, in list order | `AckStep` |
| the list emptied; `rn_message_object_update_acks` takes the ids of a settled slot, once | `AckEnd` |
| `rocketnet_server_acks_gather`, first in `update_finish` | `Gather` |
| the sweep and `protocol_update` in `update_finish` | `Sweep` |
| the message back's drain, skipping a taken slot's runs | `Drain` |

`Done` is a terminal stutter. Settled is chosen by the environment, except that
a client reset in this update's loop is never settled (its connection tag
changed). The fill phase's kind check is taken as given: inside the ack phase
only ack calls and `update_end` are taken, so no main operation runs between
the receive and the join. `RNFillPhase` models the claim that makes the check
sound.

Invariants:

- `ArrivalOrder`: each client's blocks are applied once each, in arrival order.
- `PublicOrder`: the public acked-packets batch is the serial one, with the same
  ids, clients and order.
- `CountsExact`: the counters are the serial counters.
- `EndpointsMatch`: every endpoint is the serial one.
- `MessageOnce`: every id reaches the message layer once, in the client's batch
  order.
- `NoSendBeforeApply`: no send lands on an endpoint while one of its blocks is
  deferred.

## The run

`MCRNAckPhase.cfg` has two clients and two threads, three session packets over
two updates, one reset, and room for two deferred blocks, so a third packet in
one update spills. TLC 2.19 (`tla2tools.jar` sha256 `936a2620...`) on
bench-nyc-5 with two workers: 7,162,624 states generated, 2,652,053 distinct,
no violation, in 29 s.

## The reversed witnesses

Each one restores one defect and must fail on its rule. The traces below are
the ones TLC printed (bench-nyc-5, 2026-11-13). Each was checked against the
code by hand.

- `MCRNAckPhaseBrokenTwoAppliers.cfg` (`NoOwnerCheck`), `ArrivalOrder`. Begin,
  Defer(c), OpenAcks, AckStart(t1, c), AckStart(t2, c), AckStep(t1),
  AckStep(t2): both threads read the client's list from the head and apply the
  same block, which lands on the endpoint twice. In the code,
  `rocketnet_server_transport_update_acks` enters the fill phase's ack scope
  first. A second thread's scope is refused (`FILL_TWO_FILLERS`), and
  `apply_client` never runs for it (tests/server_ack_phase_cases.cpp,
  `test_server_ack_phase_refusals`).
- `MCRNAckPhaseBrokenGatherByClient.cfg`, `PublicOrder`. Defer(c2), then
  Defer(c1), then OpenAcks and a Gather that walks the clients in the order
  c1, c2. The batch puts c1's ids (arrival 2) ahead of c2's (arrival 1). In the code, `rocketnet_server_acks_gather` copies `entries[0 ..
  num_entries)`, the arrival order, from each entry's run in its client's
  segment. The differential test fails at its first comparison when the loop
  is reversed (a mutation, run on bench-nyc-5).
- `MCRNAckPhaseBrokenSweepBeforeGather.cfg`, `NoSendBeforeApply`. Begin,
  Defer(c), OpenAcks, then Sweep with no ack call made: a keep-alive is recorded
  on c's endpoint before c's block is applied. The serial update applied the
  block before its sweep, so the endpoint would differ. In the code,
  `rocketnet_server_transport_update_finish` calls
  `rocketnet_server_protocol_ack_gather` first, before the sweep and
  `protocol_update`.
- `MCRNAckPhaseBrokenResetNoFlush.cfg`, `PublicOrder`. Defer(c), then
  ResetFlush(c) without the flush. The block is applied at the gather to the
  reset endpoint (a new epoch, an empty history), so its ids are not the serial
  ones. In the code, `rocketnet_server_acks_reset_slot` calls `apply_client`
  before `rocketnet_ack_endpoint_reset`. With that call removed, the
  differential test fails (a mutation, run on bench-nyc-5).
- `MCRNAckPhaseBrokenSpillNoFlush.cfg`, `PublicOrder`. Two Defers fill the
  entries, and the third packet spills straight to inline, so its ids reach the
  batch before the two earlier blocks, which the end-of-update gather appends
  after it. In the code, the receive site calls
  `rocketnet_server_protocol_ack_gather` before it applies the refused block
  inline. The differential test's seeds 4 and 5 force spills through the
  entry-capacity seam.
- `MCRNAckPhaseBrokenCountsBeforeApply.cfg`, `CountsExact`. Defer(c, k = 1)
  with no ack call. The gather sums the per-client counts before it applies c,
  so c's count is lost. In the code, `rocketnet_server_acks_gather` applies
  every pending client first and sums the counts after its copy.
- `MCRNAckPhaseBrokenDrainTaken.cfg`, `MessageOnce`. Defer(c), then an ack call
  for c that takes its ids (settled), Gather, Sweep, and a Drain that hands c's
  ids to the message layer a second time. In the code, the back's drain skips
  every run whose slot has `acks_taken`.
- `MCRNAckPhaseBrokenTakeTwice.cfg`, `MessageOnce`. Defer(c); the same thread
  calls for c twice, and the second call takes c's ids again. The model found
  this one in the code: `rn_message_object_update_acks` took the ids on every
  call while the client's segment held them. Its condition now requires
  `!acks_taken`, and the differential test makes a second call from the same
  thread for about one client in eight.

## Not modelled

The fill phase's claim and kind check (`RNFillPhase` models the claim); a block's
contents and `process_block` itself (`SessionAcks`); the store's byte budget,
which spills exactly as the entry count does; and the code that binds this
model to the code's traces: no trace check yet exists for the ack phase.
