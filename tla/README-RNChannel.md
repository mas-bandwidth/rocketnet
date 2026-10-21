# RNChannel

RNChannel checks a bounded message-layer instance of `docs/CHANNELS.md` §§6–9.
It restates the relevant RNWindow actions and instantiates the unchanged Link
module in each direction. It does not add a message ACK: opening a transport
packet makes its ID ACKable, and a returned packet ACK marks every reliable
number in that packet's sent-map entry.

## Instance and abstraction boundary

The input contains four reliable ordered messages. Channel 1 carries message 0
(small), message 2 (two chunks), and message 3 (small). Channel 2 carries message
1 (small). Reliable numbers are respectively `{0}`, `{2,3}`, `{4}`, and `{1}`.
The two chunks consume one ordering index. One optional code-0 unreliable
message can share a small-message packet or travel by itself; a chunk travels
alone. The payload unit is an opaque distinguishing token. There is no payload
serialization, byte order, allocator layout, or zero-copy proof here.

`SendCapacity` bounds outstanding message records; `W` bounds unacked reliable
units and live receiver ring slots. The example has K=2 and C=2. Full-buffer
send attempts have an explicit `RefuseSend` transition, which does not advance
the accepted input. An environment retry can later succeed. This is the card's
capacity abstraction; allocator exhaustion/disconnection (R5) belongs to the
memory model and implementation.

`NumberRecord` is RNWindow's atomic whole-block reservation, after application
enqueue, in the packet builder. `WritePacket` commits first emissions in
reliable-number order; it can pack both initial small messages. Reservation
and emission are separate internal steps, so this is not a proof of the exact
instruction at which a code implementation assigns its number. The oldest
unacked number pins `UPrime` to the block's first number until all chunks are
ACKed, as in RNWindow. Packet-map eviction forgets references, never the
unacked reliable record. A resend uses a fresh packet identity and the same
reliable number.

Packet identities use bounded alpha-renaming: an ID is reusable only after it
is absent from both wires, the sent map, and pending ACKs. `seen` represents
transport replay suppression; it is reset only at that quiescent reuse. This
is not a proof of transport packet-ID wrap. Reliable-number wire encoding,
half-range reconstruction, and stale-copy age constraints remain RNWindow's
separate responsibility. This model uses mathematical reliable numbers and
checks receiver-ring modulo collisions; it does not silently claim a second
wire-wrap proof.

`received` is a finite ghost history from which the sliding `R` and `RBits` are
derived. It is not an unbounded implementation table. `live` is the actual
bounded receiver allocation; `chunks[message][index]` holds its payload units
and is cleared when that update's application deliveries are released.
The fixed message/index function domain is a bounded model representation,
not a claim that every message has a permanently allocated payload buffer.

## State and transitions

| State/action | Meaning and design section |
|---|---|
| `ordered_write`, `sequenced_write`, `ordered_read`, `sequenced_watermark` | Four coupled channel counters, §7.1. Ordered send and delivery reset their respective sequenced counter. |
| `Enqueue`, `RefuseSend`, `sentTrace` | Accept or refuse one application message; assign its ordering index at acceptance, §§7.1, 9.3. |
| `NumberRecord`, `Unacked`, `UPrime` | Late reliable numbering and whole-block window admission, §§6.2, 8.2; restates RNWindow `NumberRecord`. |
| `WritePacket`, `smap` | Small-message packing, one chunk per packet, retransmission and bounded packet-reference map, §§6.2, 8, 9.3; restates RNWindow `WritePacket`. |
| `OpenPacket`, `received`, `live`, `chunks` | Transport replay suppression, per-number duplicate suppression, ring claims and indexed chunk placement, §§6.3, 8.3; restates RNWindow `OpenPacket`. |
| `DeliverMessage`, `trace` | Whole-message and per-channel ordering gate; reset sequenced receive watermark, §§7.2, 8.3. |
| `FixBatch`, `SendAck`, `EndUpdate` | Drain all ready messages before publishing ACKs; keep returned payloads live until the next update, §6.3 P6. |
| `ReceiveAck` | Translate packet IDs into all reliable references; unknown/evicted packet IDs do nothing, §6.2; restates RNWindow `ReceiveAck`. |
| `DropUnreliableForBudget`, `uDropped` | A bounded code-0 budget-drop option, counted separately from transport loss; no reliable number or resend, §§9.1–9.3 and R6. |
| `waitedForRoom`, `roomReturned` | Ghost observation of a large message queued without room and a later transport ACK that returns room. |
| `GhostSequencedWrite`, `GhostSequencedRead` | Approved counter-only future-sequenced interference probe. No sequenced wire message or application delivery exists. |

