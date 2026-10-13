# FragmentsAndAcks: fragment reassembly and the ack window

`tla/FragmentsAndAcks.tla` is one direction of a rocketnet session: a sender A hands
payloads to a receiver B over the shared lossy link `tla/Link.tla` (instance `D2R`),
and B sends ack blocks back over a second instance (`R2D`). A is the session sequence
and one `rocketnet_ack_endpoint_t` (`sdk/rocketnet_ack.cpp`: `record_sent`,
`open_aggregate`, `process_block`, `apply_run`) plus the fragmented send path
(`rocketnet_client_transport_finish_arena_packet` in `sdk/rocketnet_client_transport.cpp`
and its server mirror in `sdk/rocketnet_server_transport.cpp`;
`rocketnet_client_protocol_start_fragments` in `sdk/rocketnet_client_protocol.cpp`). B is
the replay window as a set (`sdk/rocketnet_replay.cpp`), the block builder
(`rocketnet_ack_block_build`) and reassembly (`rocketnet_reassembly_process_fragment` in
`sdk/rocketnet_fragment.cpp`). The code is origin/main at `3ad4179`.

`tla/MCFragmentsAndAcks.tla` and `tla/MCFragmentsAndAcks.cfg` are the small instance
(safety); `tla/MCFragmentsAndAcksLive.cfg` is the liveness run on the same instance. The
rows for `tla/RUNS.tsv` are listed at the end of this file and not added by this change
(six models land on that file in parallel). The inventory this card worked from is
`docs/TLA-INVENTORY.md`, M8 and M9.

## What the model covers

