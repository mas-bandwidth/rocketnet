# FragmentsAndAcks: fragment reassembly and the ack window

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

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
`sdk/rocketnet_fragment.cpp`). The model was written against the code at `3ad4179`; the
line citations below are re-checked against the code it lands on (`617d498`).

`tla/MCFragmentsAndAcks.tla` and `tla/MCFragmentsAndAcks.cfg` are the small instance
(safety); `tla/MCFragmentsAndAcksLive.cfg` is the liveness run on the same instance. Every
other run is its own root cfg, named once in `tla/CASES.tsv` (group `fragmentsandacks`),
and its measured record is in `tla/RUNS.tsv`, written by `tlacheck` (`tla/README.md`). The
inventory rows are `docs/TLA-INVENTORY.md`, M8 and M9.

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
| `BurnFragmented(cnt)` | a seal refusal inside the fragment loop | the whole block withdrawn after the payload id and the aggregate were drawn (USAGE.md, "one such failure burns all 14"). Only a seal refusal burns an id: a full ring or a failed `start_fragments` returns before the draw (`sdk/rocketnet_client_transport.cpp:871-903`, the draw at 905) and burns the one sequence `start_packet` spent, which is `BurnOne` |
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
| `SurfacedOnlyIfOpened` | a packet id surfaces only when every sequence carrying it was opened by the receiver: for a fragmented payload, every fragment (USAGE.md, "a fragmented payload's id surfaces in the acked batch only when every fragment has been acknowledged"). Opened, not delivered: a completed payload that is then dropped as `FRAGMENT_PAYLOAD_BATCH_FULL` was accepted fragment by fragment and is still acknowledged, which USAGE.md:146-148 allows at the fragment level; the gap is real and the model's rule witnesses sit in it. A fragment reassembly refuses is not in this gap (`NamedOnlyIfAccepted`) |
| `NamedOnlyIfAccepted` | a fragment reassembly refuses (stale, count or size mismatch, payload too large, duplicate index) is never named by a block, so the sender's aggregate never completes on it. The block is built from the ack-eligible window (`accepted_session` in the code), and a fragment enters it only when `rocketnet_reassembly_process_fragment` accepts it (standard/TRANSPORT.md t15.3: a packet is eligible only once its type data passes validation); its sequence is still spent in the replay window |
| `BurnedNeverNamed` | a burned sequence is never opened and never named by a block, so it is never acknowledged as received |
| `AckedMeansSurfaced` | a record with a packet id is marked acknowledged only once its id has surfaced, and an aggregate reaches zero only once its id has: the batch defers, it never drops (`sdk/rocketnet_ack.cpp`, "a packet id that cannot fit the batch is deferred, never dropped"); the safety face of `OpenedSurfacesOrLaps` |
| `DeliveredExact` | a delivered payload is exactly the fragments of one id, each index once, every non-final fragment the stride, the final at most the stride (STANDARD.md, Fragmentation, "Fragment bytes land at index × stride") |
| `DeliveredOnce` | an honest payload is delivered at most once |
| `CompleteDelivers` | no slot holds a complete payload: delivery happens in the fragment's own process call, so "within one update" holds with nothing to wait for |
| `ArenaBounded` | reassembly state stays inside what was allocated at create: placed bytes inside `stride × (cnt − 1) + final ≤ max payload`, the staging buffer one fragment of the receive maximum, the received set inside the pinned count |
| `CountPinned` (action property) | the first fragment of a payload id fixes its count (STANDARD.md, `FRAGMENT_COUNT_MISMATCH`) |
| `TypeOK` | types, including both links' |

`StaleOnlyIfOlder` passes on the code arm and fails on the plain comparison and on the
retired arm. `BatchFullNeverFires` fails on `find-batchfull`, where `DeliveredCap` is
below the bound, and passes on `MCFragmentsAndAcksBatchAtBound.cfg`, where the cap is
the bound. Neither failure is a code defect: the retired arm is a rule the code does
not run, and `find-batchfull` is a below-bound witness.

Out of scope: a liveness property that an occupant which is never completed is
eventually displaced. The shipped stale rule refuses by distance from the newest
completed id and from the occupant.

Liveness, under the fairness described below:

| Property | The rule |
|---|---|
| `OpenedSurfacesOrLaps` | a payload every fragment of which the receiver opened surfaces in the sender's batch, unless the sender's history laps it first (USAGE.md, "Missing a notification therefore does not prove that a payload was lost"); a deferred id surfaces from a later block |

### Loss patterns TLC explored

Both links take a Link pattern. The full instance is `"arbitrary"` on both (any copy may
be dropped at any time, with duplication and reordering; `find-stale-ahead2` adds a tick
of jitter); the four
deterministic patterns `"none"`, `"first"`, `"alternate"` (the first packet dropped,
the second through, and so on) and `"burst"` (`BurstStart = 3`, `BurstLen = 2`: the
third and fourth sends lost, a whole two-fragment payload) are the `pat-*` rows. An old packet arriving late is `Reorder` carrying a copy past later traffic plus
`Delay`; a replay of a captured packet is `Duplicate` at a later time, which the replay
window refuses.