The accepted input order is fixed, while channel delivery, packet composition,
loss, retries and chunk arrival order vary. This is bounded exploration, not
an arbitrary-length or arbitrary-channel theorem. Fixed input IDs make opaque
payload identity and duplicate delivery visible.

## Properties and fairness

`OrderedExactlyOnceInOrder` compares each channel's delivered sequence with a
prefix of its accepted sequence, including multiplicity. `WholeMessagesOnly`
checks completeness at delivery. `ChunksPlacedByIndex` compares every occupied
chunk destination and delivered chunk vector with the original opaque tokens.
`UnreliableAtMostOnce` depends on transport replay suppression; it does not add
a message reliability protocol. `BuffersBounded`, `TypeOK`, and
`NoSlotCollision` check the configured tables and live receiver slots.

`OrderedTraceUnchangedBySequenced` is a single-run ordered-prefix and counter
reset property. The ghost probes do not occur in either application trace;
the ordered transition checks its reset even after a probe changes a counter.
The healthy liveness case and the broken-reset case both enable one write
probe before the first accepted send and one read probe before packet opening.
This is a bounded initial-epoch perturbation, not arbitrary sequenced traffic.
This is not a relational two-execution equivalence theorem, nor a proof of
future sequenced delivery semantics. A missing reset has a real failing probe
transition despite the enabled wire types remaining only code 0 and
RELIABLE_ORDERED.

`EveryOrderedMessageDelivered` requires the complete finite accepted workload
to reach the application. `NoHeadOfLineDeadlock` checks the observed
wait/room-return episode leads to delivery of the large message and the small
message following it. Total finite-workload delivery entails that episode's
conclusion; the separate property makes its trigger and reversed witness
inspectable, rather than claiming logically independent liveness theorems.

Liveness uses RNWindow's compact fairness assumptions: fair enqueue and
builder admission, fair oldest-number retransmission including attempts that
survive the Link loss pattern, fair packet opening, application drain, update
turnover, and packet ACK transmission/reception. Infinitely many ACK sends
must yield useful ACKs before sent-map eviction. This last assumption stands
for appropriate resend delay/map sizing; the model does not prove RTT measurement.
The normal liveness case uses deterministic alternating packet loss in both
directions. Safety does not use fairness and permits arbitrary forward loss,
duplication and reordering. The safety configuration uses a TLC `SafetyView` that omits Link send counters
and delivery pulses only. Under the arbitrary pattern the counters affect no
loss or guard, and pulses only distinguish actions for temporal fairness. All
protocol state and wire copies remain in the view. Temporal configurations
retain the full state. The ACK wire has capacity one; sequential stale
or repeated packet ACKs and ACK loss remain possible, but simultaneous ACK
reordering is outside this bounded instance.

There is no kbps clock or complete priority scheduler. Fair resend opportunity
abstracts eventual reliable send-budget availability. Reliable messages are
never discarded for budget, and code-0 budget drop is explicit and counted.
No rate, latency, bandwidth-priority, zero-copy, or concrete memory-allocation
claim follows from this model. Those implementation obligations remain in the
design and their assigned cards. Neither `docs/CHANNELS.md` nor
`docs/TLA-INVENTORY.md` is changed here.

