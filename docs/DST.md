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
- buffers, queues and receipt rings remain within their
  configured bounds;
- an endpoint (the server or a client) that can make fair progress does not remain wedged.

Every simulated endpoint has explicit state: connection generation, local clock,
next frame time, restart state, sent-message identities and received-message
identities. Fault decisions are derived from the run seed and stable event
keys, so adding an unrelated client does not perturb another endpoint's schedule.
Trace hashes cover the seed, ordered fault decisions and observable endpoint
events.

Short deterministic cases belong in the ordinary functional tier. Larger seed
sweeps and multi-hour campaigns use the same invariants and replay format; a
larger run does not weaken a failed short case.

## The command line

`./bin/dst` is built Release with the tools (`make`; on Windows
`cmake --build build --config Release --target dst`, giving
`bin/Release/dst.exe`). `--seeds a-b` runs every seed in the range and ends with

    PASS seeds=a-b runs=N trace=<hash>

where the trace folds each seed's trace hash in order. A failing seed prints

    FAIL seed=<n> step=<s> check=<name> what=<detail>
    replay: ./bin/dst --seeds <n>-<n> --shape full

and the range ends with `FAIL seeds=a-b runs=N failed=F` and exit code 1. The
replay line names every non-default flag of the run (`--shape`, `--clients`,
`--steps`, `--faults`), so running it reproduces the same schedule.

## The same trace on every OS

The same seeds and options print the same trace on Linux, macOS and Windows,
so a seed that fails on one OS replays on the others. The reference is

    ./bin/dst --seeds 1-50
    PASS seeds=1-50 runs=50 trace=510908e5051b7f68

measured equal, Release, at executable source 968673ea (then-current main c456b6fc
merged into the nightly carry) on all three OSes:

    Linux   (x64, gcc)     PASS seeds=1-50 runs=50 trace=510908e5051b7f68
    macOS   (arm64, clang) PASS seeds=1-50 runs=50 trace=510908e5051b7f68
    Windows (x64, MSVC)    PASS seeds=1-50 runs=50 trace=510908e5051b7f68

The trace was f5b51aa640524001 at main 010c8167 (also equal on the three OSes);
the library changes landed between the two moved it. A change that moves this
trace changes what a run does; it updates the reference here and
`DST_REFERENCE_TRACE` in `.github/workflows/nightly-dst.yml` together, from
three equal PASS lines.

## The nightly

`.github/workflows/nightly-dst.yml` runs every night (and by
`workflow_dispatch`) on the fleet's Linux, macOS and Windows runners, for each
ref in `ROCKETNET_NIGHTLY_REFS`, with `fail-fast: false`. Each leg builds
Release `dst` and `test`, runs the nineteen `test_dst_` unit cases by `--run`,
runs `--seeds 1-50` and fails unless its PASS line carries the reference trace,
then runs one named short step per fault schedule (`clock-back`, `seq-gap`,
`seq-wrap-31`, `seq-wrap-32`, `burst` and `dup-storm`, each over seeds 1-50)
printing a `PASS schedule=<name> seeds=1-50 fired=<count>` line; a schedule
that fires zero times fails its step. Each leg then sweeps a longer range
sized to about twenty minutes on that OS (Linux 10950 seeds, macOS 13950,
Windows 5950). The reference range 1-50 stays fixed; only the sweep start
rolls: it is derived from the run number (run number 1 starts at seed 51, each
later run starts one range-length later), so consecutive nights cover
different ranges of the same length, and the log prints the range with its
replay line. `.github/workflows/nightly-channel-seeds.yml` rolls the same
way over ranges of 2000 channel seeds (run number 1 starts at seed 1). A FAIL
line or a trace mismatch fails the leg; the log holds the replay line.

