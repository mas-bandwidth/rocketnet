# RNResendTiming: the resend timing of reliable records

`proofs/tla/RNResendTiming.tla` is the timing of reliable records from their first
send to their ack or to `reliable_timeout`: when the sender resends each, how
many sends fit before the connection gives up, and that a record of one delay
class never holds back a due record of the other. `proofs/tla/RNWindow.tla`
keeps the resend delay abstract (a record is resent "after the resend delay"); this module is
the schedule alone. The code is `rn_window_ack_resend_delay`,
`rn_window_ack_chunk_resend_delay` and `rn_window_ack_check_timeout`
(`sdk/rocketnet_window_ack.h` and `.cpp`), the resend walk of
`rn_ordered_write` (`rn_ordered_walk`, `sdk/rocketnet_channel.cpp`),
`rn_endpoint_ordered_due` (`sdk/rocketnet_endpoint.cpp`) and the attempts rule of
`rocketnet_message_validate_config` (`sdk/rocketnet_config.cpp`); each cites
the module.

`proofs/tla/MCRNResendTiming.tla` and `proofs/tla/MCRNResendTiming.cfg` are the small
instance; `proofs/tla/MCRNResendTimingSeeded.cfg` is the seeded start at the
current defaults. `proofs/tla/MCRNResendTimingBroken*.cfg` are the reversed witnesses.
The cases are rows of `proofs/tla/CASES.tsv` (group `rnresendtiming`) with measured
records in `proofs/tla/RUNS.tsv`.

## The rule

- **The resend delay.** `resend_delay = clamp( 2·srtt + 4·rttvar + 30 ms, resend_delay_min, resend_delay_max )`
  over this layer's own RTT samples (`rn_window_ack_compute_resend_delay`);
  `resend_delay_initial` before the first sample. Defaults: initial 2000 ms,
  minimum 0, maximum 2000 ms (`sdk/rocketnet_internal.h`).
- **The seed.** While the layer has no sample of its own, the first update in which
  the transport reports a measured round trip for the connection takes it as the
  first sample (`rn_window_ack_seed_rtt`, after that update's acks and before its
  resend walk); a later own sample moves the estimate as any later sample does.
  A transport with no measurement leaves `resend_delay_initial` in force.
- **The delay.** Every resend of a single-packet record waits `resend_delay`
  (`rn_window_ack_resend_delay`). Every resend of a chunk of a block (a record
  whose send descriptor has more than one fragment) waits
  `min( resend_delay, reliable_timeout / 4 )` (`rn_window_ack_chunk_resend_delay`),
  the resend delay alone at the defaults. There is no back-off.
- **The FIFOs.** Each delay class has its own resend FIFO: its numbers not
  on the wire (never sent, or refused), then its numbers on the wire by last
  send from the class's sent mark (`ack->resend_sent`, `ack->chunk_sent`).
  `rn_ordered_write` walks the numbers on the wire of the small class, then
  of the chunk class, each while due, then the numbers not on the wire of
  each class from its head, and stops when the plan holds a packet. A due
  retransmit is planned ahead of every never-sent number, so a block whose
  first pass is paced by the send budget does not hold its lost chunks.
- **The timeout.** A record unacked for `reliable_timeout` (10 s) ends the
  connection with `RELIABLE_TIMEOUT`, and nothing more is sent.
- **The validation.** A config is refused when
  `reliable_message_timeout_ms < ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS × message_resend_delay_initial_ms`, with
  `ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS = 4`.

## What the module covers

Two records, `"s"` (single-packet) and `"c"` (a chunk), both first sent at
tick 0 in either order. Variables: `t` (ticks since the first send), and per
record `lastSend`, `firstSend`, `backoff`, `attempts` (saturating at
`MinAttempts`), `acked` and `refused`; `resend_delay`; `head`, the record at the head of the one FIFO of the broken
shape; `phase`, the step of the update; and `sampled` and `seeded`, whether
an own ack or the transport's round trip has given the estimator a sample
(the code's `has_rtt` is their disjunction).

Two switches are definitions with the one-record defaults, overridden in a
configuration (`Chunk <- ChunkInFlight`, `OneFifo <- OneFifoTwoDelays`):
`Chunk` puts the chunk in flight (otherwise it starts acked and the module is
the one-record timing of the first cut), and `OneFifo` puts both classes in
one FIFO (the broken shape).

The paced walk has four more definitions, overridden in its configurations:
`Room`, the `rn_ordered_write` calls of one packet each an update may make
(0, the default, keeps `Walk`, which sends every due record; the endpoint
makes one call per packet, `rn_endpoint.cpp`); `Pending`, the never-sent
chunks of the chunk's block behind it in number order (`pending` counts
them down); `Small`, whether the small record is in flight; and
`UnsentFirst`, the walk from each FIFO's head with the never-sent numbers
first (the broken shape, the code before the sent marks). `calls` counts
the update's calls. Two more cover a refused resend: `RefuseResends`, the
transport free to refuse a call's packet that carries a record on the wire,
and `RefusedOffWire`, the refused resend walked with the never-sent numbers
(the broken shape, the code before `rn_resend_fifo_push_mark`).

