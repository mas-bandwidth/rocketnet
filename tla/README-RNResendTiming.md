# RNResendTiming: the resend timing of a reliable record

`tla/RNResendTiming.tla` is the timing of one reliable record from its first
send to its ack or to `reliable_timeout`: when the sender resends it, and how
many sends fit before the connection gives up on it. `tla/RNWindow.tla`
keeps the RTO abstract (a record is resent "after the RTO"); this module is
the schedule alone. The code is `rn_window_ack_resend_delay`,
`rn_window_ack_resend` and `rn_window_ack_check_timeout`
(`sdk/rocketnet_window_ack.h` and `.cpp`), the resend walk of
`rn_ordered_write` (`sdk/rocketnet_channel.cpp`), `rn_message_ordered_due`
(`sdk/rocketnet_message_server.cpp`) and the attempts rule of
`rocketnet_message_validate_config` (`sdk/rocketnet_config.cpp`); each cites
the module. The model was written before the code.

`tla/MCRNResendTiming.tla` and `tla/MCRNResendTiming.cfg` are the small
instance. `tla/MCRNResendTimingBrokenBackoff.cfg` is the reversed witness.
The cases are rows of `tla/CASES.tsv` (group `rnresendtiming`) with measured
records in `tla/RUNS.tsv`.

## The rule

- **The RTO.** `rto = clamp( 1.25·srtt + 2·rttvar, rto_min, rto_max )` over
  this layer's own RTT samples (`ROCKETNET_MESSAGE_RTO_SCALE_PERCENT` 125, a
  constant); `rto_initial` before the first sample. Defaults: `rto_initial`
  100 ms, `rto_min` 10 ms, `rto_max` 250 ms.
- **The delay.** Every resend of every record waits
  `min( rto, resend_interval )`, `resend_interval` the constant
  `ROCKETNET_MESSAGE_RESEND_INTERVAL_MS` (100 ms). There
  is no back-off: the delay does not grow with the number of sends.
- **The timeout.** A record unacked for `reliable_timeout` (10 s) ends the
  connection with `RELIABLE_TIMEOUT`, and nothing more is sent.
- **The validation.** A config is refused when
  `reliable_timeout_ms < ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS × max( ROCKETNET_MESSAGE_RESEND_INTERVAL_MS, rto_initial_ms )`, with
  `ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS = 4`.

## What the module covers

Variables: `t` (ticks since the first send), `lastSend`, `rto`, `backoff`,
`attempts` (saturating at `MinAttempts`) and `acked`.

Actions:

- `Resend`: the record is due (`t >= lastSend + Min(rto, Interval) · backoff`, unacked, before the timeout) and is sent again; `backoff`
  doubles up to `MaxBackoff`.
- `Sample(r)`: the ack of any other packet, one RTT sample, moves `rto` to
  any `r` in `RtoMin..RtoMax` and resets `backoff` to 1.
- `Ack`: the record's own ack arrives (or never does: every send may be
  lost).
- `Tick`: the update ends with nothing due and time moves one tick. Time
  never passes an update with a resend due.
- `Done`: acked, or the timeout fired; a stutter, so the end is not a
  deadlock.

Invariants:

- `TypeOK`.
- `ConfigAccepted`: the instance passes the validation rule.
- `AttemptsBeforeTimeout`: a record still unacked at the timeout was sent at
  least `MinAttempts` times.
- `ResendGapBounded`: while the connection is up, an unacked record waits at
  most `Interval · MaxBackoff` ticks between sends (under the rule, one
  `resend_interval`).

## Abstractions

- **The estimator.** `rto` after a sample is any value in
  `RtoMin..RtoMax`, so the property holds for every sequence of RTT samples,
  not one estimator trace. The scale and the `2·rttvar` term are inside that
  range.
- **One record.** Every record waits the same delay, so the resend FIFO stays
  in due order and records do not interact in the timing; the walk itself is
  `RNWindow`'s `WriteOldest`.
- **Time.** One tick is 100 ms in the committed instances, and an update
  runs every tick. The code's updates are finer (60 Hz), which only makes
  the measured gaps closer to the delay; the 10 ms `rto_min` rounds up to one
  tick.
- **Loss.** The adversary chooses when, if ever, the record's ack arrives;
  that is any loss pattern, including every send lost.
- **The send budget.** A resend the budget cannot pay waits a later update
  (`RNSendBudget`); that is outside this module, which assumes the update
  writes each due record.

## The small instance

`MCRNResendTiming.cfg`: `Interval = 1`, `RtoInitial = 1`, `RtoMin = 1`,
`RtoMax = 3`, `Timeout = 100`, `MaxBackoff = 1`, `MinAttempts = 4` (the
defaults in 100 ms ticks). TLC explores every sample and ack schedule in
about 1,200 distinct states and every invariant holds.

## Reversed witnesses

| Configuration | Rule removed | Expected failure |
|---|---|---|
| `MCRNResendTimingBrokenBackoff.cfg` | the first version's schedule: `RtoInitial = 10` (1000 ms), `RtoMax = 20` (2000 ms), no cap (`Interval = RtoMax`), back-off doubling to 8 | `AttemptsBeforeTimeout`, exit 12: a sample at the first tick sets `rto` to 1.5 s, the record is sent at 0, 1.5 and 4.5 s, and the next send is due at 10.5 s, after the timeout: three sends where four are owed. Any `rto` above 10/7 s does the same. |

The witness passes `ConfigAccepted` (`10 s >= 4 × 2 s`), so validation
alone does not keep the property; the delay without back-off does. The unit
test `resend_timing_witness_replays` (`test_unit_resend_timing.cpp`)
replays this schedule against the code (the same config, one RTT sample of
500 ms, which drives the first version's `rto` to exactly 1.5 s, and no ack
of the record after) and asserts at least four sends
before `reliable_timeout`; at the rule's 100 ms cap the code sends the
record every 100 ms until the timeout.
