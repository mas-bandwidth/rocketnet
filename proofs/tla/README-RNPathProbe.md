# RNPathProbe: the path packet size probe, v2 (connect small, probe upward)

`RNPathProbe.tla` models the path packet size probe of the MTU fallback
design, v2. The handshake carries no probe: the client's connection response
carries P0, the smallest candidate its list gives the server's family (t12
rule 16), and both ends start the connection at P0, so every list joins as fast
as the default. After CONNECTED, a client whose list gives the family more than
one candidate walks the candidates above P, largest first, with pairs of probes
(packet type 9), raises P on an authenticated large reply and confirms the raise
to the server; the server answers probes from a connected mapping and raises its
P only on a confirm. The default list `{ 1500 }` has one candidate: no probe,
nothing new on the wire.

It is written ahead of the v2 code. The coverage rows `rn-path-probe-client` and
`rn-path-probe-server` are `owed` until sdk/rocketnet_client_protocol.cpp and
sdk/rocketnet_server_protocol.cpp cite its actions, and `rn-path-probe-trace` is
owed a rebind of the trace check (tests/tla_trace_path_probe.cpp,
TraceRNPathProbe.tla, which still bind the v1 handshake probe).

## Abstraction

- **Sizes.** Abstract wire sizes. The small probe, the confirm and its echo are
  34 bytes in the code, size 0 here, and fit every path. `ClientSet` is the
  client's candidate set for the family (P of each `link_mtus` entry, equal
  sizes collapsed), `ServerList` the server's own, which may differ; P0 =
  min(`ClientSet`) is on both (an ASSUME, as t12 rule 16 makes the server refuse
  any other). The walk's candidate `Cur` is the largest candidate above P not
  ruled out, or P itself (P0) when none is left.
- **The path.** A capacity each way, chosen at `Init` from `CapsUp` and
  `CapsDown` and fixed for the behaviour: a packet larger than its direction's
  capacity is dropped when sent, always. `Carried(S)` is S on the server's list
  and within both capacities (a size off the server's list is never answered, so
  to the client it is a size the path does not carry). Packets in flight are a
  set, delivered in any order.
- **Loss.** Loss is bursts (`LossBurst(k)`): a burst of kind `"all"` drops every
  packet, of kind `"large"` every packet above the control size (the large probe
  and its reply) while the small partners cross; each drops what is in flight
  when it starts and what is sent while it lasts, both ways. At most `MaxBursts`
  bursts, every burst ends (fairness), and all of them lie inside one loss window
  of at most `BurstSpacings` strike spacings from the first burst's start
  (`span`).
- **Pairs and strikes.** The first pair goes at connect; then one per pair
  interval (`Tick`; FIRST, 0.25 s or twice the handshake round trip, doubling
  after every pair that resolves without success up to MAX, 1 s, back to FIRST at
  a rule-out). The interval's length is abstracted: it is always shorter than the
  strike spacing (5 s). A pair is the small probe and the large at `Cur`, one new
  pair number (`pairNo` is its parity). When the next pair is due, the
  outstanding pair resolves (`ClientResolvePair`): small seen and large not is a
  strike, counted only when the pair was sent a spacing after the pair of the
  last counted strike (`pairSpaced`, fixed at send from `spaced`; `Spaced` marks
  a spacing passing); small not seen is no evidence. K counted strikes rule a
  candidate above P out (`ClientRuleOut`; the next starts fresh), and at P0 itself
  end the connection -14 (`ClientFail`). So K counted strikes span at least
  K - 1 spacings, and probing has no time limit while strikes go uncounted.
- **Large replies.** A large reply that opens is taken only while probing and
  only at the walk's current candidate, from any pair so far
  (`ClientRecvLarge`): above P it raises P at once and sets the confirm; at P
  itself (P0, every candidate above ruled out) it raises nothing. Either way the
  walk ends and no pair is left outstanding. Any other large reply is consumed and
  ignored; a forged one fails the open (`ClientRecvForged`).
- **Confirms.** The confirm goes at the next probe update after the raise and is
  resent until an echo carrying its size or larger arrives
  (`ClientSendConfirm`, `ClientRecvConfirmEcho`); the server's
  `ServerRecvConfirm` spends the small allowance, and for a size on its list
  raises the slot's P when it is above it and echoes; a confirm off its list is
  ignored.
