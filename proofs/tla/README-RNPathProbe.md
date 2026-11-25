# RNPathProbe: the path packet size probe of the handshake

`RNPathProbe.tla` models the MTU fallback design's handshake: after the
challenge, a client whose candidate list has more than one entry probes the
path with packet type 9 (connection probe) at each candidate size in turn,
takes the first matching reply, and carries the chosen size in its connection
response; the server answers probes and assigns the slot with that size. It
was written ahead of the code and then made to match it: the code cites it
action by action (sdk/rocketnet_client_protocol.cpp,
sdk/rocketnet_server_protocol.cpp), its coverage rows are `current`, and the
trace check binds the code to it (Trace binding, below).

The problem it guards: today the client sends payloads of up to the ceiling
(1452 bytes) with don't-fragment and no discovery. On a path that cannot carry
them, every handshake packet (all small) arrives, the client connects on the
small keep-alive, and every full-size packet is dropped forever while small
keep-alives keep the connection alive. `MCRNPathProbeBrokenToday*.cfg` model
that behaviour and fail.

## Abstraction

- **Sizes.** Abstract wire sizes `1 .. Ceiling` (`Ceiling` = 1452 bytes in the
  code). A control packet (connection response, keep-alive) is wire size 0 and
  fits every path; the response carries the chosen size in its padding. The
  client's list is the candidate list for the committed family, strictly
  descending (P of each `link_mtus` entry, equal sizes collapsed), a set
  `ClientSet` walked largest first (`Cand(i)`); the server's list is its own
  candidate set for that family and may differ. A one-entry client list does
  no probing and sends its response at once with that entry, exactly as today.
- **The path.** A capacity each way, chosen at `Init` from `CapsUp` and
  `CapsDown` and fixed for the behaviour, so one run covers every path of the
  instance. A packet larger than its direction's capacity is dropped when sent,
  always. A packet that fits may be lost (`LoseUp`, `LoseDown`), at most
  `MaxLoss` times in a behaviour; packets in flight are a set, delivered in any
  order (reordering; a resend of a packet already in flight is the same
  packet).
- **The proven address.** The model starts after the challenge: the client
  holds the session keys, probes and replies are sealed under them on the
  session sequences, and the server's mapping of its address is half-open. A
  second address `X` has no mapping; a forger `F` at the client's own address
  finds the client's mapping but holds neither key of the exchange; neither's
  probe opens (`AttackProbe(a, s)`). With `Forger` = TRUE the forger sends
  from `F` without limit (`ForgedNeverStarves`). An on-path attacker may also replay a client
  probe in flight (`Replay`, a copy with `n = 1`); a replay opens, which is
  why the per-update allowance exists. The attacker has `AttackBudget` sends in
  all.
- **The server's order.** `ServerRecvProbe` (a probe from `C` or `X`) and
  `ServerRecvForged` (a probe from `F`) take the code's checks in its order: a
  size on the server's own list, a mapping for the source, no slot, the
  update's allowance unspent; then the open. Only a probe that opens spends
  the allowance and is answered; a forged probe fails the open and spends
  nothing, so the genuine probe of the same update is still answered
  (rocketnet_server_process_probe). A response to a connected or removed
  mapping is ignored (`ServerRecvResponse`); the slot's keep-alives go on
  their own clock (`ServerSendKeepAlive`).
- **Time.** `Tick` is one client send interval: the client may send one packet
  (`maySend`: the send is due, or the early send an advance armed,
  rocketnet_client_protocol_send_ready) and the server runs at least one
  update, which resets the mapping's reply allowance. A probing client sends
  at every interval, so while it probes a `Tick` follows its send.
  `ServerUpdate` is a further server update inside the interval, weakly fair:
  the server keeps updating. A probe window is `W` sends of the candidate's
  probe (max(ROCKETNET_CLIENT_PROBE_WINDOW_SENDS, 3 round trips), at most
  ROCKETNET_CLIENT_PROBE_WINDOW_MAX_SENDS, 10). At the send after the `W`-th the
  client moves to the next candidate and sends its probe in the same call
  (`ClientExpireNext`, the count back at 1) or, past the last candidate, ends
  -14 (`ClientExpireFail`). A terminal state zeroes the attempt
  (`ClientReset`: the index, the count, the size and the send clock, as
  rocketnet_client_connection_reset does).