**State the sender owns** (`struct rocketnet_ack_endpoint_t`, the session sequence, the
fill phase's staging):

| Variable | Code | Meaning |
|---|---|---|
| `nextSeq` | `send_sequence_session` | the next sequence to spend |
| `nextGen` | `next_fragment_payload_id` (`send_fragment_payload_id` on the server) | the next fragmented payload's issue number; its id is `IdOf(nextGen)` |
| `sent`, `highestSent` | `sent[]`, `highest_sequence` | the sent ring: sequence, packet id or aggregate, acknowledged |
| `aggs`, `numAggs` | `aggregates[]`, `num_aggregates` | the aggregate ring: id, packet id, fragments remaining |
| `batch` | `acked_packet_ids`, `num_acked` | packet ids surfaced since the last update |
| `outbox` | the fill phase's staged frames | sealed packets not yet flushed, in fill order |

**State the receiver owns** (`rocketnet_replay_protection_t`, `rocketnet_reassembly_set_t`):

| Variable | Code | Meaning |
|---|---|---|
| `highestOpened`, `opened` | `highest_opened`, `window[]` | the replay window as the set of opened sequences in `(highestOpened - Window, highestOpened]` |
| `drained` | the receive drain | packets drained this update |
| `slots` | `slots[]` | per slot: id, count, stride, final size, the staged final, the received set, and `placed` (index to `[id, bytes]`, the slot's arena made visible) |
| `numDelivered`, `batchFull` | `num_delivered`, `batch_full` | completions delivered this update; a completion dropped at the bound |
| `lastDelivery` | `completed_payload` | the slot as it was at its last completion |

Ghosts: `burned` (sequences spent and never sent), `fragsOf` (packet id to its
sequences), `surfacedCount`, `deliveredCount`, `openedEver`, `namedEver`, `badStale`.

**Actions**, one per operation and per outside event:

| Action | Who | Code |
|---|---|---|
| `StartPayload` | the application | `start_packet` + `finish_packet` on the wire route: one sequence, one record with the packet id |
| `StartFragmented(cnt)` | the application | the arena route: `rocketnet_fragment_plan`, `start_fragments` reserves `cnt - 1` sequences after the one `start_packet` spent, the payload id is drawn, `open_aggregate` pins the packet id behind `cnt` acknowledgments, `seal_fragment` records each fragment |
| `BurnOne` | a failed finish | a sequence spent and never sent (USAGE.md, "Not every NULL from start_packet spends one") |
| `BurnFragmented(cnt)` | a seal refusal inside the fragment loop | the whole block withdrawn after the payload id and the aggregate were drawn (USAGE.md, "one such failure burns all 14"). Only a seal refusal burns an id: a full ring or a failed `start_fragments` returns before the draw (`sdk/rocketnet_client_transport.cpp:805-826`, the draw at 828) and burns the one sequence `start_packet` spent, which is `BurnOne` |
| `DefectiveSend` | a defective sender | a sealed fragment whose header says anything (`DefectIds` non-empty) |
| `Flush` | `send_packets` | the staged packets go to the wire in fill order, `Link!Send` |
| `RecvAck` | a block arrives | `rocketnet_ack_endpoint_process_block` / `rocketnet_ack_apply_run`: the walk, the retained window, unknown sequences, the batch and its deferral, aggregates |
| `SendAck` | the receiver | a session packet carrying the current block, `rocketnet_ack_block_build`: the first run, then `(gap, run)` pairs to the floor, at most `MaxRanges` |
| `RecvData` | a packet arrives | the reading order's replay check (`rocketnet_replay_already_received`, `rocketnet_replay_advance`), then a payload or `rocketnet_reassembly_process_fragment` with the standard's checks in order |
| `Tick` | time | one update on both ends: the batch drains, the delivered arena and the drain reset, both links `Age` |
| `D2REnvironment`, `R2DEnvironment` | the path | Link's `Drop`, `Duplicate`, `Reorder`, `Delay` |

Every packet that drives a transition crosses a Link instance: `Flush` and `SendAck` are
`Link!Send`, `RecvData` and `RecvAck` are `Link!Deliver`, `Tick` is `Link!Age`. A packet
the receiver refuses (replay, or any of the fragment checks) is still delivered and
discarded, so no packet type is without a receive step.

### Properties

Safety, checked under every loss pattern with no fairness:

| Property | The rule |
|---|---|
| `SurfacedOnce` | the acked batch names a packet id at most once, ever (USAGE.md, "the packet ids newly acknowledged by the last update") |
| `SurfacedOnlyIfOpened` | a packet id surfaces only when every sequence carrying it was opened by the receiver: for a fragmented payload, every fragment (USAGE.md, "a fragmented payload's id surfaces in the acked batch only when every fragment has been acknowledged"). Opened, not delivered: the acks cover a payload that reassembly then refused (`FRAGMENT_STALE`, `FRAGMENT_PAYLOAD_BATCH_FULL`), which USAGE.md:122-124 allows at the fragment level; the gap is real and D1 and D2 both sit in it |
| `BurnedNeverNamed` | a burned sequence is never opened and never named by a block, so it is never acknowledged as received |
| `AckedMeansSurfaced` | a record with a packet id is marked acknowledged only once its id has surfaced, and an aggregate reaches zero only once its id has: the batch defers, it never drops (`sdk/rocketnet_ack.cpp`, "a packet id that cannot fit the batch is deferred, never dropped"); the safety face of `OpenedSurfacesOrLaps` |
| `DeliveredExact` | a delivered payload is exactly the fragments of one id, each index once, every non-final fragment the stride, the final at most the stride (STANDARD.md, Fragmentation, "Fragment bytes land at index × stride") |
| `DeliveredOnce` | an honest payload is delivered at most once |
| `CompleteDelivers` | no slot holds a complete payload: delivery happens in the fragment's own process call, so "within one update" holds with nothing to wait for |
| `ArenaBounded` | reassembly state stays inside what was allocated at create: placed bytes inside `stride × (cnt − 1) + final ≤ max payload`, the staging buffer one fragment of the receive maximum, the received set inside the pinned count |
| `CountPinned` (action property) | the first fragment of a payload id fixes its count (STANDARD.md, `FRAGMENT_COUNT_MISMATCH`) |
| `TypeOK` | types, including both links' |

Two more are the findings below, each in its own run and expected to fail on the code:
`StaleOnlyIfOlder` (D1) and `BatchFullNeverFires` (D2).

Out of scope: a liveness property for stale-slot recycling (an occupant that is never
completed is eventually displaced). The code has no such rule, D1 is the proof, and the
property belongs beside the fix.

Liveness, under the fairness described below:

| Property | The rule |
|---|---|
| `OpenedSurfacesOrLaps` | a payload every fragment of which the receiver opened surfaces in the sender's batch, unless the sender's history laps it first (USAGE.md, "Missing a notification therefore does not prove that a payload was lost"); a deferred id surfaces from a later block |

### Loss patterns TLC explored

Both links take a Link pattern. The committed run is `"arbitrary"` on both (any copy may
be dropped at any time, with duplication and reordering; `find-stale-ahead2` adds a tick
of jitter); the four
deterministic patterns `"none"`, `"first"`, `"alternate"` (the first packet dropped,
the second through, and so on) and `"burst"` (`BurstStart = 3`, `BurstLen = 2`: the
third and fourth sends lost, a whole two-fragment payload) are the `pat-*` rows. An old packet arriving late is `Reorder` carrying a copy past later traffic plus
`Delay`; a replay of a captured packet is `Duplicate` at a later time, which the replay
window refuses.

## What the model abstracts away

- **The replay window's representation.** `opened` is a set; the 64-word bitmap and
  its large-skip clear (F1, `docs/TLA-INVENTORY.md`) belong to the RNReplayWindow
  module. Every block here is truthful by construction, so `SurfacedOnlyIfOpened` says
  what the ack layer does with truthful blocks; F1 breaks it from below.
- **The 32-bit wire fields.** `largest received` is carried whole; the reconstruction
  against `highest_sent` (`rocketnet_reconstruct_ack_largest`) and the u16 range caps
  are not modelled. Payload ids are modulo `IdSpace` instead of 2^32, with `FirstId` to
  start a run just under the wrap.
- **Bytes.** A fragment's size is its payload; `placed` records which id's bytes sit at
  each index so aliasing is observable, without the bytes themselves.
- **The reverse direction's data.** B sends only blocks (a standalone ack's content);
  the blocks A's own packets carry about B's sequences are the same machine mirrored.
  The ack debt threshold is not modelled: the standard lets a peer send a block
  whenever it likes, and `SendAck` may.
- **Many fill threads.** One sender stages one payload at a time; the sequence block a
  fragmented payload reserves is therefore contiguous, as the code's comment says it
  need not be.
- **The merged code's run pre-scan** in `rocketnet_ack_apply_run` (skip a run with
  nothing left to acknowledge): it changes only the `ACK_UNKNOWN_SEQUENCE` counter, which
  is not state here. (It also means a run of only burned sequences no longer counts
  them; a counter observation, not a defect.)
