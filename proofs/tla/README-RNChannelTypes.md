# Sequenced and unordered reliable message semantics

`RNChannelTypes` is a standalone finite message-stream model for codes 1, 2
and 4, with the ordered code-3 path needed to delimit sequencing epochs.
It follows `docs/CHANNELS.md` sections 7.1–7.3 and records the relevant
design choices below. It is not a composition proof
for `RNChannel`, `RNWindow`, a scheduler or the C++ implementation.

## Interface

- `Enqueue` assigns the message's `(ordering, sequencing)` from
  `ordered_write` and `sequenced_write`. Ordered enqueue increments the former
  and resets the latter; sequenced enqueue increments only the latter.
- `WritePacket(m, attempt)` records a fresh transport packet id carrying a
  stable message identity. Reliable number assignment happens at the first
  packet write, independently of the ordered/sequenced counters. Code 1 never
  takes a reliable number. Each packet carries one message in this bound.
- `OpenPacket(pid)` is a transport-delivered record, processed before any
  transport acknowledgment of that packet. Reliable duplicate filtering uses
  the stable message identity, not the packet id. A retransmission has a new
  packet id and the same reliable identity.
- `ReceiveAck(q)` takes a set of **transport packet ids**, resolves the sent
  map and clears reliable resend state. The bounded transport uses singleton
  batches. Redundant ACKs after a reliable identity is retired, and ACKs of
  unreliable-only packets, stutter: they have no message-state effect and
  their history is not retained. It never receives a message-level or application-delivery ACK.
- `LosePacket(pid)` is a transport loss input. It does not free reliable
  resend state. Unreliable originals may be lost and never resent.

The four counter names and the packet-input action names align with the
`RNChannel` module on the staged base. No module from it is imported or
changed: the approved standalone interface keeps this work independent.
Channel dispatch composes the models later.

## Promotion

An unreliable send too big for one packet's record is promoted to its
reliable twin: 0 to 2, 1 to 4, and 5 to the reliable code-2 path (the receipt
stays with the layer above). `Source(m)` is the API mode, kept in the queue
entry, and `Code(m)` is the code `Route(k, large)` chose at the send, the
code's nested conditional in `sdk/rocketnet_endpoint.cpp` (a large 0 or 5 to
2, a large 1 to 4, else unchanged); every sender and receiver rule runs on
`Code(m)`. The `promote` scenario sends codes 1, 0 and 5 large (`Large(m)`,
on the plan 1, 0, 5, 2) and also runs the mixed plan 1, 3, 4, 2 with nothing
large, so `MCRNChannelTypes.cfg` checks both. `PromotedIsTwin` checks that the
routed code is the twin and `PromotedNeverUnreliable` that a large send of a
promotable mode is routed to a reliable code. A promoted code-1 send follows
code-4 sequenced semantics, including their permitted supersession. Both can
now fail: `Broken = "NoPromotion"` routes every send unchanged
(`MCRNChannelBrokenNoPromotion.cfg` fails `PromotedNeverUnreliable`), and
`Broken = "PromoteSequencedToReliable"` routes a large 1 to 2
(`MCRNChannelBrokenPromoteSequencedToReliable.cfg` fails `PromotedIsTwin`).

## Types

### 1: UNRELIABLE_SEQUENCED

No reliable number or resend entry is assigned. At its current ordered epoch,
a message delivers only when its sequencing index reaches the watermark;
delivery sets the watermark to `s + 1`. Older messages are dropped. A message
ahead of the epoch competes for the channel's newest held value by `(o, s)`.

### 2: RELIABLE

The first arrival delivers once, without an ordering dependency. Later packet
arrivals carrying the same reliable identity are suppressed. Its sender keeps
resend state until a transport packet carrying that identity is acknowledged.
`ReliableExactlyOnce` checks both unique delivery and equality with the set of
arrived code-2 messages, so it is not merely an at-most-once assertion.

### 4: RELIABLE_SEQUENCED