## What the model abstracts away

- **The replay window's representation.** `opened` is a set; the 64-word bitmap and
  its large-skip clear (F1, `docs/TLA-INVENTORY.md`) belong to the ReplayWindow
  module. Every block here is truthful by construction, so `SurfacedOnlyIfOpened` says
  what the ack layer does with truthful blocks. F1 broke it from below; the code now
  clears the whole window on a large skip (`rocketnet_replay_advance`,
  `sdk/rocketnet_replay.cpp:56-61`).
- **The 32-bit wire fields.** `largest received` is carried whole; the reconstruction
  against `highest_sent` (`rocketnet_reconstruct_ack_largest`) and the u16 range caps
  are not modelled. Payload ids are modulo `IdSpace` instead of 2^32, with `FirstId` to
  start a run just under the wrap.
- **Bytes.** A fragment's size is its payload; `placed` records which id's bytes sit at
  each index so aliasing is observable, without the bytes themselves.
- **The reverse direction's data.** B sends only blocks (a standalone ack's content);
  the blocks A's own packets carry about B's sequences are the same machine mirrored.
  The ack debt threshold is not modelled: the standard lets an endpoint send a block
  whenever it likes, and `SendAck` may.
- **Many fill threads.** One sender stages one payload at a time; the sequence block a
  fragmented payload reserves is therefore contiguous, as the code's comment says it
  need not be.
- **Counters.** `ACK_UNKNOWN_SEQUENCE`, and the eviction classification of
  `rocketnet_ack_endpoint_record_sent_classified` (`SEND_HISTORY_LOST`,
  `SEND_HISTORY_OVERFLOW`, from `largest_acked_sequence`), are not state here; the
  classification is `tla/RNSentHistory.tla`'s. (The run pre-scan in
  `rocketnet_ack_apply_run` that the code at `3ad4179` carried is gone at `617d498`.)
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
only at the `live-small` instance; the `live` run on the full instance did not
finish (see the results).

## How to run it

Each run is a root cfg named once in `tla/CASES.tsv` (group `fragmentsandacks`) and runs
through the pinned `tlacheck` as `tla/README.md` describes, each case its own shard under
the 110 s budget:

```sh
for i in $(seq 0 26); do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group fragmentsandacks --shards 27 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/fragmentsandacks-$i"
done
"$job/tlacheck" merge --root . --keep tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/fragmentsandacks-*/RUNS.tsv
```

The full instance is the honest sender under the code's rules (`Witness = "none"`,
`StaleRule = "code"`), burns on, both links `"arbitrary"`: `MaxSeq = 4`,
`SentRing = Window = 4`, `AggRing = 2`, `BatchCap = 1`, `MaxRanges = 2`, `Slots = 1`,
`IdSpace = 8`, `StaleAhead = 2`, `MaxGen = 2`, counts `{2, 3}`, `Stride = 2`,
`FinalBytes = 1`, `MaxPayload = 5`, `MaxFragment = 3`, `DrainCap = 4`,
`DeliveredCap = 2`, `Capacity = 2`, `MaxDelay = 0`, duplication and reordering on. Four
sequences are two payload ids' worth: enough for an eviction, a burn beside a send, a
deferral and every witness. Its safety state space is 44.9 million distinct states
(depth 49, 12 minutes with 4 workers), far over the 110 s budget; at `MaxSeq = 6` with
jitter it passes 100 million without finishing in 30 minutes, and at `MaxSeq = 8` it
passes 95 million. `MCFragmentsAndAcksLive.cfg` is the full instance under `Spec` with
`OpenedSurfacesOrLaps` added, a `bench` row with its debt named, and the `pat-*` bench
rows are the full instance with a pattern.

`MCFragmentsAndAcks.cfg` is the committed run on the gate instance: the full instance
with five cuts, both links `"none"` (no loss), duplication off, `MaxCount = 2` with counts
`{2}`, and burns off, under `SpecSafety`, checking the thirteen invariants (nine,
`NamedOnlyIfAccepted`, and the three bitmap invariants below) and `CountPinned`. It is
1,056,577 distinct states at depth 39 and passes inside the budget, so it is a
`required` row. Reordering stays on with `Capacity = 2`, so payload 2's first fragment
can overtake payload 1's second; reassembly then refuses the late fragment as stale,
which is the case `NamedOnlyIfAccepted` is about. The gate instance therefore reaches the
refusal: `MCFragmentsAndAcksWitAckRefused.cfg` is the same instance with
`Witness = "ackrefused"` and violates the invariant at depth 12. What the cuts leave out
is checked elsewhere: duplication by `live-dup`, loss with three-fragment payloads and
burns by `pat-alternate` (a `required` pass on the full instance), and burns lapping a
held slot by `bits`. The below-bound batch witness still violates inside the budget.
The code-arm stale rows are shrunk so their passes finish inside it.

The other cfgs are the full instance with a few lines changed. Three shapes recur:

