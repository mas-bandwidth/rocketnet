# RNResendTiming: the resend timing of reliable records

`tla/RNResendTiming.tla` is the timing of reliable records from their first
send to their ack or to `reliable_timeout`: when the sender resends each, how
many sends fit before the connection gives up, and that a chunk of a block,
paced by the resend delay, never holds back a small record paced by the 100 ms cap. `tla/RNWindow.tla`
keeps the resend delay abstract (a record is resent "after the resend delay"); this module is
the schedule alone. The code is `rn_window_ack_resend_delay`,
`rn_window_ack_chunk_resend_delay`, `rn_window_ack_resend` and
`rn_window_ack_check_timeout`
(`sdk/rocketnet_window_ack.h` and `.cpp`), the resend walk of
`rn_ordered_write` (`sdk/rocketnet_channel.cpp`), `rn_message_ordered_due`
(`sdk/rocketnet_message_server.cpp`) and the attempts rule of
`rocketnet_message_validate_config` (`sdk/rocketnet_config.cpp`); each cites
the module. The model was written before the code.

`tla/MCRNResendTiming.tla` and `tla/MCRNResendTiming.cfg` are the small
instance. `tla/MCRNResendTimingBrokenBackoff.cfg` and
`tla/MCRNResendTimingBrokenOneFifo.cfg` are the reversed witnesses.
The cases are rows of `tla/CASES.tsv` (group `rnresendtiming`) with measured
records in `tla/RUNS.tsv`.

## The rule

- **The resend delay.** `resend_delay = clamp( 1.25·srtt + 2·rttvar, resend_delay_min, resend_delay_max )` over
  this layer's own RTT samples (`ROCKETNET_MESSAGE_RESEND_DELAY_SCALE_PERCENT` 125, a
  constant); `resend_delay_initial` before the first sample. Defaults: `resend_delay_initial`
  100 ms, `resend_delay_min` 10 ms, `resend_delay_max` 250 ms.
- **The delay.** Every resend of a single-packet record waits
  `min( resend_delay, resend_interval )`, `resend_interval` the constant
  `ROCKETNET_MESSAGE_RESEND_INTERVAL_MS` (100 ms). Every resend of a chunk of
  a block (a record whose send descriptor has more than one fragment) waits
  `min( resend_delay, reliable_timeout / 4 )`, the resend delay alone at the defaults. There
  is no back-off: the delay does not grow with the number of sends.
- **The FIFOs.** Each delay class has its own resend FIFO, ordered by last
  send and walked from its head while due; the small FIFO is walked first.
- **The timeout.** A record unacked for `reliable_timeout` (10 s) ends the
  connection with `RELIABLE_TIMEOUT`, and nothing more is sent.
- **The validation.** A config is refused when
  `reliable_message_timeout_ms < ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS × max( ROCKETNET_MESSAGE_RESEND_INTERVAL_MS, message_resend_delay_initial_ms )`, with
  `ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS = 4`.

## What the module covers

Two records, `"s"` (single-packet) and `"c"` (a chunk), both first sent at
tick 0 in either order. Variables: `t` (ticks since the first send), and per
record `lastSend`, `backoff`, `attempts` (saturating at `MinAttempts`) and
`acked`; `resend_delay`; `head`, the record at the head of the one FIFO of the broken
shape; and `phase`, the step of the update.

Two switches are definitions with the one-record defaults, overridden in a
configuration (`Chunk <- ChunkInFlight`, `OneFifo <- OneFifoTwoDelays`):
`Chunk` puts the chunk in flight (otherwise it starts acked and the module is
the one-record timing of the first cut), and `OneFifo` puts both classes in
one FIFO (the broken shape).

Actions:

- `Sample(x)`: in the update's first step, the ack of any other packet, one
  RTT sample, moves `resend_delay` to any `x` in `ResendDelayMin..ResendDelayMax` and resets the
  back-off to 1.
- `Ack(r)`: in the update's first step, record `r`'s own ack arrives (or
  never does: every send may be lost).
- `Walk`: the resend walk. Record `r` is due when
  `t >= lastSend[r] + Min(resend_delay, Cap(r)) · backoff[r]`, `Cap("s") = Interval`
  and `Cap("c") = ChunkCap = Timeout \div MinAttempts`. With one FIFO per
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
  most `Min(ResendDelayMax, Cap(r)) · MaxBackoff` ticks between sends (under the
  rule, one `resend_interval` for a small record and one `resend_delay_max` for a
  chunk).
