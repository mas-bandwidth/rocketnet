# RNReceipts

`RNReceipts.tla` models receipt generation for types 5, 6 and 7. It is a
standalone module over transport packet events, with no dependency on
`RNChannel`. The channel draft is not on origin at staging; the interface
names are aligned with the local draft for later composition. This is a
model of the design in `docs/CHANNELS.md` 7.4, not a claim that the message
layer implementation exists.

## Interface and state

- `Send(t)` issues a serial before any packet write. All five nonreceipt
  types use the same receipt-state transition, so their `t` values coalesce
  in the state graph. A nonreceipt send is followed by two single-record receipt sends or one
  two-fragment receipt block.
- `WritePacket(n,pid)` associates a number/fragment with a transport packet.
  `smap` contains bounded `[id, nums, unrel]` entries, matching the channel
  draft. Reliable numbers remain in `Unacked` after map eviction or loss.
  Reliable retransmission is an abstract choice of any unacked number;
  scheduling/backoff is outside this model. Unreliable records write once.
- `OpenPacket(pid)` is an open by the remote transport. `ReceiveAck(pid)` consumes
  the transport's acked-packet-id event, and only that event updates
  `acked`. `PacketLost(pid)` supplies the transport lost-id event. A late
  ACK may follow loss; no message-level ACK or second ACK channel exists.
  ACKs of evicted map entries acknowledge no numbers. Duplicate/unknown
  events with no live transport slot are stuttering inputs.
- `Tick` ages sent, unacked numbers. `SweepLoss` scans eligible type-5
  descriptors after their receipt deadline or map eviction. The receipt
  deadline is the constant `ReceiptDeadline`, the endpoint's maximum resend
  delay (`message_resend_delay_max_ms`); it does not move with the adaptive
  resend delay `RTO`, and `ASSUME RTO <= ReceiptDeadline`. Map eviction is the
  one exception: an evicted entry can never be acknowledged, so it reports
  LOSS at once. Its retained outcome suppresses a late ACK. `Disconnect` represents the documented
  `RELIABLE_TIMEOUT` error when a sent, still-unacked reliable number
  reaches its first-send deadline, even if other traffic progresses.
- `EmitAck` emits a reliable receipt only when every number of the message
  is acked. Type 7 has the same receipt rule as type 6; receiver ordering
  is a channel responsibility. An unreliable send too big for one packet is
  promoted to its reliable twin: type 0 to effective type 2, type 1 to
  effective type 4, and type 5 to effective type 6. A promoted send therefore
  never emits LOSS. `EffectiveKind` maps all three source modes; the fixed
  workload exercises the type-5 promotion, and `PromotionTable` checks the
  twin mapping for every source mode.
- `outcomes` is a ghost history of generated events, separate from `ring`.
  A full ring drops its oldest event and increments `ringDropped`, as
  CHANNELS specifies. Draining or overflowing the ring does not allow a
  second outcome. Progress refers to generation or connection end, not a
  guarantee that an application which ignores a full ring reads every event.

Payload bytes are opaque. A record is represented by its serial and fragment
index, not by a message class, serializer or factory. All live structures have
fixed bounds. This does not prove an allocator implementation, region sizing,
pointer identity, bandwidth charging or the exhaustion disconnect path;
those are obligations of the implementation and other models. The receipt
ring's explicitly specified oldest-drop rule is not a payload-memory
exhaustion policy.

## Checked scope and properties

The fixed configuration explores types 5, 6 and 7, each with one and two
fragments. Single-record workloads use three API serials and two receipt
messages; block workloads use two serials and one receipt block. There is one transport-ID
slot, one sent-map entry and one ring entry. The reusable module exposes
packet/map/ring capacities as constants. One slot still permits the last
fragment to be acked before its predecessor, map eviction followed by a late
ACK, retransmission, serial isolation and receipt-ring overflow.

Each packet contains one number. Mixed reliable/unreliable packet loads,
several simultaneously live packet IDs, simultaneous receipt blocks,
nonreceipt packet traffic and
receiver application delivery are outside this measured bound. Serial 1
exercises issuance for all five nonreceipt types but has no transport write
in this receipt projection. Arbitrary `EvictMap` steps overapproximate map
pressure from other channel traffic. Only eviction needed to unblock a
new send/retransmission is weakly fair.
The one-slot environment may retain an evicted packet until its late ACK or
lost-packet retirement, and reuses an ID only when both map and transport
release it. There is no uint32 receipt-serial or uint64 packet-ID wrap proof,
and no claim about stale events across connection generations.