Actions:

- `Sample(x)`: in the update's first step, the ack of any other packet, one
  RTT sample, moves `rto` (the resend delay) to any `x` in `RtoMin..RtoMax` and resets the
  back-off to 1.
- `Seed`: in the update's first step, owed once `t >= SeedAt` while neither
  `sampled` nor `seeded` holds (and `SeedRule`), the transport's round trip
  taken as the first sample: `resend_delay` becomes `SeedRto`. `Walk` is not
  enabled while the seed is owed, as the code takes it before the walk.
- `Ack(r)`: in the update's first step, record `r`'s own ack arrives (or
  never does: every send may be lost).
- `Walk`: the resend walk with room for every due record. Record `r` is due when
  `t >= lastSend[r] + Min(rto, Cap(r)) · backoff[r]`, `Cap("s") = RtoMax` (no
  cap) and `Cap("c") = ChunkCap = Timeout \div MinAttempts`. With one FIFO per
  class every due record is sent; with `OneFifo` the walk goes from `head`
  and stops at the first live record that is not due. A sent record moves to
  its FIFO's tail and its back-off doubles up to `MaxBackoff`.
- `WalkRefused(F)`: the walk of `Walk`, but `finish_packet` refuses the
  packet carrying the records of `F`, each never on the wire and inside
  `RefuseFor` ticks of its first offer (`rn_window_ack_packet_failed`). A
  refused record keeps `lastSend = NeverSent` (`RN_SEND_NUMBER_NEVER_SENT`),
  records no `firstSend` (`send_first_sends`), so its timeout has no clock,
  and goes back to the head of its FIFO; `refused` marks it for
  `NoDueRecordHeld`. `RefuseFor` is 0 (no refusals) except in the refusal
  cases. A refused resend is `WriteRefused`, in the paced walk.
- `WriteOne(x)`: with `Room > 0`, one call: the walk plans in its order
  (`Planned`) up to the first chunk item, as a chunk fills the call's one
  packet, and the builder writes one planned item, a retransmit (a record on
  the wire) before ordered progress. `"u"` is the first pending chunk's first
  send; any other item is a send of that record.
- `WriteRefused(x)`: with `RefuseResends`, one call whose packet carries
  `x`, a record on the wire, is refused (`rn_window_ack_packet_failed`).
  `x` keeps `firstSend` (its timeout clock) and `attempts`, takes `lastSend =
  DueNow` (the code's `-(resend_delay_max + 2)` after
  `rn_packet_builder_packet_failed`) and goes back on the wire as its class's
  mark (`rn_resend_fifo_push_mark`), so it is due at once and a retransmit;
  `refused` marks it and the call is spent.
- `EndWrites`: the update's writes end when its `Room` calls are spent or a
  call plans nothing (`rn_ordered_write` returns 0).
- `Tick`: the update ends and time moves one tick; `refused` is cleared.
- `Done`: every record acked, or the timeout fired; a stutter, so the end is
  not a deadlock.

Invariants:

- `TypeOK`.
- `ConfigAccepted`: the instance passes the validation rule.
- `AttemptsBeforeTimeout`: a record of either class still unacked at the
  timeout was sent at least `MinAttempts` times.
- `ResendGapBounded`: while the connection is up, an unacked record waits at
  most `Min(RtoMax, Cap(r)) · MaxBackoff` ticks between sends.
- `NoDueRecordHeld`: no update ends with a record of either class due; a due
  record is sent in the update it falls due, whatever the other class does,
  except one whose packet this update offered and the transport
  refused. That exception is the code's behaviour, not a loosening:
  `rn_window_ack_packet_failed` leaves every refused record due at once (a
  never-sent number keeps `RN_SEND_NUMBER_NEVER_SENT`, a resend gets the
  due-now sentinel), so it waits for the next walk or call; no sentinel
  value can remove it. `refused` is set only for records a walk or call
  offered and is cleared by every `Tick`, so a due record not offered still
  fails.
- `RetryPlanned`: in the paced walk, a resend the transport refused is among
  the items the next call of the update may write first (`Picks`): planned
  as a retransmit, ahead of every number not on the wire. The next update's
  walk plans it the same way, as a due number on the wire.