## Reversed witnesses and measurements

Measured with the pinned runner and JAR in [README.md](README.md), on Linux
2026-10-15, two workers for passing cases and one for counterexamples. Every
case is within its 110-second bounded shard. `PASS` for a broken case means
the expected invariant (exit 12) or temporal property (exit 13) failed.

| Configuration | Generated | Distinct | Seconds | Expected result |
|---|---:|---:|---:|---|
| `MCRNChannel.cfg` | 191,331 | 75,140 | 100.621 | pass (safety + liveness) |
| `MCRNChannelSafety.cfg` | 11,541,189 | 2,216,935 | 81.447 | pass (safety) |
| `MCRNChannelBrokenOutOfOrder.cfg` | 98,330 | 36,361 | 2.367 | invariant: `OrderedExactlyOnceInOrder` |
| `MCRNChannelBrokenTwice.cfg` | 841 | 376 | 0.757 | invariant: `OrderedExactlyOnceInOrder` |
| `MCRNChannelBrokenDropOnRetransmit.cfg` | 39,019 | 15,863 | 8.556 | temporal: `EveryOrderedMessageDelivered` |
| `MCRNChannelBrokenPartialChunk.cfg` | 75,941 | 27,679 | 2.020 | invariant: `WholeMessagesOnly` |
| `MCRNChannelBrokenChunkBlocksSmall.cfg` | 180,027 | 63,169 | 33.430 | temporal: `NoHeadOfLineDeadlock` |
| `MCRNChannelBrokenNoSequencedReset.cfg` | 18 | 15 | 0.679 | invariant: `OrderedTraceUnchangedBySequenced` |

The normal case uses W=2. The safety and out-of-order cases use W=4 so a
complete large message can overtake the missing first small message; W=2
would serialize that particular ordering episode and make its witness
vacuous. All cases use two send records, a one-entry packet map, and two
quiescently reusable packet IDs. The safety case retains forward capacity 2
for real duplicate/reorder transitions. These bounds still exercise sent-map
eviction, mixed packet references, partial block ACKs and receiver-ring reuse.

The final counterexamples are read as follows:

- **OutOfOrder — `OrderedExactlyOnceInOrder`:** message 0's packet is lost.
  Message 1 reaches channel 2 and is ACKed; both chunks of message 2 then
  arrive. The broken arrival gate delivers channel 1 trace `<<2>>` while its
  accepted trace begins with 0. This is precisely the missing predecessor
  hold in §7.2, with whole-block receipt from §8.3.
- **Twice — `OrderedExactlyOnceInOrder`:** reliable number 0 is written again
  under another packet ID. Its first copy is delivered, then the second
  packet is opened. Treating that already-received number as fresh releases
  message 0 again, producing `<<0,0>>` from one accepted message. Transport
  packet replay protection alone cannot replace §6.3's per-number duplicate
  suppression on retransmissions.
- **DropOnRetransmit — `EveryOrderedMessageDelivered`:** the first packet
  carrying number 0 is lost and the broken sender removes that number from
  resend eligibility. It remains unacked, but no oldest-number retransmit
  is enabled. The receive/process update loop continues forever without
  delivering it. This violates §6.2's retention of unacked records, even
  when a packet reference is lost or evicted.
- **PartialChunk — `WholeMessagesOnly`:** the first small messages are
  delivered and ACKed. Chunk 0 of message 2 (number 2) is sent and dropped;
  chunk 1 (number 3) arrives. The broken last-index gate appends message 2
  while only number 3 is present. §8.3 requires all indices, not arrival of
  the greatest index.
