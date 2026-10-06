# Channels: one connection's message channels over the lossy link

`tla/Channels.tla` is layer 3 of [docs/CHANNELS.md](../docs/CHANNELS.md), the
reliable window (6.1 to 6.5), together with the per-channel state of layer 4
(7.1, 7.2) that the window's safety argument rests on. It is written before the
reliable code, so that the code is built to the model and not the other way
round. One direction of one connection: a sender with `N` ordering channels,
the wire (`tla/Link.tla`: loss, duplication, reordering, delay), a receiver,
and the transport's ack path back to the sender.

`tla/MCChannels.tla` with `tla/MCChannels.cfg` (safety) and
`tla/MCChannelsLive.cfg` (liveness) are the small instance. Every TLC run is a
row in `tla/RUNS.tsv`.

## What the model covers

Messages are sent with a reliability chosen per send, RakNet's eight values
(docs/RAKNET-RELIABILITY.md §2), on a channel chosen per send. The application
may send only the reliabilities in `Enabled`, which is `{0, 3}` in the first
version (CHANNELS.md 7.5): UNRELIABLE and RELIABLE_ORDERED. The other six are
stubs whose state is already in the model; see "What the stubs are" below.

State the sender owns (CHANNELS.md 6.1, 7.1, 9.1):

- `desc[m]`: the reliability, channel, ordering index and sequencing index of
  message `m`. The indices are taken at send with RakNet's coupling: an ordered
  send takes `ordWrite[c]` and increments it, and zeroes `seqWrite[c]`.
- `uq` and `cq[c]`: the send queues, the unchanneled one for codes 0 and 2 and
  one FIFO per channel. Together they hold at most `QueueCap` messages; a send
  into a full queue is refused (disabled), which is R1.
- `numOf[m]`, `nextNum`, `acked`: the reliable numbers. A record takes its
  number at its **first write**, never at send (6.2, I3), and only while
  `nextNum < Base + Window` where `Base` is the oldest unacked number. Unreliable
  records never take a number.
- `sentMap`: the sent-packet map, a FIFO of `[id, num]` with at most
  `SentMapCap` entries; a commit into a full map evicts the oldest (6.4).

State the receiver owns (6.1, 6.5, 7.1, 7.2):

- `opened`: the packet ids the transport has opened and a copy of which is
  still on the wire, its replay window. (An id with no copy left on the wire
  can never arrive again, so the set is kept to the ids on the wire; that is a
  state-space cut, not a claim about the transport.)
- `R` and `rcvd`: the lowest number not yet received and the received bitset.
- `ordRead[c]`, `held[c]`: the next ordering index to deliver and the ordered
  messages waiting for a predecessor. `seqMark[c]` and `heldSeq[c]` are the
  sequenced stubs.
- `dlog[c]`: the ordered messages delivered on `c`, in order; `delivered`: every
  message delivered.

Actions, one per operation and per outside event:

| Action | Who | What |
|---|---|---|
| `AppSend(r, c)` | the application | `send_message` with reliability `r` on channel `c`; refused when `r` is not enabled or the queue is full |
| `FirstWriteUnchanneled` | the sender | the head of the unchanneled queue goes out in a packet with a fresh id; code 2 takes a number and a map entry, code 0 takes neither |
| `FirstWriteOrdered(c)` | the sender | the head of channel `c`'s queue takes the next number, a map entry and a packet |
| `Resend(n)` | the sender | the RTO fires for unacked `n`: a new packet id, the same number (I4), a new map entry |
| `Receive(p)` | the receiver | the transport opens `p`: a duplicate id is dropped; otherwise the record is dispatched (7.2) and the ack is either put on its way back or lost (P5: history overflow, a burned sequence). The loss is decided here because the sender cannot see when it happens |
| `AckArrives(p)` | the transport | the sender learns `p` was opened; if the map still holds `p` its number is acked, else the ack is ignored |
| `Environment` | the path | `Link!Drop`, `Duplicate`, `Reorder`, `Delay`, `Age`, with drops counted |

The dispatch of a RELIABLE_ORDERED record is 7.2's: a duplicate number is
dropped; a number outside the window is a violation; otherwise the bitset is
set and `R` advanced, then if the ordering index is `ordRead[c]` the message is
delivered and the drain releases every held successor in one step, if it is
ahead it is held, and if it is behind that is a violation. The drain is atomic
with the delivery, which is what the argument of 6.3 step 2 needs: a held
message is released in the update that delivers its predecessor, before any
later packet is opened.

What the transport gives, taken as given (CHANNELS.md section 2):