What the invariants claim. In each write call the due numbers on the wire
are planned first, the small class before the chunk class, each class oldest
last send first, ahead of every number not on the wire. A due number is held
only while the update's packets (the send budget, `packets_per_update`, the
transport) go to numbers ahead of it in that order, or its packet is
refused. `NoDueRecordHeld`, `ResendGapBounded` and `AttemptsBeforeTimeout`
are checked for one lost chunk and one small record with a packet for each
in every update (`Walk`, or `Room = 2`); they are not claims about two due
numbers and one packet (`MCRNResendTimingPacedSmallRoomOne.cfg`, below, is
the recorded expected failure), about a budget that admits no packet in an
update, or about refused resends. For a refused resend the claim is
`RetryPlanned`.
- `SeededGapBounded`: from `SeedAt` until the layer's own first sample, an
  unacked record waits at most `Min(SeedRto, Cap(r))` ticks past the later of
  its last send and `SeedAt`: a record lost before the transport measured the
  path is resent by `SeedAt + SeedRto`, not by its last send plus
  `resend_delay_initial`.

## Abstractions

- **The estimator.** `rto` after a sample is any value in `RtoMin..RtoMax`,
  so the property holds for every sequence of RTT samples, not one estimator
  trace.
- **Two records.** Within a class every record waits the same delay, so each
  class's FIFO stays in due order and records of one class do not interact in
  the timing; one record per class is the interaction between classes. The
  walk itself is `RNWindow`'s `WriteOldest`.
- **Time.** One tick is 500 ms in the instances at the defaults (100 ms in
  the back-off witness), and an update runs every tick. The code's updates are
  finer (60 Hz), which only makes the measured gaps closer to the delay; the
  resend delay's floor (30 ms above `2·srtt`) rounds up to one tick.
- **Loss.** The adversary chooses when, if ever, the record's ack arrives;
  that is any loss pattern, including every send lost.
- **The send budget.** A resend the budget cannot pay waits a later update
  (`RNSendBudget`); that is outside this module. `Walk` assumes the update
  writes each due record; the paced walk takes the budget as `Room`
  packets an update, a block's first pass that outlasts the timeout.
- **The pending chunks.** Only their count is kept: each is timed from its
  own first send, as the chunk is, so the chunk stands for any of them once
  sent; what the module checks is that their first sends never hold the
  chunk's resend.

## The small instance

`MCRNResendTiming.cfg`: `RtoInitial = 4`, `RtoMin = 1`, `RtoMax = 4`,
`Timeout = 20`, `MaxBackoff = 1`, `MinAttempts = 4` (the defaults in 500 ms
ticks), the chunk in flight and one FIFO per class; every invariant holds.
`RUNS.tsv` has the measured state counts.

`MCRNResendTimingSeeded.cfg` is the seeded start at the current defaults in
100 ms ticks: `RtoInitial = RtoMax = 20` (2 s), `RtoMin = 1`, `Timeout = 100`,
and the transport's round trip first reported at tick 2 (200 ms, a keep-alive's
ack) giving a 300 ms delay (`SeedRto = 3`). One record: the seed sets the one
estimate both classes read, and a chunk beside it at a 2 s `RtoMax` runs past
the 110-second budget. Every invariant holds, `SeededGapBounded` among them.
The unit test `test_resend_seeded_from_transport_rtt`
(`test_unit_resend_seed.cpp`) replays it against the code on both ends: the
CONFIG record sent at 0 and never acked, the transport reporting 50 ms from
200 ms, the record resent at the first update past 330 ms and every 330 ms
after; built without the seed it waits the 2 s initial delay.

`MCRNResendTimingPaced.cfg` is the paced walk with one packet an update
(`Room = 1`) for the one record on the wire, the chunk sent at 0 with
`2 · Timeout + 2` never-sent chunks behind it, and no small record. It is not
the product's default, which is 256 calls an update of one packet each
(`packets_per_update`), paced by the send budget; `Room` is the packets the
budget admits in an update. `MCRNResendTimingPacedSmall.cfg`
adds the small record and two packets an update, so a due small record and a
due chunk can share an update. Both pass `AttemptsBeforeTimeout`,
`ResendGapBounded` and `NoDueRecordHeld`.

`MCRNResendTimingPacedSmallRoomOne.cfg` is `MCRNResendTimingPacedSmall.cfg`
at one packet an update, a recorded expected failure, not a bug: at tick 0
the one call writes a pending chunk; at tick 1 a sample sets `rto` to 1, both
records are due, the one call writes one of them, and the update ends with
the other due: `NoDueRecordHeld` fails, exit 12. The code holds the second
due number for the next update in the same way; the invariant is a claim for
a packet per due record.

