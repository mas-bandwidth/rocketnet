# Link: the shared lossy UDP path

`proofs/tla/Link.tla` is one direction of a UDP path between client and server, carrying typed packets.
It is the module every other rocketnet model drives its state transitions through, so
that the connection, security and ack machines are checked under packet loss,
latency, jitter, duplication and reordering rather than over a perfect wire. The
transport itself never resends (USAGE.md, "The transport never resends"); the link models the path, not the transport's answer
to it.

`proofs/tla/MCLink.tla` and `proofs/tla/MCLink.cfg` are the small instance that proves the module
itself. Current measured cases are in `proofs/tla/RUNS.tsv`; earlier records are
preserved in `proofs/tla/RUNS-HISTORY.tsv`. See [the record guide](README.md).

## What the module covers

State the link owns:

- `wire`: a FIFO sequence of copies, each `[pkt |-> p, due |-> d]`. `pkt` is a value
  from `Packets`; the link never looks inside it. `due` is how many ticks until the
  copy may be delivered: latency plus jitter.
- `sent`: how many sends the link has seen, used by the loss patterns. It saturates
  (see "The send counter" in the module) so the state space stays finite.
- `pulse`: flips on every `Deliver`, so a delivery step is never confused with a drop
  that happened to leave the wire in the same shape. TLC found that confusion in the
  first draft (see "What TLC found").

Actions, one per operation and per outside event:

| Action | Who | What |
|---|---|---|
| `Send(p)` | the sender | counts the send; appends a copy with any `due` in `0..MaxDelay` unless the pattern loses this send; disabled at `Capacity` |
| `Deliver(p)` | the receiver | takes the head of the wire when it carries `p` and is due |
| `Drop(i)` | the path | removes copy `i`; enabled only under `Pattern = "arbitrary"` |
| `Duplicate(i)` | the path | appends a second copy of copy `i` with its own `due`; disabled at `Capacity` |
| `Reorder(i)` | the path | swaps copies `i` and `i+1` |
| `ReverseWindow(w)` | the path | reverses the trailing `w` copies on the wire in place |
| `Delay(i)` | the path | adds one tick of jitter to copy `i`, up to `MaxDelay` |
| `Age` | time | every `due` counts down one tick; conjoin into the model's `Tick` |

`Environment` is the disjunction of the path's own actions, for a model that wants
them as one disjunct in its `Next`.

What the path does matches what `sdk/rocketnet_network_simulator.cpp` injects: loss is
decided when the packet is sent (`rocketnet_network_simulator_send`, the
`packet_loss_percent` roll), a duplicate gets its own latency and jitter (the
`duplicate_percent` branch), and delivery order is not send order (the file's header:
"Slot-index order is not delivery-time order and it is not FIFO"). The simulator's
"queue full, packet dropped" is a harness artefact at 4096 slots and is not modelled:
`Capacity` disables `Send` and `Duplicate` rather than dropping, so it is a model-size
knob and never a hidden loss source.

### Loss patterns

`Pattern` selects which sends the link loses, so TLC can explore one deterministic
terrain at a time as well as the arbitrary one:

| `Pattern` | Loses |
|---|---|
| `"none"` | nothing |
| `"first"` | the first send on this link |
| `"alternate"` | the 1st, 3rd, 5th ... sends; even ones arrive ("first packet dropped, second packet comes through") |
| `"burst"` | sends `BurstStart .. BurstStart + BurstLen - 1` |
| `"everynth"` | every `BurstLen`-th send (`BurstLen`, `2 * BurstLen`, ...) |
| `"arbitrary"` | nothing at send; `Drop(i)` may remove any copy at any time |

A pattern counts sends on this link instance, whatever the packet. A model with two
directions instantiates the module twice and may give each direction its own pattern.

### Fairness, stated once

Models rely on `Link!Fairness` for liveness and for nothing else. No safety property
may depend on it.

- `WireFair`: a copy that is at the head and due infinitely often is delivered
  infinitely often (`SF` on `Deliver(p)` for every `p`), and time passes
  (`WF` on `Age`). This is the wire being a wire; it never brings a dropped copy back.
