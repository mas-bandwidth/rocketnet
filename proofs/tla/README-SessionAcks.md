# SessionAcks: the session ack window

`proofs/tla/SessionAcks.tla` is one direction of a rocketnet session: a sender A hands
payloads to a receiver B over the shared lossy link `proofs/tla/Link.tla` (instance `D2R`),
and B sends ack blocks back over a second instance (`R2D`). A is the session sequence
and one `rocketnet_ack_endpoint_t` (`sdk/rocketnet_ack.cpp`: `record_sent`,
`process_block`, `apply_run`) with the staged send of the client and server transports
(`sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`). B is two
windows as sets (`sdk/rocketnet_replay.cpp`), `replay_session` and `accepted_session`,
and the block builder (`rocketnet_ack_block_build`, through
`rocketnet_session_ack_block_build` on `accepted_session`).

`proofs/tla/MCSessionAcks.tla` and `proofs/tla/MCSessionAcks.cfg` are the gate instance
(safety); `proofs/tla/MCSessionAcksLive.cfg` is the liveness run on the same instance.
Every other run is its own root cfg, named once in `proofs/tla/CASES.tsv` (group
`sessionacks`), and its measured record is in `proofs/tla/RUNS.tsv`, written by
`tlacheck` (`proofs/tla/README.md`).

## What the model covers

