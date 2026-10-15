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
for appropriate RTO/map sizing; the model does not prove RTT measurement.
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

The final measured records and concrete trace interpretations are recorded
below after the Linux checks complete. Diagnostic timeouts and parse failures
are retained outside the checkout and are not acceptance evidence.