`MCRNResendTimingPacedRefused.cfg` is `MCRNResendTimingPacedSmall.cfg` with
`RefuseResends`: any call whose packet carries a record on the wire may be
refused, in any update, and two pending chunks (`PendingShort`; the full
`2 · Timeout + 2` run past the TLC budget with refusals). It checks `TypeOK` and `RetryPlanned`; the three
timing invariants are not claims about refused resends (a resend refused
until the timeout is never sent). The unit tests
`test_resend_refused_mark_is_retried_first` and
`test_resend_refused_only_number_on_the_wire`
(`test_unit_channel_ordered.cpp`) replay it against the code: the refused
resend is the class's mark, keeps its timeout clock, and the next one-packet
call resends it ahead of the due numbers behind it.

The unit tests
`test_ordered_lost_chunk_resent_ahead_of_unsent_chunks` (and its `_defaults`
and `_800_kbps` cases, `test_unit_channel_ordered.cpp`) replay them against
the code: a block paced by the budget with one payload a write, its second
chunk lost, is resent within one packet's budget and one update of falling
due, first sends stay in number order, and the walk visits at most four
numbers a call.

## Reversed witnesses

| Configuration | Rule removed | Expected failure |
|---|---|---|
| `MCRNResendTimingBrokenOneFifo.cfg` | one FIFO per class: both classes in one FIFO ordered by last send, walked from its head and stopped at the first record not due; `RtoMax = 8` (4 s), so the resend delay can exceed the chunk cap of 5 (2.5 s) | `NoDueRecordHeld`, exit 12: the small record is sent first, then the chunk, both at 0; a sample moves `rto` above 5; at tick 5 the chunk is due (its capped delay) and the small record at the head is not, so the walk stops and the update ends with the chunk unsent. |
| `MCRNResendTimingBrokenOneFifoGap.cfg` | the same | `ResendGapBounded`, exit 12: the held chunk waits past `Min(RtoMax, ChunkCap)`. |
| `MCRNResendTimingBrokenBackoff.cfg` | the first version's schedule (100 ms ticks): `RtoInitial = 10` (1000 ms), `RtoMax = 20` (2000 ms), back-off doubling to 8 | `AttemptsBeforeTimeout`, exit 12: a sample at the first tick sets `rto` to 1.5 s, the record is sent at 0, 1.5 and 4.5 s, and the next send is due at 10.5 s, after the timeout: three sends where four are owed. |
| `MCRNResendTimingBrokenUnseeded.cfg` | the seed: `MCRNResendTimingSeeded.cfg` with the transport's round trip ignored (`SeedRule = FALSE`) | `SeededGapBounded`, exit 12: the record sent at 0 is lost and no ack is sampled; the transport reports a round trip at 200 ms, but the resend delay stays the 2 s initial delay, and at 600 ms the record has waited 400 ms past the report where the seeded delay owes its resend by 500 ms. Any seed below the initial delay does the same. |
| `MCRNResendTimingBrokenUnsentFirst.cfg` | the sent marks: `MCRNResendTimingPaced.cfg` walked from each FIFO's head, never-sent numbers first (`UnsentFirst = TRUE`) | `AttemptsBeforeTimeout`, exit 12: the chunk sent at 0 is never acked; each update's one packet goes to the next never-sent chunk (42 down to 22), the chunk falls due at tick 4 and is never planned, and at tick 20 it has one send where four are owed. |
| `MCRNResendTimingBrokenUnsentFirstHeld.cfg` | the same, on `MCRNResendTimingPacedSmall.cfg` | `NoDueRecordHeld`, exit 12: both records are sent at 0 and two pending chunks fill tick 0; at tick 1 a sample sets `rto` to 1, both records are due, and the two calls write the small record and a never-sent chunk; the update ends with the chunk due and unsent. |
| `MCRNResendTimingBrokenRefusedOffWire.cfg` | the refused resend on the wire: `MCRNResendTimingPacedRefused.cfg` with a refused resend walked among the never-sent numbers (`RefusedOffWire = TRUE`, the code before `rn_resend_fifo_push_mark`) | `RetryPlanned`, exit 12: both records are sent at 0 and the two pending chunks fill tick 0; at tick 1 a sample sets `rto` to 1 and both records are due; the first call's packet carries the small record and is refused, and it is put back among the never-sent numbers, so the second call plans the chunk, the one retransmit, which fills its packet, and the refused small record is not planned. Against the code before the fix: `push_head` put the refused small record at the head of its FIFO, ahead of the mark, so the walk of the numbers on the wire reached the due chunk first and stopped at a full plan. |