- **P2**: an acked packet id means the receiver opened the payload once, and
  the ack is made at open, before this layer sees the record. In the model the
  receiver's `Receive` adds the id to `ackPending` in the same step it
  dispatches the record.
- **P4**: packet ids never repeat. The model has `PidCap` ids and recycles one
  only when it is nowhere: not on the wire, not pending as an ack, not in the
  map. A recycled id is then indistinguishable from a fresh one. `PidCap` is a
  model-size knob, like `Link`'s `Capacity`: too few ids delay the sender, they
  never lose anything. For liveness it must be at least `SentMapCap +
  Capacity`, so that a map full of entries whose acks were lost cannot pin
  every id (TLC found that lasso at `PidCap = 3`, `SentMapCap = 4`: see
  "What TLC found").
- The transport's replay window drops a duplicated packet. So this layer
  dedupes numbers and the transport dedupes packets; the model has both, as
  `NumDedupe` and `PidDedupe`, and each has a reversed witness below.
- The transport never resends. Every resend is a new id carrying the same
  number.

### Safety, checked by `MCChannels.cfg`

| Invariant | The claim |
|---|---|
| `NoDoubleDelivery` | no message, of any type, is delivered twice |
| `OrderedInSendOrder` | on every channel, the `i`-th ordered delivery is the message sent with ordering index `i-1` |
| `ReceiverNeverOutsideWindow` | a correct sender never trips `MESSAGE_NUMBER_OUTSIDE_WINDOW` (6.5) |
| `OrderedNeverBehind` | a non-duplicate ordered record is never behind `ordered_read` (7.2: "can only be behind if it is a duplicate") |
| `AllocatorPromise` | nothing grows past what was sized at create (R1, 6.3): the queue is at most `QueueCap`, the unacked span at most `Window`, the map at most `SentMapCap`, the held messages at most `Window` with no two in the same slot `n % Window`, and the received bitset inside `R .. R + Window - 1` |
| `OnlyEnabledOnWire`, `UnreliableNeverNumbered`, `DeliveredWereSent`, `HeldAreWaiting`, `MapMatchesWire` | sanity |

Safety is checked with loss unbounded (`BoundedLoss = FALSE`, `Pattern =
"arbitrary"`), duplication and reordering on. No safety property depends on
fairness.

### Liveness, checked by `MCChannelsLive.cfg`

- `EveryReliableDelivered`: every reliable message the application sent is
  eventually delivered.
- `EveryReliableAcked`: and the sender eventually learns it, so the window
  reopens.

Both are checked under `SpecFair`: strong fairness on the sender's first
writes and on its resends (a timer always fires; a free id and an open window
come and go, so weak fairness is not enough), strong fairness on an ack
arriving and on the wire delivering a due head, and weak fairness on time
passing. Each is **one condition on an existential** (`AnyResend`, `AnyAck`,
`AnyReceive`, ...), not one per message, packet or id. The reason: TLC expands
a conjunction of `k` fairness conditions into up to `2^k` branches, and
`Link!WireFair` alone is one condition per packet value, `PidCap × NumMsgs` of
them here (`MCLink` has two). With that formula TLC either ran out of memory
building the branches or reported the negated property as a tautology with 0
branches. With a finite set of messages the existential is enough: once every
other message is done, the starved one is the only one enabled, and strong
fairness fires it.

**Fair loss is the bounded-loss instance**: `BoundedLoss = TRUE` with
`MaxDrops` drops and `MaxAckLoss` lost acks per behaviour, for the same
reason: `Link!EventualDelivery` is one temporal implication per packet value.
Bounded loss says the same thing for a finite instance, "the path does not lose
forever", in a form TLC checks with action fairness alone. Duplication and
reordering are not bounded. The run `live-unbounded` below is the reversed
witness for the assumption: with `BoundedLoss = FALSE` TLC must find a lasso
that drops every copy.

### The reversed witnesses

Each is the committed file with the named lines changed, and each must fail
with the counterexample described (the project's rule: a module's guarding
rule removed, the failure shown).