- `EventualDelivery`: a packet sent infinitely often is delivered infinitely often.
  This is the assumption "a packet re-sent without bound is eventually delivered". It
  is what rules out the adversary who drops every copy forever under `"arbitrary"`,
  and (TLC showed) it is also needed under the deterministic patterns when
  `Reordering` is on: see "What TLC found".
- `Fairness == WireFair /\ EventualDelivery`.

`EventualDelivery` is a general temporal formula, not a `WF`/`SF` condition. TLC
accepts it as a conjunct of the specification (the eleven runs below all did). Two
cautions:

1. It is vacuous for a model whose receiver has no enabled receive step for some
   deliverable packet, because then no behaviour satisfies the assumption and every
   liveness property passes. Every model gives every packet type a receive action;
   receive-and-discard counts.
2. It excludes behaviours where a sender's retries land exactly on the pattern's
   losses (under `"alternate"`, a sender that alternates two packet types forever puts
   one of them on every lost send). A model whose liveness depends on a single retried
   packet type should also be checked under `SpecWireFair`-style fairness
   (`Link!WireFair` alone) with `Reordering = FALSE`, where that correlation is the
   only thing left to find.

### The config patterns: drop every Nth, reverse each window

`simulated_drop_every_nth` (N >= 2) and `simulated_reverse_window` (W >= 2) in the client
and server transport configs are these two model features and nothing else; zero is off.

- `Pattern = "everynth"` is `Send` losing the N-th, 2N-th, ... send (`N = BurstLen`; the
  counter wraps at N). The code counts from the endpoint's first send in
  `rocketnet_network_simulator_send` before any random draw, so a lost send takes no
  draw.
- `ReverseWindow(w)` reverses the trailing `w` copies on the wire in place. The code
  buffers each window of W sends and emits it reversed, whole, in
  `rocketnet_network_simulator_flush_window`; the transports call it when the window
  fills and at the end of every `send_packets` (and of the update that sends), so a
  short window is reversed too and nothing is held across updates. A short window is
  `ReverseWindow(w)` with `w < W`.

Cases of `MCLinkPatterns` (`SpecEveryNth`, `SpecReverseWindow`), each invariant with a
reversed witness that sets `Defect` to break it:

| Invariant | Pass case | Reversed witness (`Defect`) |
|---|---|---|
| `EveryNthDropped` | `MCLinkEveryNth.cfg` | `MCLinkEveryNthBrokenPeriod.cfg` (`wrong_period`: drops the (N-1)-th) |
| `WindowOrderReversed` | `MCLinkReverseWindow.cfg` | `MCLinkReverseWindowBrokenOrder.cfg` (`unreversed`: the window is delivered in send order) |
| `SurvivorsInOrder` | `MCLinkEveryNth.cfg` (loss alone keeps order) | `MCLinkReverseWindowBreaksInOrder.cfg`: a model that relies on in-order delivery breaks under the reversal |
| `MCPatternsTypeOK` | every case | `MCLinkEveryNthBrokenCounter.cfg` (`overrun`: the send counter jumps past `MaxSeq`) |

`MCLinkSpecEveryNth.cfg` and `MCLinkWireFairEveryNth.cfg` run `MCLink` under `"everynth"`
with `Spec` and `SpecWireFair`: `ResentIsDelivered` and `LinkDelivers` hold.

## What the module abstracts away

- Packet contents, sizes, addresses, sealing. `Packets` is whatever the model says.
- The transport's answer to loss: acks, the replay window, the "never resends"
  rule. Those are the models above this one.
- Real time. `due` and `Age` are ticks; the model owns the clock.
- An attacker. Replay of a captured packet is `Duplicate` at a later time. Forging is a
  model's own action because it is about what the attacker can construct, not about
  the wire; the security modules add `Inject` beside their instance of this link.
- Loss correlated with content or with the other direction. Each direction's pattern
  is independent.

## How a model instantiates it

