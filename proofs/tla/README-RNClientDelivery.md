# Client payload batch across the end of a connection

`RNClientDelivery.tla` models the client transport's payload batch
(`received_packets` in `sdk/rocketnet_client_transport.cpp`) with the code's
layout: slots `0..Cap-1`, the counts `num` (`num_packets`) and `idx`
(`receive_index`), and the readable entries `idx..num-1`. Payload ids never
repeat, so a payload offered twice, or a stale slot exposed, is visible.

| Action | Code |
|---|---|
| `Begin` | `rocketnet_client_transport_update` clears both counts before it processes a packet; the bytes stay |
| `RecvPayload` | `rocketnet_client_process_packet`: a payload opened while connected is counted `CLIENT_PAYLOAD_DELIVERED` and placed at slot `num_packets` in one step; it takes one packet of the update's budget (environment action) |
| `RecvOther` | a processed packet that places no payload (an ack, a keep-alive, a fragment that completes nothing): one packet of the budget, no slot (environment action) |
| `EndInUpdate` | the server's disconnect opens, or `rocketnet_client_protocol_update` finds a deadline passed (environment action) |
| `Return` | the end of update: `rocketnet_client_connection_reset` when this update took the connection from live to ended |
| `Read` | `rocketnet_client_transport_receive_packet`: the cursor copies out the entry at `receive_index` |
| `Disconnect` | `rocketnet_client_transport_disconnect` between updates: the burst, then the same reset |

`CountedImpliesHanded`: every payload counted delivered was returned to the
caller, is still readable, or was offered and left unread until the next
update cleared the batch. `OfferedOnce`: the readable entries are counted and
neither returned nor passed, and nothing is returned twice.
`SlotForNextPacket`: inside an update the batch holds no more entries than
the packets processed, and while the budget allows another packet the batch
has a slot for its payload. `DeliveredReachesCaller` (under fair `Read` and
`Return`): every payload counted delivered reaches the caller.

`Budget` is `ROCKETNET_CLIENT_UPDATE_PACKET_BUDGET` and `Cap` the batch's
slots. The code sizes the batch from the budget and two `static_assert`s
beside `rocketnet_client_received_packets_t` hold it there: one exposed
receive buffer fits the budget, and the batch holds one payload per packet
of the budget. The module's `ASSUME Budget <= Cap` is those asserts; only
the `countbeforeplace` witness is checked without it.

| Case | Expected |
|---|---|
| `MCRNClientDelivery.cfg` (Cap 3, 4 payloads, a caller that drains every batch) | pass, 176 distinct states |
| `MCRNClientDeliveryLazy.cfg` (the caller may leave entries unread) | pass, 902 distinct states |
| `MCRNClientDeliveryBrokenEndWipes.cfg` (the reset zeroes both counts) | `CountedImpliesHanded` violated |
| `MCRNClientDeliveryBrokenEndWipesLive.cfg` (the same, under fairness) | `DeliveredReachesCaller` violated |
| `MCRNClientDeliveryBrokenNoBeginClear.cfg` (update does not clear the counts) | `OfferedOnce` violated |
| `MCRNClientDeliveryBrokenCountBeforePlace.cfg` (Budget 4 over Cap 3, counted before the slot is found) | `CountedImpliesHanded` violated |
| `MCRNClientDeliveryBrokenBudgetOverBatch.cfg` (Budget 4 over Cap 3) | `SlotForNextPacket` violated |

## The counterexamples against the code

`BrokenEndWipes`, five states: `Begin` (num 0), `RecvPayload` (slot 0 holds
payload 1, num 1, counted {1}), `EndInUpdate` (ended), `Return` (the reset sets
num 0, idx 0). Counted {1}, nothing readable, nothing returned. In the code:
update clears `received_packets` at its start, `rocketnet_client_process_packet`
writes the payload at index `num_packets` and the protocol counts
`CLIENT_PAYLOAD_DELIVERED` (`rocketnet_client_protocol.cpp`), the server's
disconnect record in the same batch takes the protocol to disconnected, and the
reset that ran after the receive loop set `num_packets` and `receive_index` to
zero, so `rocketnet_client_transport_num_payloads` read 0. The unit case
`test_client_payload_delivered_in_ending_update_reaches_caller` reproduces this
trace on the loopback transport and goes red with that reset restored.

`BrokenNoBeginClear`, five states: `Begin`, `RecvPayload` (payload 1),
`Return` (connected), `Begin` without clearing: payload 1 is counted as passed
while it is still readable. In the code the clear at the start of update is
what keeps a batch kept through a connection's end from being offered by the
next update; the same unit case checks that the update after the end offers
nothing and counts nothing.

`BrokenCountBeforePlace`, six states: `Begin`, then four `RecvPayload`. The
first three fill slots 0..2; the fourth finds `num = Cap`, is counted and is
not placed. This is the code before the batch was tied to the budget:
`rocketnet_client_process_packet` handed the protocol a stack overflow buffer
when `num_packets` had reached the batch, the protocol counted
`CLIENT_PAYLOAD_DELIVERED`, and the placement test `payload_bytes > 0 &&
payload` skipped it. `BrokenBudgetOverBatch` stops one step earlier, at the
state where a fourth packet is allowed and no slot is left. In the code that
state cannot be built: changing the budget past the batch fails the
`static_assert`s at compile time.

## Scope

The handshake is abstracted to a live connection; payloads open only while
connected. A packet places at most one payload: an opened payload, or the
payload its fragment completed (a packet is one type, and the code places
them in one `if`/`else if`). Reassembled payloads join the batch the same
way and are not modeled separately; their bytes live in the
reassembly's delivered arena, which `rocketnet_reassembly_set_reset_connection`
does not touch and the next update's `rocketnet_reassembly_set_begin_update`
clears. A transport runs one connection, so no later connection reads the
kept batch; `ClientSession.tla` abstracts the batch as a per-connection field
that the next connection never observes, which holds because the next connect
runs on a new transport generation.