- *safety*: `SPECIFICATION SpecSafety`, no `PROPERTIES`;
- *defect*: `DefectIds = {0, 1, 5}`, `MaxGen = 1`, `Capacity = 1`, duplication,
  reordering and burns off (the defective sender is about header values, not the path);
  the two defect witnesses use it. The `defect` row itself is smaller still: `DefectIds = {0, 1}`, `MaxCount = 2`, counts `{2}`, `MaxSeq = 2`;
- *pattern p*: both `Pattern` lines set to `p`;
- *stale-small*: counts `{2}`, duplication and reordering off, burns on. The code-arm
  row uses `IdSpace = 16` and finite-burst loss so completions remain reachable; the
  full safety exploration is retained as bench debt.

| Run | Cfg | Changes | Expected | Gate |
|---|---|---|---|---|
| committed | `MCFragmentsAndAcks.cfg` | the gate instance: both links `"none"`, duplication off, `MaxCount = 2`, counts `{2}`, burns off (`SpecSafety` + `CountPinned`) | passes | required |
| live | `MCFragmentsAndAcksLive.cfg` | `Spec` + `OpenedSurfacesOrLaps` | passes | bench: over budget |
| live-small | `MCFragmentsAndAcksLiveSmall.cfg` | live with `MaxSeq = 2`, `MaxGen = 1`, `Capacity = 1`, duplication and reordering off | passes | required |
| live-dup | `MCFragmentsAndAcksLiveDup.cfg` | live with `MaxSeq = 1`, `MaxGen = 1`, `MaxCount = 2`, counts `{2}`, burns off, data link `"none"`, ack link `"arbitrary"` with duplication, reordering off; `OpenedSurfacesOrLaps` only (no INVARIANTS, no CountPinned): the instance on which the old per-link fairness failed; `live-small` has duplication off and cannot tell the two forms apart | passes: the evidence the fairness fix works on the failing case | required |
| pat-none / first / burst | `MCFragmentsAndAcksPatNone.cfg`, `...PatFirst.cfg`, `...PatBurst.cfg` | pattern, `SpecSafety` + `CountPinned` | passes | bench: over budget |
| pat-alternate | `MCFragmentsAndAcksPatAlternate.cfg` | pattern, `SpecSafety` + `CountPinned` | passes | required |
| find-stale | `MCFragmentsAndAcksFindStale.cfg` | safety + `StaleOnlyIfOlder`, `StaleRule = "code"`, `IdSpace = 16`, `FirstId = 3`, `MaxSeq = 6`, `MaxGen = 3`, `StaleAhead = 1`, counts `{2}`, burst loss at send 4, ack link `"none"`, `Capacity = 1`, duplication and reordering off | passes: the shipped rule with the reference advanced past the initial value; an honest id 5 arrives beyond `Reach` of reference 0 against a lower-generation occupant, which fails with `AdvanceCompleted` a no-op | required |
| find-stale-small | `MCFragmentsAndAcksFindStaleSmall.cfg` | code arm, `IdSpace = 16`, counts `{2}`, burns on, data loss `"burst"`, ack link `"arbitrary"`, `Capacity = 2` | over the 110 s budget; unmeasured debt, not a completed pass | bench: over budget |
| find-stale-serial-small | `MCFragmentsAndAcksFindStaleSerialSmall.cfg` | the unreduced small instance with `StaleRule = "serial"` | passes: the half-space comparison | bench: over budget |
| find-stale-ahead2 | `MCFragmentsAndAcksFindStaleAhead2.cfg` | safety + `StaleOnlyIfOlder`, `StaleRule = "code"`, `IdSpace = 16`, `MaxSeq = 8`, `MaxGen = 4`, `MaxDelay = 1`, `StaleAhead = 2`, counts `{2}`, burns off, data loss `"burst"`, ack link `"none"`, `Capacity = 1`, duplication and reordering off | over the 110 s budget; unmeasured debt, not a completed pass | bench: over budget |
| find-stale-retired | `MCFragmentsAndAcksFindStaleRetired.cfg` | the original small arbitrary-loss terrain with `StaleRule = "retired"` | **fails** `StaleOnlyIfOlder`: reversed witness for the old forward difference | required |
| find-stale-plain | `MCFragmentsAndAcksFindStalePlain.cfg` | `StaleRule = "plain"`, `FirstId = 6` | **fails** `StaleOnlyIfOlder`: a plain comparison across the wrap | required |
| find-batchfull | `MCFragmentsAndAcksFindBatchFull.cfg` | safety + `BatchFullNeverFires`, `MaxSeq = 6`, `MaxGen = 3`, `Slots = 2`, `DrainCap = 4`, `DeliveredCap = 2`, `StaleRule = "serial"`, counts `{2}`, pattern none, reordering on, duplication and burns off | **fails** `BatchFullNeverFires`: below-bound witness, not a code defect | required |
| batch-at-bound | `MCFragmentsAndAcksBatchAtBound.cfg` | `MaxSeq = 8`, `MaxGen = 4`, `DeliveredCap = 3` (the bound) and `AckLinkIdle`; the pass reaches the third completion at the cap | passes `BatchFullNeverFires` | required |
| defect | `MCFragmentsAndAcksDefect.cfg` | the small defect instance, `SpecSafety`, invariants + `CountPinned` | passes | required |
| wit-firstfrag | `MCFragmentsAndAcksWitFirstfrag.cfg` | safety, `Witness = "firstfrag"` | **fails** `SurfacedOnlyIfOpened` | required |
| wit-dropfull | `MCFragmentsAndAcksWitDropfull.cfg` | safety, `Witness = "dropfull"` | **fails** `AckedMeansSurfaced` | required |
| wit-overclaim | `MCFragmentsAndAcksWitOverclaim.cfg` | safety, `Witness = "overclaim"` | **fails** `BurnedNeverNamed` | required |
| wit-noclear | `MCFragmentsAndAcksWitNoclear.cfg` | safety, `Witness = "noclear"` | **fails** `DeliveredExact` | required |
| wit-nosizecheck | `MCFragmentsAndAcksWitNosizecheck.cfg` | defect + safety, `Witness = "nosizecheck"` | **fails** `ArenaBounded` | required |
| wit-nopin | `MCFragmentsAndAcksWitNopin.cfg` | defect, `Witness = "nopin"` | **fails** `CountPinned` | required |
| bits | `MCFragmentsAndAcksBits.cfg` | bits: live-small with `SentRing = 2`, `MaxSeq = 4`, `SpecSafety` + `CountPinned`, ack link `"none"` | passes | required |
| wit-walkskip | `MCFragmentsAndAcksWitWalkskip.cfg` | bits, `Witness = "walkskip"`, no `CountPinned` | **fails** `WalkVisitsUnacked` | required |
| wit-staleheld | `MCFragmentsAndAcksWitStaleheld.cfg` | bits, `Witness = "staleheld"`, no `CountPinned` | **fails** `BitsMatchRing` | required |
| wit-ackrefused | `MCFragmentsAndAcksWitAckRefused.cfg` | the gate instance, safety, `Witness = "ackrefused"`: a fragment is ack-eligible as soon as it opens, before reassembly judges it (the order before t15.3 was applied to type 9) | **fails** `NamedOnlyIfAccepted` | required |
| link-fair | none | `SPECIFICATION SpecLinkFair` on the live cfg | TLC refuses the formula as a tautology (the fairness note above) | no case |

