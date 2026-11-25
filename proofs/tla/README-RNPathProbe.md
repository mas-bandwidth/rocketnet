# RNPathProbe: the path packet size probe of the handshake

`RNPathProbe.tla` models the MTU fallback design's handshake: after the
challenge, a client whose candidate list has more than one entry probes the
path with packet type 9 (connection probe) at each candidate size in turn,
takes the first matching reply, and carries the chosen size in its connection
response; the server answers probes and assigns the slot with that size. It
was written ahead of the code (the code is being written on its own branch),
so its coverage row is `owed` and it has no trace binding yet.

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
  descending (P of each `link_mtus` entry, equal sizes collapsed); the
  server's list is its own candidate set for that family and may differ. A
  one-entry client list does no probing and sends its response at once with
  that entry, exactly as today.
- **The path.** A capacity each way, chosen at `Init` from `CapsUp` and
  `CapsDown` and fixed for the behaviour, so one run covers every path of the
  instance. A packet larger than its direction's capacity is dropped when sent,
  always. A packet that fits may be lost (`LoseUp`, `LoseDown`), at most
  `MaxLoss` times in a behaviour; packets in flight are a set, delivered in any
  order (reordering; a resend of a packet already in flight is the same
  packet).
- **The proven address.** The model starts after the challenge: the client
  holds the handshake keys and the server's mapping of its address is
  half-open. A second address `X` has no mapping and no keys; anything it sends
  fails to open (`AttackProbe`). An on-path attacker may also replay a client
  probe in flight from the client's own address (`Replay`, a copy with
  `n = 1`); a replay opens, which is why the per-update allowance exists. The
  attacker has `AttackBudget` sends in all.
