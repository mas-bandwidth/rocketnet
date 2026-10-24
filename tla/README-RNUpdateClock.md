# The update-time clamp resumes after a backwards step

`RNUpdateClock.tla` models the one clamp every clock owner shares:
`rocketnet_clamp_update_time_resume` (`sdk/rocketnet_client_transport.cpp`),
called by the client transport and server transport updates, the message
object's update, and the client create. The model keeps the caller's raw
sample (`raw`) beside the clamped time (`good`) and whether a backwards sample
has happened (`seen`).

`Forward(d)` is the common path before any backwards step: the clamped time is
the raw one and the returned time is exactly the caller's. After a backwards
sample, `Forward(d)` advances the clamped time by exactly `d`, until a raw
sample reaches the clamped time; then the offset is zero again and the
caller's own value is returned. `Backward(d)`
holds the clamped time, records the raw sample and sets `seen`; the misuse
count and the debug assertion are outside the model. `NonFinite` changes
neither time.

`OffsetZeroUntilStep` says the offset is exactly zero before the first
backwards sample, so a transport never computes `last_good + ( time - raw )`
on the common path. `Monotone` says the clamped time never decreases.
`ResumesExactly` says every forward raw sample after a backwards one advances
the clamped time by exactly `d`, or reaches the clamped time and returns the
caller's own value.

The control `MCRNUpdateClock.cfg` (Start 2, Limit 6, GoodLimit 12, MaxStep 2)
passes every invariant and both properties. The reversed witness
`MCRNUpdateClockBrokenStall.cfg` sets `Defect = "stall"`, the rule before this
fix: the clamp returns the last good time until the raw time passes it again.
It violates `ResumesExactly`.

A trace on the small instance: raw and good start at 2; a forward sample of 1
takes both to 3; a backwards sample of 1 records raw 2 and holds good 3; a
forward sample of 1 takes raw to 3 and good to 4. Under the stall rule the
same forward sample leaves good at 3, so the difference from the raw sample
never closes.

The code check by hand: create the object at `MO_T0` (1000.0), update at
1000.0 and 1001.0, step back to 990.0 (clamped to 1001.0, one counted misuse),
then update at 990.5. The clamped time is 1001.5 at the fix and was 1001.0
under the rule before it; `test_message_object_clock_resumes_after_backwards_step`
pins that. The existing three-argument `rocketnet_clamp_update_time` and
`test_update_time_clamp` are unchanged. The model abstracts floating-point
representation: the code's common path returns the caller's own value before
the first backwards sample, while the modelled times are exact naturals.
