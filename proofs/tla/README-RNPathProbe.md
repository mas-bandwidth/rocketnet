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
  holds the handshake keys and the server's mapping of its address is
  half-open. A second address `X` has no mapping; a forger `F` at the client's
  own address finds the client's mapping but holds no keys; neither's probe
  opens (`AttackProbe(a, s)`). An on-path attacker may also replay a client
  probe in flight (`Replay`, a copy with `n = 1`); a replay opens, which is
  why the per-update allowance exists. The attacker has `AttackBudget` sends in
  all.
- **The server's order.** `ServerRecvProbe` takes the code's checks in its
  order: a size on the server's own list, a mapping for the source, no slot,
  the update's allowance unspent; then the allowance is spent and the probe is
  opened, so a forged probe from the client's address spends the update's one
  reply as a genuine one would (rocketnet_server_process_probe: "a forged
  probe costs its source the update's one open"). A response to a connected
  mapping is ignored (`ServerRecvResponse`); the slot's keep-alives go on their
  own clock (`ServerSendKeepAlive`).
- **Time.** `Tick` is one client send interval: the client may send one packet
  (`maySend`: the send is due, or the early send an advance armed,
  rocketnet_client_protocol_send_ready) and the server runs at least one
  update, which resets the mapping's reply allowance. `ServerUpdate` is a
  further server update inside the interval. A probe window is `W` sends of
  the candidate's probe (ROCKETNET_CLIENT_PROBE_WINDOW_SENDS, 4, at a round
  trip short enough that three of them fit). At the send after the `W`-th the
  client moves to the next candidate and sends its probe in the same call
  (`ClientExpireNext`, the count back at 1) or, past the last candidate, ends
  -14 (`ClientExpireFail`). A terminal state zeroes the attempt
  (`ClientReset`: the index, the count, the size and the send clock, as
  rocketnet_client_connection_reset does).
- **Timed.** `Timed = FALSE` is the code: the client sends at every send
  opportunity whatever is in flight, a window can end with its probe or reply
  in flight, the response is resent at every send, and the silence timeout
  (-4) can end sending connection response, probing or not, at any time.
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
allowance), a zero-sequence probe (refused before the allowance is spent), the
server's half-open idle refresh and handshake deadline, the connect token, and
the per-connection payload capacities that follow from the size.

## Rules

| Rule | Definition | Reversed by |
| --- | --- | --- |
| (1) both ends hold the same size: connected means the slot is assigned with the client's size | `SlotAgrees` | `MCRNPathProbeBrokenSlotCeiling.cfg` |
| the chosen size is one of the client's candidates | `ChosenIsCandidate` | |
| (2) a probing client connects only at a size the path carries both ways | `CarriedBothWays` | `MCRNPathProbeBrokenToday.cfg` |
| (2) and a packet of that size crossed each way: the probe the reply answers, and the reply | `CrossedBothWays` | (fails with `CarriedBothWays` in the same behaviours) |
| (3) no reply to an address whose probe did not open under a proven mapping's handshake keys | `ReplyOnlyProven` | `MCRNPathProbeBrokenUnproven.cfg` |
| (3) no reply larger than the probe it answers | `ReplyNoLarger` | `MCRNPathProbeBrokenLarger.cfg` |
| the server answers only sizes on its own list | `ReplyOwnList` | `MCRNPathProbeBrokenOffList.cfg` |
| (4) at most one probe reply per mapping per update | `OneReplyPerUpdate` | `MCRNPathProbeBrokenNoAllowance.cfg` |
| the attempt ends: connected, -14 or -4 | `Ends` | `MCRNPathProbeBrokenNoExpire.cfg` |
| a probing client that connects gets full-size payload through both ways | `NoHang` | `MCRNPathProbeBrokenTodayHang.cfg` |
| a candidate on both lists that fits both ways, and loss not total (`MaxLoss < W`): it connects | `ConnectsWhenFits` | `MCRNPathProbeBrokenSkipLast.cfg` |
| no candidate fits: the probing client ends -14 | `FailsWhenNoneFits` | `MCRNPathProbeBrokenTodayConnects.cfg` |

`CarriedBothWays`, `CrossedBothWays` and `NoHang` claim nothing for a one-entry
list: the default `{ 1500 }` list keeps today's behaviour by design, hang
included, on a path that cannot carry 1452 bytes.

## Instances

`MCRNPathProbe.tla`: `Ceiling` = 3, the three-step client set `{3, 2, 1}`, every
capacity `0 .. 3` each way, `W` = 2, `MaxLoss` = 1, `AttackBudget` = 1, `Timed` =
TRUE unless the case says otherwise.

| Case | Server list | Notes |
| --- | --- | --- |
| `MCRNPathProbe.cfg` | `{3, 2, 1}` | every invariant and property under `FairSpec` |
| `MCRNPathProbeServerList.cfg` | `{3, 1}` | the server lacks the client's middle size: a probe of it is refused unanswered |
| `MCRNPathProbeOneEntry.cfg` | `{3, 2, 1}` | client set `{3}`, no probing |
| `MCRNPathProbeLate.cfg` | `{3, 2, 1}` | `Timed` = FALSE, capacities `{1, 3}` each way, invariants only (`Spec`) |
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
- **NoAllowance** (`OneReplyPerUpdate`): a forged probe from the client's
  address spends the update's allowance (it fails to open), then the client's
  probe of 3 arrives in the same update and is answered. The clean code
  withholds that probe: the forger's probe cost the client one attempt, which
  is finding (a) below.
