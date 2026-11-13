# SessionAcks: the session ack window

`proofs/tla/SessionAcks.tla` is one direction of a rocketnet session: a sender A hands
payloads to a receiver B over the shared lossy link `proofs/tla/Link.tla` (instance `D2R`),
and B sends ack blocks back over a second instance (`R2D`). A is the session sequence
and one `rocketnet_ack_endpoint_t` (`sdk/rocketnet_ack.cpp`: `record_sent`,
`process_block`, `apply_run`) with the staged send of the client and server transports
(`sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`). B is the
replay window as a set (`sdk/rocketnet_replay.cpp`) and the block builder
(`rocketnet_ack_block_build`).

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
| `highestOpened`, `opened` | `highest_opened`, `window[]` | the replay window as the set of opened sequences in `(highestOpened - Window, highestOpened]` |
| `drained` | the receive drain | packets drained this update |
| `highestAccepted`, `accepted` | `accepted_session` | the accepted history the block is built from |
| `pending` | the receive batch | payloads opened this update and not yet accepted (`AckTiming`) |
| `debt` | `ack_debt` at the threshold | an acknowledgement is owed |
| `swept`, `lateDone`, `lateRefused`, `standalones` | the update order | this update has decided its acks; the send pass has run; it found no room; standalone acks sent to the peer this update (`AckTiming`) |

Ghosts: `burned` (sequences spent and never sent), `surfacedCount`, `openedEver`,
`namedEver`, `lastExpect`, `lastVisited`.

A payload's packet id is its sequence. Every packet the model sends is a payload, which
is ack-eligible once the message layer accepts it, not when it opens (standard/TRANSPORT.md
t15.3): the block is built from the accepted history (`accepted_session`,
`rocketnet_session_ack_block_build`), which a payload enters only through the accept
(`rocketnet_server_protocol_accept_payload`, `rocketnet_client_protocol_accept_payload`),
and the accept raises the ack debt.

**Actions**, one per operation and per outside event:

| Action | Who | Code |
|---|---|---|
| `StartPayload` | the application | `start_packet` + `finish_packet`: one sequence, one record with the packet id |
| `BurnOne` | a failed start or finish | a sequence spent and never sent (USAGE.md, "Not every NULL from start_packet spends one") |
| `Flush` | `send_packets` | the staged packet goes to the wire in fill order, `Link!Send` |
| `RecvAck` | a block arrives | `rocketnet_ack_endpoint_process_block` / `rocketnet_ack_apply_run`: the reconcile, the walk, the retained window, unknown sequences, the batch and its deferral |
| `SendAck` | the receiver | a session packet carrying the current block, `rocketnet_ack_block_build`: the first run, then `(gap, run)` pairs to the floor, at most `MaxRanges` |
| `RecvData` | a packet arrives | the reading order's replay check (`rocketnet_replay_already_received`, `rocketnet_replay_advance`) |
| `Accept` | the message layer | `accept_payload`: an opened payload enters the accepted history and raises the debt; without `AckTiming` it happens in `RecvData` |
| `RecvElicit` | a keep-alive arrives | the peer's other session packets raise the debt at receive (`rocketnet_session_elicits_ack`, `AckTiming`) |
| `Sweep` | the update ack decision | `rocketnet_server_protocol_write_ack_if_due` in the maintenance sweep, the not-due branch of `rocketnet_client_protocol_update`: a debt is paid by a standalone ack, or left to the send pass; a peer with a payload waiting for the accept is always left to it (the server's batched bitmap; the client with the message layer linked never pays here) (`AckTiming`) |
| `LatePass` | the start of `send_packets` | `rocketnet_server_transport_write_accepted_acks` with `rocketnet_server_protocol_write_ack_if_due`, `rocketnet_client_protocol_write_owed_ack`: every debt still owed is paid by one standalone ack in the same update, unless there is no room (`AckTiming`) |
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
| `OneStandalonePerUpdate` | at most one standalone ack to the peer in one update (TRANSPORT.md t15.4): the sweep leaves a peer with a payload waiting to the send pass |
| `SameUpdateAck` | an update that has decided its acks and run its send pass holds no debt unless the wire had no room: an accepted payload is acknowledged in the update that accepted it (TRANSPORT.md t15.4) |

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
| `lateorder` | no send pass: the order before W2-52, the debt an accept raises waits for the next update ack decision | `SameUpdateAck` |
| `sweepspends` | the update ack decision pays a keep-alive's debt although a payload from the peer waits for the accept, so the send pass sends a second standalone ack | `OneStandalonePerUpdate` |

`lateorder` keeps the sweep's skip of a peer with a payload waiting, which the code before W2-52 did not have; what it reproduces is that code's order, the accept after the ack decision with nothing after it to pay the debt. `sweepspends` is not earlier code: before W2-52 there was no send pass, so no update could send two standalone acks. It is the fix without the sweep's skip, the wrong fix the skip exists to prevent.

`SurfacedOnce` and `BitsCoverRing` have no reversed witness.

The two W2-52 witnesses, each counterexample read by hand against the code (records of 2026-11-13, pinned tlacheck):

- `lateorder`, 7 states: A stages and flushes a payload; B receives it; the update of B decides its acks (`Sweep`) while it owes none, so no standalone ack goes; the accept that follows raises the debt (`Accept`); under the flip `LatePass` sends nothing, and the update ends owing an ack it never sent. Before W2-52 the code had the same order: the message layer accepted the payload after the update had decided its acks, and nothing sent the ack until the next update decided again, 16.7 ms later at 60 Hz.
- `sweepspends`, 8 states: A stages and flushes a payload; B receives it, so it waits in B's batch for the accept (`RecvData`, pending {1}); a keep-alive from A raises B's debt (`RecvElicit`); under the flip the update ack decision (`Sweep`) pays that debt with one standalone ack although the payload is still pending; the accept raises the debt again (`Accept`) and the send pass (`LatePass`) sends a second standalone ack in the same update. In the code the sweep in `rocketnet_server_transport_update_finish_body` skips a client whose bit is set in this update's payload bitmap (`batched`), and the client protocol update leaves the owed ack to the send pass when payload acks are deferred, so the one standalone ack goes after the accept and carries both.

`MCSessionAcksBrokenLiveness.cfg` is `OpenedSurfacesOrLaps`'s reversed witness: the
`dropfull` defect retains an opened payload without surfacing it.

## What the model abstracts away

- **The replay window's representation.** `opened` is a set; the 64-word bitmap and its
  large-skip clear belong to the ReplayWindow module. Every block here is truthful by
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
`SendAck`, `RecvData`, `RecvAck`), weakly fair on both links' `Age`, and assumes per link
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
set filter. The reconcile runs on every block received, including an empty one.

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
| `MCSessionAcksBrokenLiveness.cfg` | `Spec` | two sequences, burns off, both links `"none"`, `"dropfull"`; `OpenedSurfacesOrLaps` only | temporal: `OpenedSurfacesOrLaps` |
| `MCSessionAcksPatNone.cfg`, `PatFirst`, `PatAlternate`, `PatBurst` | `SpecSafety` | the gate instance with both links on that Link pattern | pass |
| `MCSessionAcksBits.cfg` | `SpecSafety` | `SentRing` 2, data link `"arbitrary"`, ack link `"none"`, `Capacity` 1, duplication and reordering off | pass |
| `MCSessionAcksWitDropfull.cfg` | `SpecSafety` | the gate instance, `"dropfull"` | `AckedMeansSurfaced` |
| `MCSessionAcksWitOverclaim.cfg` | `SpecSafety` | the gate instance, `"overclaim"` | `BurnedNeverNamed` |
| `MCSessionAcksWitOverclaimSurfaced.cfg` | `SpecSafety` | the gate instance, `"overclaim"`, burns off | `SurfacedOnlyIfOpened` |
| `MCSessionAcksWitWalkskip.cfg` | `SpecSafety` | the bits instance, `"walkskip"` | `WalkVisitsUnacked` |
| `MCSessionAcksWitStaleheld.cfg` | `SpecSafety` | the bits instance, `"staleheld"` | `BitsMatchRing` |
| `MCSessionAcksWitSkipdeferred.cfg` | `SpecSafety` | the bits instance, `"skipdeferred"`, without `WalkVisitsUnacked` | `RepeatExact` |
| `MCSessionAcksTiming.cfg` | `SpecSafety` | `AckTiming`, three sequences; every invariant, with `OneStandalonePerUpdate` and `SameUpdateAck` | pass |
| `MCSessionAcksTimingLive.cfg` | `Spec` | `AckTiming` on the live-small instance; `OpenedSurfacesOrLaps` | pass |
| `MCSessionAcksWitLateorder.cfg` | `SpecSafety` | `AckTiming`, `"lateorder"` | `SameUpdateAck` |
| `MCSessionAcksWitSweepspends.cfg` | `SpecSafety` | `AckTiming`, `"sweepspends"` | `OneStandalonePerUpdate` |

`AckTiming` is a definition, FALSE by default, replaced with `<-` in the four timing
cfgs: with it off a payload is accepted in the step that opens it, so the other runs
keep the state space they had while the block is still built from the accepted history.

Duplication is on only in `MCSessionAcksLiveDup.cfg`: with it on, the gate instance and
the "none" and "burst" patterns pass more than 3.6 million distinct states without
finishing inside 110 s, and with it off the gate instance finishes at about 1.8 million.
The measured records are `proofs/tla/RUNS.tsv`.

## The witnesses, checked against the code by hand

**wit-overclaim-surfaced.** Payloads 1 and 2 are sent; the data link drops 1; 2 opens;
the block names `{1, 2}` although only 2 opened. The first ack surfaces 2 and defers 1
(`BatchCap` 1), the next ack after a `Tick` surfaces 1, which never opened.
`rocketnet_ack_block_build` builds runs only from the bits set in the ack-eligible window,
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
