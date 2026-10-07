# Channels: bounded reliable ordered delivery

`Channels.tla` models one direction of one established connection, with `N`
ordering channels, per-message reliability, a sender queue, packet-to-message
ACK mapping, and per-channel receiver ordering holds. `MCChannels.cfg` selects
`N = 2`, `Window = 4`, and a two-copy link. TLC checks the finite configured
workload exhaustively, including unbounded retry/loss cycles. It is a protocol
abstraction, not a refinement proof of the C++ implementation or allocator.

## Source facts and boundaries

Read against origin/main `c4f9a8c`:

- `docs/CHANNELS.md:26-37`: fixed capacity, refusal when reliable storage is
  full, per-send reliability and a runtime channel count under the compile-time
  maximum. Here `Admit` is disabled when the queue is full; the application may
  try later. No transition grows a store.
- `docs/CHANNELS.md:87-91`: only unfragmented transport ACKs imply delivery to
  the peer's batch (P2); fragmented ACKs do not (P3); packet IDs do not repeat
  (P4); updates impose a causal boundary (P6).
- `docs/CHANNELS.md:269-292`: reliable numbering occurs on first write, with
  `next < floor + Window`. Unreliable sends consume no reliable number.
- `docs/CHANNELS.md:336-363`: packet ACKs retire referenced reliable messages;
  retries preserve the message number and ordering index.
- `docs/CHANNELS.md:395-459`: four counters per channel, per-record dispatch,
  ordering holds and a complete drain after every ordered delivery.
- `docs/RAKNET-RELIABILITY.md:35-47,55-65`: datagrams and messages have distinct
  identities; all eight modes are per send, numbered 0 through 7.
- `USAGE.md:119-124,277-291`: the transport informs and never resends; a missing ACK
  need not mean loss. The message layer owns retries.

A reliable record here occupies one **unfragmented** transport packet. The
transport's “packet ID ACKed when all fragments are ACKed” contract is not
strengthened into “the entire reassembly was retained.” Reliable blocks must
split at the message layer, as the design requires; block reassembly is not
checked here.

## State and steps

The `Traffic` sequence is an explicit finite workload. Each entry independently
names its channel and reliability; a channel has no reliability mode. The
committed workload sends ordered messages 1 and 2 on channel 1, then unreliable
message 3 and ordered message 4 on channel 2. The `SmallTraffic` operator can be
replaced for other per-send choices; this run does not enumerate every possible
workload or reliability assignment.

`accepted` is the admission cursor. `queue` holds accepted, unsent unreliable
messages and reliable messages until ACKed. `written` and `acked` are finite
history sets used to reconstruct the reliable number and sender window floor.
`packetMap` contains packet tickets `[message, generation]`; each ticket maps
to its `message` reference. A reliable first send installs a map entry, and
`Ack` frees only the reference belonging to that exact ticket. `LoseAck` may
lose any notification. `Expire` makes an unacknowledged message retryable.
Unreliable data is emitted once and removed from the queue immediately.

`orderedWrite` advances at admission. `orderedRead` advances at delivery.
`held` is the connection's shared bounded store, logically indexed by channel
and ordering index; it is not `N` independent allocations of size `Window`.
`Drain` releases consecutive ordered messages atomically in the receive update.
Duplicate reliable records are rejected by the connection-wide `receiveBase`
and bounded `receivedBits`, independently of channel dispatch. This is also
the state that future unordered reliable records need. Unreliable duplicates are rejected by the separate bounded
`transportSeen` replay abstraction. `delivered` is an observer trace: the
implementation does not retain all delivered message IDs or consult that trace.

All eight values appear in `DescriptorType`, including ordering, sequencing,
receipt and block fields. `sequencedWrite`, `sequencedWatermark` and
`heldSequenced` reserve the additional channel state. The six unsupported codes
have the explicitly disabled `StubSend` action and are refused by the workload
assumption. Their sequencing, receipt and block transitions are not implemented
or verified. The stubs are codes 1 (`UNRELIABLE_SEQUENCED`), 2 (`RELIABLE`),
4 (`RELIABLE_SEQUENCED`), 5 (`UNRELIABLE_WITH_ACK_RECEIPT`),
6 (`RELIABLE_WITH_ACK_RECEIPT`), and 7 (`RELIABLE_ORDERED_WITH_ACK_RECEIPT`).
Enabling them requires new transitions and checks, not silently treating them
as one of the two implemented modes.

