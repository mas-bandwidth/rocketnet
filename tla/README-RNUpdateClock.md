# RNUpdateClock

The update clock: the caller's time, clamped once per update by the client
transport, the server transport and the message object
(`rocketnet_advance_update_time` in `sdk/rocketnet_client_transport.cpp`,
declared beside `rocketnet_clamp_update_time` in `sdk/rocketnet_internal.h`).

A time that steps backwards is API misuse (`sdk/rocketnet.h`), counted and
asserted. Before the update clock, the clamp returned the last good time and
kept returning it until the caller's clock passed that value again, so a
backwards step of D stood the owner still for D: no keep-alive or send-rate
gate fired, reliable resends waited, and the dead-connection timeout never
fired. A server whose caller corrected its clock back by an hour held every
vanished client's slot for an hour plus the timeout.

## The state

| Variable | Code |
|---|---|
| `raw` | the caller's time at the last update: `raw_time` beside `time` in the client transport, the server transport and `rn_message_object_t` |
| `good` | the clamped time every deadline reads: `time` in each owner |
| `misuses` | `rocketnet_api_misuse_count` |

Create seeds both `raw` and `good` with the create time (`Init`).

## The actions

- `ForwardStep`: a finite time at or above `raw`. `good` advances by the raw
  step. While no backwards step has happened `raw = good` (`SeedIdentity`),
  and the code returns the caller's time unchanged, bit for bit; it computes
  `last_good + ( time - raw )` only after a backwards step.
- `BackwardStep`: a finite time below `raw`. `good` holds, `raw` records the
  new time, one misuse is counted and the assertion fires, as before.
- `NonFiniteSample`: NaN or an infinity. Neither value changes; one misuse.

The message object runs the clock first and hands its resumed time to its
transport, so the two clocks resume together and the transport never sees a
backwards step. A rejected first sample of a message object that has not yet
started its clock leaves both values as create set them.

## The properties

- `NeverDecreases`: the clamped time never decreases.
- `ResumesExactly`: after any backwards step, each forward raw step of d
  advances the clamped time by exactly d.
- `SeedIdentity`: until the first backwards step the clamped time is the
  caller's time.

## The cases

| Config | Expected |
|---|---|
| `MCRNUpdateClock.cfg` | pass: `TypeOK`, `NeverDecreases`, `ResumesExactly`, `SeedIdentity` |
| `MCRNUpdateClockBrokenStall.cfg` | `ResumesExactly` violated |

The instance: the caller's time in 0..6 from a seed of 3, steps of up to 2
either way, five updates. The terminal states at the update bound are
expected (`ignore-terminal`).

## The witness trace, replayed by hand

`MCRNUpdateClockBrokenStall.cfg` (`BrokenStall = TRUE`, the clamp before the
update clock) fails `ResumesExactly` in three states:

1. Seed: `raw = 3`, `good = 3`.
2. `BackwardStep`: `raw = 2`, `good = 3`, one misuse.
3. `ForwardStep` of 1: `raw = 3`; the old clamp returns `good = 3`, an
   advance of 0 for a raw step of 1.

In the code: the old three-argument clamp, `rocketnet_clamp_update_time( 3.0,
2.0 )` returns 3.0 and counts one misuse, then `rocketnet_clamp_update_time(
3.0, 3.0 )` returns 3.0. The update clock, `rocketnet_advance_update_time(
3.0, &raw, 2.0 )` returns 3.0, records `raw = 2.0` and counts one misuse, then
`rocketnet_advance_update_time( 3.0, &raw, 3.0 )` returns 4.0. The unit case
`test_message_object_clock_resumes_after_backwards_step` pins the same
sequence on a message server object at 1000.0, 1001.0, 990.0, 990.5: the
object and its transport stand at 1001.5, where the old clamp left them at
1001.0.