**State the sender owns** (`struct rocketnet_ack_endpoint_t`, the session sequence, the
fill phase's staging):

| Variable | Code | Meaning |
|---|---|---|
| `nextSeq` | `send_sequence_session` | the next sequence to spend |
| `sent`, `highestSent` | `sent[]`, `highest_sequence` | the sent ring: sequence, acknowledged |
| `batch` | `acked_packet_ids`, `num_acked` | packet ids surfaced since the last update |
| `outbox` | the fill phase's staged frame | the sealed packet not yet flushed |
| `held`, `unacked`, `bitsChecked` | `sent_held`, `sent_unacked`, `sent_bits_checked` | the sent-ring bitmaps (below) |

**State the receiver owns** (`rocketnet_replay_protection_t`):

| Variable | Code | Meaning |
|---|---|---|
| `highestOpened`, `opened` | `replay_session.highest_opened`, `.window[]` | the replay window as the set of opened sequences in `(highestOpened - Window, highestOpened]` |
| `highestAccepted`, `accepted` | `accepted_session.highest_opened`, `.window[]` | the sequences the endpoint took in, in `(highestAccepted - Window, highestAccepted]`: the block is built from these |
| `drained` | the receive drain | packets drained this update |

Ghosts: `burned` (sequences spent and never sent), `surfacedCount`, `openedEver`,
`namedEver`, `lastExpect`, `lastVisited`.

A payload's packet id is its sequence. Every packet the model sends is a payload. A
payload enters `accepted_session` when it is accepted
(`rocketnet_server_protocol_accept_payload`, `rocketnet_client_protocol_accept_payload`),
not when it opens. The model takes the payload acks as not deferred, the default: the
transport accepts each payload in the same call that processed it, so `RecvData`
advances both windows in one step and they hold the same set. Deferred acks (the
application accepts later, or never) are not modelled here; `ReplayWindow`'s `Accept`
is that order, and the roadmap entry `session-acks-deferred-accept` (in the notes for
the release) is the step that would bring it here.

**Actions**, one per operation and per outside event:

| Action | Who | Code |
|---|---|---|
| `StartPayload` | the application | `start_packet` + `finish_packet`: one sequence, one record with the packet id |
| `BurnOne` | a failed start or finish | a sequence spent and never sent (USAGE.md, "Not every NULL from start_packet spends one") |
| `Flush` | `send_packets` | the staged packet goes to the wire in fill order, `Link!Send` |
| `RecvAck` | a block arrives | `rocketnet_ack_endpoint_process_block` / `rocketnet_ack_apply_run`: the reconcile, the walk, the retained window, unknown sequences, the batch and its deferral; an empty block (largest 0) changes nothing, as both `process_block` and `process_block_full` return before the reconcile |
| `RecvAckRepeat` | the memo's block arrives again | the reconcile, nothing walked |
| `SendAck` | the receiver | a session packet carrying the current block, `rocketnet_ack_block_build` on `accepted_session`: the first run, then `(gap, run)` pairs to the floor, at most `MaxRanges` |
| `RecvData` | a packet arrives | the reading order's replay check on `replay_session` (`rocketnet_replay_already_received`, `rocketnet_replay_advance`), then the accept advancing `accepted_session` |
| `Tick` | time | one update on both ends: the batch drains, the drain resets, both links `Age` |
| `D2REnvironment`, `R2DEnvironment` | the path | Link's `Drop`, `Duplicate`, `Reorder`, `Delay` |

Every packet that drives a transition crosses a Link instance: `Flush` and `SendAck` are
`Link!Send`, `RecvData` and `RecvAck` are `Link!Deliver`, `Tick` is `Link!Age`.

### Properties

Safety, checked under every loss pattern with no fairness:

| Property | The rule |
|---|---|
| `SurfacedOnce` | the acked batch names a packet id at most once, ever (USAGE.md, "the packet ids newly acknowledged by the last update") |
| `SurfacedOnlyIfOpened` | a packet id surfaces only when the receiver opened its sequence |
| `BurnedNeverNamed` | a burned sequence is never opened and never named by a block |
| `AckedMeansSurfaced` | a record is marked acknowledged only once its packet id surfaced: the batch defers, it never drops |
| `BitsCoverRing` | every slot whose record is in the window, written and not acknowledged is in `unacked`, and `unacked` is inside `held` |
| `BitsMatchRing` | once the reconcile has caught up (`bitsChecked = highestSent`), `held` is exactly the slots whose record is in the window and `unacked` exactly those not acknowledged |
| `WalkVisitsUnacked` | the bitmap walk visits exactly the named sequences whose ring record holds them unacknowledged |

Liveness, under `Spec`:

| Property | The rule |
|---|---|
| `OpenedSurfacesOrLaps` | a payload the receiver opened surfaces in the sender's batch, unless the sender's history laps it first (USAGE.md, "Missing a notification therefore does not prove that a payload was lost") |

Reversed witnesses, one constant flip each (the code is `Witness = "none"`):

| Witness | The flip | Violates |
|---|---|---|
| `dropfull` | a packet id that does not fit the batch is dropped instead of deferred | `AckedMeansSurfaced` |
| `overclaim` | the block names every sequence in the window, opened or not | `BurnedNeverNamed`, and with burns off `SurfacedOnlyIfOpened` |
| `walkskip` | the walk skips the lowest named sequence whose unacknowledged bit is set | `WalkVisitsUnacked` |
| `staleheld` | the reconcile keeps a lapped slot's held bit | `BitsMatchRing` |
| `skipdeferred` | the block memo is kept after an apply that deferred an id | `RepeatExact` |

`SurfacedOnce` and `BitsCoverRing` have no reversed witness.
`MCSessionAcksBrokenLiveness.cfg` is `OpenedSurfacesOrLaps`'s reversed witness: the
`dropfull` defect retains an opened payload without surfacing it.

## What the model abstracts away

- **The windows' representation.** `opened` and `accepted` are sets; the 64-word bitmap
  and its large-skip clear belong to the ReplayWindow module, and so does the order in
  which a packet enters the two windows. Every block here is truthful by
  construction, so `SurfacedOnlyIfOpened` says what the ack layer does with truthful
  blocks.
- **The 32-bit wire fields.** `largest received` is carried whole; the reconstruction
  against `highest_sent` (`rocketnet_reconstruct_ack_largest`) and the u16 range caps are
  not modelled.
- **The reverse direction's data.** B sends only blocks (a standalone ack's content); the
  blocks A's own packets carry about B's sequences are the same machine mirrored. The ack
  debt threshold is not modelled: the standard lets an endpoint send a block whenever it
  likes, and `SendAck` may.