- **Keep-alive, disconnect and standalone-ack records** (no packet id): nothing
  surfaces for them, so they are not sent.

### Fairness, and why it is stated per link

`Spec` is weakly fair on `Tick`, strongly fair on the four sends and receives (`Flush`,
`SendAck`, `RecvData`, `RecvAck`), weakly fair on both links' `Age`, and assumes per
link that a link sent on without bound delivers without bound:
`[]<> <<Flush>>_vars => []<> <<RecvData>>_vars`, and for the ack link
`[]<> <<SendAck>>_vars => []<> <<RecvAck /\ Head(r2dWire).pkt = Block>>_vars`, where
`Block` is the block the receiver holds at that moment. The ack-link form names the
current block on purpose: without it (the first version of this model said
`[]<> <<RecvAck>>_vars`) the assumption is unsound, because a wire that keeps
delivering stale duplicates of an old block while every current block is dropped
satisfies it; a cold read proved that with TLC (`MaxSeq = 1`, an arbitrary ack link with
duplication: `OpenedSurfacesOrLaps` violated after 5,308 distinct states, a lasso from
state 26 back to 15). The corrected form is Link's `WireFair` for the one deliverable
copy (the head) and its `EventualDelivery` for the packet that matters, rather than
either stated for every packet value.

The reason for not using `Link!Fairness` itself is TLC, not the model: with it (strong
fairness on `Deliver(p)` for every `p` in `Packets`, and the ack link's `Packets` is
`SUBSET Seqs`, 256 values) TLC 2.19 reports every temporal property "a tautology (its
negation is unsatisfiable), satisfiability problem has 0 branches" and checks nothing.
The bisection (`link-fair` row, and the probe runs it summarises): the model's own
fairness alone, `WF` on `Age` alone, or a single `SF_vars(Deliver(p))` each give a
checkable formula; either link's quantified `WireFair` alone gives 0 branches.
`MCClientConnection` carries the same quantification over a few dozen packet values and
TLC checks it. That the limit is the size of the expansion is a guess; the TLC source
was not read for this.

Two lassos fixed the strength of the model's own fairness, both the kind README-Link.md
warns of:

1. *Weak fairness on the receives.* Two copies of a block sit at the ack link's
   capacity; `Age` makes the head due, `Delay` puts a tick of jitter back before anyone
   receives, and `SendAck` is disabled by the full wire so the per-link implication is
   vacuous (13 states, back to state 8). A head that is due infinitely often must be
   received infinitely often: `SF` on `RecvAck` and `RecvData`.