| Run | Change | Expected failure |
|---|---|---|
| `wit-pid` | `PidDedupe = FALSE` | `NoDoubleDelivery`: `Link!Duplicate` makes a second copy of an UNRELIABLE packet and both are delivered. The layer cannot tell them apart (an unreliable record has no number); at-most-once for unreliable rests on the transport's replay window. |
| `wit-num` | `NumDedupe = FALSE` | `OrderedNeverBehind`: an ordered record's ack is lost, the number is resent, and the second copy reaches the channel with an ordering index below `ordered_read`. That is 7.2's claim, "a reliable ordered record can only be behind if it is a duplicate, and layer 3 already caught those", shown from the other side. |
| `wit-count` | `CountBound = FALSE`, on the one-channel instance below | `AllocatorPromise` (`WindowBounded`) at the sender's third numbering; with that invariant left out of the run (`wit-count-rx`), `ReceiverNeverOutsideWindow`: the receiver sees `n >= R + Window`. |
| `wit-fifo` | `ChannelFifo = FALSE`, on the one-channel instance below | `AllocatorPromise` (`NoSlotCollision`): the sender first-writes ordering index 1 before 0; index 1 is held and, because a held record is acked at open, the window reopens past it and index 2 takes a number that shares a slot `n % Window` with the held one. This is the argument of 6.3 step 2 reversed: without in-order first writes, "every ordering predecessor of `n` has a number below `n`" is false. |
| `live-unbounded` | `MCChannelsLive.cfg` with `BoundedLoss = FALSE` | `EveryReliableDelivered`: a lasso in which every copy is dropped. |

The two sender-side witnesses run on a one-channel instance so that breadth-first
search reaches the counterexample at a shallow depth: `NumChannels = 1`,
`Window = 2`, `SentMapCap = 2`, `PidCap = 4`, with `NumMsgs = 3`, `QueueCap =
3` for `wit-count` and `NumMsgs = 4`, `QueueCap = 4` for `wit-fifo`. On the
committed instance the same traces exist at about twice the depth.

## What the model does not cover

- **Blocks** (layer 5): `U'` equals `U`, the ring and the byte bound of 6.2 are
  absent. `RNBlocks` and `RNRing` are their cards.
- **Aggregation and priority** (layer 6): one record per packet, no priority
  levels, no round robin. `RNScheduler` is its card. Within a channel the FIFO
  first-write rule of 9.2 is here, because 6.3 needs it.
- **Update boundaries** (P6) and the delivered list's lifetime: the receiver
  dispatches each record as it is opened, and the drain is atomic with the
  delivery. `RNWindow` models updates and P6.
- **Time**: no RTT estimator, no RTO value, no `reliable_timeout`. A resend is
  enabled once the last copy of its number is settled: no copy the receiver
  has not opened is on the wire, and no ack for it is on the way back. With
  `SpuriousResend = TRUE` it is enabled at any moment the number is unacked,
  which is every timing the design admits and a far larger state space (the
  first run of that instance passed 39 million distinct states at depth 18
  with the queue still growing, and was abandoned). The committed runs use
  `FALSE`; a spurious resend reaches the receiver as a duplicate number, the
  same path a resend after a lost ack takes, so the receiver's duplicate branch
  is covered either way.
- **The sequenced dispatch** (codes 1 and 4) and **receipts** (5, 6, 7): see
  below.
- **CONFIG**, the error state, disconnect, the server's slots: `RNEndpoint` and
  `RNSlots`.
- **Wire encoding**: the record is `desc[m]` and `numOf[m]`, read from the
  sender's state, which never changes after the send. Layer 2's codec is a
  known-answer and fuzz matter, not a state machine.
- The second direction. Acks ride a separate path (`ackPending`) with its own
  loss, not a second `Link` instance, because the transport repeats acks in
  every ack block and this layer treats silence as "resend after the RTO",
  never as a fact (P5).

### What the stubs are

The task puts UNRELIABLE and RELIABLE_ORDERED first and the other six after,
and asks that nothing here corner them. The model carries, from the first
version:

- the eight reliability values and the wire mapping of the receipt variants
  (`WireCode`: 5 as 0, 6 as 2, 7 as 3), so a receipt type dispatches exactly as
  its base type (7.4);
- RELIABLE (2): the unchanneled queue, a number taken at first write, delivery
  on arrival after the window check. Its path is written and reachable by
  setting `Enabled = {0, 2, 3}`;
- the per-channel sequenced counters `seqWrite`, `seqMark`, `heldSeq` and their
  coupling with ordered sends and deliveries (7.1), which run on every ordered
  send and delivery even though no sequenced type is enabled. The sequenced
  dispatch itself (the watermark test, the newest-held rule of 7.3) is the
  `RNChannel` card's and is not written: `Enabled` must not contain 1 or 4
  (an `ASSUME` enforces it), so `Receive` stays total for every packet the
  sender can put on the wire (Link's vacuity caution);
- receipts: the serial and the ACKED/LOSS outcome are the `RNReceipts` card's.
  Types 5 to 7 may be enabled here and behave as their base types.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19 or later), from
`tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 16 -config MCChannels.cfg MCChannels
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 16 -config MCChannelsLive.cfg MCChannels
```

Give each run its own `-metadir` if they run at once; TLC's `states/`
directory is shared otherwise.

The committed safety instance: `NumChannels = 2`, `Window = 4`, `QueueCap =
4`, `SentMapCap = 4`, `PidCap = 6`, `NumMsgs = 2`, `Enabled = {0, 3}`, the
witness knobs `TRUE`, `SpuriousResend = FALSE`, and the link with `Capacity =
2`, `MaxDelay = 0`, `Duplication = TRUE`, `Reordering = TRUE`, `Pattern =
"arbitrary"`. Two messages are one ordered and one of either type on either
channel: enough for a resend after a lost ack, a window that reopens on the
ack, a held message released by its predecessor, and a map eviction. With two
messages and a window of four the window never closes on this instance: the
window closing and reopening is exercised by the one-channel `Window = 2`
witness instances (where it is what breaks when the rule is removed) and by
the unfinished `safety-w2` run below; `NumMsgs = 5` on the committed knobs is
the run to make on a quiet bench (see "The committed runs, and the bench" for
how far the larger instances got, and how deep the state space is even at two
messages).

The committed liveness instance has the smaller map and id knobs, because
TLC's liveness check is several times the cost of the safety check on the same
states: `NumMsgs = 2`, `SentMapCap = 2`, `PidCap = 4`, `BoundedLoss = TRUE`,
`MaxDrops = 2`, `MaxAckLoss = 2`, the rest as above.

## What TLC found

Every counterexample below was printed by TLC and replayed by hand through
the module's actions. None is a defect in rocketnet code: there is no reliable
code yet, which is the point of writing the model first. Three are model
errors, found and fixed; the rest are the witnesses doing their job.

### Model errors TLC found, fixed

1. **The eager resend is too big to check.** The first instance let `Resend(n)`
   fire at any moment `n` was unacked. At `NumMsgs = 5` TLC passed 39 million
   distinct states at depth 18 with 25 million on the queue and was abandoned.
   The gate is now "the last copy is settled" (`InFlight`), with
   `SpuriousResend = TRUE` keeping the eager version for a bench with more
   time. Nothing the receiver does depends on which; a spurious resend arrives
   as a duplicate number, the same path a resend after a lost ack takes.
2. **A duplicate stream held the timer.** With the gate "no copy of `n` on the
   wire", TLC rejected `EveryReliableAcked` at `NumMsgs = 3` with a 10-state
   lasso: number 1 is delivered, its ack is lost, and from then on the path
   duplicates the one remaining copy of packet id 1 every time the receiver
   drops it as a replay. A copy is always on the wire, `Resend(1)` is never
   enabled, and number 1 is never acked. Replayed by hand: every step is
   `Environment` (`Link!Duplicate`) or `Receive` of an opened id. The real RTO
   fires whatever the wire carries; the fix is that a copy whose id the receiver
   has already opened does not count as in flight.
3. **The double-delivery flag was never set.** `dup' = dup \/ (S \cap delivered
   # {})` parses as `(dup' = dup) \/ (...)`, so on a double delivery `dup'` was
   unassigned. TLC reported it as "successor state is not completely specified"
   the first time the `wit-pid` witness reached a double delivery, which is loud
   enough, but it meant `NoDoubleDelivery` had been checked against a flag that
   could not go TRUE. Fixed with parentheses; the witness then fails the
   invariant as it should.

Two more findings are about TLC rather than the model, and shaped the fairness
and the liveness instance: the `2^k` branch expansion of per-packet fairness
(see "Liveness" above), and a `PidCap` too small to let the sender commit once
the map fills with entries whose acks were lost (a 10-state lasso at `PidCap =
3`, `SentMapCap = 4`: three stale map entries pin all three ids; with `PidCap
>= SentMapCap + Capacity` the wire drains, the ids free, and a commit evicts).

### The witnesses