- **Time.** `Tick` is one client send interval: the client may send one packet
  and the server runs at least one update, which resets the mapping's reply
  allowance. `ServerUpdate` is a further server update inside the interval.
  The probe window of candidate `idx` is an abstract timer: `W` sends of the
  candidate's probe, the client sending again only when nothing of its last
  exchange is in flight (the design's window, `max(4 send intervals, 3 x the
  challenge RTT)`, covers a round trip), and `ClientExpire` once all `W` are
  spent and nothing is in flight. The response phase has the same shape:
  `W` responses, then `ClientTimeout`, the client state -4.
- **Late replies.** With `LateExpire` a window may expire after any send with
  its packets still in flight, so a reply arrives in a later window and the
  client takes it when its size is a candidate at or below the one it is now
  probing (`MCRNPathProbeLate.cfg`, safety only; `MCRNPathProbeReachLateReply.cfg`
  shows the path is reached).
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
padding checks (a probe that fails to open stands for every refusal before the
size check), the server's half-open idle refresh and handshake deadline, the
connect token, and the per-connection payload capacities that follow from the
size.

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

`MCRNPathProbe.tla`: `Ceiling` = 3, the three-step client list `<< 3, 2, 1 >>`,
every capacity `0 .. 3` each way, `W` = 2, `MaxLoss` = 1, `AttackBudget` = 1.

| Case | Server list | Notes |
| --- | --- | --- |
| `MCRNPathProbe.cfg` | `{3, 2, 1}` | every invariant and property under `FairSpec` |
| `MCRNPathProbeServerList.cfg` | `{3, 1}` | the server lacks the client's middle size: a probe of it is refused unanswered |
| `MCRNPathProbeOneEntry.cfg` | `{3, 2, 1}` | client list `<< 3 >>`, no probing |
| `MCRNPathProbeLate.cfg` | `{3, 2, 1}` | `LateExpire`, `MaxLoss` = 2, invariants only (`Spec`) |
| `MCRNPathProbeReachLateReply.cfg` | `{3, 2, 1}` | as Late, must violate `NoLateReplyTaken` |
| each `MCRNPathProbeBroken*.cfg` | as named | must violate the one property it names |

## Counterexamples

Each read against the design by hand (2026-11-25, TLC 2.19 on hetzner1).

- **Today** (`CarriedBothWays`): capacity 0 up, 1 down. The client sends its
  response at once with the ceiling 3; the response (control size) arrives, the
  slot takes 3, the keep-alive arrives: connected at 3 on a path that carries
  no candidate either way.
- **TodayHang** (`NoHang`): capacity 0 both ways; the first response is lost,
  the second connects at 3; payload of 3 is dropped every time and nothing ends
  the connection: stuttering with no payload delivered.
- **TodayConnects** (`FailsWhenNoneFits`): capacity 0 both ways; the client
  connects at 3 where the design ends the attempt -14.
- **Unproven** (`ReplyOnlyProven`): `X` sends a probe of size 1; the server
  answers it.
- **Larger** (`ReplyNoLarger`): capacity 2 up, 0 down; both probes of 3 are
  dropped, the window expires, the probe of 2 arrives, the server answers with
  3.
- **OffList** (`ReplyOwnList`): server list `{3, 1}`, capacity 2 up; the probe
  of 2 arrives and the server answers it.
- **NoAllowance** (`OneReplyPerUpdate`): the probe of 3 is replayed in flight;
  both copies arrive in one update and both are answered.
- **SlotCeiling** (`SlotAgrees`): capacity 2 both ways; the client probes 3
  twice (dropped), then 2, takes the reply of 2, and sends a response carrying
  2; the server's slot takes 3; the client connects at 2.
- **NoExpire** (`Ends`): capacity 0 up; the client sends probes of 3 forever,
  each dropped.
- **SkipLast** (`ConnectsWhenFits`): capacity 1 both ways; the windows of 3 and
  2 expire and the walk ends at the second candidate, -14, though 1 fits.
- **ReachLateReply** (`NoLateReplyTaken`): the window of 3 expires with its
  probe in flight; the probe arrives, the reply of 3 arrives while the client
  probes 2, and the client takes 3.

## Records

`tlacheck run --group rnpathprobe` on hetzner1 (16 CPUs), one shard per case,
pinned tlacheck `0c97d595` and the pinned JAR; the 15 rows are in `RUNS.tsv`,
merged with `tlacheck merge --keep proofs/tla/RUNS.tsv`, and
`tlacheck groups --root proofs --stale` printed `[]`.

| Case | Result | Generated | Distinct | Seconds |
| --- | --- | --- | --- | --- |
| `MCRNPathProbe.cfg` | pass | 86,108 | 17,509 | 10.7 |
| `MCRNPathProbeServerList.cfg` | pass | 85,823 | 17,590 | 10.4 |
| `MCRNPathProbeOneEntry.cfg` | pass | 10,170 | 2,820 | 3.5 |
| `MCRNPathProbeLate.cfg` | pass | 2,375,799 | 327,248 | 8.9 |
| `MCRNPathProbeReachLateReply.cfg` | `NoLateReplyTaken` (12) | 1,338 | 648 | 1.3 |
| `MCRNPathProbeBrokenToday.cfg` | `CarriedBothWays` (12) | 273 | 193 | 1.3 |
| `MCRNPathProbeBrokenTodayHang.cfg` | `NoHang` (13) | 10,170 | 2,820 | 2.6 |
| `MCRNPathProbeBrokenTodayConnects.cfg` | `FailsWhenNoneFits` (13) | 10,170 | 2,820 | 2.5 |
| `MCRNPathProbeBrokenUnproven.cfg` | `ReplyOnlyProven` (12) | 86 | 85 | 1.2 |
| `MCRNPathProbeBrokenLarger.cfg` | `ReplyNoLarger` (12) | 2,244 | 947 | 1.4 |
| `MCRNPathProbeBrokenOffList.cfg` | `ReplyOwnList` (12) | 2,244 | 947 | 1.3 |
| `MCRNPathProbeBrokenNoAllowance.cfg` | `OneReplyPerUpdate` (12) | 711 | 400 | 1.3 |
| `MCRNPathProbeBrokenSlotCeiling.cfg` | `SlotAgrees` (12) | 7,026 | 2,444 | 1.5 |
| `MCRNPathProbeBrokenNoExpire.cfg` | `Ends` (13) | 10,860 | 2,366 | 2.8 |
| `MCRNPathProbeBrokenSkipLast.cfg` | `ConnectsWhenFits` (13) | 40,589 | 8,542 | 3.8 |

## Trace binding

Owed with the code. When the code lands, a walk
`tests/tla_trace_path_probe.cpp` drives the client's and the server's
protocol objects over a fake path and binds them to this module through
`TraceRNPathProbe.tla` (README-TraceCheck.md); the coverage row then turns
`current` and the code cites `proofs/tla/RNPathProbe.tla` with each action.
Until then a citation of this module from `sdk/` fails the coverage check's
owed-but-cited rule, so the code's first citation and the row's change to
`current` land together.