The data path uses `Link.Send`, arbitrary `Link.Drop`, `Link.Duplicate` and
`Link.Reorder`. Receive can remove any copy: this fuses finitely many adjacent
reorders followed by delivery into one step. It toggles the link delivery pulse,
so a dropped copy cannot satisfy delivery fairness. Delay is nondeterministic
waiting, without a wall clock or a configured real-time bound. ACKs are an
unordered set of pending notifications, with independent loss; they are not
an additional byte-level UDP link.

## The finite packet identity abstraction

Only one transport attempt per message is live at a time. `Expire` waits until
that attempt has no link copies or pending ACKs. `TicketGenerations` controls
how many names represent fresh concrete packet IDs: the committed main and
window-2 runs use one; the wider safety run uses two alternating names. No live
ID is recycled. There is no bound on how often a reliable message can retry.

The one-name instance removes an unobservable distinction. After expiry,
`generation[m]` is the only state remembering the old reliable ticket name:
the packet map entry has been removed, link copies and ACKs have drained, and
the receiver retains reliable **message numbers**, not reliable packet IDs.
Changing the name on the next attempt cannot change any message-level action
or checked property. With two names, copies of two generations of the same
message can never coexist. All fairness conditions are grouped by message,
so renaming cannot make an unfair loss schedule fair. This is alpha-renaming
retired opaque IDs, not permission for a concrete transport to reuse IDs.
The two-name safety run retains the wider naming instance as a separate check.

This abstraction excludes overlapping retries, sent-map eviction while old
copies or ACKs remain in flight, and the associated stale-ACK races. It must
not be cited as verification of the full sent-map design in section 6.4.
First writes also follow global admission order, which is stronger than the
production scheduler's per-channel FIFO rule. Queue priorities and a proof
that the production scheduler preserves the necessary order remain separate.

## Checked properties and fairness

Safety is checked over **all** states reachable through `Next`; fairness does
not prune the reachable graph. `SafetySpec` also provides an explicitly unfair
specification for a separate safety run.

- `TypeOK`, `Bounded`: sender queue, outstanding reliable messages, packet map,
  receiver holds and receive-window bits are each at most `Window`; link copies
  are at most `LinkCapacity`. ACK/replay history is at most `2 * Len(Traffic)`, its explicit
  finite transport bound. The finite workload histories and observer trace are
  specification bookkeeping, not allocator storage. These checks do not prove
  byte-ring geometry, alignment, no-malloc behavior, or total process memory.
- `NoDuplicateDelivery`, `OrderedDelivery`: every delivered message appears
  at most once, and every ordered predecessor on its channel already appears.
  There is no ordering promise across channels or for unreliable messages.
- `NoSlotCollision`: live ordering holds cannot share a reliable-number slot
  modulo `Window`.
- `AckedMeansReceived`, `MapSound`, `DeliveredWereSent`: ACKs cannot invent
  peer receipt, live mappings name outstanding messages, and delivery cannot
  invent a send. Receipt may mean held, not yet delivered to the application.
- `EveryReliableDelivered`, `EveryReliableAcked`: every admitted reliable
  message eventually reaches the application and retires at the sender. The
  positive configs check the stronger `WorkloadCompletes`: eventually **all**
  configured reliable messages are in both histories. Those histories only
  grow, so this implies both per-message obligations, including messages
  admitted late because the queue was full. This uses one TLC temporal branch
  instead of six without changing the transition system or fairness.

`Spec` adds weak fairness for application admission and expiry, strong fairness
for each message's eligible send, receive, and ACK action. Receive fairness is
**fair loss by logical message**: offering a message infinitely often cannot
lose every copy forever. ACK fairness similarly excludes losing every ACK for
a repeatedly received message. Grouping by message, not by a finite attempt
number, expresses fairness across retries. Receive-any-position ensures that
a buffered copy is eligible regardless of reorder position. Fairness imposes
no delivery guarantee on an unreliable message whose only copy was lost.