The back-off witness passes `ConfigAccepted` (`10 s >= 4 × 1 s`), so
validation alone does not keep the property; the delay without back-off does. The unit
test `test_resend_timing_witness_replays` (`test_unit_resend_timing.cpp`)
replays this schedule against the code (one RTT sample, no ack of the record
after) and asserts at least four sends before `reliable_timeout`.

The unit test `test_chunk_behind_a_small_record_not_held`
(`test_unit_resend_timing.cpp`) replays the one-FIFO witness against the
code: `reliable_message_timeout_ms` 4000 (a chunk cap of 1 s), a 200 ms RTT
sample (a resend delay of 1230 ms), a small record sent before a block; at
1011 ms the chunks are resent and the small record is not, and the small record
goes at 1231 ms. Walked as one FIFO, the small record at the head would stop
the walk and hold the chunks.

`MCRNResendTimingChunkLate.cfg` sends the chunk first at tick 18 (9 s), late
in its block's first timeout, as when the send budget paces a block out over
longer than the timeout. Each record is timed from
its own first send (MESSAGES m9.3), so it passes. The reversed witness
`MCRNResendTimingBrokenBlockClock.cfg` times the chunk from its block's first
send at tick 0, as the code did before `send_first_sends`: at tick 20 the
chunk has one send where four are owed, and `AttemptsBeforeTimeout` fails.
The unit test `test_ordered_chunk_timeout_runs_from_its_own_first_send`
(`test_unit_channel_ordered.cpp`) replays it against the code: a chunk first
sent 12 s after its block's first chunk is not timed out until 10 s after its
own first send, and an unsent oldest number has no clock.

`MCRNResendTimingRefused.cfg` lets the record's first send be refused for
18 ticks (9 s) after it is offered (one record: with the chunk beside it the
run passes the 110-second budget). A refused first send stays never sent, so
the timeout runs from the one real first send and every invariant holds. The
reversed witness `MCRNResendTimingBrokenRefusedClock.cfg` is the code before
the fix (`StampRefused`): the refusal at tick 0 records the first send and
stamps the due-now sentinel, the record is refused to tick 7 and then sent at
8, 12 and 16, and at tick 20 it has three sends where four are owed:
`AttemptsBeforeTimeout` fails, exit 12. The unit test
`test_ordered_refused_first_send_starts_no_clock`
(`test_unit_channel_ordered.cpp`) replays it against the code: a refused first
send leaves `last_send` never sent and no clock, no timeout past
`reliable_timeout`, and the real send starts the clock at its own time.

## Revision 1.1: the timeout off, probe pacing and the silence filter

Standard rules: MESSAGES.md m9.2 (no sample across an acknowledgement
silence) and m9.3 (`reliable_message_timeout_ms` 0 disables the timeout, the
default; probe pacing). The ticks are 500 ms, so `Probe = 2` is the one
second of probe pacing and `SilenceTs = {3, 4, 5}` are the silences T of 1.5,
2 and 2.5 s the timeout claim is quantified over. The switches are
definitions replaced with `<-` in each configuration: `TimeoutOn`
(`TimeoutOff` is the default rule), `TimeoutWhenOff`, `ProbeMode`
(`"rule"`, `"off"`, `"norelease"`, `"holdsu"`), `Third` (a third small record
`s2`, so a number other than the oldest unacked `U` is on the wire), `Filter`
and `FilterRule`, `TrackQuiet`. The ghosts `sinceProgress`, `quiet`,
`sinceAck`, `sinceEnd` and `badSample` are the code's `last_progress`,
the acknowledgement silence, `last_ack`, `silence_end` and a sample the rule
forbids.

| Property | Kind | Claim |
|---|---|---|
| `OffMeansNoStallEnd` | invariant | with the timeout off no record ends the connection |
| `NoEndBeforeT` | invariant | for every T in `SilenceTs`, a timeout fires only after T of acknowledgement silence |
| `ProbeOnlyU` | action | while probing, no on-wire number other than `U` is resent |
| `NoDueRecordHeld` | invariant | now also allows a number held by probe pacing (`Held`) |
| `RetryPlanned` | invariant | now excludes a held number |
| `NoSampleAcrossSilence` | invariant | no sample is taken from a packet sent before `silence_end - srtt` |
| `EveryReliableAcked` | temporal | under `LiveSpec` (strong fairness on acks of sends after `GoodFrom`) every record is acked |

