# Deterministic simulation testing

RocketNet's deterministic simulation tier drives clients and a server with a
seeded in-process wire. The seed controls loss, duplication, delay, jitter and
reordering; mocked monotonic time controls endpoint updates. A failure must
print the seed and a command that reproduces the same schedule.

The harness checks properties that also appear in the TLA+ models:

- accepted payload bytes equal the sent message bytes;
- reliable messages arrive exactly once and ordered streams arrive in order;
- unreliable messages arrive at most once;
- stale packets from an earlier connection cannot enter a reused slot;
- acknowledgements name authenticated opened packets;
- buffers, queues, reassembly slots and receipt rings remain within their
  configured bounds;
- an endpoint (the server or a client) that can make fair progress does not remain wedged.

Every simulated endpoint has explicit state: connection generation, local clock,
next frame time, restart state, sent-message identities and received-message
identities. Fault decisions are derived from the run seed and stable event
keys, so adding an unrelated client does not perturb another endpoint's schedule.
Trace hashes cover the seed, ordered fault decisions and observable endpoint
events.

Short deterministic cases belong in the ordinary functional tier. Larger seed
sweeps and multi-hour campaigns are planned hardening gates. They must use the
same invariants and replay format; a larger run does not weaken a failed short
case.

## Fault kinds

`--faults` takes `all`, `none` or a comma list of `loss`, `dup` and
`jitter`; `all` is the default. The names `skew`, `crash-client`,
`crash-server`, `reorder` and `delay` are removed: no code ever applied
them, and implementing them under `all` would change every existing seed's
trace; clock faults are the `clock-back` schedule below, and restarts stay the
client churn the core already draws. Every name `--faults` still takes is
applied by the world; a fault the world cannot apply is not named.

## Fault schedules

`./bin/dst --seeds a-b --schedule <name>` adds one fault schedule to every
seed of the range. A schedule draws only from its own substream,
`schedule.<name>`, after every other draw of the world, so it never moves
another fault's draws, and a run without `--schedule` draws and traces exactly
as before (seeds 1-20 keep `trace=15cb721e6f6af79f`). Each schedule fires
inside the fault window (`--steps`), from a tenth of the way in, so the
quiet window still judges liveness. A schedule's events are appended to the
trace; the PASS or FAIL summary line ends `schedule=<name> fired=<seeds>`,
the number of seeds on which it fired. Every existing check judges every
schedule; a failure prints its seed and the replay command, which carries
`--schedule <name>`:

    ./bin/dst --seeds <seed>-<seed> --shape full --schedule <name>

- `clock-back`: draws an endpoint (the server or one client), a step, and a
  step back of 10 ms (one frame) to 5 s in whole milliseconds. From that
  step on, the endpoint's transport runs on its own clock, the shared step
  clock plus its offset, so its update is handed a time below the one before.
  The wire stays on the shared clock. The fault is applied below the message
  object, whose update clamps a backwards game time
  (`rocketnet_clamp_update_time`), so the protocol sees it. The skew check's
  bound for that endpoint is the 5 s maximum (plus 1 ms of rounding); every
  other endpoint keeps 0.45 s.
- `seq-gap`: draws a side (the client's sends or the server's), a client, a
  step and a gap of 1025 to 8192. At the first step from there on where the
  client holds a session on both ends, that side's next session send sequence
  jumps by the gap, past the 1024-packet replay history, so the receiver sees
  its highest opened sequence rise by more than 1024 at once.
- `seq-wrap-31` and `seq-wrap-32`: draw nothing. Every session starts 256
  below 2^31 or 2^32 through `ROCKETNET_DEV_SESSION_SEQUENCE_START`
  (`sdk/rocketnet_test_seams.h`), which the library reads once per process
  at the first create: `./bin/dst` sets it before `rocketnet_init`, and
  refuses to run if the library holds another value. A session fires the
  schedule when its send sequence passes the boundary. The unit suite does
  not set it, so there these two only replay.
- `burst`: draws a client, a window of 100 to 300 steps and its start. While
  the window is open the client reads nothing, and the server holds what the
  client sends (up to 1024 packets; past that a packet is lost, as a full wire
  loses it). The window's last step releases both: the client receives every
  packet the wire kept for it, and the server receives every held packet, in
  one step. The delivery ring of one endpoint holds what one step delivers,
  so it is as wide as the window the schedule holds (1024 payloads).
- `dup-storm`: draws a window of 50 to 200 steps and its start. Every packet
  sent in the window is also delivered 2 to 5 more times, each copy with its
  own drawn delay of 0 to 50 ms over the base latency, so the copies arrive
  reordered. The copies take no draw from the wire's own stream. The storm is
  the duplicate fault at its worst, so it explains the refusals the duplicate
  fault explains, and no others.

`test_dst_schedule_same_seed_same_trace`, `test_dst_clock_back_reaches_endpoints`
and `test_dst_seq_gap_exceeds_history` (`tests/test_unit_dst_core.cpp`, run by
`./bin/test`) pin that each schedule replays and fires.
`test_dst_burst_releases_a_batch_in_one_step` pins that the burst releases a
hundred packets in one step, and `test_dst_dup_storm_duplicates_payloads` pins
that the storm's copies reach the receivers as a payload taken twice, with
the duplicate fault off.