Instances shrunk to fit the 110 s budget at two TLC workers: the committed run from the
full instance to the gate instance (above); `live-small` from
`MaxSeq = 3` with `Capacity = 2` to `MaxSeq = 2` with `Capacity = 1`; `defect` from
`MaxSeq = 4` to `MaxSeq = 2`. The code-arm stale rows use `IdSpace = 16`, so `Reach` is
4. `find-stale` is the measured pass: `FirstId` 3 and `MaxGen` 3 give honest ids 3, 4
and 5; the finite burst drops payload 4's final, so payload 3's completion advances the
reference to 3 while payload 4's first fragment remains as the lower-generation
occupant. Id 5 is farther than `Reach` from reference 0 but within it of reference 3,
so the row fails if the reference does not advance. `find-stale-small` and
`find-stale-ahead2` keep larger terrains and the ack link `"none"`; their records are
exit 124 timeouts, so they are unmeasured debt, not completed passes. `find-batchfull`
is not shrunk: `DeliveredCap` 2 is the below-bound witness and it still violates inside
the budget. `batch-at-bound` keeps that geometry and the reorder, raises `MaxGen` to 4,
sets `DeliveredCap` to 3, and constrains the ack link idle so the pass finishes at the
third completion. `find-stale-serial-small` stays a bench row: the half-space
comparison on the unreduced small instance is 19.7 million distinct states.

`link-fair` has no case: TLC exits 77 with "Temporal formula is a tautology (its
negation is unsatisfiable)" after "Implied-temporal checking--satisfiability problem
has 0 branches" (24 s), an outcome `tlacheck` has no kind for (a pass, an invariant, an
action or a temporal violation). `SpecLinkFair` stays in `MCFragmentsAndAcks.tla` so the
probe can be repeated by hand.

### Records

The measured records are the `fragmentsandacks` rows of `tla/RUNS.tsv` (TLC 2.19, the
pinned jar, two workers per case, 110 s per case, each case its own shard, four at
once), re-measured after the three bitmap invariants were added to every cfg with an
`INVARIANTS` section. Every case reaches its declared result except the seven `bench`
rows that time out (exit 124), which is a failed measurement, not a proof.
`MCFragmentsAndAcksPatAlternate.cfg` is a required check, not bench debt. With the bitmap state it
is 1,422,444 distinct states and the recorded run finishes in 57.104 s, inside the budget, so a
regression to exit 124 fails the gate. A saturated bench can still reach that budget with no
counterexample. The
required control for the bitmaps is `MCFragmentsAndAcksBits.cfg`.

