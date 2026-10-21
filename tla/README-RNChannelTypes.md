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
stays with the layer above). `Source(m)` is the API mode and `Code(m)` is the
wire twin, so every sender and receiver rule runs on the twin. The `promote`
scenario sends codes 1, 0 and 5 large (`Large(m)`, on the plan 1, 0, 5, 2)
and also runs the mixed plan 1, 3, 4, 2 with nothing large, so
`MCRNChannelTypes.cfg` checks both; `PromotedIsTwin` checks
the mapping and `PromotedNeverUnreliable` checks that a promoted code-0 or
code-5 send is delivered, never dropped as unreliable. A promoted code-1 send
follows code-4 sequenced semantics, including their permitted supersession.

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
| Capacities | Fixed send catalogue and held descriptors; real exhaustion disconnects with `MESSAGE_MEMORY_EXHAUSTED` | Owner exhaustion rule supersedes older CHANNELS wording about refused sends; every hold is bounded |
| Packet input | Fresh packet ids; transport replay suppression is a precondition | Repeated packet ids are refused by the transport before reliability dispatch |
| ACK | Only packet-id notification retires resend state | Same I8/I12 semantic meaning; no new message ACK protocol |
| Ordering/sequence wrap | Small nonwrapping counters | Wire half-range/wrap arithmetic is outside this model |
| Scheduling | First packet writes follow enqueue order; retransmits may overtake | CHANNELS §9.2 requires channel FIFO; priority never reorders sends within a channel |
| Payload | Opaque stable byte-record identity | Allocation, byte lengths, copy counts and view lifetime need implementation tests |
| Channels | Two channels in the bounded transport | Runtime range checks and up-to-65536 scaling are outside this bound |

The fair-transport transport permits one original plus one retransmission for each
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

## Evaluation and measurements

The recursive drain binds its receiver through a singleton mapping before
recursing. The mapping contains exactly the old drain result: for its sole
input, each branch and returned record is unchanged. This forces eager value
binding in TLC and avoids repeatedly expanding nested record updates. It
changes no transition, scheduling rule, loss action, bound or fairness.

All seven healthy scenarios and four reversed witnesses complete with the
pinned runner and JAR from `README.md`, on Linux with a 110-second budget
per shard. Positive cases use two workers; witnesses use one. They retain
all seven programs, the same arbitrary arrival and ACK timing, original
loss and retry behavior. The original completed scenarios retain their
exact generated/distinct counts after the evaluation change.

| Configuration | Generated | Distinct | Seconds | Expected result |
|---|---:|---:|---:|---|
| `MCRNChannelTypes.cfg` | 2058385 | 376853 | 65.946 | pass - |
| `MCRNChannelBrokenSequencedBackwards.cfg` | 49 | 32 | 0.659 | invariant SequencedNeverBackwards |
| `MCRNChannelBrokenHeapDrainNoReset.cfg` | 723 | 304 | 0.721 | invariant EpochWatermark |
| `MCRNChannelBrokenReliableTwice.cfg` | 53 | 35 | 0.663 | invariant ReliableExactlyOnce |
| `MCRNChannelBrokenHoldAllSequenced.cfg` | 321 | 157 | 0.682 | invariant BuffersBounded |
| `MCRNChannelTypesStale.cfg` | 171729 | 37065 | 4.343 | pass - |
| `MCRNChannelTypesBackwards.cfg` | 21971 | 5927 | 1.641 | pass - |
| `MCRNChannelTypesReset.cfg` | 56938 | 11595 | 3.617 | pass - |
| `MCRNChannelTypesHold.cfg` | 102358 | 21795 | 3.957 | pass - |
| `MCRNChannelTypesUnordered.cfg` | 253601 | 52469 | 5.385 | pass - |
| `MCRNChannelTypesChannels.cfg` | 99178 | 20995 | 3.708 | pass - |

Earlier failed measurements remain in the job evidence and historical ledger;
they are not passing results. The reset scenario originally exhausted its
110-second budget during recursive expression evaluation. Eager binding
completes the same bounded scenario without restricting its action graph.