- **SlotCeiling** (`SlotAgrees`): the client chooses a size below the ceiling,
  the response carries it, the server's slot takes the ceiling.
- **NoExpire** (`Ends`): capacity 0 up; the client sends probes of 3 forever,
  each dropped.
- **SkipLast** (`ConnectsWhenFits`): capacity 1 both ways; the windows of 3 and 2
  end and the walk stops at the second candidate, -14, though 1 fits.
- **ReachLateReply** (`NoLateReplyTaken`): the client sends two probes of 3 (one
  in flight, the set holds one), the server answers, the window ends and the
  client sends its probe of 2 in the same call, and the reply of 3 arrives
  while it probes 2: it takes 3 (rocketnet_client_process_probe: a candidate
  at an index at or below probe_index).

## Findings in the code

Behaviour the model now matches, reported and not changed:

- (a) The allowance is spent before the open, so a probe forged at the client's
  address (on-path, or spoofing its source) spends the update's one reply; an
  attacker who does so every update withholds every genuine probe, and the
  client fails -14 (or -4) on a path that carries a candidate. Spending it
  only on a probe that opens would let a forger make the server do one failed
  open per probe instead of one per update.
- (b) The server's handshake deadline (`handshake_deadline_seconds`, default 5 s)
  is not refreshed by probes; a client at a low send rate or a long round
  trip whose walk (each window `max(4 / send_rate, 3 x RTT)`) outlasts it loses
  its mapping mid-walk.
- (c) The silence timeout (the token's `timeout_seconds`) runs while probing and
  ends the attempt -4 before -14 when the walk outlasts it with nothing
  answered.
- (d) A response to a connected mapping is ignored; a client whose first
  keep-alive was lost relies on the slot's own keep-alives.

## Records

The 15 records in `RUNS.tsv` were measured on hetzner1 with the pinned tlacheck
`0c97d595` before the model was made to match the code; they are stale against
the model in this commit (tools/check_tla_records.py names them) and are owed
to the next Linux run (`tlacheck run --group rnpathprobe`, which refuses to run
on macOS). TLC 2.19 run directly on studio.local, 4 workers (Late: 2):

| Case | Result | Generated | Distinct | Seconds |
| --- | --- | --- | --- | --- |
| `MCRNPathProbe.cfg` | pass | 287,266 | 44,881 | 9 |
| `MCRNPathProbeServerList.cfg` | pass | 287,562 | 45,137 | 9 |
| `MCRNPathProbeOneEntry.cfg` | pass | 52,696 | 10,426 | 1 |
| `MCRNPathProbeLate.cfg` | pass | 20,571,272 | 1,739,718 | 32 |
| `MCRNPathProbeReachLateReply.cfg` | `NoLateReplyTaken` | 8,277 | 2,616 | 0 |
| `MCRNPathProbeBrokenToday.cfg` | `CarriedBothWays` | 468 | 308 | 0 |
| `MCRNPathProbeBrokenTodayHang.cfg` | `NoHang` | 52,696 | 10,426 | 1 |
| `MCRNPathProbeBrokenTodayConnects.cfg` | `FailsWhenNoneFits` | 52,696 | 10,426 | 1 |
| `MCRNPathProbeBrokenUnproven.cfg` | `ReplyOnlyProven` | 164 | 160 | 0 |
| `MCRNPathProbeBrokenLarger.cfg` | `ReplyNoLarger` | 2,749 | 1,127 | 0 |
| `MCRNPathProbeBrokenOffList.cfg` | `ReplyOwnList` | 2,749 | 1,127 | 0 |
| `MCRNPathProbeBrokenNoAllowance.cfg` | `OneReplyPerUpdate` | 1,460 | 707 | 0 |
| `MCRNPathProbeBrokenSlotCeiling.cfg` | `SlotAgrees` | 9,568 | 3,253 | 0 |
| `MCRNPathProbeBrokenNoExpire.cfg` | `Ends` | 34,624 | 5,781 | 0 |
| `MCRNPathProbeBrokenSkipLast.cfg` | `ConnectsWhenFits` | 130,921 | 20,996 | 1 |

## Trace binding

Layer `path-probe` of the trace check (README-TraceCheck.md):
`tests/tla_trace_path_probe.cpp` drives the real client and server protocols
over a path of its own (a capacity each way, loss, reorder, replay, forged
probes from the client's address and from an address with no mapping) and
`TraceRNPathProbe.tla` checks each record is the model step it names, with
`Timed` = FALSE and the code's own constants (the candidate sets, the ceiling,
the window of 4). The walk's scenario is the seed modulo 4: a path that
carries every size (with deliveries to the client held back, so a late reply
is taken), one whose capacities are two lower candidates, one that carries
none (-14), and any path ended by the silence timeout (-4). The data actions
are untraced (no payload is sent) and the ghosts are left to the model. Reach
predicates `LateReplyTaken` and `ConnectedBelowFirst`.

The plant `ROCKETNET_PATH_PROBE_PLANT` stops rocketnet_server_process_probe
reading the per-update allowance. On seeds 1, 4 and 8 of 1 to 8 a second
probe of one update is answered (a replay, or the client's next probe), and
TLC rejects the planted trace exactly at the first record where it differs
from the clean one (seed 1: record 46; seed 4: 22; seed 8: 13; at seed 8's
record 6 the plant's extra reply to a replayed copy was the same packet as a
reply already in flight, so the trace first changed at 13). The clean traces
of seeds 1 to 8 are accepted whole (401 records each); together seeds 1 to 4
take every traced action and reach both predicates.