The reliable-number duplicate rule runs first, then the same sequencing rule
as code 1. A stale or superseded code-4 message is still acknowledged by its
transport packet, and that ACK clears its resend state even if it never reaches
the application. This is the study's I12: acknowledged means transport arrival,
not application delivery. No sender-side superseding removes resend state.

### Code-4 blocks and the one in-turn rule

The `blocks` scenario sends each code-4 message as a block of two fragments
(`ArriveFragment`, in any order); a block is complete only when the fragments
counted since its anchor (`counted`, the code's `block_have`) include both.
`InTurnOneRule` states one rule for an in-turn sequenced message, block or
record: it is delivered as soon as it is complete and newer than the
watermark; a newer in-turn message supersedes an incomplete in-turn block, and
one no newer is superseded by it; nothing in turn is held as future state
(`InTurnNeverHeldAsFuture`). An in-turn block collects in `inTurn`, apart from
the one future-state slot; a held block still incomplete when its epoch opens
moves there (`Drain`). `InTurnDeliveredWhenComplete` is the observable half:
`missed` records an in-turn message, every fragment of which arrived in its
turn, that was `Deliverable` when complete and was not delivered.
`MCRNChannelTypesBlocks.cfg` checks the rule with every shared invariant.

`Broken = "BlockInTurnHeld"` is the code-4 dispatch of
`sdk/rocketnet_channel_sequenced.cpp` before the rule: an in-turn block is
checked against the watermark, then collected in the future-state slot and
compared with the held state there.

### Every sequenced refusal is counted

`released` holds every sequenced record that left the receiver without a
delivery, and `DroppedCount(r)` is its count; `refused` holds the records
dropped by a refusal. `SequencedArrivedAccounted` (m7.4, "nothing is
silent") requires every sequenced record that arrived to be in the delivered
trace, still held, refused, or released. The paths into `released`: a record
superseded by a newer one; an in-turn record older than the watermark
(`rn_sequenced_in_turn`, SEQUENCED_STALE); and, in `Drain`, a held record the
watermark refuses (`DrainStale`, the code's `Sequence < watermark` test) and
a held code-1 record meeting a spent unreliable allowance `Du`
(UNRELIABLE_DELIVERY_FULL). Into `refused`: a future record that is oversize
(refused before it is compared with the held record, so a refused newer
record leaves the older hold in place) or meets a full pool (HOLD_FULL,
after the stale and superseded comparison).
`DrainStale` is not reachable at the model's small indexes: a held record is
newer than the watermark of the epoch before, and the new epoch's watermark
starts at zero; the half-range case that reaches it is `RNSequencingWrap`'s
`OpenEpoch`. Its witness is therefore the in-turn branch:
`Broken = "InTurnStaleSilent"` drops an in-turn stale record without counting
it, and `MCRNChannelBrokenInTurnStaleSilent.cfg` fails
`SequencedArrivedAccounted`. (The former `DrainStaleSilent` witness reached
the invariant only through another path and is removed.)

Code-4 blocks are accounted too (MA018): a block counts once any fragment of
it has arrived, and every path that drops one releases it. Stale blocks (a
fragment of a closed epoch, an in-turn block behind the watermark, an in-turn
block still collecting when the next ordered record closes its epoch) count
SEQUENCED_STALE; blocks superseded by a newer block or record, in turn or in
the future-state slot, count SEQUENCED_SUPERSEDED. `MCRNChannelTypesBlocks.cfg`
now declares `SequencedArrivedAccounted`, and `Broken = "StaleBlockSilent"`
(`MCRNChannelBrokenStaleBlockSilent.cfg`) drops the stale blocks without a
count and fails it.

`OpenPacket` no longer ends the connection on a sequenced or unreliable
overflow: a full hold pool, an oversize future record and a spent `Du` are
dropped and counted, as the code does. Two receive-side overflows still end
the connection in the code, both invariant breaks the window bound rules
out: a full held-ordered index (`rn_held_insert`, RNHeldOwnership `Hold`)
and a reliable record meeting a full delivery table (the region exhausted;
the `Drain` comment in the module calls it an invariant break).

## Shared delivery and comparison

`DeliverMessage` is the common direct/held delivery function. Ordered delivery
advances `ordered_read` and resets `sequenced_watermark`; sequenced delivery
sets it to `s + 1`. `Drain` releases the current epoch's held sequenced message
before its held ordered successor, and repeats. Each iteration removes a held
record, so the finite recursion bound exceeds the maximum drain length.

`OrderedTraceUnchangedBySequenced` compares two actual values. The mixed
receiver produces its ordered trace through `Dispatch`, holds and `Drain`.
A separate ordered-only reference receives the same ordered arrival ids with
sequenced traffic erased; it independently computes the contiguous ordered
prefix from that received set. Its ordering rank counts earlier ordered
enqueues independently of the mixed sender's stamped ordering index. It never
uses the mixed receiver's read cursor,
watermark, hold state or delivery function. Their per-channel output traces
must match after every packet event.

Other checks cover nondecreasing sequencing within an epoch, no duplicate
sequenced delivery, lexicographic epoch/sequence progress, bounded storage,
ordered exactly-once prefixes, and transport-ACK causality/resend retirement.
`EpochWatermark` independently reconstructs the current epoch's watermark
from its delivered sequenced trace; it detects stale watermark leakage.

## Deliberate differences and abstractions

| Rule | Model/design choice | Notes |
|---|---|---|
| Ahead-of-epoch sequencing | Keep only newest `(o, s)` per channel | Older ahead-of-epoch values are superseded, never held (CHANNELS §7.3) |
| Held sequenced delivery | Same `s + 1` watermark as direct delivery | Study item 18's held path uses `s`, admitting a duplicate |
| Held ordered delivery | Reset watermark to zero on the same delivery path | Study item 19 omits reset and can wrongly discard new-epoch sequenced input |
| Capacities | Fixed send catalogue and held descriptors; a full send capacity refuses the start (Revision 1.1), a full or oversize sequenced hold drops and counts | Nothing on the send or sequenced receive path ends the connection; every hold is bounded |
| Packet input | Fresh packet ids; transport replay suppression is a precondition | Repeated packet ids are refused by the transport before reliability dispatch |
| ACK | Only packet-id notification retires resend state | Same I8/I12 semantic meaning; no new message ACK protocol |
| Ordering/sequence wrap | Small nonwrapping counters | Wire half-range/wrap arithmetic is outside this model |
| Scheduling | First packet writes follow enqueue order; retransmits may overtake | CHANNELS §9.2 requires channel FIFO; priority never reorders sends within a channel |
| Payload | Opaque stable byte-record identity | Allocation, byte lengths, copy counts and view lifetime need implementation tests |
| Channels | Two channels in the bounded transport | Runtime range checks and up-to-65536 scaling are outside this bound |

The fair transport permits one original plus one retransmission for each
reliable message, arbitrary arrival order, original loss, and delayed ACKs.
Retransmissions are not lost; weak fairness drains the finite actions. Thus
liveness is conditional on this explicitly bounded fair environment, not a
claim under permanent loss. A reliable-sequenced message need not be delivered
to the application. Memory exhaustion terminates the connection rather than
counting as delivery. No bandwidth-budget or fragment rule is proved here.

## Finite transport bounds

The control explores seven fixed four-message programs, not all possible type
sequences. They cover mixed 1/3/4/2 traffic, multiple sequencing indices in one
epoch, stale code-4 arrival after newer code-1 or ordered delivery, held ordered drain across three epochs, newest-only ahead holds, two
unordered reliable records, and traffic on two independent channels. Each
message has at most two distinct packet ids. Packet arrivals and ACK timing
are nondeterministic within that bound. Send capacity is four records; ordered
hold capacity is two; sequenced hold capacity is two globally and one per
channel. Historical traces/counters are specification observations, not a
runtime allocation design.

## Reversed witnesses

- `BrokenSequencedBackwards`: packet 3 delivers sequencing index 1, then
  packet 1 delivers index 0 in the same epoch. The application trace is
  `<<2, 1>>`; `SequencedNeverBackwards` rejects the second delivery.
- `BrokenHeapDrainNoReset`: messages 2 (sequenced) and 3 (ordered) arrive
  ahead of ordered message 1. Arrival of message 1 drains `<<1, 2, 3>>`,
  advances the ordered cursor to 2, but leaves watermark 1 from epoch 1.
  `EpochWatermark` requires zero in the new epoch. This models study item
  19's excessive filtering, not a backwards or duplicate-delivery claim.
- `BrokenReliableTwice`: original packet 1 and retry packet 2 carry the same
  code-2 identity before either ACK. Both deliver, producing `<<1, 1>>`;
  `ReliableExactlyOnce` rejects the second delivery.
- `BrokenHoldAllSequenced`: packets 3 and 5 carry successive sequenced
  messages ahead of missing ordered message 1. Keeping both leaves two held
  values on channel 1, violating the one-per-channel `BuffersBounded` rule.
- `BrokenBlockInTurnHeld`: block 1 (ordering 0) has its first fragment held
  in turn; block 4 (ordering 1, after the ordered message) arrives and, newer,
  replaces it in the one slot; block 1's second fragment is then superseded by
  block 4. Block 1 was complete in its turn and newer than the watermark but
  never delivered: `InTurnDeliveredWhenComplete` fails. Checked by hand
  against the code: the second fragment takes the hold path, `gap` is
  negative against block 4 and it returns STALE with SEQUENCED_SUPERSEDED.
- `BrokenInTurnStaleSilent`: in the backwards scenario the newer sequenced
  record (sequencing index 1) is delivered, the watermark becomes 2, and the
  older one (index 0) then arrives in turn and is dropped with no count.
  `SequencedArrivedAccounted` fails. Checked by hand against the code:
  `rn_sequenced_in_turn`'s stale branch counts SEQUENCED_STALE.
- `BrokenNoPromotion`: a large code-1 send is routed unchanged;
  `PromotedNeverUnreliable` fails at the first send.
- `BrokenPromoteSequencedToReliable`: a large code-1 send is routed to 2, not
  its twin 4; `PromotedIsTwin` fails at the first send.

## Evaluation and measurements

The recursive drain binds its receiver through a singleton mapping before
recursing. The mapping contains exactly the old drain result: for its sole
input, each branch and returned record is unchanged. This forces eager value
binding in TLC and avoids repeatedly expanding nested record updates. It
changes no transition, scheduling rule, loss action, bound or fairness.

All eight healthy scenarios and the reversed witnesses below complete with the
pinned runner and JAR from `README.md`, on Linux with a 110-second budget
per shard. Positive cases use two workers; witnesses use one. They retain
all seven programs, the same arbitrary arrival and ACK timing, original
loss and retry behavior. The original completed scenarios retain their
exact generated/distinct counts after the evaluation change.

| Configuration | Generated | Distinct | Seconds | Expected result |
|---|---:|---:|---:|---|
| `MCRNChannelTypes.cfg` | 2168240 | 400234 | 76.097 | pass - |
| `MCRNChannelBrokenSequencedBackwards.cfg` | 49 | 32 | 0.680 | invariant SequencedNeverBackwards |
| `MCRNChannelBrokenHeapDrainNoReset.cfg` | 723 | 304 | 0.744 | invariant EpochWatermark |
| `MCRNChannelBrokenReliableTwice.cfg` | 53 | 35 | 0.648 | invariant ReliableExactlyOnce |
| `MCRNChannelBrokenHoldAllSequenced.cfg` | 321 | 157 | 0.670 | invariant BuffersBounded |
| `MCRNChannelBrokenBlockInTurnHeld.cfg` | 366 | 210 | 0.702 | invariant InTurnDeliveredWhenComplete |
| `MCRNChannelTypesStale.cfg` | 171729 | 37065 | 5.208 | pass - |
| `MCRNChannelTypesBackwards.cfg` | 21971 | 5927 | 1.976 | pass - |
| `MCRNChannelTypesReset.cfg` | 56938 | 11595 | 4.734 | pass - |
| `MCRNChannelTypesHold.cfg` | 102358 | 21795 | 5.164 | pass - |
| `MCRNChannelTypesUnordered.cfg` | 253601 | 52469 | 6.569 | pass - |
| `MCRNChannelTypesChannels.cfg` | 99178 | 20995 | 4.774 | pass - |
| `MCRNChannelTypesBlocks.cfg` | 31046 | 9594 | 2.266 | pass - |
| `MCRNChannelBrokenInTurnStaleSilent.cfg` | 49 | 32 | 0.762 | invariant SequencedArrivedAccounted |
| `MCRNChannelBrokenNoPromotion.cfg` | 3 | 3 | 0.847 | invariant PromotedNeverUnreliable |
| `MCRNChannelBrokenPromoteSequencedToReliable.cfg` | 3 | 3 | 0.754 | invariant PromotedIsTwin |

The rows from `MCRNChannelTypesBlocks.cfg` down were measured on 2026-11-13
(aws-mut-3, a shared bench, so the seconds are loaded) after the receive side
stopped ending the connection and began refusing an oversize or full future
record before comparing it with the hold: the blocks scenario reaches a few
more states. The other rows keep their counts.

Earlier failed measurements remain in the job evidence and historical ledger;
they are not passing results. The rows below were measured with the blocks
scenario in the module; the existing scenarios keep their counts, since no
fragment is enabled outside it. The reset scenario originally exhausted its
110-second budget during recursive expression evaluation. Eager binding
completes the same bounded scenario without restricting its action graph.

## Revision 1.1: a full send capacity refuses

Standard rule: MESSAGES.md m1.7. The action `MemoryExhausted` (the
application's send finding the send capacity full ended the connection with
MESSAGE_MEMORY_EXHAUSTED) is replaced by `Refuse`: the start is refused and
counted and nothing of the connection changes. (The receive side's overflow
in `OpenPacket` kept its end here until the model followed the code, which
drops and counts it.) `Broken = "SendKick"` is the reversed witness,
v1.0's end from the send path; the ghost `sendEnded` records an end a send
caused. The invariant `SendNeverEnds == ~sendEnded` is added. The code that
cites this step is the start's refusal in `sdk/rocketnet_endpoint.cpp`
(`RNChannelTypes.tla Refuse`); revision 1.0's citation of `MemoryExhausted`
named a step the code no longer took.

| Case | Expected | Exit | Distinct |
|---|---|---:|---:|
| `MCRNChannelTypesRefuse.cfg` (send capacity 2, every safety invariant and `SendNeverEnds`) | pass | 0 | 61 |
| `MCRNChannelBrokenSendKick.cfg` | `SendNeverEnds` | 12 | 5 |

`MCRNChannelTypes.cfg` keeps its invariants and liveness property. Its cost was the liveness check of `WF_vars(Next)`: TLC evaluated `ENABLED <<Next>>_vars` on every state and `<<Next>>_vars` on every edge (on vision with the record JVM flags: 2 min 56 s with the property, 50 s without). `Spec` now writes the fairness out by its definition with `StepPossible` for the enabledness and `<<TRUE>>_vars` for the step, both exact (`StepPossibleIsEnabled` checks the first in five configurations); the state graph is unchanged at 2,168,240 generated and 400,234 distinct, and the control passes in 79.8 s.

Witness trace, read by hand (5 states): two sends fill the send capacity of
2, the third offered send takes `Refuse` and, with `SendKick`, sets
`connected = FALSE`, `reason = "MESSAGE_MEMORY_EXHAUSTED"`. The code before
revision 1.1: a failed send allocation exhausted the region and the endpoint
ended the connection with that reason. The code now returns no pointer from
the start and counts the refusal in `send_refused`
(`rn_endpoint_start`); a start while the receive side has exhausted
the region still ends the connection.

## Revision 1.1.2: the refused start in the code's order

Revision 1.1's `Refuse` was `UNCHANGED vars` and the ghost `sendEnded` behind
the invariant `SendNeverEnds` was never written on the passing instance (a
probe `[][Refuse => UNCHANGED vars]_vars` passed on
`MCRNChannelTypesRefuse.cfg`, 61 distinct). The ghost is removed.

- Every start (`Enqueue(cap)` or `Refuse(cap)`, `StartAny`) takes the
  transport cap `cap` from `TransportCaps` and is written in the order of
  `rn_endpoint_start` (`sdk/rocketnet_endpoint.cpp`): an unreliable start
  writes `max_payload_bytes` (441) before the over-max check (444) and before
  the room refusal; a channels start finding no room counts `send_refused`
  (466; the unreliable path 491). `maxPayload`, `sendRefused` (mod 3) and the
  ghost `calls` (every start, so a refused start is never a stutter) are the
  variables a start may change; `Kept` is every other variable.
- `RefusalLeavesStateUnchanged`: a start with the send capacity full (named by
  the state before the step) leaves `Kept` as it was, moves `sendRefused` by
  one, and changes `maxPayload` only for an unreliable start
  (`NextCode \in UnreliableSources`). That write is the one change a refusal
  makes outside the count, and it is harmless: its readers are the finish
  (572), which runs only after a start that succeeded and wrote the same
  field, and `rn_endpoint_send` (961, used by the tests), which passes it as
  the next start's cap.
- `SendNeverEnds` is an action property read on the resulting state: every
  start leaves the connection connected.
- `StepPossible`'s refusal disjunct names the refusal's guard
  (`Len(queue) = SendCapacity /\ Len(queue) < MaxMessages`);
  `StepPossibleIsEnabled` checks it against `ENABLED`.
- Reversed witnesses: `Broken = "CountersAtStart"` advances the sequence
  counters at the start (the code assigns them in `rn_channels_finish`,
  `sdk/rocketnet_channel.cpp`; `rn_channels_start` writes nothing
  before the take); `Broken = "StaleStepPossible"` drops the refusal
  disjunct.

| Property | Fails when | Witness |
|---|---|---|
| `SendNeverEnds` | a refused start ends the connection | `MCRNChannelBrokenSendKick.cfg` |
| `RefusalLeavesStateUnchanged` | a refused start advances the send counters | `MCRNChannelBrokenCountersAtStart.cfg` |
| `StepPossibleIsEnabled` | `StepPossible` omits the refusal | `MCRNChannelBrokenStaleStepPossible.cfg` |

| Case | Expected | Exit | Distinct | Seconds (2 workers, vision) |
|---|---|---:|---:|---:|
| `MCRNChannelTypes.cfg` (`TransportCaps = {1}`) | pass | 0 | 400234 | 73.3 |
| `MCRNChannelTypesRefuse.cfg` (`TransportCaps = {1, 2}`) | pass | 0 | 345 | 1.4 |
| `MCRNChannelBrokenSendKick.cfg` | `SendNeverEnds` | 13 | 8 | 1.1 |
| `MCRNChannelBrokenCountersAtStart.cfg` | `RefusalLeavesStateUnchanged` | 13 | 8 | 1.1 |
| `MCRNChannelBrokenStaleStepPossible.cfg` | `StepPossibleIsEnabled` | 12 | 110 | 1.1 |

CountersAtStart's trace, checked by hand: two sends fill the send capacity;
the third offered send, a sequenced one, is refused and advances
`sequenced_write` from `<<0, 0>>` to `<<1, 0>>`. In the code the counters are
assigned in `rn_channels_finish` (`sdk/rocketnet_channel.cpp`), after a
start that succeeded; `rn_channels_start` (1325) checks the pending start,
the type and the exhaustion and then takes, writing nothing before the take,
and `rn_endpoint_start` returns at 473 on a refusal.

## Revision 1.2: a refused code-1 record leaves the channel unchanged

Standard rule: MESSAGES.md m6.3. A code-1 record ahead of its epoch with no
free hold slot (Ru) is dropped as `MESSAGE_SEQUENCED_HOLD_FULL`, and one in
turn when the update's unreliable allowance (Du) is spent is dropped as
`MESSAGE_UNRELIABLE_DELIVERY_FULL`; in both the channel's state is unchanged.
The code (`rn_channels_dispatch_sequenced` and `rn_sequenced_in_turn`,
`sdk/rocketnet_channel_sequenced.cpp`) released the channel's code-4 hold or
in-turn block as superseded and only then refused the record: an accepted
(ACK-eligible) code-4 message was lost, and an older message could be
delivered in its place. Before this revision the model had no such refusal
(the hold pool and the allowance are not modelled), so no configuration could
reach it.

What each piece of evidence carries. The model carries the order of refusal
and release only: the refusal is an environment boolean (the pool and the
allowance themselves are not modelled), its indexes are small integers with
no 24-bit arithmetic, and its ghosts record what the model's own actions do,
so it is not a proof of the C++ composition and cannot see a clear the code
makes without the model's action. The unit tests in
`tests/test_unit_channel_reliable_sequenced.cpp` carry the code: the
descriptors and payloads actually kept, the counters, and the later delivery
of the kept message (`test_hold_full_code_1_keeps_the_accepted_code_4_hold`,
`test_allowance_full_code_1_keeps_the_in_turn_block`), and the 24-bit half
range (`test_stale_code_1_at_the_half_range_keeps_the_in_turn_block`). They
drive the receive window and the dispatch in memory: they show ACK-eligible
state (accepted by the window, so the transport acks its packet) preserved,
not an ack exchange observed on a link.

- The `refused` scenario runs the blocks plans and `<<3,4,1,2>>` (a future
  code-4 block, then a newer future code-1 record). `OpenPacket(pid, full)`
  may refuse a code-1 record (`Refusable`): the pool and the allowance are
  spent by other channels and codes, so the environment chooses. Every other
  scenario has `full = FALSE` only and keeps its counts.
- `Dispatch` refuses before it releases: in turn, after the stale and
  superseded checks; ahead, after the not-newer check and only when no slot
  would be freed (releasing a code-1 hold frees the slot the record takes; a
  code-4 hold or no hold frees none).
- Ghosts: `refused` (the dropped records) and `superseders` (records that
  released the hold or in-turn block, recorded in this scenario only).
  `SequencedArrivedAccounted` counts a refused record as accounted.
- `SupersederKept`: no superseder is refused. Reversed by
  `Broken = "ReleaseBeforeRefuse"`, the code before the fix.
  `RefusalLeavesChannelUnchanged` (an action property, in the passing
  configuration) states m6.3 on the channel's own state: a refusing step
  changes no hold, in-turn block, release or delivery.
- Out of reach here: the stale drop at the 24-bit half range (the model's
  indexes are small; `RNSequencingWrap` owns the widths), covered by
  `test_stale_code_1_at_the_half_range_keeps_the_in_turn_block`; a refused
  record meeting a code-1 hold (each plan has one code-1 message; the code's
  slot return there is unchanged); and the later delivery of the kept hold,
  covered by the two other unit tests named above.

| Case | Expected | Exit | Generated | Distinct | Seconds (2 workers, bench-xdp-9) |
|---|---|---:|---:|---:|---:|
| `MCRNChannelTypesRefused.cfg` | pass | 0 | 114613 | 28825 | 2.8 |
| `MCRNChannelBrokenReleaseBeforeRefuse.cfg` | `SupersederKept` | 12 | 162 | 92 | 0.8 |

(Remeasured 2026-11-13 on aws-mut-3 after the oversize refusal and the drain's
`Du` refusal joined the model; 88553 / 24565 and 150 / 92 before.)

ReleaseBeforeRefuse's trace, checked by hand: the first fragment of the
in-turn code-4 block (message 1, ordering 0, sequencing 0) arrives and is in
`inTurn`; the newer in-turn code-1 record (message 2, sequencing 1) opens with
the allowance spent; `inTurn` is emptied and message 2 is refused. The code at
5a2dbf403 does the same: `rn_sequenced_in_turn` released `block_in_turn` and
counted SUPERSEDED, then `rn_sequenced_room` refused the record
(`test_allowance_full_code_1_keeps_the_in_turn_block` is this trace).
