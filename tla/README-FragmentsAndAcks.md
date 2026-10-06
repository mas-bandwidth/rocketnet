# FragmentsAndAcks: fragment reassembly and the ack window over the lossy link

`tla/FragmentsAndAcks.tla` is the two machines the channels layer will build on:
the receiver's fragment reassembly (`rocketnet_reassembly_process_fragment`) and the
sender's ack window (`rocketnet_ack_endpoint_record_sent`, `open_aggregate`,
`process_block` and `apply_run`), with the session sequence and its burns between
them (USAGE.md "Sending"). Both directions cross `tla/Link.tla`, so every transition
is driven by a packet the path may drop, duplicate, reorder or delay.

`tla/MCFragmentsAndAcks.tla` and `tla/MCFragmentsAndAcks.cfg` are the small
instance. Every TLC run is a row in `tla/RUNS.tsv`.

## What the model covers

Two peers on one connection. A sends session packets to B; B sends ack packets to A.

State A owns (the sender: `rocketnet_ack_endpoint_t` and the session sequence):

- `nextSeq`: `send_sequence_session`, the next sequence `start_packet` spends.
- `highestSent`: the endpoint's `highest_sequence`, the highest sequence recorded.
- `sentRec`: the sent ring, `seq % SentRing` to `[seq, acked, hasPid, pid, agg]`.
- `aggs`, `numAggs`: the aggregate ring and its monotonic id.
- `batch`: the acked batch of packet ids the application drains at its update.
- `pending`: fragments sealed and recorded, waiting for the flush.

State B owns (the receiver: `rocketnet_replay_protection_t` and the reassembly set):

- `highest`, `opened`: the replay window as `highest_opened` and the set of opened
  sequences, with the width `W` applied by `AlreadyReceived`.
- `slots`: the reassembly slots, `payload_id % Slots` to
  `[pid, cnt, stride, finalBytes, finalStaged, received]`.
- `numDelivered`, `recvCount`: this update's delivered batch and drain budget.

Actions, one per operation and per outside event:

| Action | Who | Code |
|---|---|---|
| `SendPayload` | A's application | start, seal with a packet id, flush: one sequence |
| `StartFragmented(n)` | A's application | `fragment_plan`; count sequences, one aggregate, every fragment recorded before any is flushed (`client_transport.cpp:821-861`) |
| `TransmitPending` | A's flush | the next sealed fragment goes on the wire |
| `AbortPacket` | A's application | a sequence spent and given back by nothing (USAGE.md: abort, acquisition failure, a zero-returning single finish) |
| `SealFailFragmented(n, i)` | A's seal loop | the fragmented finish fails at fragment `i` after the block is reserved: every frame withdrawn, all count sequences burn, the records of `0..i-1` stay (`client_transport.cpp:841-847`) |
| `SendForged(h)` | a defective sender | a fragment with a header from `Forged` |
| `RecvPacket(p)` | B | `C2S!Deliver`; replay check and advance; a fragment through `ProcessFragment`, check by check in the code's order; a complete payload into the delivered batch or dropped `BATCH_FULL` (`fragment.cpp:432-446`) |
| `SendAck` | B | `S2C!Send` of the block `ack_block_build` makes: every opened sequence from the floor to `highest` |
| `RecvAck(b)` | A | `S2C!Deliver`; `process_block` over every named sequence from the highest down, as `apply_run`: unknown, retained window, deferral when the batch is full, surfacing, aggregate countdown |
| `UpdateA` | A's update | the acked batch is handed over and starts empty; the ack path ages |
| `UpdateB` | B's update | the delivered batch is handed over, the drain budget resets; the payload path ages |
| `C2SEnvironment`, `S2CEnvironment` | the path | `Link!Environment`: drop, duplicate, reorder, delay |

Ghosts record what the invariants need: packet ids surfaced, the sequences each
packet id was transmitted on, the honest sender's geometry per payload id, the burned
sequences, the withdrawn packet ids, every delivery's geometry, the payload ids
abandoned from a slot, and the flags `surfacedTwice`, `deliveredTwice`, `batchFull`,
`historyOverflow`.

### Invariants