- `SmallNeverHeldByChunk`: no update ends with the small record due; a due
  small record is sent in the update it falls due, whatever the chunk beside
  it does.

## Abstractions

- **The estimator.** `resend_delay` after a sample is any value in
  `ResendDelayMin..ResendDelayMax`, so the property holds for every sequence of RTT samples,
  not one estimator trace. The scale and the `2·rttvar` term are inside that
  range.
- **Two records.** Within a class every record waits the same delay, so each
  class's FIFO stays in due order and records of one class do not interact in
  the timing; one record per class is the interaction between classes. The
  walk itself is `RNWindow`'s `WriteOldest`.
- **Time.** One tick is 100 ms in the committed instances, and an update
  runs every tick. The code's updates are finer (60 Hz), which only makes
  the measured gaps closer to the delay; the 10 ms `resend_delay_min` rounds up to one
  tick.
- **Loss.** The adversary chooses when, if ever, the record's ack arrives;
  that is any loss pattern, including every send lost.
- **The send budget.** A resend the budget cannot pay waits a later update
  (`RNSendBudget`); that is outside this module, which assumes the update
  writes each due record.

## The small instance

`MCRNResendTiming.cfg`: `Interval = 1`, `ResendDelayInitial = 1`, `ResendDelayMin = 1`,
`ResendDelayMax = 3`, `Timeout = 100`, `MaxBackoff = 1`, `MinAttempts = 4` (the
defaults in 100 ms ticks), the chunk in flight and one FIFO per class. TLC
explores every sample and ack schedule and both send orders in about 173,000
distinct states and every invariant holds.

## Reversed witnesses

| Configuration | Rule removed | Expected failure |
|---|---|---|
| `MCRNResendTimingBrokenOneFifo.cfg` | one FIFO per class: both classes in one FIFO ordered by last send, walked from its head and stopped at the first record not due, at the defaults | `SmallNeverHeldByChunk`, exit 12: the chunk is sent first, then the small record, both at 0; a sample moves `resend_delay` to 200 ms; at 100 ms the small record is due (`min(resend_delay, resend_interval)`) and the chunk at the head is not (`resend_delay`), so the walk stops and the update ends with the small record unsent. Any `resend_delay` above 100 ms does the same. |
| `MCRNResendTimingBrokenBackoff.cfg` | the first version's schedule: `ResendDelayInitial = 10` (1000 ms), `ResendDelayMax = 20` (2000 ms), no cap (`Interval = ResendDelayMax`), back-off doubling to 8 | `AttemptsBeforeTimeout`, exit 12: a sample at the first tick sets `resend_delay` to 1.5 s, the record is sent at 0, 1.5 and 4.5 s, and the next send is due at 10.5 s, after the timeout: three sends where four are owed. Any `resend_delay` above 10/7 s does the same. |

The witness passes `ConfigAccepted` (`10 s >= 4 × 2 s`), so validation
alone does not keep the property; the delay without back-off does. The unit
test `test_resend_timing_witness_replays` (`test_unit_resend_timing.cpp`)
replays this schedule against the code (the same config, one RTT sample of
500 ms, which drives the first version's `resend_delay` to exactly 1.5 s, and no ack
of the record after) and asserts at least four sends
before `reliable_timeout`; at the rule's 100 ms cap the code sends the
record every 100 ms until the timeout.

The unit test `test_small_record_behind_a_chunk_not_held`
(`test_unit_resend_timing.cpp`) replays the one-FIFO witness against the
code: a block's chunks sent first, a small record 10 ms after, an RTT sample
of 200 ms (resend_delay 250 ms); at 111 ms the small record is resent and the chunks
are not, and the chunks go at 251 ms. Built against the one-FIFO walk it
fails, resending the chunks at 100 ms.

`MCRNResendTimingChunkLate.cfg` sends the chunk first at tick 98, late in its
block's first timeout, as when the send budget paces a block out over longer
than the timeout (1 MiB at the default 1000 kbps). Each record is timed from
its own first send (MESSAGES m9.3), so it passes. The reversed witness
`MCRNResendTimingBrokenBlockClock.cfg` times the chunk from its block's first
send at tick 0, as the code did before `send_first_sends`: at tick 100 the
chunk has two sends where four are owed, and `AttemptsBeforeTimeout` fails.
The unit test `test_ordered_chunk_timeout_runs_from_its_own_first_send`
(`test_unit_channel_ordered.cpp`) replays it against the code: a chunk first
sent 12 s after its block's first chunk is not timed out until 10 s after its
own first send, and an unsent oldest number has no clock.