Exploratory two-ID runs and a one-ID run with two simultaneous blocks
exceed the 110-second budget. They are
retained as incomplete experiments, not accepted proofs. The committed
one-ID configuration checks the same properties without a state constraint
that prunes transitions or hides violations.

- `ExactlyOneOutcomePerReceiptSerial`: at most one generated outcome, only
  for an issued receipt serial. `ReceiptProgress` supplies the eventual half.
- `LossOnlyForUnreliableReceipt`: only effective type 5 emits LOSS; large
  type 5, and the promoted type 0 and type 1 twins, are reliable and excluded.
- `PromotedIsTwin` and `PromotedNeverLoses`: each promoted source maps to
  its declared twin (0 to 2, 1 to 4, 5 to 6) and never emits LOSS.
- `PromotionTable`: the twin mapping covers every unreliable source mode.
- `AckedMeansPacketOpened`: acknowledged numbers were transport-opened, and
  an unreliable ACKED receipt has a matched transport packet ACK.
- `BlockAckedOnlyWhenAllFragmentsAcked`: reliable receipt generation, including
  a promoted unreliable send, waits for all fragment numbers, independently of
  their index/arrival order.
- `ReceiptRingBounded`: receipt queue length never exceeds its capacity.
- `LossOnlyAfterDeadline`: a type-5 LOSS is generated only once its first
  number's age has reached `ReceiptDeadline`, or after its sent-map entry
  was evicted. The fixed configuration uses `RTO = 1` and
  `ReceiptDeadline = 2`, so an ACK after the resend delay and inside the
  deadline is reachable and must report ACKED; the mid instance has
  `ReliableTimeout = 1`, which caps age at 1, so its deadline is 1.
- `UnreliableNeverResent`: effective type 5 is written at most once.
- `ReceiptProgress`: every issued receipt send eventually generates an
  outcome or its connection ends. Fairness requires eventual transport-slot completion, allowing either
  packet ACK or loss/retirement; it never forces a lost packet to open or
  any packet to be ACKED. Persistent loss is included, so this condition
  is weaker than fair Link delivery. Fairness also applies to packet writes,
  eviction under actual send pressure, update ticks,
  receipt processing and timeout handling. No safety property relies on
  fairness. The reliable timeout is part of the design, not a spontaneous
  end transition added solely to satisfy liveness.

## Reversed witnesses

The witness configurations enable exactly one fault and check its named
property, plus `TypeOK`. They must exit 12 and identify that property.

- `BrokenLastIndexAck`: the seven-state trace chooses a two-fragment type-5
  message, promoted to effective type 6. After the two API sends, packet 0
  carries fragment `<<2,1>>`; open and packet ACK leave only that last index
  in `acked`. `EmitAck(2)` then violates
  `BlockAckedOnlyWhenAllFragmentsAcked`, with `<<2,0>>` still unacked.
- `BrokenEarlyLoss`: the six-state trace chooses single-record type 5.
  After the two API sends, packet 0 carries `<<2,0>>`; one `Tick` brings its
  age to `RTO = 1`, below `ReceiptDeadline = 2`. The broken sweep, the rule
  before this change (LOSS at the adaptive resend delay), then emits LOSS,
  violating `LossOnlyAfterDeadline`. The same order in the code (send, an
  update after the resend delay, then the ACK) is
  `test_receipt_late_ack_inside_deadline_is_acked`.
- `BrokenTwoOutcomes`: the ten-state trace chooses single-record type 5.
  The sends and packet write are followed by two `Tick` steps to the receipt deadline, transport
  open, then `SweepLoss(2)`. Its late transport ACK matches the retained
  map entry. `EmitAck(2)` appends ACKED after LOSS, violating
  `ExactlyOneOutcomePerReceiptSerial` even though the full one-entry ring
  now contains only ACKED. Generation history catches what the ring hides.