The Linux and macOS sweep lengths were refreshed in Release at executable
source 968673ea: Linux 10950 seeds PASS in 1442.42 s and macOS 13950 seeds
PASS in 1153.10 s. Windows (x64, MSVC, Release) was run at executable source
809ad504 (then-current main 6fa04f88 merged into the same carry), holding the
Windows host exclusively: the build had 0 warnings, the fifteen cases passed,
`--seeds 1-50` printed `trace=510908e5051b7f68` in 10.90 s, and 5950 seeds
passed in three chunks with no FAIL line, 1273.25 s in all (0.21 s/seed):

    PASS seeds=51-2050 runs=2000 trace=3398a85cc19ce52f    432.65 s
    PASS seeds=2051-4050 runs=2000 trace=a89d7fe691fe9216  421.72 s
    PASS seeds=4051-6000 runs=1950 trace=b9551ab88eec7cbd  418.88 s

Each leg's `timeout-minutes: 50` is about
twice the sweep plus the build.

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
  other endpoint keeps 0.45 s. A step back past the server's three-second
  cookie lifetime makes the rewound client answer a cookie challenge after
  the shared clock has passed its expiry, so the refusal check explains
  `CONNECTION_REQUEST_COOKIE_EXPIRED` only on a run where a rewound clock
  was actually observed; on any other run it stays unexplained.
- `seq-gap`: draws a side (the client's sends or the server's), a client, a
  step and a gap of `ROCKETNET_HISTORY_PACKETS + 1` to
  `2 * ROCKETNET_HISTORY_PACKETS` (4097 to 8192 by default). At the first step
  from there on where the client holds a session on both ends, that side's
  next session send sequence jumps by the gap, past the whole replay history,
  so the receiver sees its highest opened sequence rise by more than
  `ROCKETNET_HISTORY_PACKETS` at once.
- `seq-wrap-31` and `seq-wrap-32`: draw nothing. Every session starts 256
  below 2^31 or 2^32 through `ROCKETNET_DEV_SESSION_SEQUENCE_START`
  (`sdk/rocketnet_test_seams.h`), which the library reads once per process
  at the first create: `./bin/dst` sets it before `rocketnet_init`, and
  refuses to run if the library holds another value. A session fires the
  schedule when its send sequence passes the boundary. The unit suite does
  not set it, so there these two only replay.
- `burst`: draws a client, a window of 100 to 300 steps and its start. The
  window opens at the first step from there on where the client holds a
  session on both ends, and ends before a drawn churn replaces it. In that one
  opening update the client sends 300 small unreliable messages (each one
  packet) and the server sends the same to it; unreliable sends are not
  retransmitted, so the window carries the drawn batch and not a resend storm.
  While the window is open the client reads nothing and both directions are
  quiet apart from the batch; the server holds what the client sends (up to
  1024 packets; past that a packet is lost, as a full wire loses it). The
  window's last step releases both: the client receives every packet the wire
  kept for it, and the server receives every held packet, in one step. The
  report counts the packets the burst endpoint sent in its one update apart
  from the packets it received in its one release step, and a run without the
  schedule moves neither. The delivery ring of one endpoint holds what one
  step delivers, so it is as wide as the window the schedule holds (1024
  payloads).
- `dup-storm`: draws a window of 50 to 200 steps and its start. Every packet
  sent in the window is also delivered 2 to 5 more times, each copy with its
  own drawn delay of 0 to 50 ms over the base latency, so the copies arrive
  reordered. The copies take no draw from the wire's own stream. The storm is
  the duplicate fault at its worst, so it explains the refusals the duplicate
  fault explains, and no others.

`test_dst_schedule_same_seed_same_trace`, `test_dst_clock_back_reaches_endpoints`
and `test_dst_seq_gap_exceeds_history` (`tests/test_unit_dst_core.cpp`, run by
`./bin/test`) pin that each schedule replays and fires.
`test_dst_burst_releases_a_batch_in_one_step` pins that the burst sends at
least two hundred packets in one update and receives at least two hundred in
one release step, counted apart, and `test_dst_dup_storm_duplicates_payloads`
pins that the storm's copies reach the receivers as a payload taken twice,
with the duplicate fault off.