- **The server's order.** `ServerRecvProbe` (a probe from `C`, a replayed copy, or
  from `X`, an address whose mapping is not connected) takes the code's checks
  in its order: small or a size on the server's list, a connected mapping, the
  update's allowance for its kind (one small or confirm and one large per
  mapping per update); then the open, the allowance spent, and the echo of the
  same kind and size. `ServerRecvForged` takes any packet from `F`, a forger at
  the client's address: it reaches the open and fails, and spends nothing.
- **The attacker.** An on-path attacker may replay a client packet in flight
  (`Replay`, a copy with `n = 1`), send packets from `X` or `F`
  (`AttackProbe`) or a forged large reply (`AttackReply`), `AttackBudget` sends
  in all. With `Forger` = TRUE, `F` sends without limit, one packet in flight at
  a time.
- **Timed.** TRUE: the pair interval covers a round trip, so a `Tick` waits
  until nothing of the pair is in flight (the liveness and loss-only cases).
  FALSE is the code's untimed view: a pair's packets may arrive after it
  resolved, until the pair after next reuses the pair number's parity, when what
  is left of it is dropped (the code's u32 pair number keeps them distinct; a
  packet two intervals late is taken as lost).
- **Payload.** `ClientSendData` and `ServerSendData` send at the end's P and
  leave no state: no property waits on payload, and `RaisedWithinPath` says what
  a payload needs.

Not modelled: the handshake (it carries no probe), the IPv4/IPv6 split, the
packet layout and padding checks (a packet that fails to open stands for every
refusal after the allowance), the message layer (the fragment stride stays
F(P0); a raise only grows the payload capacity), and the confirm's 1 s clock (a
confirm may be resent whenever none is in flight).

## Rules

| Rule | Definition | Reversed by |
| --- | --- | --- |
| P never decreases, on either end | `PNeverDecreases` (action) | `MCRNPathProbeBrokenLower.cfg` |
| every P above P0 is a size the path carries both ways | `RaisedWithinPath` | (the raise witnesses below reach it too) |
| the server is never ahead of the client | `ServerNeverAhead` | |
| loss alone never rules out a size the path carries (Timed, `BurstSpacings <= K - 2`) | `NoWrongRuleOut` | `MCRNPathProbeBrokenUnspacedStrike.cfg` |
| a strike counts only for a pair whose small reply came back | `StrikeNeedsSmall` (action) | `MCRNPathProbeBrokenStrikeOnLoss.cfg` |
| a strike counts only on a spaced pair | `StrikeNeedsSpacing` (action) | (`MCRNPathProbeBrokenUnspacedStrike.cfg`) |
| the server raises only on a confirm carrying the size | `ServerRaisesOnConfirm` (action) | `MCRNPathProbeBrokenRaiseOnProbe.cfg` |
| the client raises only on an authenticated large reply of the size | `ClientRaisesOnAuthLarge` (action) | `MCRNPathProbeBrokenForgedRaise.cfg` |
| -14 only when P0 itself collected K strikes | `FailOnlyAtP0` | |
| P is never a candidate ruled out | `NeverAtRuledOut` | |
| no pair once P is the largest candidate or the walk has ended | `NoProbeAfterDone` (action) | `MCRNPathProbeBrokenProbeAfterMax.cfg` |
| the server answers only from a connected mapping | `ReplyOnlyConnected` | `MCRNPathProbeBrokenUnproven.cfg` |
| no reply larger than the probe it answers | `ReplyNoLarger` | `MCRNPathProbeBrokenLarger.cfg` |
| a large reply only at a size on the server's list | `ReplyOwnList` | `MCRNPathProbeBrokenOffList.cfg` |
| at most one small (or confirm) and one large answered per mapping per update | `OneReplyPerUpdate` | `MCRNPathProbeBrokenNoAllowance.cfg` |
| the walk ends and both ends hold the same P | `Converges` | |
| both ends reach the largest candidate the path carries | `ReachesBest` | |
| a path that does not carry P0 ends -14 | `FailsWhenP0NotCarried` | `MCRNPathProbeBrokenSkipLast.cfg` |
| a forger at the client's address, without limit, cannot starve the walk | `ForgedNeverStarves` | `MCRNPathProbeBrokenForgedStarves.cfg` |