`AttemptsBeforeTimeout` and `ResendGapBounded` are claims about the timeout
and hold only when it is configured (`TimeoutOn`).

| Case | Spec | Exit | Distinct |
|---|---|---:|---:|
| `MCRNResendTimingOff.cfg` (three records, chunk in flight, `ProbeOnlyU`) | Spec | 0 | 313438 |
| `MCRNResendTimingPacedOff.cfg` | Spec | 0 | 31218 |
| `MCRNResendTimingOffLive.cfg` (`EveryReliableAcked`) | LiveSpec | 0 | 1777 |
| `MCRNResendTimingOffQuiet.cfg` (`NoEndBeforeT`) | Spec | 0 | 2062 |
| `MCRNResendTimingFilter.cfg` (`NoSampleAcrossSilence`) | Spec | 0 | 3938 |
| `MCRNResendTimingBrokenResendAllWhileStalled.cfg` | `ProbeOnlyU` | 13 | 330 |
| `MCRNResendTimingBrokenProbeNoRelease.cfg` | `NoDueRecordHeld` | 12 | 492 |
| `MCRNResendTimingBrokenProbeHoldsU.cfg` | `EveryReliableAcked` | 13 | 1822 |
| `MCRNResendTimingBrokenTimeoutInsideT.cfg` | `NoEndBeforeT` | 12 | 1800 |
| `MCRNResendTimingBrokenTimeoutWhenOff.cfg` | `OffMeansNoStallEnd` | 12 | 753 |
| `MCRNResendTimingBrokenSampleAcrossSilence.cfg` | `NoSampleAcrossSilence` | 12 | 77 |

The existing cases' counts are unchanged. Each witness trace was read by
hand against `sdk/rocketnet_window_ack.cpp` and `sdk/rocketnet_channel.cpp`:

- **ResendAllWhileStalled** (`ProbeMode = "off"`): `s`, `c` and `s2` are
  sent at 0 and nothing is acked; at tick 2 the stall has lasted one second
  and the walk resends `c` and `s2` beside `U = s`. The code's
  `rn_window_ack_probing` is true there and `rn_ordered_walk` skips every
  on-wire number but `U`.
- **ProbeNoRelease** (`"norelease"`): probing holds `s2`; `Ack(s)` acks a
  number, which in the code sets `last_progress = now` and ends probing, but
  the witness keeps `s2` held after it falls due.
- **ProbeHoldsU** (`"holdsu"`): `U` is held with the rest, so no send after
  `GoodFrom` is ever acked and `U` stays unacked. The code always resends
  `U` when due.
- **TimeoutWhenOff** and **TimeoutInsideT** (`TimeoutWhenOff`): with the
  timeout off the record still times out 6 s (12 ticks) after its first send,
  with an ack sample at tick 11, so the acknowledgement silence is one tick.
  This is the code before revision 1.1: `rn_window_ack_check_timeout` and the
  chunk cap ran with no `reliable_timeout > 0` guard.
- **SampleAcrossSilence** (`FilterRule` off): an ack after two ticks of
  silence samples a packet sent before `silence_end - srtt`. The code's
  `on_ack` keeps the first sample, then takes one only when
  `send_time >= silence_end - srtt`.

## Revision 1.2: a silence needs an acknowledgement owed

Standard rule: MESSAGES.md m9.2. Revision 1.1 ended a silence on any ack
that came two ticks (1 s) after the previous one. A sender with one message
every two seconds has nothing in flight between messages, so each ack ended a
silence and its own sample, above srtt, was refused: the estimate could only
fall. Now `EndsSilence` (`OwedSilence`) also needs `U` on the wire and its
first send two ticks or more ago, the code's
`now - max(last_ack, oldest_first_send) >= RN_PROBE_SECONDS` with
`has_oldest` in `rn_window_ack_on_acks`. `SilenceOnGap` is the revision 1.1
rule. `PriorAck` starts the instance with the previous message acknowledged
0 to 2 ticks before the record's first send and its sample taken, a sparse
sender; `SrttUnderTick` makes srtt shorter than one tick, so an ack one tick
after its send is above it. The ghosts `sinceOwedEnd` and `badRefusal` follow
the owed rule beside the configured one and record a refusal it would not
make.

| Property | Kind | Claim |
|---|---|---|
| `RefuseOnlyAfterOwedSilence` | invariant | a sample is refused only after a silence in which an ack was owed |

| Case | Spec | Exit | Distinct |
|---|---|---:|---:|
| `MCRNResendTimingFilter.cfg` (now also `RefuseOnlyAfterOwedSilence`) | Spec | 0 | 3938 |
| `MCRNResendTimingFilterSparse.cfg` (`PriorAck`, `SrttUnderTick`) | Spec | 0 | 3911 |
| `MCRNResendTimingBrokenSilenceOnGap.cfg` | `RefuseOnlyAfterOwedSilence` | 12 | 15 |

