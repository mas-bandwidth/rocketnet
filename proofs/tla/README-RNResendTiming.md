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
- **The FIFOs.** Each delay class has its own resend FIFO, ordered by last
  send and walked from its head while due; the small FIFO is walked first.
- **The timeout.** A record unacked for `reliable_timeout` (10 s) ends the
  connection with `RELIABLE_TIMEOUT`, and nothing more is sent.
- **The validation.** A config is refused when
  `reliable_message_timeout_ms < ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS × message_resend_delay_initial_ms`, with
  `ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS = 4`.

## What the module covers

Two records, `"s"` (single-packet) and `"c"` (a chunk), both first sent at
tick 0 in either order. Variables: `t` (ticks since the first send), and per
record `lastSend`, `backoff`, `attempts` (saturating at `MinAttempts`) and
`acked`; `resend_delay`; `head`, the record at the head of the one FIFO of the broken
shape; `phase`, the step of the update; and `sampled` and `seeded`, whether
an own ack or the transport's round trip has given the estimator a sample
(the code's `has_rtt` is their disjunction).

Two switches are definitions with the one-record defaults, overridden in a
configuration (`Chunk <- ChunkInFlight`, `OneFifo <- OneFifoTwoDelays`):
`Chunk` puts the chunk in flight (otherwise it starts acked and the module is
the one-record timing of the first cut), and `OneFifo` puts both classes in
one FIFO (the broken shape).

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
- `Walk`: the resend walk. Record `r` is due when
  `t >= lastSend[r] + Min(rto, Cap(r)) · backoff[r]`, `Cap("s") = RtoMax` (no
  cap) and `Cap("c") = ChunkCap = Timeout \div MinAttempts`. With one FIFO per
  class every due record is sent; with `OneFifo` the walk goes from `head`
  and stops at the first live record that is not due. A sent record moves to
  its FIFO's tail and its back-off doubles up to `MaxBackoff`.
- `Tick`: the update ends and time moves one tick.
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
  record is sent in the update it falls due, whatever the other class does.
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
  (`RNSendBudget`); that is outside this module, which assumes the update
  writes each due record.

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

## Reversed witnesses

| Configuration | Rule removed | Expected failure |
|---|---|---|
| `MCRNResendTimingBrokenOneFifo.cfg` | one FIFO per class: both classes in one FIFO ordered by last send, walked from its head and stopped at the first record not due; `RtoMax = 8` (4 s), so the resend delay can exceed the chunk cap of 5 (2.5 s) | `NoDueRecordHeld`, exit 12: the small record is sent first, then the chunk, both at 0; a sample moves `rto` above 5; at tick 5 the chunk is due (its capped delay) and the small record at the head is not, so the walk stops and the update ends with the chunk unsent. |
| `MCRNResendTimingBrokenOneFifoGap.cfg` | the same | `ResendGapBounded`, exit 12: the held chunk waits past `Min(RtoMax, ChunkCap)`. |
| `MCRNResendTimingBrokenBackoff.cfg` | the first version's schedule (100 ms ticks): `RtoInitial = 10` (1000 ms), `RtoMax = 20` (2000 ms), back-off doubling to 8 | `AttemptsBeforeTimeout`, exit 12: a sample at the first tick sets `rto` to 1.5 s, the record is sent at 0, 1.5 and 4.5 s, and the next send is due at 10.5 s, after the timeout: three sends where four are owed. |
| `MCRNResendTimingBrokenUnseeded.cfg` | the seed: `MCRNResendTimingSeeded.cfg` with the transport's round trip ignored (`SeedRule = FALSE`) | `SeededGapBounded`, exit 12: the record sent at 0 is lost and no ack is sampled; the transport reports a round trip at 200 ms, but the resend delay stays the 2 s initial delay, and at 600 ms the record has waited 400 ms past the report where the seeded delay owes its resend by 500 ms. Any seed below the initial delay does the same. |

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
