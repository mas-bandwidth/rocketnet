# RNWindow: the reliable window

`proofs/tla/RNWindow.tla` is the message layer's reliable window, CHANNELS layer 3
(`docs/CHANNELS.md` sections 6, 13.5 and 15): a sender numbering records late under the count
bound `n < U' + W`, its sent-packet map and acks, and a receiver with `R`, the received
bitset, the descriptors and their live flags, holds, the delivered list, the update
boundary, and the reconstruction of the 24-bit wire number. Both directions run through
`proofs/tla/Link.tla`.

The claim it checks: every reliable message number is delivered to the application exactly
once and, on the ordered channel, in order, under loss, resend duplicates, reordering and
wraparound of the number space, with every table inside its bound; and, under fair links,
every number is eventually acked and delivered.

**The window code does not exist yet.** At `371e5a7` (main) nothing under `sdk/`
implements layer 3: `git grep "SLOT_COLLISION\|received_bitset" -- 'sdk/*.cpp'` finds
nothing on main or on any branch. What the code has is the wire codec, which writes and
reads the number as a raw u24 (`sdk/rocketnet_message.h:22`, `:70`, `:110`;
`sdk/rocketnet_message.cpp:202`, `:312`) and checks a block's fragment count against `W`
(`sdk/rocketnet_message.cpp:357-361`, matching 13.3 check 7). The model is therefore the
design, written before the code as the standing practice asks: the card
`the-receive-window-accepts-each-reliable-number-once` builds to it, and the card
`the-window-code-replays-every-rnwindow-and-rnring-witness` replays the traces below
against that code. Every "the code does" below reads "the design says".

`proofs/tla/MCRNWindow.tla` holds the instance, and there are seven committed cases, each one
inside tlacheck's 110 s budget:
- `proofs/tla/MCRNWindow.cfg`: `W = 2`, numbers 0..4 wrapping at `NumMod = 4`, Link capacity 2,
  the `"alternate"` loss pattern both ways, every invariant and the liveness property.
- `proofs/tla/MCRNWindowSafety.cfg`: numbers 0..5, `"arbitrary"` loss, safety only.
- The five reversed witnesses, `proofs/tla/MCRNWindowBroken*.cfg`, each `MCRNWindow.cfg` with one
  constant flipped (`diff` shows one line).

The `"arbitrary"` loss run with liveness takes 14 minutes. It is a bench run, listed under
"Every run", and not a committed case.

## What the module covers

The sender (6.1, 6.2, 6.4):

- `next`: the next message number. A record takes it late, when it is first written, and
  only while `n < U' + W`. The block takes all its numbers at one moment under one check
  of its last number (section 8).
- `acked`: the acked flags of the per-number table. `U`, the oldest unacked number, and
  `U'`, the window base, are derived: `U' = U` except when `U` is inside the block and
  the block is not fully acked, when `U'` is the block's first number.
- `smap`: the sent-packet map, a ring of `P` entries from packet id to the numbers the
  packet carried. When full, the oldest entry is evicted (`evicted` records that it
  happened); its numbers keep their state and stay resendable.

The receiver (6.1, 6.3, 6.5, 13.5):

- `R`: the lowest number not yet received. `rbits`: the received bitset, as the set of
  received numbers in `R .. R+W-1`; advancing `R` clears the bits below it.
- `desc`: the descriptors indexed `n % W`, each `[num, live]`.
- `held`: numbers held, waiting on an ordering predecessor or part of an incomplete
  block. `delivered`: the delivered list of the current update. A number is **live**
  while it is in either (6.3 step 1).
- `phase` and `pending`: the update loop. Packets are opened while the batch is open
  (`"receive"`); `FixBatch` closes it; acks for the opened ids leave only after that
  (`"process"`); `EndUpdate` expires the delivered list and reopens the batch.
- The wire number. A packet carries the number itself (so the ghosts know which record is
  which); the receiver sees only `n % NumMod` and reconstructs it against `R` by signed
  half range, `Recon(w, R)`. `NumMod` is the model's `2^24`; the committed instance has
  `NumMod = 4` and numbers `0..4`, so `4` is sent as `0` and the numbers really wrap.

Ghosts: `everDelivered` and `dup` (for `DeliveredOnce`), `violations` (which of the
receiver's 6.5 checks fired), `evicted`, and `misread` (a wire number was reconstructed as
a different number).

Actions, one per operation and per outside event:

| Action | Who | What |
|---|---|---|
| `NumberRecord` | sender | gives `next` to a record under the count bound; the block takes `BlockLen` numbers at once |
| `WritePacket(S, pid)` | sender | writes a packet with a fresh id carrying any set of unacked numbered records, first sends and resends alike (the resend delay firing); records it in the map, evicting the oldest entry when full; `Fwd!Send` |
| `ReceiveAck(q)` | sender | `Rev!Deliver`; every id of `q` still in the map marks its numbers acked and leaves the map; other ids are ignored |
| `OpenPacket(p)` | receiver | `Fwd!Deliver`; the id joins `pending` (P2); each wire number is reconstructed against `R` (a misread is recorded and nothing else happens); then `Accept`: 6.5 per number (duplicate dropped, outside the window or slot live with another number is a violation), accepted numbers set bits, claim descriptors, advance `R`; then 7.2 dispatch: unordered delivered now, ordered delivered when every ordered predecessor is received (draining holds), else held |
| `FixBatch` | receiver | the batch is fixed; no more opens this update (P6) |
| `SendAck` | receiver | `Rev!Send(pending)` after the batch is fixed |
| `EndUpdate` | receiver | the delivered list expires; its descriptors stop being live |
| `FwdEnvironment`, `RevEnvironment` | the paths | `Link!Environment` on each direction |

The resend delay is abstract: any unacked number may be resent at any time, so `last_send`, the
resend FIFO and the RTT estimator have no counterpart. Blocks appear only as their effect
on `U'` and on the receiver's holds. One ordered channel is modelled abstractly: the
numbers in `Ordered` are on it in number order (layer 6 first-writes a channel's records
in ordering order), the block is one ordered message at one ordering slot, every other
number is unordered (code 2).

Packet ids never repeat in the transport (P4). A finite model cannot mint ids forever,
so an id is reused only once no copy of it exists anywhere: not in the map, not pending,
not on either wire. When no id is free the write waits, like Link's `Capacity`: a
model-size knob, never a loss.

### Invariants

- `TypeOK`, and `TablesBounded`: `Len(smap) <= P`; `next <= U' + W` and at most `W`
  unacked numbers (the per-number table); the bitset inside `R .. R+W-1`; at most `W`
  live numbers (descriptors, holds and the delivered list of 6.3 step 4); the links
  inside `Capacity`.
- `ReceiverNeverOutsideWindow`: a correct sender never trips 6.5's outside-window check.
- `NoSlotCollision`: no two live numbers share a slot `n % W`, and the receiver's own
  slot check never fires.
- `AckedImpliesReceived`: an acked number was received (P2 carried through the map).
- `DeliveredOnce`: no number is delivered twice, and only numbered records are delivered.
- `OrderedDeliveredInOrder`: an ordered number is delivered only once every ordered number
  below it, and the rest of its block, has been delivered too. Deliveries in one step are
  a set; the code drains them in ordering order (7.2).
- `WireNumberReconstructs`: every wire number the receiver read was reconstructed as
  itself (13.5).
- `HeldIsUnreleasable`: a held number is received and has an unreceived ordered
  predecessor; the descriptors name the live numbers.

### Liveness

- `EveryReliableAckedAndDelivered`: `<>(acked = Nums /\ everDelivered = Nums)`. The
  module also states its three parts (`EveryRecordEventuallyNumbered`,
  `EveryReliableEventuallyAcked`, `EveryReliableEventuallyDelivered`). `acked`,
  `everDelivered` and `next` only grow and `Nums` is finite, so in this instance each is
  the same as its per-number form `\A n : (n < next) ~> (n \in acked)`. The single
  `<>` form is one tableau branch for TLC instead of one per number per property: the
  draft's nine branches took 46 minutes on 25,738 states.

### Fairness

Link's `Fairness` is restated at this model's size, as `ClientConnection` does (its
README, "Fairness"): per-value strong fairness over every data packet value and every ack
value exhausts TLC's tableau.

- `SendFair`: weak fairness on `NumberRecord` and on the update loop (`FixBatch \/
  EndUpdate`: exactly one is enabled in each phase, so weak fairness on the disjunction is
  weak fairness on each); strong fairness on writing the oldest unacked number, plain and
  "through" (the pattern lets it onto the wire), and on the ack send, plain and through.
  This is Link's `EventualDelivery` stated on the sender.
- `DeliveryFairHead`: strong fairness on opening the head of the forward wire and on
  receiving acks. Enough for the deterministic patterns on a FIFO link (`Spec`).