| Case | Result | Generated | Distinct | Time |
|---|---|---|---|---|
| `MCFragmentsAndAcks.cfg` | pass: the gate instance | 3,796,546 | 1,056,577 | 85.518 s |
| `MCFragmentsAndAcksLive.cfg` | timeout, bench debt |  |  | 110.397 s |
| `MCFragmentsAndAcksLiveSmall.cfg` | pass | 20,552 | 6,721 | 9.422 s |
| `MCFragmentsAndAcksLiveDup.cfg` | pass | 47,836 | 10,276 | 9.381 s |
| `MCFragmentsAndAcksPatNone.cfg` | timeout, bench debt |  |  | 110.703 s |
| `MCFragmentsAndAcksPatFirst.cfg` | timeout, bench debt |  |  | 110.791 s |
| `MCFragmentsAndAcksPatAlternate.cfg` | pass | 5,924,703 | 1,422,444 | 73.012 s |
| `MCFragmentsAndAcksPatBurst.cfg` | timeout, bench debt |  |  | 110.339 s |
| `MCFragmentsAndAcksFindStale.cfg` | pass: shipped rule, REACH comparison exercised | 1,365,215 | 489,695 | 15.479 s |
| `MCFragmentsAndAcksFindStaleSmall.cfg` | timeout, bench debt |  |  | 110.266 s |
| `MCFragmentsAndAcksFindStaleSerialSmall.cfg` | timeout, bench debt |  |  | 110.337 s |
| `MCFragmentsAndAcksFindStaleAhead2.cfg` | timeout, bench debt |  |  | 110.173 s |
| `MCFragmentsAndAcksFindStaleRetired.cfg` | `StaleOnlyIfOlder` violated (exit 12): retired-arm witness | 62,405 | 23,863 | 2.410 s |
| `MCFragmentsAndAcksFindStalePlain.cfg` | `StaleOnlyIfOlder` violated (exit 12) | 205,517 | 73,495 | 4.247 s |
| `MCFragmentsAndAcksFindBatchFull.cfg` | `BatchFullNeverFires` violated (exit 12): below-bound witness | 1,179,685 | 459,388 | 28.869 s |
| `MCFragmentsAndAcksBatchAtBound.cfg` | pass: reaches the third completion at the cap | 99,320 | 26,650 | 2.792 s |
| `MCFragmentsAndAcksDefect.cfg` | pass | 374,661 | 89,755 | 3.887 s |
| `MCFragmentsAndAcksWitFirstfrag.cfg` | `SurfacedOnlyIfOpened` violated (exit 12) | 1,495 | 679 | 0.862 s |
| `MCFragmentsAndAcksWitDropfull.cfg` | `AckedMeansSurfaced` violated (exit 12) | 23,327 | 8,765 | 1.679 s |
| `MCFragmentsAndAcksWitOverclaim.cfg` | `BurnedNeverNamed` violated (exit 12) | 1,768 | 809 | 0.882 s |
| `MCFragmentsAndAcksWitNoclear.cfg` | `DeliveredExact` violated (exit 12) | 30,943 | 11,465 | 1.736 s |
| `MCFragmentsAndAcksWitNosizecheck.cfg` | `ArenaBounded` violated (exit 12) | 7,612 | 7,068 | 1.362 s |
| `MCFragmentsAndAcksWitNopin.cfg` | `CountPinned` violated (exit 13) | 149,647 | 70,574 | 4.393 s |
| `MCFragmentsAndAcksBits.cfg` | pass | 760,466 | 265,345 | 10.165 s |
| `MCFragmentsAndAcksWitWalkskip.cfg` | `WalkVisitsUnacked` violated (exit 12) | 618 | 316 | 0.867 s |
| `MCFragmentsAndAcksWitStaleheld.cfg` | `BitsMatchRing` violated (exit 12) | 1,152 | 567 | 0.910 s |
| `MCFragmentsAndAcksWitAckRefused.cfg` | `NamedOnlyIfAccepted` violated (exit 12) on the gate instance | 8,335 | 3,858 | 2.310 s |

### Earlier results without the budget (TLC 2.19, 4 workers, `-Xmx6g`, 2026-10-13)

These runs completed outside the 110 s budget, before the per-case records existed. They
are not current records; they are the evidence for the bench rows above.

| Run | Result | States | Distinct | Depth | Time |
|---|---|---|---|---|---|
| full instance (`SpecSafety`, the invariants then + CountPinned) | pass | 317,743,656 | 44,927,746 | 49 | 11 min 52 s |
| live (`Spec` + `OpenedSurfacesOrLaps`, full instance) | not finished: stopped at 1,928,008 distinct states, BFS at depth 18, 2 h 20 min; it carries no evidence | | | | |
| live-small (`MaxSeq = 3`, `MaxGen = 1`, no dup, no reorder) | pass | 229,436 | 54,283 | 25 | 1 min 0 s |
| find-stale-serial-small (unreduced small instance, `StaleRule = "serial"`) | pass: the half-space comparison | 90,830,297 | 19,697,444 | 43 | 3 min 21 s |
| find-stale-ahead2 (unreduced: `MaxSeq = 8`, `MaxGen = 4`, `MaxDelay = 1`, counts `{2, 3}`, duplication and reordering on, old forward difference) | violated `StaleOnlyIfOlder` under the retired arm | 8,174,784 | 1,739,380 | | 21 s |
| defect (`DefectIds = {0, 1}`, `MaxCount = 2`, counts `{2}`, `MaxGen = 1`, `MaxSeq = 4`, `Capacity = 1`, no dup, no reorder, no burns) | pass | 82,754,545 | 17,468,462 | 30 | 2 min 44 s |