| Run | Result | The trace, replayed |
|---|---|---|
| `wit-pid` | `NoDoubleDelivery` violated at depth 6 (2,388 states generated, 1,360 distinct) | `AppSend(0)`, `FirstWriteUnchanneled` (id 1), `Link!Duplicate`, `Receive` (delivered, `delivered = {1}`), `Receive` of the second copy: without the replay drop it is delivered again, `dup = TRUE`. |
| `wit-num` | `OrderedNeverBehind` violated at depth 8 (3,003 states, 1,661 distinct) | three `AppSend`s, `FirstWriteOrdered` (message 3, number 1, id 1), `Receive` with the ack lost (`R = 2`, `ordRead[1] = 1`), `Resend(1)` as id 2, `Receive`: number 1 is below `R`, and without the number check it reaches the channel with ordering index 0 behind `ordered_read = 1`, `behind = TRUE`. |
| `wit-count` | `AllocatorPromise` (`WindowBounded`) violated at depth 8 (6,070 states, 2,901 distinct); `wit-count-rx`: `ReceiverNeverOutsideWindow` violated at depth 10 (15,167 states, 6,923 distinct) | three `AppSend(3)`s, numbers 1 and 2 written, `Link!Drop` of number 1's copy, number 3 written with two unacked and `Window = 2`, `Receive` of 2 (`R = 1`, `rcvd = {2}`), `Receive` of 3: `3 >= R + Window`, `outside = TRUE`. |
| `wit-fifo` | `AllocatorPromise` (`NoSlotCollision`) violated at depth 12 (194,527 states, 88,704 distinct) | four `AppSend`s (ordering 0, an unreliable, ordering 1, ordering 2), ordering 1 written first as number 1, ordering 0 as number 2, `Receive` of 1 (held, acked), `AckArrives` (base moves to 2, the window reopens), ordering 2 written as number 3, `Link!Drop` of number 2's copy, `Receive` of 3: `held = {ordering 1 at number 1, ordering 2 at number 3}`, and `1 % 2 = 3 % 2`. |
| `live-unbounded` | `EveryReliableDelivered` violated: a 19-state lasso back to state 11 (1,108,444 states generated, 463,758 distinct, 12 min 41 s) | three messages: an ordered one delivered, an unreliable one delivered, and a second ordered one (number 2) whose every copy `Link!Drop` removes; in the loop number 2 is resent and dropped, and the duplicate copies of number 1 are delivered and acked after the two-entry map has evicted their entry, so nothing ever changes. This is the behaviour `BoundedLoss` exists to rule out, and the committed liveness run passing while this fails shows the assumption is doing the work. |

### The committed runs, and the bench

The bench (`vision`, 64 cores) was shared with other jobs for the whole of this
card, at a load average between 220 and 400, so every run below got a fraction
of the machine. The instances that finished are the committed ones; the larger
instances were run as far as the time allowed and stopped, with no invariant
violated to the depth reached, and are the runs to repeat on a quiet bench.

**`MCChannels.cfg` (safety), `Spec`, loss unbounded: not finished on this bench. Stopped at depth 41 after 20 minutes at load 170 to 420 with 87,599,972 states generated, 21,811,412 distinct and 2.8 million on the queue, no invariant violated to that depth. Breadth-first, so every state at depth 40 or less was checked. This is the run to complete on a quiet bench; the gate of this card is owed until it is.**

**`MCChannelsLive.cfg` (liveness), `SpecFair`, `-lncheck final`: `Model checking completed. No error has been found.` 2,830,922 states generated, 836,516 distinct states found, 39 min 57 s of which 16 min was the final liveness check; `EveryReliableDelivered` and `EveryReliableAcked` hold.**

Stopped without finishing, no violation to the depth reached (all breadth-first,
so every state up to the depth before the last was checked):

| Instance | Reached |
|---|---|
| committed safety knobs with `NumMsgs = 3` | 107,360,954 states generated, 35,714,908 distinct, depth 24, 48 minutes at load 150 to 420, 12.6 million states on the queue |
| committed safety knobs with `NumMsgs = 5` | 24,464,469 states generated, 10,441,135 distinct, depth 17, 11 minutes at load 280; and before that, with the eager resend (`SpuriousResend = TRUE`), 99,990,925 generated, 39,311,290 distinct, depth 18 |
| committed safety knobs with `NumMsgs = 4` | 98,124,098 states generated, 40,905,377 distinct, depth 19, 52 minutes at load 300 to 400, 23 million states on the queue |
| `Window = 2`, `SentMapCap = 2`, `PidCap = 4`, `NumMsgs = 4`, two channels (`safety-w2`, the window closing on two channels) | 56,437,052 states generated, 16,445,533 distinct, depth 21, 27 minutes |
| committed liveness knobs with `NumMsgs = 3` (`live-m3s`) | 639,925 states generated, 310,001 distinct, depth 14, in 16 minutes with `-lncheck final`; an earlier attempt without it reached 708,312 distinct in 25 minutes |
