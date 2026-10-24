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
    PASS seeds=1-50 runs=50 trace=f5b51aa640524001

measured equal at the commit that added the nightly on amd-2 (Linux x64, gcc),
mini-m5-1 (macOS arm64, Apple clang) and captain (Windows x64, MSVC). A change
that moves this trace changes what a run does; it updates the reference here
and `DST_REFERENCE_TRACE` in `.github/workflows/nightly-dst.yml` together, from
three equal PASS lines.

## The nightly

`.github/workflows/nightly-dst.yml` runs every night (and by
`workflow_dispatch`) on the fleet's Linux, macOS and Windows runners, for each
ref in `ROCKETNET_NIGHTLY_REFS`, with `fail-fast: false`. Each leg builds
Release `dst` and `test`, runs the fifteen `test_dst_` unit cases by `--run`,
runs `--seeds 1-50` and fails unless its PASS line carries the reference trace,
then sweeps a longer range sized to about twenty minutes on that OS (Linux
51-11000, macOS 51-14000, Windows 51-6000). A FAIL line or a trace
mismatch fails the leg; the log holds the replay line.

The ranges come from Release measurements on each OS's fleet member: Linux
(amd-2) ran seeds 1-500 in 55 s and 51-11000 in 1297 s; macOS (mini-m5-1) ran
1-500 in 43 s and 51-14000 in 1261 s; Windows (captain) ran 1-50 in 10 s, or
0.2 s a seed, so about 1200 s is 6000 seeds, 51-6000. The Windows figure is the
1-50 measurement alone; the first nightly's sweep time line (`seeds=... took
<n>s`) is the check on it. Each leg's `timeout-minutes: 50` is about twice the
sweep plus the build.