- **Many fill threads.** One sender stages one payload at a time.
- **Counters.** `ACK_UNKNOWN_SEQUENCE`, and the eviction classification of
  `rocketnet_ack_endpoint_record_sent_classified` (`SEND_HISTORY_LOST`,
  `SEND_HISTORY_OVERFLOW`), are not state here; the classification is
  `proofs/tla/RNSentHistory.tla`'s.
- **Keep-alive, disconnect and standalone-ack records** (no packet id): nothing surfaces
  for them, so they are not sent.

## Fairness, and why it is stated per link

`Spec` is weakly fair on `Tick`, strongly fair on the four sends and receives (`Flush`,
`SendAck`, `RecvData`, and the two receives of a block, `RecvAck \/ RecvAckRepeat`,
written `RecvAck` below), weakly fair on both links' `Age`, and assumes per link
that a link sent on without bound delivers without bound:
`[]<> <<Flush>>_vars => []<> <<RecvData>>_vars`, and for the ack link
`[]<> <<SendAck>>_vars => []<> <<RecvAck /\ Head(r2dWire).pkt = Block>>_vars`, where
`Block` is the block the receiver holds at that moment. The ack-link form names the
current block on purpose: `[]<> <<RecvAck>>_vars` is unsound, because a wire that keeps
delivering stale duplicates of an old block while every current block is dropped
satisfies it, and TLC finds that lasso on the `MCSessionAcksLiveDup.cfg` instance (one
sequence, an arbitrary ack link with duplication). The corrected form is Link's
`WireFair` for the one deliverable copy (the head) and its `EventualDelivery` for the
packet that matters, rather than either stated for every packet value.

`Link!Fairness` itself is not used: it states strong fairness on `Deliver(p)` for every
`p` in `Packets`, and the ack link's `Packets` is `SUBSET Seqs`; at that size TLC reports
every temporal property "a tautology (its negation is unsatisfiable), satisfiability
problem has 0 branches" and checks nothing.

Two lassos fix the strength of the model's own fairness, both the kind README-Link.md
warns of:

1. *Weak fairness on the receives.* Two copies of a block sit at the ack link's capacity;
   `Age` makes the head due, `Delay` puts a tick of jitter back before anyone receives,
   and `SendAck` is disabled by the full wire so the per-link implication is vacuous. A
   head that is due infinitely often must be received infinitely often: `SF` on
   `RecvAck` and `RecvData`.
2. *Weak fairness on the sends.* Two copies of the first, empty block fill the ack link;
   every `RecvAck` frees a slot and `Duplicate` refills it with another copy of the empty
   block before `SendAck` runs, so the block naming the opened payload never reaches the
   wire. `Capacity` is a model-size knob and never a reason a sender is shut out, so a
   sender that finds room infinitely often sends infinitely often: `SF` on `Flush` and
   `SendAck`.

`Window = SentRing` in every cfg but the bitmap instance means a sequence the sender
still retains is one the receiver can still name (the code's `ROCKETNET_HISTORY_PACKETS`
sizes both).

**The lapping instance** (`MCSessionAcksLiveLap.cfg`). Every other liveness cfg has
`MaxSeq <= SentRing` and `MaxSeq <= Window`, so no record is lapped and the receiver's
floor stays at 1: the `~Retained(s)` disjunct of `OpenedSurfacesOrLaps` is never
exercised there. The lapping instance sends three sequences over a ring of two and a
window of two, on lossy links: sequence 3 overwrites the record of 1, and the floor
rises to 2. `OpenedSurfacesOrLaps` holds, and the reach witness
`MCSessionAcksReachLap.cfg` (the same constants under `SpecSafety`, with
`NoLapBeforeSurface` added last) shows a payload the receiver opened lapped before its
id surfaced, every other invariant holding on the way.

## The sent-ring bitmaps

Beside the sent ring the endpoint keeps two bitmaps, one bit per slot: held (the slot
holds a written record) and unacked (written and not yet acknowledged), and the highest
sequence whose slot has been reconciled (`sent_held`, `sent_unacked`,
`sent_bits_checked` in `struct rocketnet_ack_endpoint_t`).