```tla
EXTENDS Naturals, Sequences
CONSTANTS ClientPackets, ServerPackets, C2SPattern, S2CPattern
VARIABLES c2sWire, c2sSent, c2sPulse, s2cWire, s2cSent, s2cPulse, clock, ...

C2S == INSTANCE Link WITH wire <- c2sWire, sent <- c2sSent, pulse <- c2sPulse,
                          Packets <- ClientPackets, Capacity <- 2, MaxDelay <- 1,
                          Duplication <- TRUE, Reordering <- TRUE,
                          Pattern <- C2SPattern, BurstStart <- 1, BurstLen <- 2
S2C == INSTANCE Link WITH wire <- s2cWire, sent <- s2cSent, pulse <- s2cPulse,
                          Packets <- ServerPackets, ...

ClientSendsRequest == /\ cstate = "requesting"
                      /\ C2S!Send([type |-> "request", ...])
                      /\ UNCHANGED <<server vars, s2c vars>>

ServerReceivesRequest == \E p \in ClientPackets :
                            /\ p.type = "request"
                            /\ C2S!Deliver(p)
                            /\ (server handles p, or discards it)
                            /\ UNCHANGED <<client vars, s2c vars>>

Tick == /\ clock' = clock + 1 /\ C2S!Age /\ S2C!Age /\ (timeouts fire here)

Next == ClientSendsRequest \/ ServerReceivesRequest \/ ...
        \/ (C2S!Environment /\ UNCHANGED <<everything but c2s vars>>)
        \/ (S2C!Environment /\ UNCHANGED <<everything but s2c vars>>)
        \/ Tick

Spec == Init /\ [][Next]_vars /\ WF_vars(Tick) /\ C2S!Fairness /\ S2C!Fairness
```

Conjoin `Send` into the action that sends, `Deliver` into the action that receives, and
`Age` into `Tick`. Give every deliverable packet a receive action (caution 1 above).
Check safety under every pattern; it needs no fairness. Check liveness under
`Fairness`. Start with `"first"` and `"alternate"`: those are the terrains the request
named.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19 or later), from
`proofs/tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -config MCLink.cfg MCLink
```

`MCLink.cfg` is the committed run: `Pattern = "arbitrary"` under `Spec`
(`Link!Fairness`), checking `MCTypeOK`, `InFlightBounded`, `DeliveredWereSent`,
`WireCarriesOnlySent`, `FirstSendLost` and the liveness property `ResentIsDelivered`.
The other ten runs in `RUNS.tsv` are the same file with up to three lines changed:

| Run | `SPECIFICATION` | `Pattern` | `PROPERTIES` | Expected |
|---|---|---|---|---|
| committed | `Spec` | `"arbitrary"` | `ResentIsDelivered` | passes |
| spec-none / first / alternate / burst | `Spec` | that pattern | `ResentIsDelivered` | passes |
| wf-none / first / alternate / burst | `SpecWireFair` | that pattern | `LinkDelivers` | passes |
| wf-arbitrary-witness | `SpecWireFair` | `"arbitrary"` | `LinkDelivers` | **fails**: the reversed witness |
| wf-alternate-resent | `SpecWireFair` | `"alternate"` | `ResentIsDelivered` | **fails**: see below |

For example:

```sh
sed -e 's/^SPECIFICATION .*/SPECIFICATION SpecWireFair/' \
    -e 's/Pattern     = ".*"/Pattern     = "arbitrary"/' \
    -e 's/^    ResentIsDelivered/    LinkDelivers/' MCLink.cfg > /tmp/witness.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -config /tmp/witness.cfg MCLink
```

The small instance is `Packets = {a, b}`, `Capacity = 3`, `MaxDelay = 1`,
`Duplication = TRUE`, `Reordering = TRUE`, `BurstStart = 1`, `BurstLen = 2`. Every run
finishes in a few seconds with about 1,300 to 2,200 distinct states.

## What TLC found

Two counterexamples were printed and replayed by hand through the module's actions.
Neither is a defect in rocketnet code; this module models the path, not the code.