The evidence for an over-budget run of `pat-alternate` (not the gate record) is its run with the bitmaps outside the
budget (TLC 2.19, 2 workers, 2026-10-17, before the three invariants were in its cfg):
pass, 5,924,703 states, 1,422,444 distinct, depth 43, 168.8 s.

Never run to completion: `pat-none`, `pat-first`, `pat-burst` at the full instance,
a three-id defect instance (272 million distinct states in 50 minutes) and the serial
rule at `MaxSeq = 6` with jitter (106 million).

## The sent-ring bitmaps

The design the pro card `rocketnet-ack-walk-unacked-only` implements in
`sdk/rocketnet_ack.cpp` (owed: the fields `sent_held`, `sent_unacked` and
`sent_bits_checked` are not in the code yet). Beside the sent ring the endpoint keeps two
bitmaps, one bit per slot: held (the slot holds a written record) and unacked (written and
not yet acknowledged), and the highest sequence whose slot has been reconciled.

| Variable | Code (owed) | Meaning |
|---|---|---|
| `held` | `sent_held` | the slots holding a written record, as a set of slot indexes |
| `unacked` | `sent_unacked` | the slots written and not yet acknowledged |
| `bitsChecked` | `sent_bits_checked` | the highest sequence whose slot the reconcile has checked |
| `lastExpect` | ghost | the named sequences the last block's walk should visit, read from the ring before the walk: in the window, the slot holding the sequence, not acknowledged |
| `lastVisited` | ghost | the named sequences the last block's walk did visit |

**Writes and acks.** `StartPayload` and `StartFragmented(cnt)` set every slot they write in
both bitmaps (`rocketnet_ack_endpoint_record_sent_classified`); a refused write sets
nothing, and with sequences spent in order the model never refuses one. A burn writes no
record and leaves the bits. `ApplyOne` clears its slot from `unacked` wherever it sets
`acked` (`rocketnet_ack_apply_run`); a deferred record (batch full) keeps its bit.

**The reconcile and the walk** (`RecvAck`, the top of
`rocketnet_ack_endpoint_process_block`). For each sequence x from `bitsChecked + 1` to
`highestSent`, clamped below to `WindowLow`, a slot that does not hold x (`sent[x % SentRing].seq # x`: a burned or refused x, or a record that fell below the window) loses
both bits; then `bitsChecked = highestSent`. The walk then visits a named sequence s in the
window only when slot `s % SentRing` has its unacked bit set and holds s.

**Invariants**, listed after the existing ones in every FragmentsAndAcks cfg with an
`INVARIANTS` section (`MCFragmentsAndAcksLiveDup.cfg` has none):

| Property | The rule |
|---|---|
| `BitsCoverRing` | always: every slot whose record is in the window, written and not acknowledged is in `unacked`, and `unacked` is inside `held`; the walk never misses a record |
| `BitsMatchRing` | when `bitsChecked = highestSent`: `held` is exactly the slots whose record is written and at or above `WindowLow`, `unacked` exactly those of them not acknowledged; the check the pro card's `test_ack_block_walk_bits_match_ring` test runs |
| `WalkVisitsUnacked` | `lastVisited = lastExpect`: the bitmap walk visits exactly the sequences the ring says it should |

**Abstracted.** The bits are sets of slot indexes, not words; the walk's word scan (find the
next set bit) is a set filter. The unknown-sequence counter, which the design keeps as a
popcount of `held` against the named span, is not modelled (counters are not state here,
as above). The reconcile runs on every block received, including an empty one.

**The instance.** Every older cfg has `SentRing = MaxSeq`, so no slot is ever lapped and the
reconcile never has a held slot to clear. `MCFragmentsAndAcksBits.cfg` is live-small with
`SentRing = 2` and `MaxSeq = 4` (burns on), so a burn laps a held slot: with sequence 1
in slot 1, burning 3 and sending 4 leaves slot 1 holding a sequence below the window. At
`MaxSeq = 3` no burn can lap a held slot (the lapping sequence must be followed by a
higher send), so 4 is the smallest instance that exercises the clear. Under `Spec` with
`OpenedSurfacesOrLaps` it did not finish inside 110 s (54,886 states generated in a
minute), and under `SpecSafety` with both links `"arbitrary"` it passed in 87 s (591,212
distinct states), too close to the budget on a loaded bench; with the ack link `"none"`
(lossless, data link still `"arbitrary"`) it is the row below. The two witnesses use the
same instance under `SpecSafety`.

```sh
java -XX:+UseParallelGC -cp /opt/tla/tla2tools.jar tlc2.TLC -workers 2 \
  -config MCFragmentsAndAcksBits.cfg MCFragmentsAndAcks.tla
java -XX:+UseParallelGC -cp /opt/tla/tla2tools.jar tlc2.TLC -workers 1 \
  -config MCFragmentsAndAcksWitWalkskip.cfg MCFragmentsAndAcks.tla
java -XX:+UseParallelGC -cp /opt/tla/tla2tools.jar tlc2.TLC -workers 1 \
  -config MCFragmentsAndAcksWitStaleheld.cfg MCFragmentsAndAcks.tla
```