**Writes and acks.** `StartPayload` sets the slot it writes in both bitmaps
(`rocketnet_ack_endpoint_record_sent_classified`); a burn writes no record and leaves the
bits. `ApplyOne` clears its slot from `unacked` wherever it sets `acked`
(`rocketnet_ack_apply_run`); a deferred record (batch full) keeps its bit.

**The reconcile and the walk** (`RecvAck`, the top of
`rocketnet_ack_endpoint_process_block`). For each sequence x from `bitsChecked + 1` to
`highestSent`, clamped below to `WindowLow`, a slot that does not hold x (a burned x, or
a record that fell below the window) loses both bits; then `bitsChecked = highestSent`.
The walk then visits a named sequence s in the window only when slot `s % SentRing` has
its unacked bit set and holds s.

**Abstracted.** The bits are sets of slot indexes, not words; the walk's word scan is a
set filter. The reconcile runs on every nonempty block received; an empty block
(largest 0) changes nothing in `RecvAck`, because `process_block` and
`process_block_full` return before `rn_ack_reconcile` when `largest == 0`. Before
2026-11-13 the model reconciled on the empty block too, so after a burn lapped a held
slot it could clear bits the code leaves until the next nonempty block.

**The instance.** `MCSessionAcksBits.cfg` has `SentRing = 2` and `MaxSeq = 4` with burns
on, so a burn laps a held slot: with sequence 1 in slot 1, burning 2 and 3 and sending 4
leaves slot 1 holding a sequence below the window. At `MaxSeq = 3` no burn can lap a held
slot (the lapping sequence must be followed by a higher send), so 4 is the smallest
instance that exercises the clear. The two bitmap witnesses use the same instance.

## The runs

| Cfg | Spec | Instance | Expected |
|---|---|---|---|
| `MCSessionAcks.cfg` | `SpecSafety` | four sequences, `SentRing` 4, burns on, both links `"arbitrary"`, `Capacity` 2, reordering on, duplication off | pass |
| `MCSessionAcksLive.cfg` | `Spec` | the gate instance, with `OpenedSurfacesOrLaps` | pass; over the 110 s budget, `bench` debt |
| `MCSessionAcksLiveSmall.cfg` | `Spec` | two sequences, `Capacity` 1, duplication and reordering off | pass |
| `MCSessionAcksLiveDup.cfg` | `Spec` | one sequence, burns off, data link `"none"`, ack link `"arbitrary"` with duplication, reordering off; `OpenedSurfacesOrLaps` only | pass |
| `MCSessionAcksLiveLap.cfg` | `Spec` | three sequences, `SentRing` 2, `Window` 2, `MaxRanges` 1, burns off, both links `"arbitrary"`, `Capacity` 1, duplication and reordering off | pass |
| `MCSessionAcksReachLap.cfg` | `SpecSafety` | the lapping instance, with `NoLapBeforeSurface` last | `NoLapBeforeSurface` (a reach witness) |
| `MCSessionAcksBrokenLiveness.cfg` | `Spec` | two sequences, burns off, both links `"none"`, `"dropfull"`; `OpenedSurfacesOrLaps` only | temporal: `OpenedSurfacesOrLaps` |
| `MCSessionAcksPatNone.cfg`, `PatFirst`, `PatAlternate`, `PatBurst` | `SpecSafety` | the gate instance with both links on that Link pattern | pass |
| `MCSessionAcksBits.cfg` | `SpecSafety` | `SentRing` 2, data link `"arbitrary"`, ack link `"none"`, `Capacity` 1, duplication and reordering off | pass |
| `MCSessionAcksWitDropfull.cfg` | `SpecSafety` | the gate instance, `"dropfull"` | `AckedMeansSurfaced` |
| `MCSessionAcksWitOverclaim.cfg` | `SpecSafety` | the gate instance, `"overclaim"` | `BurnedNeverNamed` |
| `MCSessionAcksWitOverclaimSurfaced.cfg` | `SpecSafety` | the gate instance, `"overclaim"`, burns off | `SurfacedOnlyIfOpened` |
| `MCSessionAcksWitWalkskip.cfg` | `SpecSafety` | the bits instance, `"walkskip"` | `WalkVisitsUnacked` |
| `MCSessionAcksWitStaleheld.cfg` | `SpecSafety` | the bits instance, `"staleheld"` | `BitsMatchRing` |
| `MCSessionAcksWitSkipdeferred.cfg` | `SpecSafety` | the bits instance, `"skipdeferred"`, without `WalkVisitsUnacked` | `RepeatExact` |