The connection remains established; there is no crash, reset, malformed peer,
CONFIG negotiation or reliability timeout. Real-time “eventually” has no upper
bound. With finite disconnect timers, the concrete liveness promise must be
“delivered or connection fails,” not unconditional eventual delivery.

## Run and reproduce

Run on a Linux bench with Java and TLA+ tools 2.19, from `tla/`:

```sh
flock /tmp/stella-tlc-bench.lock \
  java -XX:+UseParallelGC -Xmx6g -cp /opt/tla/tla2tools.jar tlc2.TLC \
  -workers 4 -lncheck final -cleanup -config MCChannels.cfg MCChannels
```

`-lncheck final` defers temporal checking until graph construction finishes;
it does not disable liveness. `CHECK_DEADLOCK FALSE` permits the intended
completed finite workload to stutter. There are no state constraints, depth
limits or symmetry reductions. The main and no-retry configs use the explicit
bookkeeping projection described below. Results, including
negative controls, are recorded in `RUNS.tsv`. `MCChannelsPerMessage.cfg`
retains the initial two-name, six-branch formulation: its graph completed safety checking
at 471,553 distinct states, but final temporal analysis hit the 1,800-second run
limit. That run is recorded as incomplete, not a pass. The joint-completion
formulation is the committed main gate. `MCChannelsTwoTickets.cfg` retains
the two-name instance with joint completion; the committed main config uses
the one-name reduction explained above. `MCChannelsNoView.cfg` retains the
one-name run without a view: its 125,317-state safety graph completed, but its
temporal analysis also reached the 1,800-second limit.

`ProtocolView` omits only `dataSent`, the link's send-count bookkeeping. With
`Pattern = "arbitrary"`, `LostAtSend` is always false: this counter affects only
its own next value, never an endpoint guard, wire operation, or message-level
property. Equal projected states therefore have identical projected successors
and enabled fairness actions. Every `Send` changes the preserved wire; every
`Receive` changes the preserved pulse, so the projection neither makes a fair
action stutter nor confuses loss with receipt. All fairness and temporal labels
are evaluated on these same projected transitions. A projected behavior lifts
to a concrete one by reconstructing the counter from sends. Its omitted range
is independently inductive: initially 0; `NextCount` maps `0..Horizon` into
itself; all other actions leave it unchanged. The unprojected two-name safety
run checks that range with all other invariants. This is a bookkeeping-variable
view of the kind described in [Specifying Systems, section 14.3.3](https://lamport.azurewebsites.net/tla/book-02-03-04.pdf),
not an arbitrary state merge. The no-retry temporal mutation uses the same view;
window-2 and all other safety/mutation checks retain full states.

The additional configs run the same model: `MCChannelsWindow2.cfg` forces
window turnover with the same four-message workload;
`MCChannelsSafety.cfg` checks safety with no fairness;
`MCChannelsNoRetry.cfg`, `MCChannelsNoDedup.cfg`, and
`MCChannelsNoOrder.cfg` deliberately remove one mechanism. Each negative
control must fail its named property. A passing negative control is a broken
validation, not success.

The negative traces were replayed through the actions:

- No deduplication: duplicate the packet carrying unreliable message 3, receive
  both copies, and obtain `delivered = <<1, 2, 3, 3>>` (13 trace states).
- No ordering: send messages 1 and 2 on channel 1, receive 2 first, and obtain
  `delivered = <<2>>` while packet 1 remains on the wire (7 trace states).
- No retry: drop each first attempt, expire its mapping, then stutter forever
  with reliable messages still queued and `delivered = <<>>` (16 states plus
  the stuttering step). Fair loss cannot recover a message never sent again.

These are defects deliberately injected into the model, not findings against
RocketNet's C++ implementation.

This is not a proof for arbitrary channel counts, infinite traffic, wire-counter
wraparound, transport fragment reassembly, reliable blocks, byte-ring capacity,
multiple records per packet, mixed-record packet aggregation, zero-copy buffer
lifetimes, receipts, sequenced delivery, connection reuse, or implementation
performance. The narrower model gives the next layer a reproducible safety and
fair-loss liveness baseline; the focused RNWindow/RNRing/RNChannel models in
`docs/CHANNELS.md` still need their own checks.
