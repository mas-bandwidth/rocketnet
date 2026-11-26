# RNPathProbe: the path packet size probe, v2 (connect small, probe upward)

`RNPathProbe.tla` models the path packet size probe of the MTU fallback
design, v2. The handshake carries no probe: the client's connection response
carries P0, the smallest candidate its list gives the server's family (t12
rule 16), and both ends start the connection at P0, so every list joins as fast
as the default. After CONNECTED, a client whose list gives the family more than
one candidate walks up a ladder with pairs of probes (packet type 9): the next
candidate above P, smallest first. An authenticated large reply raises P,
confirms the raise to the server and goes on to the next size up; K strikes
rule the size out and the walk waits, then re-tries that same size on its
schedule (10, 20, 30 s, then gaps doubling to 300 s, every 300 s after), so
sizes above a ruled-out size are never tried before P reaches it, and a size
heavy loss ruled out is taken once the loss ends; when the first step is
ruled out for the first time, P0 itself is checked, and K strikes there end
the connection -14. The server answers probes from a
connected mapping and raises its P only on a confirm. The default list `{ 1500 }` has one candidate: no probe,
nothing new on the wire.

The coverage rows `rn-path-probe-client` and `rn-path-probe-server` cite the
code's actions in sdk/rocketnet_client_protocol.cpp and
sdk/rocketnet_server_protocol.cpp, and the trace check binds them (Trace
binding, below).

## Abstraction

- **Sizes.** Abstract wire sizes. The small probe, the confirm and its echo are
  34 bytes in the code, size 0 here, and fit every path. `ClientSet` is the
  client's candidate set for the family (P of each `link_mtus` entry, equal
  sizes collapsed), `ServerList` the server's own, which may differ; P0 =
  min(`ClientSet`) is on both (an ASSUME, as t12 rule 16 makes the server refuse
  any other). The walk's candidate `Cur` is the smallest candidate above P
  while nothing is ruled out, P itself (P0) in P0's check, and otherwise the
  smallest candidate above P, which is the size ruled out: `ruled` holds the
  one size the walk waits to re-try, and a raise clears it.
- **The path.** A capacity each way, chosen at `Init` from `CapsUp` and
  `CapsDown` and fixed for the connection: a packet larger than its direction's
  capacity is dropped when sent, always. `Carried(S)` is S on the server's list
  and within both capacities (a size off the server's list is never answered, so
  to the client it is a size the path does not carry). Packets in flight are a
  set, delivered in any order.
- **Reuse.** `Reuse(cu, cd)`, at most `MaxReuses` times (outside a burst): the
  connection ends and its server slot is given to a new connection, which is
  either a new client or the same client object connecting again, on a path
  of capacities `cu` from `ReuseCapsUp` and `cd` from `ReuseCapsDown`. Both ends
  start as at `Init`: P0, nothing ruled out, no strikes, no confirm, no wait,
  the first pair due, both allowances clear. The packets in flight were sealed
  under the old connection's keys and are dropped. The reuse sets are kept
  apart from `CapsUp` and `CapsDown` so that a trace's `Init` has the one path
  its first connection had. `MaxReuses` is 0, and the two sets are empty, in
  every case but the two reuse cases.
- **Loss.** Loss is bursts (`LossBurst(k)`): a burst of kind `"all"` drops every
  packet, of kind `"large"` every packet above the control size (the large probe
  and its reply) while the small partners cross; each drops what is in flight
  when it starts and what is sent while it lasts, both ways. At most `MaxBursts`
  bursts, and every burst ends (fairness). The loss assumption: loss of large
  packets alone never covers K consecutive pairs whose small probes came back.
  `pairInBurst` marks that a burst met the outstanding pair (it was on when the
  pair was sent, or started while the pair was out); `burstStrikes` counts the
  strikes in the current row taken on such pairs, and goes to zero with the
  strikes (a small-lost pair, a rule-out). A burst may meet a pair that can
  still strike (a large burst, or one of every packet once the small reply is
  in) only while `burstStrikes < K - 1` (`BurstMayMeet`): a pair is not sent
  into a large burst past that, and such a burst does not start over an
  outstanding pair. A pair sent into a burst of every packet loses its small
  probe and cannot strike, and needs no bound.