2. *Weak fairness on the sends.* Two copies of the first, empty block fill the ack
   link; every `RecvAck` frees a slot and `Duplicate` refills it with another copy of the
   empty block before `SendAck` runs, so the block naming the opened payload never
   reaches the wire (12 states, back to state 7, 405,930 distinct states, 8 min 37 s).
   `Capacity` is a model-size knob and never a reason a sender is shut out, so a sender
   that finds room infinitely often sends infinitely often: `SF` on `Flush` and
   `SendAck`.

`Window = SentRing` means a sequence the sender still retains is one the receiver can
still name (the code's `ROCKETNET_HISTORY_PACKETS` sizes both). Liveness was then checked
only at the `live-small` instance; the `live` run on the committed instance did not
finish (see the results).

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19), from `tla/`, each run
with its own `-metadir` and `java.io.tmpdir` (parallel starts collide on both):

```sh
java -Djava.io.tmpdir=./meta-main -cp tla2tools.jar tlc2.TLC -workers 4 -cleanup \
     -metadir ./meta-main -config MCFragmentsAndAcks.cfg MCFragmentsAndAcks
```

`MCFragmentsAndAcks.cfg` is the committed run: the honest sender under the code's rules
(`Witness = "none"`, `StaleRule = "code"`), burns on, both links `"arbitrary"` under
`SpecSafety`, checking the nine invariants and `CountPinned`. `MCFragmentsAndAcksLive.cfg`
is the same instance under `Spec` with `OpenedSurfacesOrLaps` added. The
instance: `MaxSeq = 4`, `SentRing = Window = 4`, `AggRing = 2`, `BatchCap = 1`,
`MaxRanges = 2`, `Slots = 1`, `IdSpace = 8`, `StaleAhead = 2`, `MaxGen = 2`, counts
`{2, 3}`, `Stride = 2`, `FinalBytes = 1`, `MaxPayload = 5`, `MaxFragment = 3`,
`DrainCap = 4`, `DeliveredCap = 2`, `Capacity = 2`, `MaxDelay = 0`, duplication and
reordering on. Four sequences are two payload ids' worth: enough for an eviction, a
burn beside a send, a deferral and every witness. The safety state space at this size
is 44.9 million distinct states (depth 49, 12 minutes with 4 workers); at `MaxSeq = 6`
with jitter it passes 100 million without finishing in 30 minutes, and at `MaxSeq = 8`
it passes 95 million. The two findings reproduce in seconds at any of these sizes, so
their rows keep the larger instances.

The other runs are the committed file with a few lines changed. Three shapes recur:

- *safety*: `SPECIFICATION SpecSafety`, no `PROPERTIES`;
- *defect*: `DefectIds = {0, 1, 5}`, `MaxGen = 1`, `Capacity = 1`, duplication,
  reordering and burns off (the defective sender is about header values, not the path);
  the two defect witnesses use it. The `defect` row itself is smaller still: `DefectIds
  = {0, 1}`, `MaxCount = 2`, counts `{2}`, because the three-id instance passes 272
  million distinct states without finishing;
- *pattern p*: both `Pattern` lines set to `p`;
- *stale-small*: counts `{2}`, duplication and reordering off, pattern none, burns on:
  the instance on which the code's stale rule and the standard's are compared.