Duplication is on only in `MCSessionAcksLiveDup.cfg`: with it on, the gate instance and
the "none" and "burst" patterns pass more than 3.6 million distinct states without
finishing inside 110 s, and with it off the gate instance finishes at about 1.8 million.
The measured records are `proofs/tla/RUNS.tsv`.

## The witnesses, checked against the code by hand

**wit-overclaim-surfaced.** Payloads 1 and 2 are sent; the data link drops 1; 2 opens;
the block names `{1, 2}` although only 2 opened. The first ack surfaces 2 and defers 1
(`BatchCap` 1), the next ack after a `Tick` surfaces 1, which never opened.
`rocketnet_ack_block_build` builds runs only from the bits set in `accepted_session`,
so the honest builder never names 1 here.

**wit-overclaim.** A burned sequence below an opened one is named by the overclaiming
block. The honest builder never names a sequence with no bit set, and a burned sequence
is never received.

**wit-dropfull.** Two payloads open and the block names both; with `BatchCap` 1 the walk
surfaces 2 and marks 1 acknowledged without surfacing it. `rocketnet_ack_apply_run`
leaves a record whose packet id does not fit the batch unacknowledged, with its unacked
bit set, so a later block surfaces it.

**wit-walkskip.** `StartPayload` records 1 in slot 1, it opens and the block `{1}`
arrives; the walk skips 1, so `lastVisited = {}` while `lastExpect = {1}`. The code's
walk visits every named sequence in the window whose unacked bit is set and whose slot
holds it.

**wit-staleheld.** `StartPayload` records 1 in slot 1, two burns spend 2 and 3,
`StartPayload` records 4 in slot 0 (`WindowLow = 3`), and a block arrives: the reconcile
clears slot 1 from `unacked` but keeps it in `held`, so `held = {0, 1}` while only slot 0
holds a sequence in the window. The code's reconcile clears both bits of a slot that does
not hold its sequence.

## The block memo

process_block remembers the last block it applied in full when that apply
deferred nothing (`block_memo` in `rocketnet_ack_endpoint_t`); a recorded send clears it.
The same block again, at the same high-water mark, is `RecvAckRepeat`: the reconcile,
then no walk. `RepeatExact` holds that the repeat leaves the ring, the batch, the
surfaced counts and the bits that `RecvAck`'s full apply would, and it passes in every
safety cfg above. In this model every block names only sequences already sent, so the
send clear never changes what a repeat would ack: it keeps the code's unknown-sequence
count exact and covers a block naming an unsent sequence, which the unit test
`test_ack_block_memo_matches_the_full_apply` drives against
`rocketnet_ack_endpoint_process_block_full`.

**wit-skipdeferred.** Payloads 1 and 2 are recorded (slots 1 and 0) and both open; the
block `{1, 2}` arrives: the walk surfaces 2 and defers 1 (`BatchCap` 1), and the
witness keeps the memo. A `Tick` drains the batch and the same block arrives again:
the repeat skips it, while the full apply would surface 1, so `RepeatExact` fails.
`WalkVisitsUnacked` is left out of this cfg because it fires first, on a repeat before
the `Tick` (the state is still equal there; in the code the skipped walk would miss
one `batch_full` count). The code keeps the memo only when `batch_full_counter` did
not move during the apply (`rocketnet_ack_endpoint_process_block`).