| Invariant | STOP line | Holds |
|---|---|---|
| `AckBatchBounded` | the acked batch respects its capacity (the deferral discipline) | yes |
| `SurfacedOnce` | a packet id surfaces at most once | yes |
| `SurfacedOnlyWhenAllArrived` | the acked batch names a packet id only when every fragment arrived | yes |
| `BurnedNeverAcked` | a burned sequence is never acked as received; a withdrawn payload never surfaces | yes |
| `ReassemblyBounded` | reassembly state never exceeds its bound: placed bytes fit the arena, a staged final fits the staging buffer, the received set is inside the pinned count, no slot holds a complete payload | yes, honest and defective sender alike |
| `DeliveredWhole` | a payload is delivered whole: every delivery of an honest id has the sender's exact count, stride and final size | yes for the honest sender; see the forge run |
| `DeliveredOnce` | never twice | yes for the honest sender; see the forge run |
| `DeliveredConsistent` | a delivery's geometry is one the arena holds (the forge run's form of "whole") | yes |
| `CompleteIsDelivered`, `BatchFullNeverFires` | a fully received payload is delivered in the update it completed in | **no**: D1 below |
| `NoResurrection` | an id abandoned in a slot never occupies it again | **no**: F10 of the inventory, confirmed |

### Liveness