1. **A drop read as a delivery (model error, fixed).** The first draft had no `pulse`.
   TLC rejected `ResentIsDelivered` under `Spec` with a lasso that only sent `b` and
   dropped it, which the `EventualDelivery` assumption should have excluded. Replayed:
   with three identical copies `b` on the wire, `Drop(i)` and `Deliver(b)` both turn
   `<<b, b, b>>` into `<<b, b>>` with `sent` unchanged, so the action formula
   `<<Deliver(b)>>_vars` was true of the drop step and the assumption was satisfied by
   drops. The fix is `pulse`, which only `Deliver` changes. With it the same run passes.

2. **Reorder starvation under `WireFair` alone (expected; why `EventualDelivery` is in
   `Fairness`).** `wf-alternate-resent`: `Pattern = "alternate"`, `SpecWireFair`,
   `ResentIsDelivered`. TLC's lasso (19 states, back to state 7): two copies of `a`
   enter the wire; from then on every send of `a` lands on an odd count and is lost,
   every send of `b` lands on an even count and enters, `Reorder` carries each `b` past
   the two `a` copies, `Age` fires only while `b` is at the head, and `Delay` keeps the
   head `a` at `due = 1` whenever it is at the head. `Deliver(a)` is therefore never
   enabled, so `SF(Deliver(a))` has nothing to force, `WF(Age)` is satisfied because
   `Age` does fire, and `a` is sent without bound and never delivered. Replayed by hand:
   every step is one of `Send`, `Duplicate`, `Reorder`, `Delay`, `Age`, `Deliver` as
   written. This is the path indefinitely postponing a copy behind later traffic, which
   a real path does not do, and it is what `EventualDelivery` excludes. Under `Spec`
   the same pattern passes (`spec-alternate`).

The reversed witness, `wf-arbitrary-witness`, fails `LinkDelivers` with a 44-state
lasso in which every copy is dropped before it is delivered (no `MCDeliver` step in the
trace). That is the behaviour `EventualDelivery` exists to rule out, and the committed
run passing while the witness fails shows the assumption is doing the work.

## Reproducible case files

The original seven-column records are preserved unchanged in
[`RUNS-HISTORY.tsv`](RUNS-HISTORY.tsv). Current measurements use the runner
and refresh procedure in [`README.md`](README.md). Each historical row now
has its own committed configuration; no edit to a shared file is needed.
The table follows the original ledger order. A declared debt retains its
original parameters and is not a completed proof.

| Historical row | Committed configuration | Declared outcome |
|---|---|---|
| 1 | `MCLink.cfg` | pass |
| 2 | `MCLinkSpecNone.cfg` | pass |
| 3 | `MCLinkSpecFirst.cfg` | pass |
| 4 | `MCLinkSpecAlternate.cfg` | pass |
| 5 | `MCLinkSpecBurst.cfg` | pass |
| 6 | `MCLinkWireFairNone.cfg` | pass |
| 7 | `MCLinkWireFairFirst.cfg` | pass |
| 8 | `MCLinkWireFairAlternate.cfg` | pass |
| 9 | `MCLinkWireFairBurst.cfg` | pass |
| 10 | `MCLinkWireFairArbitraryWitness.cfg` | temporal / LinkDelivers |
| 11 | `MCLinkWireFairAlternateResent.cfg` | temporal / ResentIsDelivered |
| 12 | `MCLinkSpecEveryNth.cfg` | pass |
| 13 | `MCLinkWireFairEveryNth.cfg` | pass |
| 14 | `MCLinkEveryNth.cfg` | pass |
| 15 | `MCLinkEveryNthBrokenPeriod.cfg` | invariant / EveryNthDropped |
| 16 | `MCLinkReverseWindow.cfg` | pass |
| 17 | `MCLinkReverseWindowBrokenOrder.cfg` | invariant / WindowOrderReversed |
| 18 | `MCLinkEveryNthBrokenCounter.cfg` | invariant / MCPatternsTypeOK |
| 19 | `MCLinkReverseWindowBreaksInOrder.cfg` | invariant / SurvivorsInOrder |