- `AcksLand`: acks that leave without bound include, without bound, acks naming an id the
  map still holds. The model's stand-in for real time; see "What TLC found".
- `OldestLands` (`SpecTyped`, for `"arbitrary"`, where `Drop` removes copies in flight):
  the oldest number, written through without bound, is opened without bound.

## What the module abstracts away

- The byte bound and the ring (`RNRing`, the sibling model). Channels beyond the one
  abstract ordered channel, sequenced delivery, receipts (layer 4). Fragment contents
  and the split (layer 5). The scheduler (layer 6): its first-write order is assumed as
  "number order is ordering order".
- Real time: the resend delay, backoff, the RTT estimator, `reliable_timeout`. Any unacked number
  is due at any moment.
- The transport: only P2 and P6 remain, as the receiver's ack and update rules. Its
  replay window (which drops a packet seen before or more than 4096 sequences old) is not
  modelled; Link's duplication and reordering reach the receiver unfiltered, which is a
  stronger adversary than the transport lets through, except where it matters for
  wraparound (below).
- The receiver reconstructs every number of a packet against the `R` the packet found;
  the code will reconstruct record by record as `R` advances inside the packet. Both
  values lie in one window, and `WireNumberReconstructs` holds for the reading the model
  makes.

## How to run it

The model's operation sequences are also driven in C++ by
`test_unit_window_sequences.cpp` (a unit-file test in `./bin/test`, registered
with `REGISTER_UNIT_TEST`). An independent reference of count, `U'`, ring
occupancy, descriptor ownership and delivery is compared with the sender and
receiver window operations after every action. Those operations are
`rn_window_send_*` and `rn_window_receive_*`. A tree that has
`sdk/rocketnet_window_send.h` drives that code. This base does not have those
files yet, so the test compiles the landed admit, `U'` and receive rules and
drives them. The count bound and the ring floor both read `U'`, not `U`.
Putting `U` in either place is the pre-fix sender: the partial-block count
probe and the floor-at-`U` probe go red.

Every sequence of write, block, ack, release and retransmit of length six at
`W = 8` is walked (the operations take any power-of-two width; the message
config's minimum is 64, so the exhaustive region is synthetic). A seeded
million-step walk runs at the real constants (`W = 1024`, `B = 65536`) against
the real descriptor pool. Directed probes cover `W - 1`, `W`, `W + 1`, the ring
boundary, a partial-block ack floor, descriptor release only after the update
has released a held record, and the 24-bit wrap. After every action the test
asserts `n < U' + W`, that live ring spans are disjoint and inside `B`, that an
ack does not free a held record, and that each reliable number is delivered
once. A failed check prints a replayable seed and the action list. No open
window-sequence defect card is known on this base, so the test carries no
expected-fail marker.

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19 or later), from `proofs/tla/`:

```sh
java -Xmx6g -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -config MCRNWindow.cfg MCRNWindow
```