`NoLateRaise` is reachability, not a rule: `MCRNPathProbeReachLateReply.cfg`
must violate it, so the untimed path where a large reply of an earlier pair
raises P is explored.

## Instances

`MCRNPathProbe.tla` extends the module. Unless the case says otherwise: client
and server lists `{3, 2, 1}`, `K` = 2, one burst (`MaxBursts` = 1) in a window of
no spacing (`BurstSpacings` = 0), `AttackBudget` = 1, `Forger` = FALSE, `Timed` =
TRUE.

| Case | Notes |
| --- | --- |
| `MCRNPathProbe.cfg` | capacities `{0, 1, 3}` up and `{1, 3}` down, every invariant and property under `FairSpec`; reduced from `MCRNPathProbeFull.cfg` only for the 110 s case budget |
| `MCRNPathProbeFull.cfg` | the same at every capacity `0 .. 3` each way: nightly timeout debt (`NIGHTLY-DEBT.tsv`) |
| `MCRNPathProbeServerList.cfg` | server list `{3, 1}`: the client's middle size is never answered and is ruled out; the same checks at the same capacities |
| `MCRNPathProbeOneEntry.cfg` | client set `{2}`, server list `{2, 1}`: no probing |
| `MCRNPathProbeLate.cfg` | `Timed` = FALSE, capacities `{1, 3}`, no attacker, safety (`Spec`); reduced from `MCRNPathProbeLateFull.cfg` only for the 110 s case budget |
| `MCRNPathProbeLateFull.cfg` | the same with one attacker send: nightly timeout debt (`NIGHTLY-DEBT.tsv`) |
| `MCRNPathProbeSpacing.cfg` | client and server `{2, 1}`, `K` = 3, two bursts in a window of one spacing, capacities `{1, 2}`, no attacker: `NoWrongRuleOut` at a second spacing (safety) |
| `MCRNPathProbeForged.cfg` | `Forger` = TRUE and no other attacker, capacities `{1, 3}`: `ForgedNeverStarves` and `Converges` |
| `MCRNPathProbeReachLateReply.cfg` | `Timed` = FALSE, capacity 3 each way, `K` = 2, no loss: must violate `NoLateRaise` |
| each `MCRNPathProbeBroken*.cfg` | must violate the one property it names; its header says its instance |

## Counterexamples

Each read against the model by hand (2026-11-25, TLC 2.19 on studio.local);
against the code once the v2 code is in.