| Run | Changes | Expected |
|---|---|---|
| committed | none (`SpecSafety` + `CountPinned`) | passes |
| live | `MCFragmentsAndAcksLive.cfg`: `Spec` + `OpenedSurfacesOrLaps` | not finished |
| live-small | live with `MaxSeq = 3`, `MaxGen = 1`, duplication and reordering off | see results |
| pat-none / first / alternate / burst | pattern, `SpecSafety` + `CountPinned` | not run |
| find-stale | safety + `StaleOnlyIfOlder`, `MaxSeq = 6`, `MaxGen = 3`, `StaleAhead = 1` (the `num_slots = 256` geometry: one whole payload lost poisons) | **fails**: D1 |
| find-stale-small | find-stale + stale-small | **fails**: D1 |
| find-stale-serial-small | find-stale-small + `StaleRule = "serial"` | passes: the standard's rule |
| find-stale-ahead2 | safety + `StaleOnlyIfOlder`, `MaxSeq = 8`, `MaxGen = 4`, `MaxDelay = 1` (`StaleAhead = 2`: two whole payloads lost poison) | **fails**: D1 |
| find-stale-plain | find-stale + `StaleRule = "plain"`, `FirstId = 6` | **fails**: the standard's own witness, a plain comparison across the wrap |
| find-batchfull | safety + `BatchFullNeverFires`, `MaxSeq = 6`, `MaxGen = 3`, `Slots = 2`, `StaleRule = "serial"`, counts `{2}`, pattern none, duplication and burns off | **fails**: D2 |
| defect | the small defect instance, `SpecSafety`, invariants + `CountPinned` | passes |
| wit-firstfrag | safety, `Witness = "firstfrag"` | **fails** `SurfacedOnlyIfOpened` |
| wit-dropfull | safety, `Witness = "dropfull"` | **fails** `AckedMeansSurfaced` |
| wit-overclaim | safety, `Witness = "overclaim"` | **fails** `BurnedNeverNamed` |
| wit-noclear | safety, `Witness = "noclear"` | **fails** `DeliveredExact` |
| wit-nosizecheck | defect + safety, `Witness = "nosizecheck"` | **fails** `ArenaBounded` |
| wit-nopin | defect, `Witness = "nopin"` | **fails** `CountPinned` |
| link-fair | `SPECIFICATION SpecLinkFair` | TLC: tautology, 0 branches (the fairness note above) |

### Results, hetzner2, 2026-10-13 (TLC 2.19, 4 workers, `-Xmx6g`)