The other committed cases are the same command with `MCRNWindowSafety.cfg` and each
`MCRNWindowBroken*.cfg`. The bench runs below are `MCRNWindow.cfg` with the lines named in
the table changed. Every run uses `MaxDelay = 0`. A run with `MaxDelay > 0` also needs
weak fairness on `Fwd!Age` and `Rev!Age` (Link's `WireFair`), which `Fairness` does not
include. Without it a copy that is not yet due never becomes due.

### Every run

TLC 2.19 on hetzner2, 2026-10-13, `-workers 4 -Xmx6g`. The `proofs/tla/RUNS.tsv` records are not
written by hand. They are owed to `tlacheck run --group rnwindow`.

Committed cases:

| Case | Checks | States | Distinct | Depth | Time | Result |
|---|---|---|---|---|---|---|
| `MCRNWindow.cfg` (W 2, numbers 0..4 wrapping at NumMod 4, cap 2, `"alternate"` both ways, `Spec`) | every invariant, `EveryReliableAckedAndDelivered` | 130,597 | 51,850 | 91 | 32 s | pass |
| `MCRNWindowSafety.cfg` (numbers 0..5, `"arbitrary"`, `Spec`) | every invariant | 3,267,640 | 912,812 | 59 | 11 s | pass |
| `MCRNWindowBrokenBlockUPrime.cfg` | every invariant | 19,705 | 8,692 | 33 | 1 s | fail as expected: `NoSlotCollision` |
| `MCRNWindowBrokenSameUpdateReply.cfg` | every invariant | 1,386 | 678 | 16 | 0 s | fail as expected: `NoSlotCollision` |
| `MCRNWindowBrokenHalfRange.cfg` | every invariant | 176 | 107 | 8 | 1 s | fail as expected: `WireNumberReconstructs` |
| `MCRNWindowBrokenHoldOrdered.cfg` | every invariant | 1,431 | 741 | 19 | 2 s | fail as expected: `OrderedDeliveredInOrder` |
| `MCRNWindowBrokenDropDuplicates.cfg` | every invariant | 173 | 105 | 8 | 1 s | fail as expected: `DeliveredOnce` |

Bench runs (each is `MCRNWindow.cfg` with the changes named):

| Changes | Checks | States | Distinct | Depth | Time | Result |
|---|---|---|---|---|---|---|
| `"arbitrary"` both ways, cap 1, `SpecTyped` | every invariant, liveness | 1,221,332 | 388,260 | 52 | 13 min 59 s | pass |
| `"arbitrary"`, cap 2, `SpecTyped` | same | 2,124,794 | 596,772 | 52 | 25 min cap | safety complete, no violation; liveness stopped at the cap |
| `"arbitrary"`, cap 1, numbers 0..3, `Ordered = {0, 1, 2}` (no wrap), `SpecTyped` | same | 586,962 | 191,866 | 48 | 5 min 29 s | pass |
| `"first"` both ways | same | 128,381 | 46,844 | 50 | 33 s | pass |
| `"none"` both ways | same | 190,215 | 66,175 | 44 | 45 s | pass |
| `"burst"` both ways | same | 124,469 | 45,294 | 56 | 26 s | pass |
| `"arbitrary"`, `W = 3`, `NumMod = 6`, no liveness | every invariant | 16,276,205 | 4,371,974 | 52 | 48 s | pass |
| `"arbitrary"`, `MaxPerPacket = 2`, no liveness | every invariant | 4,523,801 | 1,198,503 | 52 | 25 s | pass |
| `"arbitrary"`, `Duplication`, `Reordering`, `NumMod = 10`, no liveness | every invariant | 326,352,691 | 48,089,524 | 71 | 14 min 31 s | pass |
| `"arbitrary"`, `Duplication`, `Reordering`, no liveness | every invariant | 342,400 | 101,591 | 17 | 3 s | fail: `WireNumberReconstructs`; the transport's replay window, below |

## What TLC found

Every counterexample was replayed by hand through the module's actions and through the
argument of CHANNELS.md 6.3 and 13.5. The window code does not exist yet, so none of
them is a defect in code. The traces are written `id{numbers}` for a packet. The
committed instance is `W = 2`, Link capacity 2, `"alternate"` loss both ways (the 1st,
3rd, 5th ... send on each link is lost), `NumMod = 4`, numbers `0..4`, the block
`{1, 2}`, `Ordered = {0, 1, 2, 4}`. The state counts are TLC's trace lengths, the
initial state included. A write the pattern loses still takes an id and a map entry
(and may evict the oldest), but it never reaches the wire.

### Witness 1: `U'` ignoring partial blocks (`MCRNWindowBrokenBlockUPrime.cfg`)

TLC: `Invariant NoSlotCollision is violated`, 33 states.

1. `0` is numbered. Its first write is lost; `1{0}` gets through and is opened: `R = 1`,
   `0` delivered. More writes of `0` follow, every other one lost, and so does the first
   ack. `0{0}` arrives in the next update as a duplicate, and its ack `{0}` lands:
   `acked = {0}`, `U = U' = 1`.
2. The block takes `1` and `2` at one moment (`2 < U' + W = 3`). `0{1}` and `1{1}` get
   through. In the next update `0{1}` is opened: `R = 2`, but `2` is missing, so `1` is
   **held**, `desc[1] = [1, live]`. That ack is lost. `1{1}` arrives later as a
   duplicate and its ack `{1}` lands: `acked = {0, 1}`, `U = 2`.
3. `2` is inside a block whose first fragment is acked and whose second is not, so the
   design's `U'` is `1`. The witness sets `U' = U = 2`, and `3` is numbered:
   `3 < 2 + 2`. Under the design the check is `3 < 1 + 2`, false.
4. `1{3}` is opened in the next update: `3 < R + W = 4` is inside the window, but
   `3 % 2 = 1` and `desc[1]` is live with the held `1`. The receiver's slot check fires.

Through 6.3 step 2: `m = 3 = n + W` with `n = 1`. The argument needs every block
containing a number at or below `n` fully acked when `m` was numbered, and the witness is
the sender numbering `m` without that.

### Witness 2: dropping P6 (`MCRNWindowBrokenSameUpdateReply.cfg`)

TLC: `Invariant NoSlotCollision is violated`, 16 states.

1. The receiver fixes its batch first (`phase = "process"`) and never ends the update
   in this trace. `0`'s first write is lost, and `1{0}` is opened **after** the batch
   was fixed, which the witness allows: `0` delivered, `desc[0]` live.
2. The first ack is lost. A resend `0{0}` is opened as a duplicate, and the ack `{0}`
   lands: `acked = {0}`, `U' = 1`.
3. The block takes `1` and `2`, correctly: `0` is acked, so by P2 it was opened. `0{2}`
   gets through and is opened in the same update. `2 % 2 = 0`, and `desc[0]` is still
   live with `0`, whose delivered list has not yet expired. The slot check fires.

Through 6.3 step 2: "By P6, `m` arrives in a later update, after that update's delivered
list expired." Remove P6 and `m = 2` arrives while `n = 0` is still delivered in the
update whose ack elicited it.

### Witness 3: unsigned reconstruction (`MCRNWindowBrokenHalfRange.cfg`)

TLC: `Invariant WireNumberReconstructs is violated`, 8 states.

1. `0` is numbered and written four times. The 1st and 3rd writes are lost, so `1{0}`
   and `0{0}` are on the wire (write, write, then open, open).
2. `1{0}` is opened: `R = 1`, `0` delivered.
3. `0{0}` is opened. Its wire value is `0`; unsigned, `R + ((0 - 1) % 4) = 1 + 3 = 4`.
   The signed half range gives `d = 3 >= 2`, so `1 + 3 - 4 = 0`, a duplicate, dropped.

A duplicate after a resend is the normal case (6.5), so an unsigned reading misreads one
on every lossless resend. With `W` at most `2^20` and `2^24` on the wire, the unsigned
reading puts the duplicate about `2^24` ahead, outside the window: a correct sender would be
disconnected for `MESSAGE_NUMBER_OUTSIDE_WINDOW`.

### Witness 4: ordered numbers delivered on arrival (`MCRNWindowBrokenHoldOrdered.cfg`)

TLC: `Invariant OrderedDeliveredInOrder is violated`, 19 states.

1. `0` is delivered and acked, after the lost writes and the lost first ack the pattern
   forces. The block takes `1` and `2`.
2. `0{2}` gets through and is opened in the next update. The witness delivers `2` at
   once, although `1` (an ordered predecessor, and the rest of its block) has not
   arrived: `everDelivered = {0, 2}`, and `OrderedPred(2) = {0, 1}` is not inside it.

With the hold (7.2), `2` is held until `1` arrives, then both are released together in
number order.

### Witness 5: duplicates taken as fresh (`MCRNWindowBrokenDropDuplicates.cfg`)

TLC: `Invariant DeliveredOnce is violated`, 8 states.

1. `0`'s first write is lost; `1{0}` is opened: `0` delivered, `R = 1`.
2. The next write is lost; `0{0}` gets through and is opened: `0 < R`, a duplicate under
   6.5, but the witness takes it as fresh and delivers it again: `dup = TRUE`.

### Wraparound under reordering: the half range rests on the transport's replay window

`MCRNWindow.cfg` with `Duplication = TRUE` and `Reordering = TRUE` (`Spec`, safety only)
fails: `Invariant WireNumberReconstructs is violated`, 17 states. Replayed (the trace
needs only reordering, no duplication):

1. `0` is written twice, `0{0}` and `1{0}`. `0{0}` is opened, acked: `U' = 1`.
2. The block takes `1` and `2`; `0{2}` is written. Link swaps it ahead of the stale
   `1{0}`, and it is opened: `2` is held, `R = 1`.
3. `2{1}` is written, swapped ahead of `1{0}` too, and opened: `R = 3`, `1` and `2` are
   delivered.
4. `1{0}` arrives at last. Its wire value `0` against `R = 3`: `d = (0 - 3) % 4 = 1`,
   below the half range, so it reads as `4`, which is inside the window. The resend of
   `0` would be accepted as message `4`.

This is the model being honest about what 13.5 assumes, not a defect in the design or in
any code: the half range reads a value correctly only while it lies within `2^23` of `R`.
13.5 argues that "every live value lies within `W` of its reference", which covers live
numbers but not a stale resend still in flight. On a FIFO path a stale copy is at most `W`
behind `R` (any packet opened before it was written before it, so `R <= n + W`), and the
committed runs prove `NumMod = 2W` enough there. Under reordering the age of a copy is
bounded only by the transport. Its replay window refuses any packet
`ROCKETNET_REPLAY_WINDOW_BITS = ROCKETNET_HISTORY_PACKETS` sequences or more behind the
highest it has opened, and any packet it has seen before
(`sdk/rocketnet_replay.cpp:32-39`, `sdk/rocketnet_replay.h:26`). That constant is 4096 by
default and at most 32768 (`sdk/rocketnet_history.h:8,13`). A 1416-byte payload carries
at most 202 numbered records (a code 2 record is at least a 6-byte header and 1 data
byte, 13.2), and a block numbers up to 911 fragments at once. So in the worst case a copy
that survives the replay window is
`W + 32767 × 202 + 911 <= 7,668,421 ≈ 7.3 × 2^20` behind `R`, under `2^23`.
This rests on transport sequences rising in packet write order. A sequence is taken at
`start_packet` by an atomic increment (`sdk/rocketnet_client_protocol.cpp:1068`,
`sdk/rocketnet_server_protocol.cpp:1524`), so this layer must fill a connection's
packets in the order it started them, as `write_packets` (9.3) does. The safety margin is
real, but it comes from the transport's replay window and the payload size. 13.5 now says
so. With `NumMod = 10` (no aliasing possible at these sizes) the same duplication and
reordering run passes every other invariant (48,089,524 distinct states).

### Two fairness gaps in the first draft (model errors, fixed)

Both under `Pattern = "alternate"` both ways, `Spec`, at `W = 3`, each found in seconds.

1. **The lost first write was never forced.** The draft had only the "goes through" form
   of the write. Lasso of 3 states: `0` is numbered, then the receiver's update loop turns
   forever and nothing is ever written. Under `"alternate"` the first send is lost, so a
   write of `0` would leave the wire unchanged, the "through" form is not enabled, and
   strong fairness on it forces nothing. The fix is the pair `ClientConnection` uses:
   fairness on the plain write, which burns the lost index, and on the through form.
2. **The ack send was never forced.** An 11-state lasso: `0` is written three times (the
   first and third sends lost, the second opened), `pending = {0, 1}`, and the update loop
   turns without ever sending the ack. `SendAck` is enabled only in the `"process"` phase,
   which `EndUpdate` leaves, so it is never *continuously* enabled and weak fairness
   forces nothing. The transport puts its ack block in every packet of every update, so the
   plain ack send is strongly fair, and so is the plain write.

### The abstract resend delay evicts its own acks (model-size artefact with a design note)

With both plain sends strongly fair, `"alternate"` under head fairness alone gave a
130-state lasso in which `0`, `1` and `2` are acked and delivered but `3` is written
forever and never acked. The sent map holds `P = 2` entries. Between the receiver opening
a packet carrying `3` and the ack for it landing, the sender writes `3` twice more (the
abstract resend delay is due at every moment), so the opened packet's id has been evicted when its
ack lands and the ack is ignored, as 6.4 says. Every ack in the lasso names an evicted id.

This is the model's resend delay outpacing the ack path, which the real resend delay (above the RTT, RFC
6298 with back-off) does not do. `AcksLand` excludes it. The design note for the code
card: `P` must exceed the packets a connection writes in one RTT, resends included, or a
sender under back-off can evict its own acks and stall until `reliable_timeout`. The rule
that bounds it is `P >= W` with a resend delay above the RTT: `proofs/tla/RNSentMap.tla` checks it
(README-RNSentMap.md), and config validation refuses `packets_in_flight` below
`reliable_messages_in_flight` (standard/MESSAGES.md m9.1).

### A model error the DeliveredOnce witness found

The first run of `MCRNWindowBrokenDropDuplicates.cfg` stopped on "Successor state is not
completely specified ... dup". The draft wrote `dup' = dup \/ (now \cap everDelivered #
{})`, which parses as `(dup' = dup) \/ (...)`: when a duplicate is delivered TLC may take
the second disjunct and leave `dup'` unassigned. With every guarding rule on, the second
disjunct is never true, so no run of the committed model was affected; it would have hidden
a duplicate delivery. Fixed to `dup' = (dup \/ ...)`.