(The recorded runs go through `tlacheck`, as above.)

| Run | Result | Generated | Distinct | Time |
|---|---|---|---|---|
| `MCFragmentsAndAcksBits.cfg` | pass | 760,466 | 265,345 | 17.3 s |
| `MCFragmentsAndAcksWitWalkskip.cfg` | `WalkVisitsUnacked` violated (exit 12) | 618 | 316 | 1.4 s |
| `MCFragmentsAndAcksWitStaleheld.cfg` | `BitsMatchRing` violated (exit 12) | 1,152 | 567 | 1.4 s |

**wit-walkskip** (`Witness = "walkskip"`: the walk skips the lowest named sequence whose
unacked bit is set). Six states: `StartPayload` records sequence 1 in slot 1 (`held = unacked = {1}`), `Flush`, `RecvData` opens 1, `SendAck` sends the block `{1}`, `RecvAck`:
the reconcile finds slot 1 holding 1, `lastExpect = {1}`, and the walk skips 1, so
`lastVisited = {}` and the record stays unacknowledged with its bit set. `BitsCoverRing`
and `BitsMatchRing` hold (the bits still match the ring); `WalkVisitsUnacked` fails. Hand
check: the design's walk visits every named sequence in the window whose unacked bit is set
and whose slot holds it; slot 1 holds 1 with the bit set, so the design visits it, and a
walk that skips it leaves a received packet unacknowledged until a later block names it.

**wit-staleheld** (`Witness = "staleheld"`: the reconcile clears a mismatched slot's unacked
bit but keeps its held bit). Seven states: `StartPayload` records 1 in slot 1,
`BurnFragmented(2)` burns 2 and 3, `Flush`, `StartPayload` records 4 in slot 0 (`held = unacked = {0, 1}`, `highestSent = 4`, `WindowLow = 3`), `SendAck` sends the empty block,
`RecvAck`: the reconcile checks 3 and 4 (`bitsChecked + 1 = 1` clamped to 3); slot 1 holds
1, not 3, so it is cleared from `unacked` but stays in `held`; `bitsChecked = 4`. Now
`held = {0, 1}` while the ring's slots in the window are `{0}`: `BitsMatchRing` fails, and
`BitsCoverRing` holds (`unacked = {0}` inside `held`). Hand check: the design's reconcile
clears both bits of a slot that does not hold its sequence; sequence 1 is below the window
(`highestSent - SentRing + 1 = 3`), so its slot is no longer held, and a held bit left set
would let the next block's walk, or the unknown-sequence count, treat slot 1 as live.

**What the bitmaps cost the other rows.** The new state is not derivable from the ring:
`bitsChecked` records when the last block arrived relative to the sends, and the two ghosts
record what the last block named. `MCFragmentsAndAcksPatAlternate.cfg` grows from 483,020
distinct states (93.1 s, inside the budget, before the bitmaps) to 1,422,444. Run outside
the budget on a loaded bench (TLC 2.19, two workers, before the three invariants were
added to its cfg): 5,924,703 states generated, 1,422,444 distinct, depth 43, no error, in
168.8 s. With the two ghosts held constant in a scratch copy it is 1,027,312 distinct
states in 138.2 s; with `bitsChecked` collapsed to a caught-up flag (the reconcile
starting at `WindowLow` every time, which is equivalent because the reconcile is
idempotent) and the ghosts kept it is 1,147,728 in 117.8 s. No encoding of the bitmap
state keeps that instance inside 110 s on a loaded bench, but on an idle bench its recorded
run, with the three invariants, finishes in 57.104 s, inside the gate budget, so
`MCFragmentsAndAcksPatAlternate.cfg` is a required check (its CASES.tsv row gates) rather than
`bench` debt; the other three pattern rows stay over budget and remain bench debt.

## What TLC found

The rows below are the code arm, the retired arm, and the delivered-bound witness.

The code arm of the stale rule passes `StaleOnlyIfOlder`. That pass is not a code
defect. The retired arm is the old forward difference; its violation of
`StaleOnlyIfOlder` is a reversed witness of a rule the code does not run.
`find-batchfull` is a below-bound witness, not a code defect: `DeliveredCap` is under
the bound in `sdk/rocketnet_fragment.h`, and `batch-at-bound` passes at that bound.

The `find-stale` pass is non-vacuous for the REACH comparison. Its terrain
(`tla/MCFragmentsAndAcksFindStale.cfg`) puts an honest id farther than `Reach` from the
initial reference while a lower-generation occupant is present. With the correct
`AdvanceCompleted` the reference has moved and the id is accepted; with
`AdvanceCompleted` replaced by a no-op the same fragment is refused and TLC reports
`StaleOnlyIfOlder` violated (5,121 distinct states before the violation), so the case
distinguishes the two. The recorded pass generates 1,365,215 states and 489,695
distinct in 15.479 s at two workers.

### The stale rule the code runs