- **The handshake deadline.** With `Deadline` > 0, `age` counts the Ticks the
  mapping has been half-open, and `Tick` removes it at `Deadline` Ticks, or
  `Deadline + Budget` when the server's own list gives more than one
  candidate (rocketnet_server_handshake_deadline_seconds). `Budget` is the
  probe's time bound (ROCKETNET_LINK_MTU_PROBE_BUDGET_SECONDS, 8 s: eight
  candidates of ten sends at the handshake rate); an ASSUME holds it to at
  least the longest walk, `N * W`, as the code's static_assert does.
  `Deadline` = 0 leaves the deadline out. It is checked at the code's timing
  (`Timed = FALSE`), where a Tick follows each send of the walk; under `Timed`
  = TRUE a client waiting out a round trip lets Ticks pass without sends, and
  those Ticks are not the code's time.
- **Timed.** `Timed = FALSE` is the code: the client sends at every send
  opportunity whatever is in flight, a window can end with its probe or reply
  in flight, the response is resent at every send, and the silence timeout
  (-4) can end sending connection response at any time; it waits while the
  client probes (rocketnet_client_check_deadlines, `!client->probing`).
  `Timed = TRUE` adds the assumption the liveness properties need, that a
  window covers a round trip: the client sends again only when nothing of its
  last exchange is in flight, a window ends only then, and the response phase
  is `W` responses ended -4 only while the server holds no slot.
- **Late replies.** Under `Timed = FALSE` a reply can arrive in a later window,
  and the client takes it when its size is a candidate at or below the one it
  is now probing, without resetting the window's count
  (`MCRNPathProbeLate.cfg`, safety only; `MCRNPathProbeReachLateReply.cfg` shows
  the path is reached; the trace check's reach predicate `LateReplyTaken` shows
  the code takes one).
- **The allowance as loss.** A client probe that arrives in the same server
  update as an earlier one (delay bunched them) is withheld unopened. To the
  client that is a lost attempt, so a withheld honest probe spends the loss
  budget; a withheld replayed copy does not (the reply to the copy reached the
  client). Without this the liveness claim fails: one network loss plus one
  bunching ends a fitting window, and with `W` = 2 the client fails -14 on a
  path that carries a candidate. That is a real property of the design: each
  bunched probe costs one of the window's attempts, which the design's window
  of four sends absorbs.
- **After connect.** Both ends send payload at the connection's size (the
  client's `size`, the server's `slot`); the ghosts `upGot` and `downGot` are
  set when one arrives. No connection timeout is modelled: in the code the
  keep-alives keep a connection whose full-size packets are all dropped alive
  (`reliable_message_timeout_ms` defaults to 0), which is the hang `NoHang`
  names. `Done` is the terminal stutter.