- `BrokenReliableLoss`: the seven-state trace chooses a two-fragment type-5
  message promoted to effective type 6. Fragment `<<2,0>>` is written;
  Two `Tick` steps reach the receipt deadline 2. The broken `SweepLoss(2)` emits LOSS for this reliable
  block, violating `LossOnlyForUnreliableReceipt`. Actual types 6 and 7 share this checked rule; the enumerated witness
  selects the promoted type first.

The measured traces and their exact state counts are recorded with the run
records and the task's raw TLC evidence; `proofs/tla/RUNS.tsv` holds the current counts. No witness uses an unrelated type
error or a different safety violation as a substitute for its named property.

## Reference behavior and differences

The receipt rules are in `standard/MESSAGES.md` and `docs/RELIABILITY.md`.
The design allocates a serial at API send, use packet ACKs rather than
application-consumption reports, never retransmit an unreliable receipt
record, and never emit reliable LOSS. Types 5/6/7 dispatch as base wire types
0/2/3; receipt bookkeeping does not introduce a wire ACK protocol. Large type
5 promotes to reliable, matching the reference.

The deliberate receipt differences are:

1. A block waits for all fragment numbers, rather than firing on the
   highest-index fragment alone (study 7.3).
2. Sent-map eviction explicitly produces unreliable LOSS in addition to
   the resend delay sweep; the reference receipt history is separate from its
   datagram history (study 6.1 and 7.3).
3. The fixed receipt ring drops/counts its oldest event when full. The
   reference puts five-byte notification messages on its output queue;
   this model exposes structured `(serial,status)` events instead.
4. Receipt identity uses the send descriptor's issued serial. The reference
   loopback path reads the global counter and may report the wrong serial
   (study 7.4). Loopback itself is not modeled here.
5. The transport owns packet IDs, loss detection and ack production. This
   module does not recreate the reference reliability layer's datagram
   ACK/NAK machinery. Reliable first-send timeout maps to the documented
   endpoint error/disconnect path.

Ordering, sequencing, wire encoding, bandwidth scheduling, allocator and
zero-copy implementation differences remain in their own design/model cards.
This receipt module does not claim to validate those unrelated semantics.

## Reproduction

Use the exact runner and JAR pin in `proofs/tla/README.md`, on a Linux bench. The
six registered cases belong to `rnreceipts`. Run shards 0 through 5 with
`--shards 6 --shard N --workers 2 --timeout 110s`, each in a private output
directory; witnesses use one worker automatically. Merge their measured
records with `tlacheck merge --keep proofs/tla/RUNS.tsv`, then require
`tlacheck groups --root . --stale` to print `[]`. Keep superseded records
verbatim in `RUNS-HISTORY.tsv`; never edit fingerprints or measurements.

Measured final outcomes (generated/distinct, elapsed seconds): fixed
67,144/23,800, 7.740; mid 3,602,016/792,811, 13.245; last-index 409/244,
0.951; two-outcomes 2,481/1,207, 1.104; reliable-loss 371/221, 0.705;
early-loss 134/85, 0.681. The fixed run completes temporal checking; the
four witnesses each exit 12 with precisely their declared invariant.

## Mid-scale instance

`MCRNReceiptsMid.cfg` is the same safety invariants with `MaxPid = 2` instead
of 0 (three transport packet IDs), a one-entry sent map and receipt ring, and
`ReliableTimeout = 1`. It passes: 3,602,016 generated / 792,811 distinct
states in 83.357 s, exit 0, and is a required case in CASES.tsv. The larger
packet-ID space lets an ACK, a loss notification, map eviction and a fresh
write coexist without reusing the only ID, which the one-ID instance cannot.

The deployed receipt ring and sent map are `max_clients`-sized (and bounded
by the connection's receipt capacity), not one entry. `ReceiptRingBounded`,
`ExactlyOneOutcomePerReceiptSerial`, the promotion table and the
all-fragments rule are local predicates over a serial, a number and a ring
position; a larger ring adds events and evictions but does not weaken them.
The mid instance checks two live packet IDs and one map/ring entry, so it does
not cover a split map with several simultaneously live receipt blocks; that
size-dependent case remains the exploratory bound recorded above. The
`uint32` serial wrap and `uint64` packet-ID wrap are still not proved.