- **ChunkBlocksSmall — `NoHeadOfLineDeadlock`:** message 2 queues while there
  is insufficient reliable room. The bad early admission reserves all
  staging slots. Existing small packets can still drain; their ACK returns
  actual window room and sets `roomReturned`. The large block is then
  numbered, but its premature whole-ring reservation leaves no free staging
  slot for its first write. The following small message is also queued;
  the update loop repeats with neither delivered. §8.2 requires whole-block
  admission only when room is available, without consuming progress room
  while waiting. This witness reaches the property antecedent; it is not
  merely a generic transport stall.
- **NoSequencedReset — `OrderedTraceUnchangedBySequenced`:** the approved
  ghost write probe raises channel 1's sequenced counter to 1. The first
  ordered enqueue advances its ordered counter but omits the reset, leaving
  1 and making `resetOK` false. This exercises §7.1's reset obligation;
  no sequenced application message is delivered and no relational trace
  equivalence is claimed.

Additional private diagnostics retain exact source/config hashes and logs.
On the healthy model, negative invariants find a returned-room episode
(11,504 generated / 4,498 distinct), chunk 1 received before chunk 0
(333,704 / 83,989), and the two initial small records packed together
(231 / 123). A separate private mutation replaces indexed block placement
with next-arrival-position placement; `ChunksPlacedByIndex` fails
(331,074 / 83,455). These are nonvacuity/diagnostic checks, not extra committed
acceptance cases and not invented ledger rows.

Earlier diagnostics include parse/domain mistakes and over-budget larger
state spaces. In particular, the first duplicate witness encounters a
`SubSeq` domain error rather than an invariant violation; the final prefix
predicate explicitly checks the length before taking the prefix. Only the
final eight measured rows above count as acceptance evidence.

`tlacheck merge --keep tla/RUNS.tsv` adds the eight new measured rows. Every
pre-existing row and `RUNS-HISTORY.tsv` remains byte-for-byte unchanged, and
`tlacheck groups --root . --stale` returns `[]`. Existing unrelated bench
debt remains recorded; this card does not turn those historical timeouts into
passes.

To reproduce, build the runner at the pin in [README.md](README.md), then run
one bounded shard at a time with `--group rnchannel --shards 8 --shard N`
for N=0 through 7, `--workers 2 --timeout 110s`, the pinned JAR, and a private
output directory per shard. Merge the eight `RUNS.tsv` files with `--keep`,
and check `groups --stale`. No C++ changes, Go/C++ tests, sequenced wire types,
allocator proof, rate scheduler proof, or full-width wrap proof are part of
this card.

## Mid-scale instance

`MCRNChannelMid.cfg` is the safety configuration with a larger send window and
packet map: `W = 4` (from 2), `P = 2` (from 1) and `MaxPid = 2` so two live
packet references can leave room for a fresh retransmission ID. It keeps every
invariant the committed case lists: ordered exactly-once delivery, whole
messages, indexed chunk placement, unreliable at-most-once, bounded buffers,
no slot collision and the sequenced-reset trace. It uses the same alternating
loss pattern, with `SendCapacity = 1` so one reliable message is outstanding
at a time. It passes: 723,930 generated / 281,764 distinct states in 33.015 s,
exit 0, and is a required case.

The deployed channel window is `reliable_messages_in_flight` per channel, not `W = 4`.
The checked properties split into two kinds. The ordering, chunk-placement and
at-most-once rules are local to each accepted message, number and ring slot,
and their statements do not mention the window size; the mid instance changes
which reliable numbers share a ring position and still checks the same
predicates. Ring reuse and the send window's admission rule
(`n < UPrime + W`) are size-dependent: a wider window admits more numbers
before the oldest is delivered, and the mid instance exercises that wider
admission but not the deployed `reliable_messages_in_flight`. With `SendCapacity = 1` the
mid instance does not cover several reliable messages outstanding at once in
a `W = 4` window; `MCRNChannelSafety.cfg` covers `W = 4` with
`SendCapacity = 2` under arbitrary loss, so that dimension is checked in a
separate required case rather than in this one. Packet-ID wrap and half-range
sequence reconstruction remain outside this module, as recorded above.