Every other case of the module was re-run with it and kept its distinct
count. The witness, read by hand against `sdk/rocketnet_window_ack.cpp`:
the previous ack was two ticks ago (`sinceAck = 2`), the small record is
first sent at tick 0 and nothing else is in flight; at tick 0 an ack of a
packet one tick old arrives. Under `SilenceOnGap`, the v1.1.0 code,
`now - last_ack >= 1 s` sets `silence_end = now`, and the packet, sent before
`silence_end - srtt`, gives no sample. Under the rule `U`'s first send is now,
no ack was owed for a second, no silence ends, and the packet is sampled.
The unit tests `test_rtt_filter_follows_a_sparse_sender` and
`test_rtt_filter_idle_connection_is_not_a_silence`
(`test_unit_resend_timing.cpp`) replay it against the code, and
`test_rtt_filter_skips_the_samples_an_outage_delays` keeps the outage case.

## Revision 1.2: the due test agrees with the walk

Standard rules: MESSAGES.md m9.3 (while probing only `U` is resent) and m1.8
item 2 (no connection changes another connection's send budget). The code
asks the endpoint whether a frame is due (`rn_endpoint_fifo_due`) before it
starts one; a frame started and then left empty is aborted, and on every
server transport an aborted frame keeps its slot of the update's shared send
capacity. So the due test must report work exactly when the walk writes some.

`ReportedDue` is the due test: for each class, the head of its FIFO
(never-sent first, then oldest last send), and when that head is on the wire
and held by probe pacing, `U` in its place (`rn_window_ack_held`,
`rn_window_ack_probe_slot`). `DueMeansWritten` states that, in a walk with no
room left by earlier calls (`Room = 0`), the due test is true exactly when the
walk sends a record. `DueRule` switches the held head's replacement; `DueRuleOff`
is the code before the fix, which read the head's own time.

| Case | Property | Exit | Distinct |
|---|---|---:|---:|
| `MCRNResendTiming.cfg`, `MCRNResendTimingOff.cfg`, `MCRNResendTimingOffLive.cfg`, `MCRNResendTimingFilter.cfg` | `DueMeansWritten` added | 0 | unchanged |
| `MCRNResendTimingBrokenDueIgnoresHold.cfg` (`MCRNResendTimingOff.cfg` with `DueRule <- DueRuleOff`) | `DueMeansWritten` | 12 | 498 |

The witness trace, read by hand against `sdk/rocketnet_endpoint.cpp` and
`sdk/rocketnet_channel.cpp`: `s`, `c` and `s2` are sent at 0 and nothing is
acked; a sample at tick 2 sets `rto` to 2 and only `U = s` is resent at tick 2;
at tick 3 the stall has lasted past one second, `s2` and `c` are held and past
their delay, and `U` is not due. The old due test reads the held head and
reports work; the walk skips every held number and writes nothing. The unit
tests `test_endpoint_probe_hold_requests_frame_only_for_u`
(`test_unit_endpoint.cpp`) and
`test_server_send_cap_dark_neighbours_take_only_their_probes`
(`test_unit_server_send_cap_fair.cpp`) replay it against the code: the first
counts frames asked for during the hold (one exactly when `U` is due, none
aborted); the second shows three held clients taking only their probe slots of
a shared send capacity of four.

## Revision 1.3: the silence clock, and the RTT filter in its own module

Standard rules: MESSAGES.md m9.3 (with the reliable timeout off no unacknowledged
number ends the connection; only the silence timeout `T` does) and m9.2 (the
acknowledgement-silence filter).

Before this revision `NoEndBeforeT` and `OffMeansNoStallEnd` held vacuously in
the timeout-off cases: no step ended the connection, so a bound of `FALSE` in
their place also passed. The model now carries the silence clock: `quiet`
counts the ticks since the other side was last heard (`KeepAlive` resets it),
and `SilenceEnd` ends the connection once `quiet` reaches `SilenceAt` (the code:
`SilenceT`). Every sending step waits while a silence end is due, as the update
ends the connection before it writes. The properties:

| Property | Kind | Meaning |
|---|---|---|
| `OffMeansNoStallEnd` | invariant | with the reliable timeout off, every end is a silence end |
| `NoEndBeforeT` | invariant | with the reliable timeout off, no end comes before `T` ticks unheard |
| `SilenceEndsAtT` | invariant | no live connection has gone more than `T` ticks unheard |

| Case | Property | Exit |
|---|---|---:|
| `MCRNResendTimingOffQuiet.cfg` (T = 3), `MCRNResendTimingOffQuietFive.cfg` (T = 5), `MCRNResendTimingPacedOff.cfg` | all three, with `DueMeansWritten` | 0 |
| `MCRNResendTimingBrokenSilenceEarly.cfg` (the end one tick before T) | `NoEndBeforeT` | 12 |
| `MCRNResendTimingBrokenSilenceLate.cfg` (the end one tick after T) | `SilenceEndsAtT` | 12 |
| `MCRNResendTimingBrokenDueIgnoresHoldPaced.cfg` (the v1.1.0 due test on the paced walk) | `DueMeansWritten` | 12 |

`DueMeansWritten` now also covers a paced walk with room left (`Room > 0`):
the due test is true exactly when the call plans a record.

The RTT filter moved to `RNRttFilter.tla`, which models it against a path that
slows, goes dark and loses packets, with send records, a moving `srtt` and an
owed outage defined from the sender alone; the filter cases of revision 1.1 and
1.2 (`MCRNResendTimingFilter*.cfg`, `MCRNResendTimingBrokenSampleAcrossSilence.cfg`,
`MCRNResendTimingBrokenSilenceOnGap.cfg`) are replaced by its cases. `Sample`
here sets only the resend delay.

## Revision 1.4: the timeout floor, and acks before the timeout check

`ConfigAccepted` now carries the 1000 ms floor of
`rocketnet_message_validate_config_internal` (`sdk/rocketnet_config.cpp`): a
configured timeout is at least 1000 ms and at least `MinAttempts` sends of
the initial resend delay. The floor is `TimeoutFloor` ticks: 2 at the
500 ms tick, `TimeoutFloorFine` (10) at the 100 ms tick of the seeded and
back-off cases (`TimeoutFloor <- TimeoutFloorFine`). At 500 ms it never
binds; `MCRNResendTimingBelowTimeoutFloor.cfg` (100 ms tick, `Timeout = 8`,
800 ms, with `MinAttempts × RtoInitial = 8`) is a configuration the rule
without the floor accepts and the code refuses, and it violates
`TypedConfigAccepted` (`TypeOK /\ ConfigAccepted`) in the initial state.
TLC evaluates a constant-level invariant once before the search and reports
a false one as an error, not a counterexample; read in a state it is one.

The message update applies the update's received ids
(`rn_endpoint_on_acked_ids`, `sdk/rocketnet_message_server.cpp`) before
`rn_endpoint_tick` checks the timeout, so an ack processed in the update the
deadline falls in keeps the connection. Before this revision `Ack` and
`Sample` were disabled as soon as `TimedOut` held, which is the check before
the acks. They now need `AcksOpen` (not silenced, a record unacked), and
`TimedOut` gates them only in the reversed witness (`CheckBeforeAcks <-
CheckBeforeAcksOn`).

| Property | Meaning |
|---|---|
| `DeadlineUpdateTakesAcks` | at the start of the update a record's deadline falls in, before any call, when it is the only record at or past its deadline, the record (sent, unacked, not silenced) can still be acknowledged and that step leaves the connection up |

The property restates the order of the update's steps, which is all the code
fixes; it adds no timing claim.

Measured by `tlacheck run` on aws-mut-3, 2026-11-13:

| Case | Property | Exit | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|---:|
| `MCRNResendTiming.cfg` (now with `DeadlineUpdateTakesAcks`) | every invariant | 0 | 100040 | 29878 | 2.4 |
| `MCRNResendTimingPaced.cfg` (the same) | every invariant | 0 | 35516 | 13904 | 1.7 |
| `MCRNResendTimingBrokenCheckBeforeAcks.cfg` | `DeadlineUpdateTakesAcks` | 12 | 83836 | 25735 | 2.2 |
| `MCRNResendTimingBelowTimeoutFloor.cfg` | `TypedConfigAccepted` | 12 | 1 | 1 | 0.8 |

Witness trace for `MCRNResendTimingBrokenCheckBeforeAcks.cfg`, 42 states,
read by hand: the small record and the chunk are sent at 0 and resent up to
four times each, the second small record is acked; the small record is acked
at 19; at 20, the chunk's deadline (first send 0 plus `Timeout = 20`), the
update begins with the chunk sent four times and unacked, and with the check
first no ack can be taken in that update. In the code the update's acks
(`rn_endpoint_on_acked_ids`) come before `rn_endpoint_tick`, so an ack of
the chunk in that update is applied and the timeout check finds nothing
unacked.