| Run | Result | States | Distinct | Depth | Time |
|---|---|---|---|---|---|
| committed (`SpecSafety`, all invariants + CountPinned) | pass | 317,743,656 | 44,927,746 | 49 | 11 min 52 s |
| live (`Spec` + `OpenedSurfacesOrLaps`, committed instance) | not finished: stopped at 1,928,008 distinct states, BFS at depth 18, 2 h 20 min; no row, it carries no evidence | | | | |
| live-small (`MaxSeq = 3`, `MaxGen = 1`, no dup, no reorder) | pass | 229,436 | 54,283 | 25 | 1 min 0 s |
| find-stale (`MaxSeq = 6`, `MaxGen = 3`, `StaleAhead = 1`) | **StaleOnlyIfOlder violated**, 10 states (D1) | 852,185 | 185,694 | | 4 s |
| find-stale-small (find-stale, counts `{2}`, no dup, no reorder) | **StaleOnlyIfOlder violated**, 10 states (D1) | 65,364 | 23,258 | | 0 s |
| find-stale-serial-small (find-stale-small, `StaleRule = "serial"`) | pass: the standard's rule on the instance the code's rule fails | 90,830,297 | 19,697,444 | 43 | 3 min 21 s |
| find-stale-ahead2 (`MaxSeq = 8`, `MaxGen = 4`, `MaxDelay = 1`) | **StaleOnlyIfOlder violated**, 11 states (D1) | 8,174,784 | 1,739,380 | | 21 s |
| find-stale-plain (find-stale, `"plain"`, `FirstId = 6`) | **StaleOnlyIfOlder violated**, 10 states (the standard's witness) | 693,732 | 152,783 | | 4 s |
| find-batchfull (`MaxSeq = 6`, `MaxGen = 3`, `Slots = 2`, serial, counts `{2}`, none, no dup, no burns) | **BatchFullNeverFires violated**, 18 states (D2) | 527,835 | 190,328 | | 4 s |
| defect (`DefectIds = {0, 1}`, `MaxCount = 2`, counts `{2}`, `MaxGen = 1`, `Capacity = 1`, no dup, no reorder, no burns) | pass | 82,754,545 | 17,468,462 | 30 | 2 min 44 s |
| wit-firstfrag | SurfacedOnlyIfOpened violated, 6 states | 2,042 | 887 | | 0 s |
| wit-dropfull | AckedMeansSurfaced violated, 9 states | 21,845 | 7,792 | | 1 s |
| wit-overclaim | BurnedNeverNamed violated, 6 states | 3,350 | 1,412 | | 0 s |
| wit-noclear | DeliveredExact violated, 9 states | 29,470 | 10,359 | | 0 s |
| wit-nosizecheck (`DefectIds = {0, 1, 5}`, `MaxGen = 1`, `Capacity = 1`, no dup, no reorder, no burns) | ArenaBounded violated, 4 states | 16,193 | 10,853 | | 0 s |
| wit-nopin (same defect settings) | CountPinned violated, 9 states | 362,985 | 171,050 | | 2 s |
| link-fair | TLC: "Temporal formula is a tautology", 0 branches | | | | 3 s |

Not run: `pat-none`, `pat-first`, `pat-alternate`, `pat-burst`. Stopped without finishing
and not recorded: the `live` run above, a three-id defect instance (272 million distinct
states in 50 minutes) and the serial rule at `MaxSeq = 6` with jitter (106 million).

## What TLC found

Every counterexample below was replayed by hand through the functions it names.

**Severity, from the cold reads.** D1 is HIGH: it is permanent for the connection's
life, and the payloads it refuses were acknowledged to the sender (the ack block is
built from the replay window, before reassembly), so a delta-compression scheme built on
the notifications takes a baseline the peer never delivered. Its fix is the serial
comparison STANDARD.md:1913-1919 specifies. D2 is MEDIUM on the server and LOW on the
client.

### D1. A loss burst, or one late fragment, poisons a reassembly slot for the connection's life (defect, HIGH)

`sdk/rocketnet_fragment.cpp:262-268`, the stale test:

```c
if ( slot->payload_id != 0 && payload_id != slot->payload_id &&
     payload_id - slot->payload_id > ROCKETNET_MAX_FRAGMENTS )
    ... FRAGMENT_STALE, return 0;
```

The comment above it argues that the next honest id for a slot is at most
`ROCKETNET_MAX_FRAGMENTS` (256) ahead of its occupant, because the id counter advances
by one per fragmented payload and the slot recurs every `num_slots` ids. That holds only
while the occupant is the slot's latest id. It is not when payloads die whole: a payload
none of whose fragments arrive never touches the slot, so the occupant stays and the
gap to the next honest id grows.

TLC's trace (`find-stale` and `find-stale-small`, 10 states, `Slots = 1`,
`StaleAhead = 1`): payload id 1
(gen 1) is sent and its fragment 0 opened, so slot 0 holds id 1 with one of two
fragments. Id 2 is consumed with no fragment reaching the receiver (in the trace by a
burned finish; a link that loses both its fragments is the same state). Id 3's
fragment 0 arrives: `3 - 1 = 2 > 1`, refused as stale, counted `FRAGMENT_STALE`.
`find-stale-ahead2` is the same with `StaleAhead = 2` and four ids: ids 2 and 3 lost
whole, id 4 refused (11 states). The occupant is still id 1, incomplete, and every later
honest id is further ahead, so every later fragmented payload for this slot is refused
as stale until `rocketnet_reassembly_set_reset_connection` (a reconnect). The abandoned
counter never fires for them: the loss is reported as "stale", which the standard
reserves for superseded payloads.

Replayed through the code at the shipped geometry, there are two routes in.

*The loss route.* With `num_slots = 8` (the default, `sdk/rocketnet.h:433`) the ids of
one slot are 8 apart, so with an incomplete occupant X the slot tolerates the next 32
payloads of its own series lost whole (X+8 .. X+256; X+256 is exactly 256 ahead and
passes) and refuses the 33rd, X+264, and everything after. Only the slot's series has to
be lost: the other seven slots' payloads can arrive and complete throughout. At
`num_slots = 256`, which the configuration admits (`sdk/rocketnet.h:512`, up to
`ROCKETNET_MAX_FRAGMENTS`), the tolerance is zero. A seal refusal inside the fragment
loop (`BurnFragmented`) consumes an id with nothing reaching the wire and counts toward
the same 32.

*The one-packet route.* The stale test is skipped for an empty slot
(`slot->payload_id != 0`, `fragment.cpp:262`). A delayed fragment of a payload that was
abandoned long ago lands in a slot that a completion has just emptied, re-occupies it
with its old id, and from then on every live id is more than 256 ahead of the occupant:
from id X+264 on, every payload in that slot is refused, with no loss burst at all. The
reader's harness (`scratchpad/d1h/harness.cpp` on the Studio, to become the fix card's
test) drove this: 644 of 736 payloads delivered, 92 `FRAGMENT_STALE`, exactly one slot
in eight, no other loss. It holds for payloads of up to about 15 fragments (past that the
delayed fragment is outside the 4,096-sequence replay window and refused there). The
model's window (`Window = SentRing = MaxSeq = 4`) is too small to tell "late but inside
the replay window" from "too late", so this route is in the hand replay, not in a TLC
trace.