- `ArrivedSurfaces`: a packet id every packet of which B opened eventually surfaces in
  A's batch. The instance has no laps (`MaxSeq <= SentRing`, `MaxSeq <= W`), because a
  lapped record or a sequence below the block's floor is never spoken for again, by
  design (USAGE.md: "Missing a notification therefore does not prove that a payload
  was lost").
- `BatchDrains`: a packet id in the batch is recorded as surfaced.

Checked under `LiveSpec`: both ends keep updating, B keeps sending blocks, A keeps
taking them, the ack path is a wire (`Link!WireFair`), and blocks sent without bound
are delivered without bound (`AcksGetThrough`). `AcksGetThrough` is
`Link!EventualDelivery` stated once for the path rather than once per block value:
the per-block form quantifies over every subset of `1..MaxSeq`, and with sixteen of
them TLC's tableau for the negated property came out empty ("Temporal formula is a
tautology", the `live-tautology` run), which would have proved nothing. The path-level
form implies the per-block one when the link does not duplicate (every copy is
delivered at most once, and only finitely many blocks differ from the one B sends once
A's sends are exhausted), so the liveness runs set `Duplication = FALSE`; they also set
`Reordering = FALSE`, which the properties do not need and which keeps the liveness
state graph small. The reversed witness `LiveSpecNoLinkFairness` (the peers fair,
the ack path not: no `WireFair`, no `AcksGetThrough`, so `"arbitrary"` may drop every
block forever) fails `ArrivedSurfaces`, so the link's fairness is doing the work.
`Link!WireFair` alone is not a witness in this model: with `MaxDelay = 0` every copy
is deliverable the moment it is sent, so strong fairness on `Deliver` already defeats
the dropper (the `live-wf-small` run passes), which is why `AcksGetThrough` is stated
beside it rather than instead of it.

Nothing is assumed about the payload path: the transport never resends, so no
liveness property needs a fragment to arrive. A payload whose fragment is lost is
abandoned when a newer id claims its slot, and that is the design.

## What the model abstracts away

- Bytes are sizes. A fragment carries its byte count; the slot records the stride,
  the final size and which indexes are placed. Contents never alias in the model
  because placement is by index and stride, which is the code's own argument.
- The replay window is the set of opened sequences with the width `W`. The 64-bit
  word representation and its large-skip clear (inventory F1) belong to the
  replay-window module; with it, the block B sends can name sequences it never opened
  and `SurfacedOnlyWhenAllArrived` would be broken through B's blocks.
- Ack packets carry the block and no sequence of their own, so a duplicated block is
  applied twice. `process_block` is idempotent on an acked record, so this is the
  stricter adversary, not a weaker one.
- The block is never truncated by packet room (255 ranges, the 16-bit run cap).
- A's receive budget per update is unbounded; B's is `RecvPerUpdate`, the drain bound
  the delivered batch is sized from (`client_transport.cpp:222-229`,
  `server_transport.cpp:804-811`).
- Payload ids wrap at `IdMod` with the code's serial arithmetic (`Older`). The
  instance starts at `FirstPid = 7` of 8 so the second payload crosses the wrap.
- Time is two update actions and the links' `Age`; there are no timeouts here.
- rtt, statistics, keep-alives, the ack debt and the standalone-ack rule are not here.
- A's sends are serialised: a new payload starts only when the previous one's
  fragments are all on the wire (`pending = <<>>`). Fill threads interleaving is the
  packet-leases module's business.
- The defective sender (`Forged`) emits a fixed set of headers, not every header.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19 or later), from
`tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 32 -cleanup -config MCFragmentsAndAcks.cfg MCFragmentsAndAcks
```

The committed cfg is the honest sender with burns, both links `"arbitrary"` with
duplication and reordering, no latency (`MaxDelay = 0`: delay only affects order
here, and `Reorder` already permits any order), four sequences into a sent ring and a
window of four, two slots, payload ids `7, 1` of modulus 8 (the wrap, and one slot
shared so the second payload displaces the first), payload size 3 over 2-byte
fragments into a 5-byte arena, a drain of 2 packets per update and a delivered batch
of 1 (the code's `drain / 2`), an acked batch of 1 so a block that acks two packet
ids defers one.

The other runs are the same file with a few lines changed (`sed` edits, in
`RUNS.tsv` as the edit list):

| Run | Lines changed | Expected | Result on vision |
|---|---|---|---|
| committed | none | passes | 74,581,370 states, 9,318,983 distinct, 4 min 21 s, pass |
| c2s-none / first / alternate / burst | `C2SPattern` | passes | pass (5.35 M / 980 k / 189 k / 141 k distinct) |
| s2c-first / alternate / burst | `S2CPattern` | passes | pass (7.45 M / 5.77 M / 6.87 M distinct) |
| laps | `MaxSeq = 6`, `FirstPid = 6`, both patterns `"none"`, `Duplication = FALSE` | passes; the sent ring and the window lap, an old packet is refused by age, the history overflows | pass (11.05 M distinct, 21 min 31 s) |
| batch-full-code | `FirstPid = 6`, plus `CompleteIsDelivered`, `BatchFullNeverFires` | **fails**: D1 | fails, 13-state trace |
| batch-full-fix | as above with `DeliveredCap = 2` (`= RecvPerUpdate`) | passes | pass (9.15 M distinct) |
| no-resurrection | plus `NoResurrection` | **fails**: F10 | fails, 14-state trace |
| forge | `Forged <- MCForgedHeaders`, both patterns `"none"`, no duplication, no reordering, `Burns = FALSE`, `DeliveredConsistent` for `DeliveredWhole`, no `DeliveredOnce` | passes | pass (1.04 M distinct, 1 min 11 s) |
| forge-deliveredwhole / forge-deliveredonce | the forge edits with the honest-sender invariants kept | **fail**: the defective sender's own payload, see below | fail, 6-state and 11-state traces |
| live | `SPECIFICATION LiveSpec`, `MaxSeq = 3`, `Capacity = 1`, `Burns = FALSE`, `Duplication = FALSE`, `Reordering = FALSE`, `PROPERTIES ArrivedSurfaces BatchDrains` | passes | pass (8,486 distinct, 3 min 29 s) |
| live-wf | the live edits with `SPECIFICATION LiveSpecWireFair` | passes (not a witness here, see above) | pass |
| live-nolink | the live edits with `SPECIFICATION LiveSpecNoLinkFairness` | **fails**: the reversed witness | fails, 19-state lasso, every block dropped |

Two runs are in `RUNS.tsv` as what they were: the first liveness draft with
`Link!Fairness` per block value (vacuous, "Temporal formula is a tautology"), and the
`MaxSeq = 4` liveness instance, whose temporal check had not finished after 23 minutes
and was stopped; `MaxSeq = 3` is the liveness instance.

## What TLC found

Every counterexample below was replayed by hand through the functions it names.

### D1, defect: a fully received payload is dropped when the delivered batch fills

`rocketnet_reassembly_process_fragment` (`sdk/rocketnet_fragment.cpp:432-446`): a
completed payload is copied into the delivered mirror only if
`num_delivered < delivered_capacity`; otherwise it is counted `BATCH_FULL`, the slot
clears (`:448`), and the payload is gone. The capacity is sized by the argument in
`sdk/rocketnet_client_transport.cpp:222-229` and
`sdk/rocketnet_server_transport.cpp:804-811`, restated in the create function's
comment (`fragment.cpp:120-129`): "a payload takes at least two fragments, so one
update completes at most half the packets its receive path drains, which is what
makes BATCH_FULL a true must-stay-zero invariant".

The argument is wrong when payloads straddle an update boundary. TLC's trace
(`batch-full-code` run, `FirstPid = 6` so the two payloads use both slots): A sends
payload 6 (sequences 1, 2) and payload 7 (sequences 3, 4). B drains sequences 1 and 3
in one update: two slots each hold one fragment. B's next update drains sequences 2
and 4: sequence 2 completes payload 6 (`num_delivered` 1 of 1), sequence 4 completes
payload 7, the batch is full, payload 7 is dropped. The first run of the model found
the same shape with laps and a burn in the way (`MaxSeq = 6`).

Replayed in the code: `process_fragment` for the last fragment passes every check,
sets the bit, `num_received == fragment_count`, takes the `else` branch at `:440-444`,
bumps `batch_full`, and `clear_slot` at `:448` discards the bytes. At the shipped
sizes the same shape needs, on a client (`ROCKETNET_NUM_CLIENT_PACKETS = 1024`,
capacity 512, 8 slots): 8 payloads each one fragment short at the update boundary
plus 508 two-fragment payloads in the same update, 516 completions in 1024 packets.
On a server (`ROCKETNET_RECEIVE_PACKETS_PER_CLIENT = 16`, capacity `clients x slots`
= 8 per client): 8 one-short payloads plus 4 two-fragment payloads, 12 completions in
16 packets, 4 dropped. The bound on completions per update is
`slots + (drain - slots) / 2`, not `drain / 2`; `drain` itself is a safe capacity,
since every completion consumes at least one packet of the update. The
`batch-full-fix` run with `DeliveredCap = RecvPerUpdate` passes both invariants.

No code change on this card.

### F10, confirmed: an abandoned id re-occupies its slot

`rocketnet_reassembly_clear_slot` (`fragment.cpp:181-184`) zeroes `payload_id`, so
after a delivery the stale check at `:253` has nothing to compare against. TLC's trace
(`no-resurrection` run, 14 states): payload 7's fragment 1 (sequence 2) is reordered
behind payload 1 (sequences 3, 4; the same slot); payload 1's fragment 0 displaces 7
(counted `ABANDONED`), payload 1 completes and clears the slot, then the late
fragment 1 of payload 7 arrives and occupies the empty slot with `received = {1}`.
Replayed in the code: `:253` is skipped because `slot->payload_id == 0`, every other
check passes, `:371-382` installs id 7 again and stages the final. The inventory's
reading holds: no byte aliases (the slot was empty), the next newer id displaces it
and counts a second `ABANDONED`. Counter noise; the inventory's verdict stands as an
observation.

### The forge run: what a defective sender gets

With `Forged <- MCForgedHeaders` the sender may also emit a zero id, a count of one,
an index past the count, a count that differs from the first fragment's, a non-final
of the wrong size, a final larger than the stride, a geometry too large for the
arena, and a stale id behind the wrap. Every refusal fires; `ReassemblyBounded`,
`DeliveredConsistent` and the ack invariants hold. Two invariants written for the
honest sender do not, and TLC's traces say why both are the sender's own doing, not
the receiver's:

- `DeliveredWhole` (`forge-deliveredwhole` run): a forged final of id 7 with 2 bytes
  arrives before the honest fragment 0 fixes the stride at 2; the staged final fits
  the stride, the payload completes with final 2 where the honest plan says 1. The
  receiver cannot tell the two finals apart; the standard's pin discipline is about
  memory, not about which of a peer's two different finals is the real one.
- `DeliveredOnce` (`forge-deliveredonce` run): after id 7 is delivered and its slot
  cleared, the honest final of 7 arrives late and re-occupies the slot (F10), and a
  forged fragment 0 of 7 completes it a second time. A peer that sends two fragment
  sets under one id gets two deliveries.

The forge run therefore checks `DeliveredConsistent` in place of those two.

### Model errors fixed on the way

- TLA+ parses `x' = a \/ b` as `(x' = a) \/ b`. The first run stopped on "successor
  state not completely specified" in `StartFragmented`; every ghost flag assignment
  is now parenthesised.
- The first instance (`MaxSeq = 6`, two payload sizes, `MaxDelay = 1`) was too big
  for the bench's budget (11.5 million distinct states at depth 14, still growing when
  it hit D1); `MaxSeq = 4`, one size and `MaxDelay = 0` are the committed instance,
  and the laps live in their own run over a quieter link.
- `Link!EventualDelivery` per block value made the liveness check vacuous (above);
  `AcksGetThrough` replaces it for this model.