- **Pairs and strikes.** The first pair goes at connect; then one per pair
  interval, and at once after a raise (`Tick`; FIRST, 0.25 s or twice the handshake round trip, doubling
  after a pair whose small probe was lost, up to MAX, 1 s, back to FIRST at a
  rule-out; the interval is not tracked). A pair is the small probe and the large
  at `Cur`, one new pair number (`pairNo` is its parity). When the next pair is
  due, the outstanding pair resolves (`ClientResolvePair`): small seen and large
  not is a strike, and every strike counts; small not seen is evidence of loss,
  not of size, and sets the candidate's strikes to zero (a large reply ends the
  pair where it arrives, so none resolves on one). K strikes in a row rule a
  candidate above P out and end the walk (`ClientRuleOut`); at the first step,
  with P still P0, the walk goes on to P0's own check, fresh, and K strikes at
  P0 end the connection -14 (`ClientFail`). So the walk takes one pair interval
  for each size the path carries and K for the first it does not.
- **Large replies.** A large reply that opens is taken only while probing and
  only at the walk's current candidate, from any pair so far
  (`ClientRecvLarge`): above P it raises P at once, sets the confirm and goes
  on to the next size up: no pair outstanding, the strikes and the loss
  assumption's row at zero, and the next pair due at once; at the largest
  candidate the walk ends. At P itself (P0's check) it raises nothing and the
  walk waits to re-try the first step. Any other large reply is consumed and ignored; a forged one
  fails the open (`ClientRecvForged`). A confirm can be outstanding while the
  walk goes on (the code sends a confirm due in the same update as a pair
  first and the pair at the next update; the model does not order them).
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
  until nothing of the pair is in flight (the liveness and loss-only cases),
  and the pair a raise makes due at once keeps the same promise:
  `ClientSendPair` waits until nothing of the last pair is in flight and the
  server's update that answered the last large probe has ended. Without it TLC
  finds a size the path carries ruled out by one burst and one strike that no
  loss made: the next step's large probe withheld by the update's large
  allowance, spent by the last step's large probe or by an on-path replay of
  it still in flight (2026-11-26, space). Untimed, the code's own timing, both
  can happen, which is one more reason `NoWrongRuleOut` is claimed Timed only.
  FALSE is the code's untimed view: a pair's packets may arrive after it
  resolved, until the pair after next reuses the pair number's parity, when what
  is left of it is dropped (the code's u32 pair number keeps them distinct; a
  packet two intervals late is taken as lost).
- **Re-try.** `ClientRuleOut` sends the walk to `waiting` (or, the first time
  the first step is ruled out, to P0's `check`); `ClientRetry` ends the wait
  when the next pair is due, a fresh step at the ruled-out size with its
  strikes at zero. The schedule's times are not here: they are
  `RNPathProbeRetryLayout.tla`'s (below); here a wait lasts at least one pair
  interval. When `Timed`, the wait also covers the confirm's exchange (the
  code's 10 s against a confirm a second): a re-try starts after the echo of
  a raise before it. Without that, the model's one small allowance a pair
  interval lets each re-try's small probe beat the confirm for ever, and the
  server never reaches P. `LossBound` = FALSE drops the loss assumption, so a
  burst of large packets may meet any number of pairs and rule a carried size
  out: `NoWrongRuleOut` is not claimed there, and `RetryReaches` is.
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
| loss alone never rules out a size the path carries (Timed, under the loss assumption) | `NoWrongRuleOut` | `MCRNPathProbeBrokenNoReset.cfg`; `MCRNPathProbeBrokenUnboundedBurst.cfg` drops the assumption |
| a strike counts only for a pair whose small reply came back | `StrikeNeedsSmall` (action) | `MCRNPathProbeBrokenStrikeOnLoss.cfg` |
| the server raises only on a confirm carrying the size | `ServerRaisesOnConfirm` (action) | `MCRNPathProbeBrokenRaiseOnProbe.cfg` |
| the client raises only on an authenticated large reply of the size | `ClientRaisesOnAuthLarge` (action) | `MCRNPathProbeBrokenForgedRaise.cfg` |
| -14 only when P0 itself collected K strikes | `FailOnlyAtP0` | |
| P is never a candidate ruled out | `NeverAtRuledOut` | |
| no pair once P is the largest candidate or the walk has ended | `NoProbeAfterDone` (action) | `MCRNPathProbeBrokenProbeAfterMax.cfg` |
| the ladder waits at a rule-out: no pair and no large probe of the client's in flight above a size ruled out | `NeverAboveRuled` | `MCRNPathProbeBrokenAboveRuled.cfg` |
| the size ruled out is the next size up from P, the one the walk re-tries | `RuledIsTheNextStep` | |
| the ladder skips no step: every pair is at the smallest candidate above P not ruled out, or at P for P0's check | `NoSkippedStep` (action) | `MCRNPathProbeBrokenSkipStep.cfg` |
| the server answers only from a connected mapping | `ReplyOnlyConnected` | `MCRNPathProbeBrokenUnproven.cfg` |
| no reply larger than the probe it answers | `ReplyNoLarger` | `MCRNPathProbeBrokenLarger.cfg` |
| a large reply only at a size on the server's list | `ReplyOwnList` | `MCRNPathProbeBrokenOffList.cfg` |
| at most one small (or confirm) and one large answered per mapping per update | `OneReplyPerUpdate` | `MCRNPathProbeBrokenNoAllowance.cfg` |
| both ends come to hold the same P for good (the walk may wait and re-try for ever) | `Converges` | |
| every wait ends in a re-try | `WaitEnds` | |
| a size the path carries that was ruled out is reached once loss ends, P0's check aside | `RetryReaches` | `MCRNPathProbeBrokenNoRetry.cfg` (no re-try: the walk ends at the rule-out) |
| both ends reach the top of the ladder: the largest candidate up to which the path carries every candidate above P0 | `ReachesBest` | |
| a path that does not carry P0 ends -14 | `FailsWhenP0NotCarried` | `MCRNPathProbeBrokenSkipLast.cfg` |
| a forger at the client's address, without limit, cannot starve the walk | `ForgedNeverStarves` | `MCRNPathProbeBrokenForgedStarves.cfg` |
| a connection in a reused slot, or the same client connecting again, starts fresh at both ends: P0, nothing ruled out, no strikes, no confirm, no wait | `ReuseStartsFresh` | `MCRNPathProbeBrokenReuseKeep.cfg` |

`NoRetryRaise` is reachability: `MCRNPathProbeReachRetryRaise.cfg` must
violate it, so a raise by a re-try's large reply is explored.

`NoLateRaise` is reachability, not a rule: `MCRNPathProbeReachLateReply.cfg`
must violate it, so the untimed path where a large reply of an earlier pair
raises P is explored.

## Instances

`MCRNPathProbe.tla` extends the module. Unless the case says otherwise: client
and server lists `{3, 2, 1}`, `K` = 2, one burst (`MaxBursts` = 1),
`AttackBudget` = 1 of every kind (`AttackKinds` all three), `Forger` = FALSE,
`Timed` = TRUE, no reuse (`MaxReuses` = 0, the reuse paths empty).

| Case | Notes |
| --- | --- |
| `MCRNPathProbe.cfg`, `MCRNPathProbeDown3.cfg` | capacity `0 .. 2` up and `0 .. 2` down (3 down in the second; split on 2026-11-26 when the re-try took the one case to 60 s, a split that loses no behaviour), every invariant and every property but the re-try's two under `FairSpec` |
| `MCRNPathProbeRetry.cfg` | capacity `0 .. 2` up and `0 .. 3` down, every invariant, and the re-try's `WaitEnds` and `RetryReaches` under `FairSpec` (split from `MCRNPathProbe.cfg` on 2026-11-26: the liveness check's cost grows with each property, and the re-try took the one case past the 110 s case budget) |
| `MCRNPathProbeUp3.cfg`, `MCRNPathProbeUp3Down3.cfg` | capacity 3 up and `0 .. 2` down (3 down in the second, split as `MCRNPathProbe.cfg` is), the same checks: with `MCRNPathProbe.cfg`, every path `0 .. 3` each way (the capacities are fixed for a behaviour, so the split loses no behaviour; split 2026-11-26 when the ladder took the one sweep to 72.9 s, over 60% of the case budget) |
| `MCRNPathProbeUp3Retry.cfg` | capacity 3 up and `0 .. 3` down with the re-try's two properties, split as `MCRNPathProbeRetry.cfg` is |
| `MCRNPathProbeServerList.cfg` | server list `{3, 1}`: the client's middle size is never answered, is ruled out and re-tried for ever, so 3 is never tried; the same checks at the same capacities |
| `MCRNPathProbeServerListRetry.cfg` | `MCRNPathProbeServerList.cfg`'s instance with the re-try's two properties, split as `MCRNPathProbeRetry.cfg` is |
| `MCRNPathProbeOneEntry.cfg` | client set `{2}`, server list `{2, 1}`: no probing |
| `MCRNPathProbeLate.cfg` | `Timed` = FALSE, capacity 1 up and `{1, 3}` down, one attacker send and no burst, safety (`Spec`) |
| `MCRNPathProbeLateUp3.cfg`, `MCRNPathProbeLateUp3Down3.cfg`, `MCRNPathProbeLateUp3Down3Probe.cfg`, `MCRNPathProbeLateUp3Down3Reply.cfg` | the same at capacity 3 up, 1 down and 3 down: with `MCRNPathProbeLate.cfg`, `{1, 3}` each way (split on 2026-11-26 when the re-try took the one case to 67.6 s, over 60% of the case budget). The 3-down case is split again by the kind of its one attacker send (`AttackKinds`): the replay, a packet from X or F, a forged reply. With a budget of one send, the three cover every behaviour of the one case; split on 2026-11-26 when it took 66.7 s, and the attacker send is 2.4M of its 2.5M distinct states |
| `MCRNPathProbeLateBurst.cfg` | `Timed` = FALSE, capacities `{1, 3}`, one burst and no attacker send, safety (`Spec`); split from `MCRNPathProbeLate.cfg` on 2026-11-26, when the ladder took the two together to 107 s of the 110 s case budget (the Timed cases keep the burst and the attacker together) |
| `MCRNPathProbeLossRow.cfg` | client and server `{2, 1}`, `K` = 2, three bursts of either kind, capacities `{1, 2}`, no attacker: `NoWrongRuleOut` across a row broken by a lost small probe (safety); `MCRNPathProbeBrokenNoReset.cfg` is its instance at capacity 2 each way with the reset removed |
| `MCRNPathProbeForged.cfg` | `Forger` = TRUE and no other attacker, capacities `{1, 3}`: `ForgedNeverStarves` and `Converges` |
| `MCRNPathProbeReachLateReply.cfg` | `Timed` = FALSE, capacity 3 each way, `K` = 2, no loss: must violate `NoLateRaise` |
| `MCRNPathProbeHeavyLoss.cfg`, `MCRNPathProbeHeavyLossUp3.cfg` | `LossBound` = FALSE, capacity 2 up (3 up in the second; split on 2026-11-26 when the two together passed the case budget) and 3 down, two bursts, one attacker send: every safety property but `NoWrongRuleOut`, and `Converges` under `FairSpec`, with `WaitEnds` and `RetryReaches` in the two Retry cases below (`ReachesBest` is not claimed: a burst may also fail P0's own check, -14) |
| `MCRNPathProbeHeavyLossRetry.cfg`, `MCRNPathProbeHeavyLossUp3Retry.cfg` | each heavy-loss instance with `WaitEnds` and `RetryReaches`, which the case of the same instance leaves out (split on 2026-11-26 when each case passed 70 s, over 60% of the case budget) |
| `MCRNPathProbeReachRetryRaise.cfg` | must violate `NoRetryRaise`: a re-try's large reply raises P |
| `MCRNPathProbeBrokenNoRetry.cfg` | a ruled-out size is never re-tried: must violate `RetryReaches` |
| `MCRNPathProbeReuse.cfg` | one reuse (`MaxReuses` = 1), capacities `{1, 3}` each way for both connections, one burst, one attacker send: every invariant with `ReuseStartsFresh`, and the action properties, under `Spec` (`PNeverDecreases`, `NoProbeAfterDone` and `NoSkippedStep` hold within a connection: the reuse step is the one that lowers P) |
| `MCRNPathProbeBrokenReuseKeep.cfg` | the server slot keeps its P across the reuse: must violate `ReuseStartsFresh` |
| each `MCRNPathProbeBroken*.cfg` | must violate the one property it names; its header says its instance |

## Counterexamples

Each read against the model by hand (2026-11-25, TLC 2.19 on studio.local;
the ladder's, 2026-11-26 on space); against the code once the v2 code is in.

- **Lower** (`PNeverDecreases`, untimed, `K` = 1): the pair at 2 is answered but
  its large reply is late; the pair resolves on the small alone, a strike, and 2
  is ruled out at the first step, so P0's check sends a pair at 1; the late
  reply of 2 is taken (the broken client takes any candidate's reply), P = 2;
  then the reply of 1 is taken, P = 1.
- **StrikeOnLoss** (`StrikeNeedsSmall`): a burst of every packet; the pair sent
  inside it loses both probes, and the broken client counts a strike.
- **NoReset** (read on space, 2026-11-26; `NoWrongRuleOut`, `MaxBursts` = 3, `K` = 2, a path that carries
  2): the first pair at 2 has its small reply in when a burst of every packet
  starts and drops its large probe: a strike, the row at 1. The next pair is
  sent inside that burst and loses its small probe; the broken client keeps the
  strike (the code sets it to zero). The burst ends, the next pair's small
  reply comes back and a burst of large packets only drops its large: a second
  strike, the row at 1 again, and 2 is ruled out. TLC's shortest trace uses two
  bursts (every packet, then large only); large, every packet, large is the same
  mechanism one burst longer.
- **UnboundedBurst** (`NoWrongRuleOut`, not the code): with the loss assumption
  dropped, one burst of large packets only meets two pairs at 2 on a path that
  carries 2: both small replies come back, both large are lost, two strikes,
  and 2 is ruled out.
- **RaiseOnProbe** (`ServerRaisesOnConfirm`): the server answers the first large
  probe at 2 and raises its P to 2 with no confirm.
- **ForgedRaise** (`ClientRaisesOnAuthLarge`): a forged large reply at 2 on a path
  of capacity 1; the broken client raises to 2.
- **ProbeAfterMax** (`NoProbeAfterDone`): the first pair at 2 succeeds and P = 2,
  the largest candidate; the broken walk does not end, and after the server's
  update the next pair goes, at 2 again.
- **Unproven** (`ReplyOnlyConnected`): a probe from `X`, whose mapping is not
  connected, is answered.
- **Larger** (`ReplyNoLarger`): client `{2, 1}`, server `{3, 2, 1}`: the probe at 2
  is answered at 3.
- **OffList** (`ReplyOwnList`): client `{3, 2, 1}`, server `{2, 1}`: the ladder
  raises to 2, the next pair goes at 3, and the probe at 3 is answered.
- **NoAllowance** (`OneReplyPerUpdate`): the large probe and a replayed copy of it
  arrive in one update and both are answered.
- **SkipLast** (`FailsWhenP0NotCarried`): a path that carries nothing above the
  control size; 2 is ruled out at the first step and the broken walk ends
  without probing P0, so the client stays at a P0 the path drops.
- **ForgedStarves** (`ForgedNeverStarves`): in every update the forger's packet
  arrives first, fails the open and spends the allowance, so the client's probes
  are withheld and the walk never reaches 2.
- **ReachLateReply** (`NoLateRaise`): the pair at 2 is answered, its small probe
  still in flight up and its large reply in flight down; it resolves with the
  small not seen (strikes stay at zero), the next pair at 2 is sent, and the late
  reply of the first is taken at the walk's candidate: P = 2.
- **AboveRuled** (`NeverAboveRuled`): client `{3, 2, 1}`, server `{3, 1}`,
  capacity 3 each way: two pairs at 2 come back small only (2 is off the
  server's list), 2 is ruled out, and the broken walk sends its next pair at 3,
  above the size ruled out.
- **SkipStep** (`NoSkippedStep`): client and server `{3, 2, 1}`: the broken walk's
  first pair goes at 3, the largest candidate (the old order), not at 2.
- **ReuseKeep** (read 2026-11-26 on hetzner2; `ReuseStartsFresh`): capacity 3
  each way; the pair at 2 is answered, the client takes the large reply and
  raises to 2, and its confirm raises the server to 2. The connection ends and
  the slot goes to a new one on a path of capacity 1 down: the client starts
  at P0 = 1, the broken server slot at 2, a size the new path drops.

## Records

The records in `RUNS.tsv` are measured on hetzner2 with the pinned tlacheck
(`tlacheck run --group rnpathprobe`, which refuses to run on macOS), one case
per bounded run, two workers, one case at a time, 2026-11-26, after the
re-try and the slot reuse. Cases over 60% of the 110 s case budget were split
so that no behaviour is lost. The capacity sweep is split on the up and down
capacities, which are fixed per behaviour. The untimed case is split into its
attacker send and its burst, then on the capacities, and its 3-up, 3-down part
by the kind of its one attacker send. The re-try's two liveness properties,
and the heavy-loss cases' two, are split into cases of their own. The slowest
case is 48.0 s. `MCRNPathProbe.cfg` measured 73.2 s once while another job
took the bench from load 5 to 29, and 40.3 s at load 1; the record is from a
quiet bench. No case of the group is timeout debt.

| Case | Result | Generated | Distinct | Seconds |
| --- | --- | --- | --- | --- |
| `MCRNPathProbe.cfg` | pass | 541,150 | 90,606 | 41.9 |
| `MCRNPathProbeDown3.cfg` | pass | 299,610 | 49,004 | 23.4 |
| `MCRNPathProbeRetry.cfg` | pass | 840,760 | 139,610 | 43.8 |
| `MCRNPathProbeUp3.cfg` | pass | 530,369 | 82,309 | 40.9 |
| `MCRNPathProbeUp3Down3.cfg` | pass | 266,717 | 37,601 | 21.7 |
| `MCRNPathProbeUp3Retry.cfg` | pass | 797,086 | 119,910 | 42.4 |
| `MCRNPathProbeServerList.cfg` | pass | 649,202 | 112,558 | 48.0 |
| `MCRNPathProbeServerListRetry.cfg` | pass | 649,202 | 112,558 | 35.2 |
| `MCRNPathProbeOneEntry.cfg` | pass | 2,457 | 432 | 2.0 |
| `MCRNPathProbeLate.cfg` | pass | 1,456,762 | 191,676 | 6.0 |
| `MCRNPathProbeLateUp3.cfg` | pass | 1,226,087 | 160,120 | 5.3 |
| `MCRNPathProbeLateUp3Down3.cfg` | pass | 9,724,470 | 1,076,271 | 28.6 |
| `MCRNPathProbeLateUp3Down3Probe.cfg` | pass | 13,253,602 | 1,362,699 | 36.8 |
| `MCRNPathProbeLateUp3Down3Reply.cfg` | pass | 4,839,346 | 524,115 | 14.8 |
| `MCRNPathProbeLateBurst.cfg` | pass | 2,453,290 | 289,916 | 8.4 |
| `MCRNPathProbeLossRow.cfg` | pass | 36,687 | 7,382 | 1.5 |
| `MCRNPathProbeForged.cfg` | pass | 177,372 | 26,624 | 20.9 |
| `MCRNPathProbeReachLateReply.cfg` | `NoLateRaise` | 335 | 120 | 1.0 |
| `MCRNPathProbeBrokenLower.cfg` | `PNeverDecreases` | 12,768 | 3,182 | 1.2 |
| `MCRNPathProbeBrokenStrikeOnLoss.cfg` | `StrikeNeedsSmall` | 103 | 42 | 0.9 |
| `MCRNPathProbeBrokenNoReset.cfg` | `NoWrongRuleOut` | 9,003 | 1,644 | 1.1 |
| `MCRNPathProbeBrokenUnboundedBurst.cfg` | `NoWrongRuleOut` | 1,622 | 311 | 1.0 |
| `MCRNPathProbeBrokenRaiseOnProbe.cfg` | `ServerRaisesOnConfirm` | 7 | 4 | 0.9 |
| `MCRNPathProbeBrokenForgedRaise.cfg` | `ClientRaisesOnAuthLarge` | 66 | 26 | 0.9 |
| `MCRNPathProbeBrokenProbeAfterMax.cfg` | `NoProbeAfterDone` | 130 | 40 | 0.9 |
| `MCRNPathProbeBrokenUnproven.cfg` | `ReplyOnlyConnected` | 37 | 28 | 0.9 |
| `MCRNPathProbeBrokenLarger.cfg` | `ReplyNoLarger` | 7 | 4 | 0.9 |
| `MCRNPathProbeBrokenOffList.cfg` | `ReplyOwnList` | 183 | 53 | 0.9 |
| `MCRNPathProbeBrokenNoAllowance.cfg` | `OneReplyPerUpdate` | 378 | 129 | 1.0 |
| `MCRNPathProbeBrokenSkipLast.cfg` | `FailsWhenP0NotCarried` | 11,692 | 2,785 | 2.1 |
| `MCRNPathProbeBrokenForgedStarves.cfg` | `ForgedNeverStarves` | 39,789 | 6,456 | 2.8 |
| `MCRNPathProbeBrokenAboveRuled.cfg` | `NeverAboveRuled` | 91 | 27 | 0.9 |
| `MCRNPathProbeBrokenSkipStep.cfg` | `NoSkippedStep` | 2 | 2 | 0.8 |
| `MCRNPathProbeHeavyLoss.cfg` | pass | 671,794 | 97,230 | 19.3 |
| `MCRNPathProbeHeavyLossRetry.cfg` | pass | 671,794 | 97,230 | 35.4 |
| `MCRNPathProbeHeavyLossUp3.cfg` | pass | 902,266 | 120,519 | 26.5 |
| `MCRNPathProbeHeavyLossUp3Retry.cfg` | pass | 902,266 | 120,519 | 47.3 |
| `MCRNPathProbeReuse.cfg` | pass | 1,041,646 | 128,010 | 4.6 |
| `MCRNPathProbeBrokenReuseKeep.cfg` | `ReuseStartsFresh` | 386 | 98 | 0.9 |
| `MCRNPathProbeBrokenNoRetry.cfg` | `RetryReaches` | 2,433 | 459 | 1.3 |
| `MCRNPathProbeReachRetryRaise.cfg` | `NoRetryRaise` | 2,377 | 461 | 1.0 |

## Trace binding

The trace check's layer `path-probe` (tests/tla_trace_path_probe.cpp,
TraceRNPathProbe.tla) binds the client and server protocols to this model
with `Timed` = FALSE, a re-try being the record `client_retry`
(`ClientRetry`); seed scenario 0 runs under heavy loss (`LossBound` = FALSE),
and the layer must reach `Waiting` and `Retrying`. Seed scenario 3 reuses the
slot once, past half the walk (the record `reuse`, `Reuse`, with the new path's
capacities): the client and the server disconnect, a new token's handshake
must land in the same slot, and the layer must reach `Reused`. The trace's
`CapsUp` and `CapsDown` are the first connection's path alone, its
`ReuseCapsUp` and `ReuseCapsDown` the reuse's.

## The re-try schedule: RNPathProbeRetryLayout

`RNPathProbeRetryLayout.tla` is the schedule's arithmetic, a layout model
beside this one: the code's count of re-tries (saturating at
`ROCKETNET_LINK_MTU_RETRY_SATURATE`, the first re-try whose gap is the cap)
and the time of the next, advanced by `rocketnet_link_mtu_retry_next`
(sdk/rocketnet_link_mtu.h) at the rule-out and at the end of each re-try ruled
out again, whatever time after its slot that is (`Schedule(d)`, its `Advance`
the code's loop, with the jump over passed slots at the cap, its `CodeGap` the
code's gap). `NextIsTheDesigns`: the next re-try is the design's first time
after the call (10, 20, 30, then 50, 90, 170, 330, then every 300 s);
`CountSaturates`: the count is the design's index of it, to the saturation;
`GapIsTheDesigns`: the code's gap is the design's.

| Case | Notes |
| --- | --- |
| `MCRNPathProbeRetryLayout.cfg` | the small scale: gaps 1, 1, 1, doubling to a cap of 5, saturating at 6, calls up to 7 past a slot, to 60 |
| `MCRNPathProbeRetryLayoutCode.cfg` | the code's scale: 10, 3, 300, saturating at 8, calls up to 610 past a slot, to 3000 |
| `MCRNPathProbeRetryLayoutBrokenNoCap.cfg` | the gap is not capped: must violate `NextIsTheDesigns` |
| `MCRNPathProbeRetryLayoutBrokenEarly.cfg` | the gap doubles one re-try early: must violate `NextIsTheDesigns` |
| `MCRNPathProbeRetryLayoutBrokenWrap.cfg` | the count wraps instead of saturating, and the gaps start over: must violate `NextIsTheDesigns` |
| `MCRNPathProbeRetryLayoutBrokenNoSkip.cfg` | a call adds one gap whatever the time, so a slot passed while a re-try ran is due at once: must violate `NextIsTheDesigns` |

The trace check's layer `path-probe-retry` (tests/tla_trace_path_probe_retry.cpp,
TraceRNPathProbeRetryLayout.tla, header-only) binds the code: its walk calls
`rocketnet_link_mtu_retry_next` at the code's constants, at the rule-out and at
random times after each slot, some past several slots at the cap, and must
reach `AtCap`; its plant (`ROCKETNET_LINK_MTU_RETRY_PLANT`) doubles the gap
one re-try early.