In both routes the refused payloads were acknowledged to the sender, and the abandoned
counter never fires for them: the loss is reported as "stale", which the standard
reserves for superseded payloads.

The standard (Fragmentation, "Older is serial-number arithmetic on the 32-bit values,
so no id can poison a slot", STANDARD.md:1913-1919) specifies the half-space comparison.
The model's `StaleRule = "serial"` is that comparison; `find-stale-serial-small` checks
it on the instance where the code's rule fails (`find-stale-small`), and the result is
in the table. The code's comment chose the bounded difference against a hostile id
"just under 2^31 ahead", which only the connection's own sealed sender can produce.
`find-stale-plain` (`StaleRule = "plain"`, `FirstId = 6` so the id counter wraps during
the run) fails as the standard says a plain comparison must. The fix is a code card, not
this one: the serial comparison the standard names.

### D2. `BATCH_FULL` fires under legitimate load (design claim contradicted; MEDIUM on the server, LOW on the client)

`sdk/rocketnet_fragment.h:107-115` and `sdk/rocketnet_client_transport.cpp:222-229`:
`delivered_capacity` is sized to half the receive drain ("a payload takes at least two
fragments, so one update completes at most half the packets it drains"), and
`BATCH_FULL` is called "a true invariant: zero under any legitimate load". On the client
the half-the-drain argument counts only fragments drained this update; a payload whose
first fragments arrived in an earlier update completes on one packet. On the server the
argument is simply not the code's sizing (below).

TLC's trace (`find-batchfull`, 18 states, `Slots = 2`, `DrainCap = 4`,
`DeliveredCap = 2`, no loss, in-order delivery except one `Reorder`): update 1 drains
fragment 0 of payload 1 (sequence 1) and fragment 0 of payload 2 (sequence 3, reordered
ahead of sequence 2), two slots half full; `Tick`; update 2 drains sequence 2 (payload 1
complete, delivered), sequence 4 (payload 2 complete, delivered, `num_delivered = 2 =
delivered_capacity`), then sequences 5 and 6, payload 3, whose completion is dropped at
`sdk/rocketnet_fragment.cpp:458-462`, counted `FRAGMENT_PAYLOAD_BATCH_FULL`, the slot
cleared: a payload lost with nothing lost on the wire, four packets drained in the
update that the sizing says can complete at most two.

Replayed at the shipped sizes.

*Client (LOW).* `delivered_capacity = 1024 / 2 = 512` (`ROCKETNET_NUM_CLIENT_PACKETS /
2`), 8 slots: 8 carried-over completions plus `(1024 - 8) / 2 = 508` fresh ones is 516.
This needs the carried-over half payloads, which take reordering (as in the trace) or
interleaved fill threads on the sender (each fragment's ring slot is its own
`atomic_add`, `sdk/rocketnet_client_transport.cpp:804`), and a 1,024-packet update.

*Server (MEDIUM).* The server has no per-client drain. It drains one global array of
`ROCKETNET_RECEIVE_PACKETS_PER_CLIENT × ROCKETNET_MAX_CLIENTS = 16 × 1000 = 16,000`
packets per update (`sdk/rocketnet_constants.h:30`,
`sdk/rocketnet_server_socket_linux.cpp:2005`), while the delivered pool is
`max_clients × num_reassembly_slots` (`sdk/rocketnet_server_transport.cpp:834`), 64 × 8
= 512 at the defaults (`sdk/rocketnet.h:718`). So it overflows with no carried-over
payloads at all: 64 clients each completing 9 two-fragment payloads in one update is
1,152 fragments drained, 576 reassembled, 64 `BATCH_FULL`. The comment at
`sdk/rocketnet_server_transport.cpp:826-831` makes the same false claim as
`fragment.h` ("completions cap at eight per client at the defaults"). `drain / 2 +
num_slots` would not fix the server; the pool has to follow the drain (16,000 / 2) or
the drain the pool. A model of one connection cannot see this: the overflow is across
connections, in a pool the model's receiver does not share. The fix is a code card.

### Model errors found and fixed on the way

1. **Two assignments parsed as disjunctions.** `badStale' = badStale \/ (...)` is
   `(badStale' = badStale) \/ (...)` in TLA+; TLC reported `RecvData` leaving `badStale`
   unassigned on exactly the D1 trace. Parenthesised, with `batchFull'`.
2. **Per-packet link fairness.** See "Fairness, and why it is stated per link".
3. **Receive by search.** `RecvData` and `RecvAck` first chose the packet with
   `\E p \in Packets` and let `Link!Deliver(p)` pick the head; TLC evaluated that
   choice for every state and again for every fairness check, at 9,000 states a minute.
   Only the head of the wire is deliverable, so both now name it.
4. **Unsound per-link fairness** (found by the first cold read, see "Fairness"): the
   ack-link assumption did not name the current block, so stale duplicates satisfied it.
   Fixed by `Head(r2dWire).pkt = Block` in the delivery step it demands.

## RUNS.tsv

The rows this card would add, in the layout `tla/README.md` describes (module, config,
states, distinct, result, bench, date). They are not added here because six models land
in parallel on that file.

```
FragmentsAndAcks	MCFragmentsAndAcks.cfg (SpecSafety; checks the nine invariants, CountPinned)	317743656	44927746	pass	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcksLive.cfg with MaxSeq 3, MaxGen 1, Duplication FALSE, Reordering FALSE (Spec; checks the nine invariants, CountPinned, OpenedSurfacesOrLaps)	229436	54283	pass	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with MaxSeq 6, MaxGen 3, StaleAhead 1; checks StaleOnlyIfOlder	852185	185694	fail as expected (D1): Invariant StaleOnlyIfOlder is violated, 10 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with MaxSeq 6, MaxGen 3, StaleAhead 1, Counts {2}, Duplication FALSE, Reordering FALSE; checks StaleOnlyIfOlder	65364	23258	fail as expected (D1): Invariant StaleOnlyIfOlder is violated, 10 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with MaxSeq 6, MaxGen 3, StaleAhead 1, Counts {2}, Duplication FALSE, Reordering FALSE, StaleRule "serial"; checks StaleOnlyIfOlder	90830297	19697444	pass	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with MaxSeq 8, MaxGen 4, MaxDelay 1; checks StaleOnlyIfOlder	8174784	1739380	fail as expected (D1): Invariant StaleOnlyIfOlder is violated, 11 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with MaxSeq 6, MaxGen 3, StaleAhead 1, StaleRule "plain", FirstId 6; checks StaleOnlyIfOlder (the standard's witness)	693732	152783	fail as expected: Invariant StaleOnlyIfOlder is violated, 10 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with MaxSeq 6, MaxGen 3, Slots 2, StaleRule "serial", Counts {2}, Pattern "none", Duplication FALSE, Burns FALSE; checks BatchFullNeverFires	527835	190328	fail as expected (D2): Invariant BatchFullNeverFires is violated, 18 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with DefectIds {0, 1}, MaxCount 2, Counts {2}, MaxGen 1, Capacity 1, Duplication FALSE, Reordering FALSE, Burns FALSE; checks the nine invariants, CountPinned	82754545	17468462	pass	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with Witness "firstfrag" (reversed witness)	2042	887	fail as expected: Invariant SurfacedOnlyIfOpened is violated, 6 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with Witness "dropfull" (reversed witness)	21845	7792	fail as expected: Invariant AckedMeansSurfaced is violated, 9 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with Witness "overclaim" (reversed witness)	3350	1412	fail as expected: Invariant BurnedNeverNamed is violated, 6 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with Witness "noclear" (reversed witness)	29470	10359	fail as expected: Invariant DeliveredExact is violated, 9 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with DefectIds {0, 1, 5}, MaxGen 1, Capacity 1, Duplication FALSE, Reordering FALSE, Burns FALSE, Witness "nosizecheck" (reversed witness)	16193	10853	fail as expected: Invariant ArenaBounded is violated, 4 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with DefectIds {0, 1, 5}, MaxGen 1, Capacity 1, Duplication FALSE, Reordering FALSE, Burns FALSE, Witness "nopin" (reversed witness)	362985	171050	fail as expected: Action property CountPinned is violated, 9 states	hetzner2	2026-10-13
FragmentsAndAcks	MCFragmentsAndAcks.cfg with SpecLinkFair (Link!Fairness on both links)	0	0	TLC 2.19: Temporal formula is a tautology (0 branches); see README-FragmentsAndAcks.md	hetzner2	2026-10-13
```