- **Lower** (`PNeverDecreases`, untimed, `K` = 1): the pair at 3 is answered but
  its large reply is late; the pair resolves on the small alone, a strike, and 3
  is ruled out; the pair at 2 is sent; the late reply of 3 is taken (the broken
  client takes any candidate's reply), P = 3; then the reply of 2 is taken, P =
  2.
- **StrikeOnLoss** (`StrikeNeedsSmall`): a burst of every packet; the pair sent
  inside it loses both probes, and the broken client counts a strike.
- **UnspacedStrike** (`NoWrongRuleOut`): a burst of large packets only, inside a
  window of no spacing, over two pairs at 2 on a path that carries 2: both
  small replies come back, both large are lost, the broken client counts both,
  and 2 is ruled out.
- **RaiseOnProbe** (`ServerRaisesOnConfirm`): the server answers the first large
  probe at 2 and raises its P to 2 with no confirm.
- **ForgedRaise** (`ClientRaisesOnAuthLarge`): a forged large reply at 2 on a path
  of capacity 1; the broken client raises to 2.
- **ProbeAfterMax** (`NoProbeAfterDone`): the first pair at 2 succeeds and P = 2,
  the largest candidate; the broken walk does not end and the next pair goes.
- **Unproven** (`ReplyOnlyConnected`): a probe from `X`, whose mapping is not
  connected, is answered.
- **Larger** (`ReplyNoLarger`): client `{2, 1}`, server `{3, 2, 1}`: the probe at 2
  is answered at 3.
- **OffList** (`ReplyOwnList`): client `{3, 2, 1}`, server `{2, 1}`: the probe at 3
  is answered.
- **NoAllowance** (`OneReplyPerUpdate`): the large probe and a replayed copy of it
  arrive in one update and both are answered.
- **SkipLast** (`FailsWhenP0NotCarried`): a path that carries nothing above the
  control size; 2 is ruled out and the broken walk ends without probing P0, so
  the client stays at a P0 the path drops.
- **ForgedStarves** (`ForgedNeverStarves`): in every update the forger's packet
  arrives first, fails the open and spends the allowance, so the client's probes
  are withheld and the walk never reaches 2.
- **ReachLateReply** (`NoLateRaise`): the pair at 3 resolves on its small reply
  with its large reply still in flight; the next pair at 3 is sent, and the late
  reply of the first is taken at the walk's candidate: P = 3.

## Records

The records in `RUNS.tsv` are measured on space with the pinned tlacheck
(`tlacheck run --group rnpathprobe`, which refuses to run on macOS), one case
per bounded run, two workers, 2026-11-25. `MCRNPathProbeFull.cfg` and
`MCRNPathProbeLateFull.cfg` are over the 110 s record cap, so their records
are the bounded FAIL/124 and their completed counts are in `NIGHTLY-DEBT.tsv`,
from one run each with no record cap (`--manual`, space, 2 workers).

| Case | Result | Generated | Distinct | Seconds |
| --- | --- | --- | --- | --- |
| `MCRNPathProbe.cfg` | pass | 744,712 | 119,777 | 38.5 |
| `MCRNPathProbeFull.cfg` | pass (nightly debt; bounded record FAIL/124) | 2,322,494 | 366,825 | 115.5 |
| `MCRNPathProbeServerList.cfg` | pass | 717,382 | 115,813 | 38.1 |
| `MCRNPathProbeOneEntry.cfg` | pass | 2,457 | 432 | 1.0 |
| `MCRNPathProbeLate.cfg` | pass | 2,605,454 | 335,422 | 6.8 |
| `MCRNPathProbeLateFull.cfg` | pass (nightly debt; bounded record FAIL/124) | 69,842,238 | 7,416,104 | 136.6 |
| `MCRNPathProbeSpacing.cfg` | pass | 84,044 | 16,325 | 1.5 |
| `MCRNPathProbeForged.cfg` | pass | 240,596 | 37,408 | 10.1 |
| `MCRNPathProbeReachLateReply.cfg` | `NoLateRaise` | 325 | 104 | 0.8 |
| `MCRNPathProbeBrokenLower.cfg` | `PNeverDecreases` | 8,289 | 1,919 | 0.9 |
| `MCRNPathProbeBrokenStrikeOnLoss.cfg` | `StrikeNeedsSmall` | 103 | 42 | 0.8 |
| `MCRNPathProbeBrokenUnspacedStrike.cfg` | `NoWrongRuleOut` | 1,671 | 341 | 0.8 |
| `MCRNPathProbeBrokenRaiseOnProbe.cfg` | `ServerRaisesOnConfirm` | 7 | 4 | 0.8 |
| `MCRNPathProbeBrokenForgedRaise.cfg` | `ClientRaisesOnAuthLarge` | 66 | 26 | 0.8 |
| `MCRNPathProbeBrokenProbeAfterMax.cfg` | `NoProbeAfterDone` | 192 | 56 | 0.8 |
| `MCRNPathProbeBrokenUnproven.cfg` | `ReplyOnlyConnected` | 37 | 28 | 0.7 |
| `MCRNPathProbeBrokenLarger.cfg` | `ReplyNoLarger` | 7 | 4 | 0.8 |
| `MCRNPathProbeBrokenOffList.cfg` | `ReplyOwnList` | 7 | 4 | 0.8 |
| `MCRNPathProbeBrokenNoAllowance.cfg` | `OneReplyPerUpdate` | 378 | 129 | 0.9 |
| `MCRNPathProbeBrokenSkipLast.cfg` | `FailsWhenP0NotCarried` | 16,497 | 3,615 | 1.8 |
| `MCRNPathProbeBrokenForgedStarves.cfg` | `ForgedNeverStarves` | 66,249 | 10,056 | 3.5 |


## Trace binding

Owed: the trace check of v1 (layer `path-probe`, tests/tla_trace_path_probe.cpp
and TraceRNPathProbe.tla) binds the handshake probe, which v2 removes. It is
rewritten against this model once the v2 code is in.