The shipped test is two comparisons. `sdk/rocketnet_fragment.cpp:378-380` reads the
reference and refuses an id farther than `ROCKETNET_REASSEMBLY_REACH` (a quarter of the
id space, `sdk/rocketnet_fragment.h:87`) from the newest completed id in both
directions, before the slot is consulted:

```
const uint32_t reference = set->newest_completed[connection_index];
if ( payload_id - reference > ROCKETNET_REASSEMBLY_REACH && reference - payload_id > ROCKETNET_REASSEMBLY_REACH )
```

`sdk/rocketnet_fragment.cpp:387-388` then refuses an id at most
`ROCKETNET_REASSEMBLY_BEHIND` (`ROCKETNET_MAX_FRAGMENTS`,
`sdk/rocketnet_fragment.h:86`) behind an occupied slot's id:

```
if ( slot->payload_id != 0 && payload_id != slot->payload_id &&
     slot->payload_id - payload_id <= ROCKETNET_REASSEMBLY_BEHIND )
```

`tla/FragmentsAndAcks.tla:513-522` scales those two constants, and `:530-534` is the
`"code"` arm: `Far(new, newestCompleted) \/ BehindOccupant(new, old)`. `Reach` is
`IdSpace \div 4` and `Behind` is `StaleAhead`. `newestCompleted` starts at 0 and
advances on a completion, including one the delivery bound drops, when the forward
distance is nonzero and below half the id space (`tla/FragmentsAndAcks.tla:526-528`,
`sdk/rocketnet_fragment.cpp:600-602`).

Before this change the model's `code` arm was `((new - old) % IdSpace) > StaleAhead`,
one unsigned forward distance against the slot's occupant. The shipped test above is two
comparisons: the reach pair against the newest completed id (`sdk/rocketnet_fragment.cpp:380`)
and the behind bound against the occupant (`:387-388`). The old arm had neither a
reference id nor the occupant's behind bound, so it is kept only as the retired arm.

`find-stale` runs that arm and passes `StaleOnlyIfOlder`. Its terrain reaches a
completion before the far honest id arrives and leaves a lower-generation occupant in
the slot, so the pass fails when `AdvanceCompleted` is a no-op; the `find-stale` table
row and its cfg header state the terrain. `find-stale-small` and `find-stale-ahead2` are `bench`
rows whose records are exit 124 timeouts, not completed passes: they are unmeasured
debt, as the table at "Instances shrunk to fit" says. `find-stale-plain`
(`StaleRule = "plain"`, `FirstId = 6`) still violates `StaleOnlyIfOlder`: a plain
comparison across the wrap. `StaleRule = "retired"` is
`(new - old) % IdSpace > StaleAhead`, the old forward difference against the occupant.
On the small instance that arm violates `StaleOnlyIfOlder`. The violation stays a
reversed witness in `MCFragmentsAndAcksFindStaleRetired.cfg`. `StaleRule = "serial"` remains the
half-space comparison; `find-stale-serial-small` is the bench row for it.

### The delivered bound, and the below-bound witness

`sdk/rocketnet_fragment.h:135-139`: an update that drains N packets on one connection
completes at most `(N + min(N, connections × slots)) / 2` payloads, and at that
capacity `BATCH_FULL` is zero under any load the drain delivers. A set sized below the
bound counts `BATCH_FULL` for each completion past it.

`find-batchfull` is that below-bound witness. `DrainCap` is 4 and `Slots` is 2, so the
bound is 3, and `DeliveredCap` is 2. One reorder leaves two slots half full; the next
update completes both and then a third two-fragment payload, and the third is dropped.
`BatchFullNeverFires` is violated. `MCFragmentsAndAcksBatchAtBound.cfg` is the same
geometry with `MaxGen` 4, which supplies enough payloads to reach the third completion,
and `DeliveredCap` 3, the bound. The pass reaches that third completion at the cap; a
fourth completion in one update would violate `BatchFullNeverFires`, not demonstrate
the pass. The `DeliveredCap` 2 control at the same size drops the third completion
(`batchFull = TRUE`). The ack link stays idle (`AckLinkIdle` in
`MCFragmentsAndAcks.tla`): that schedule sends no ack, and an ack step does not change
reassembly, so every fragment schedule remains and the pass finishes inside the budget.
`BatchFullNeverFires` holds.

### Model errors found and fixed on the way

1. **Two assignments parsed as disjunctions.** `badStale' = badStale \/ (...)` is
   `(badStale' = badStale) \/ (...)` in TLA+; TLC reported `RecvData` leaving `badStale`
   unassigned on the retired-arm trace. Parenthesised, with `batchFull'`.
2. **Per-packet link fairness.** See "Fairness, and why it is stated per link".
3. **Receive by search.** `RecvData` and `RecvAck` first chose the packet with
   `\E p \in Packets` and let `Link!Deliver(p)` pick the head; TLC evaluated that
   choice for every state and again for every fairness check, at 9,000 states a minute.
   Only the head of the wire is deliverable, so both now name it.
4. **Unsound per-link fairness** (found by the first cold read, see "Fairness"): the
   ack-link assumption did not name the current block, so stale duplicates satisfied it.
   Fixed by `Head(r2dWire).pkt = Block` in the delivery step it demands.