Not modelled: the IPv4/IPv6 split (one family's list), the packet layout and
padding checks (a probe that fails to open stands for every refusal after the
allowance), a zero-sequence probe (refused before the open), the server's
half-open idle refresh (10 s, past the probe's bound), the integrity limit a
forger's failed opens count toward (2^36), the connect token's claim, and the
per-connection payload capacities that follow from the size.

## Rules

| Rule | Definition | Reversed by |
| --- | --- | --- |
| (1) both ends hold the same size: connected means the slot is assigned with the client's size | `SlotAgrees` | `MCRNPathProbeBrokenSlotCeiling.cfg` |
| the chosen size is one of the client's candidates | `ChosenIsCandidate` | |
| (2) a probing client connects only at a size the path carries both ways | `CarriedBothWays` | `MCRNPathProbeBrokenToday.cfg` |
| (2) and a packet of that size crossed each way: the probe the reply answers, and the reply | `CrossedBothWays` | (fails with `CarriedBothWays` in the same behaviours) |
| (3) no reply to an address whose probe did not open under a proven mapping's session key | `ReplyOnlyProven` | `MCRNPathProbeBrokenUnproven.cfg` |
| (3) no reply larger than the probe it answers | `ReplyNoLarger` | `MCRNPathProbeBrokenLarger.cfg` |
| the server answers only sizes on its own list | `ReplyOwnList` | `MCRNPathProbeBrokenOffList.cfg` |
| (4) at most one probe reply per mapping per update | `OneReplyPerUpdate` | `MCRNPathProbeBrokenNoAllowance.cfg` |
| the attempt ends: connected, -14 or -4 | `Ends` | `MCRNPathProbeBrokenNoExpire.cfg` |
| a probing client that connects gets full-size payload through both ways | `NoHang` | `MCRNPathProbeBrokenTodayHang.cfg` |
| a candidate on both lists that fits both ways, and loss not total (`MaxLoss < W`): it connects | `ConnectsWhenFits` | `MCRNPathProbeBrokenSkipLast.cfg` |
| no candidate fits: the probing client ends -14 | `FailsWhenNoneFits` | `MCRNPathProbeBrokenTodayConnects.cfg` |
| (5) a forger at the client's address, without limit, cannot starve a client whose path fits | `ForgedNeverStarves` | `MCRNPathProbeBrokenForgedStarves.cfg` |
| (6) the walk ends inside the server's handshake deadline | `WalkInsideDeadline` | `MCRNPathProbeBrokenNoBudget.cfg` |
| (7) the walk ends in -14 or a chosen size, never in the silence timeout | `NoTimeoutWhileProbing` | `MCRNPathProbeBrokenTimeoutProbing.cfg` |

`CarriedBothWays`, `CrossedBothWays` and `NoHang` claim nothing for a one-entry
list: the default `{ 1500 }` list keeps today's behaviour by design, hang
included, on a path that cannot carry 1452 bytes.

## Instances

`MCRNPathProbe.tla`: `Ceiling` = 3, the three-step client set `{3, 2, 1}`, every
capacity `0 .. 3` each way, `W` = 2, `MaxLoss` = 1, `AttackBudget` = 1, `Forger` =
FALSE, `Deadline` = 0, `Timed` = TRUE unless the case says otherwise.

| Case | Server list | Notes |
| --- | --- | --- |
| `MCRNPathProbe.cfg` | `{3, 2, 1}` | every invariant and property under `FairSpec` |
| `MCRNPathProbeServerList.cfg` | `{3, 1}` | the server lacks the client's middle size: a probe of it is refused unanswered |
| `MCRNPathProbeOneEntry.cfg` | `{3, 2, 1}` | client set `{3}`, no probing |
| `MCRNPathProbeLate.cfg` | `{3, 2, 1}` | `Timed` = FALSE, capacities `{1, 3}` each way, invariants only (`Spec`) |
| `MCRNPathProbeForged.cfg` | `{3, 2, 1}` | `Forger` = TRUE, no other attacker, capacities `{0, 1, 3}`: `ForgedNeverStarves` and `Ends` under `FairSpec` |
| `MCRNPathProbeDeadline.cfg` | `{2, 1}` | the per-PR deadline check, reduced to two candidates (`Ceiling` = 2, client set `{2, 1}`) to fit the 110 s case budget: `Deadline` = 3, `Budget` = 4, `Timed` = FALSE, capacities `{1, 2}`, one loss and one attacker send: `WalkInsideDeadline` (safety) |
| `MCRNPathProbeDeadlineFull.cfg` | `{3, 2, 1}` | the same check at three candidates: `Deadline` = 3, `Budget` = 6, `Timed` = FALSE, capacities `{1, 3}`, one loss and one attacker send; over the case budget, so `bench` timeout debt run every night (`NIGHTLY-DEBT.tsv`) |
| `MCRNPathProbeReachLateReply.cfg` | `{3, 2, 1}` | `Timed` = FALSE, must violate `NoLateReplyTaken` |
| each `MCRNPathProbeBroken*.cfg` | as named | must violate the one property it names |

## Counterexamples

Each read against the code by hand (2026-11-25, TLC 2.19 on studio.local, the
model as it matches the code).

- **Today** (`CarriedBothWays`): on a path that cannot carry the ceiling the
  client sends its response at once with it; the response (control size)
  arrives, the slot takes the ceiling, the keep-alive arrives: connected at a
  size the path drops.
- **TodayHang** (`NoHang`): capacity 0 both ways; connected at 3, payload of 3
  dropped every time, and nothing ends the connection.
- **TodayConnects** (`FailsWhenNoneFits`): capacity 0 both ways; the client
  connects at 3 where the design ends the attempt -14.
- **Unproven** (`ReplyOnlyProven`): a forger at the client's address sends a
  probe of 1 that does not open; the server answers it.
- **Larger** (`ReplyNoLarger`): a probe the server answers with its largest size,
  3, not the probe's.
- **OffList** (`ReplyOwnList`): server list `{3, 1}`; a probe of 2 arrives and the
  server answers it.
- **NoAllowance** (`OneReplyPerUpdate`): the client's probe and a replayed
  copy of it arrive in one update and both are answered.
- **SlotCeiling** (`SlotAgrees`): the client chooses a size below the ceiling,
  the response carries it, the server's slot takes the ceiling.
- **NoExpire** (`Ends`): capacity 0 up; the client sends probes of 3 forever,
  each dropped.
- **SkipLast** (`ConnectsWhenFits`): capacity 1 both ways; the windows of 3 and 2
  end and the walk stops at the second candidate, -14, though 1 fits.
- **ForgedStarves** (`ForgedNeverStarves`): capacity 1 both ways; the walk
  reaches 1, and in every update from then on the forger's probe arrives
  before the client's, fails to open and spends the allowance, so the client's
  probe of 1 is withheld and never answered: the client probes forever on a
  path that carries 1 (the code before 1527892f9).
- **NoBudget** (`WalkInsideDeadline`, the per-PR instance, 7 states): capacity
  1 up; the client's two probes of 2 are dropped, it moves to 1 at the second
  Tick, and the mapping is removed at the third Tick while it probes 1.
- **TimeoutProbing** (`NoTimeoutWhileProbing`): the silence timeout ends the
  attempt -4 while the client is still probing its first candidate.
- **ReachLateReply** (`NoLateReplyTaken`): the client sends two probes of 3 (one
  in flight, the set holds one), the server answers, the window ends and the
  client sends its probe of 2 in the same call, and the reply of 3 arrives
  while it probes 2: it takes 3 (rocketnet_client_process_probe: a candidate
  at an index at or below probe_index).

## Findings in the code

Findings (a) to (c) of the previous model are fixed in 1527892f9 and are now
properties: a forged probe spends nothing (`ForgedNeverStarves`), a server
whose list probes adds the probe's bound to the handshake deadline
(`WalkInsideDeadline`), and the silence timeout waits while probing
(`NoTimeoutWhileProbing`). Reported and not changed:

- (d) A response to a connected mapping is ignored; a client whose first
  keep-alive was lost relies on the slot's own keep-alives.
- (e) The deadline's budget follows the server's list, but the walk's length
  follows the client's. A server whose list gives one candidate for the
  family (the default list) adds nothing, while a client with a longer list
  still walks down to that size: with server list `{1}`, client set
  `{3, 2, 1}`, `Deadline` = 3 and capacity 0 up, the mapping is removed while
  the client probes 2 (TLC, `WalkInsideDeadline` violated in 833 distinct
  states). In the code: a client with an eight-entry list at a long round
  trip walks up to 8 s against a default list's 5 s deadline (1 s at the
  configured minimum), and fails -14 though the server's one size fits.
- (f) A probe that fails to open no longer spends the allowance, so a source
  can make the server do one failed open per probe it sends, not one per
  update; each counts toward the mapping's integrity limit (2^36).

## Records

The records in `RUNS.tsv` are measured on a Linux bench with the pinned
tlacheck (`tlacheck run --group rnpathprobe`, which refuses to run on
macOS), one case per bounded run, two workers: the 21 records of this model
were measured on space, 2026-11-25. `MCRNPathProbeDeadlineFull.cfg` is over
the 110 s record cap, so its record is the bounded FAIL/124 and its completed
counts are in `NIGHTLY-DEBT.tsv`, from one run with no record cap
(`--manual`, space, 2 workers):

| Case | Result | Generated | Distinct | Seconds |
| --- | --- | --- | --- | --- |
| `MCRNPathProbe.cfg` | pass | 286,182 | 44,741 | 22.5 |
| `MCRNPathProbeServerList.cfg` | pass | 286,268 | 44,902 | 22.5 |
| `MCRNPathProbeOneEntry.cfg` | pass | 47,072 | 9,408 | 4.1 |
| `MCRNPathProbeLate.cfg` | pass | 20,530,837 | 1,735,922 | 38.1 |
| `MCRNPathProbeForged.cfg` | pass | 109,993 | 14,232 | 6.2 |
| `MCRNPathProbeDeadline.cfg` | pass | 9,812,282 | 987,571 | 17.7 |
| `MCRNPathProbeDeadlineFull.cfg` | pass (nightly debt; bounded record FAIL/124 at 110 s) | 87,404,030 | 7,512,847 | 154.4 |
| `MCRNPathProbeReachLateReply.cfg` | `NoLateReplyTaken` | 5,720 | 2,194 | 1.0 |
| `MCRNPathProbeBrokenToday.cfg` | `CarriedBothWays` | 465 | 289 | 0.9 |
| `MCRNPathProbeBrokenTodayHang.cfg` | `NoHang` | 47,072 | 9,408 | 3.3 |
| `MCRNPathProbeBrokenTodayConnects.cfg` | `FailsWhenNoneFits` | 47,072 | 9,408 | 3.2 |
| `MCRNPathProbeBrokenUnproven.cfg` | `ReplyOnlyProven` | 137 | 136 | 0.7 |
| `MCRNPathProbeBrokenLarger.cfg` | `ReplyNoLarger` | 2,505 | 1,037 | 0.9 |
| `MCRNPathProbeBrokenOffList.cfg` | `ReplyOwnList` | 2,505 | 1,037 | 0.9 |
| `MCRNPathProbeBrokenNoAllowance.cfg` | `OneReplyPerUpdate` | 1,229 | 604 | 0.8 |
| `MCRNPathProbeBrokenSlotCeiling.cfg` | `SlotAgrees` | 8,756 | 3,006 | 1.0 |
| `MCRNPathProbeBrokenNoExpire.cfg` | `Ends` | 34,269 | 5,714 | 2.9 |
| `MCRNPathProbeBrokenSkipLast.cfg` | `ConnectsWhenFits` | 130,202 | 20,881 | 6.3 |
| `MCRNPathProbeBrokenForgedStarves.cfg` | `ForgedNeverStarves` | 170,985 | 20,960 | 8.7 |
| `MCRNPathProbeBrokenNoBudget.cfg` | `WalkInsideDeadline` | 847 | 374 | 0.8 |
| `MCRNPathProbeBrokenTimeoutProbing.cfg` | `NoTimeoutWhileProbing` | 6 | 6 | 0.7 |

## Trace binding

Layer `path-probe` of the trace check (README-TraceCheck.md):
`tests/tla_trace_path_probe.cpp` drives the real client and server protocols
over a path of its own (a capacity each way, loss, reorder, replay, forged
probes from the client's address that reach the open and fail, and probes
from an address with no mapping) and `TraceRNPathProbe.tla` checks each
record is the model step it names, with `Timed` = FALSE and the code's own
constants (the candidate sets, the ceiling, the window of 4, and `Deadline`
and `Budget` in ticks of the walk's clock: 60 s and 8 s at 64 a second).
The mapping's `age` is read off its own handshake deadline against the
server's last update, so a deadline without the budget would start the
trace at an age the model never has. The walk's scenario is the seed modulo
4: a path that carries every size (with deliveries to the client held back
and nothing lost down while it attempts, so a late reply is taken), one
whose capacities are two lower candidates, one that carries none (-14), and
any path ended by the silence timeout (-4) once the walk has chosen a size.
The data actions are untraced (no payload is sent) and the ghosts are left
to the model. Reach predicates `LateReplyTaken` and `ConnectedBelowFirst`.

The plant `ROCKETNET_PATH_PROBE_PLANT` spends the allowance in
rocketnet_server_process_probe before the open, as the code did before
1527892f9. It fires on seeds 1, 2 and 4 to 8 of 1 to 8, and TLC rejects each
planted trace exactly at the first record where a forged probe spent the
allowance (seed 1: record 16; 2: 14; 4: 27; 5: 4; 6: 20; 7: 6; 8: 23). The
clean traces of seeds 1 to 8 are accepted whole (401 records each); together
seeds 1 to 4 take every traced action (`ClientTimeout` in seed 3) and reach
both predicates (`LateReplyTaken` in seed 4, `ConnectedBelowFirst` in seed 1).
