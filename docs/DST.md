# Deterministic simulation testing

RocketNet's deterministic simulation tier drives clients and a server with a
seeded in-process wire. The seed controls loss, duplication, delay, jitter and
reordering; mocked monotonic time controls endpoint updates. A failure must
print the seed and a command that reproduces the same schedule.

The harness checks properties that also appear in the TLA+ models:

- accepted payload bytes equal the sent message bytes;
- reliable messages arrive exactly once and ordered streams arrive in order;
- unreliable messages arrive at most once;
- every receipt the senders read names a type 5, 6 or 7 message that
  sender sent, is that message's only outcome, is LOSS only for type 5, and
  is ACKED only for a message the receiver delivered (type 7: or holds
  behind an earlier undelivered sequence);
- every type 5 send of a live session has its outcome by its receipt
  deadline, `message_resend_delay_max_ms` after the send, plus
  `ROCKETNET_DST_RECEIPT_DUE_SLACK` for the update that writes its packet and
  the one that surfaces the outcome (`receipt_outcome_due`; a session that
  ends first owes none);
- stale packets from an earlier connection cannot enter a reused slot;
- an acknowledgement names a packet the acknowledged endpoint itself issued,
  and names it once;
- every reliable message a live session sent, in the fault window or after
  it, is delivered by the end of the run, apart from those sent within the
  in-flight slack before the end (`ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS`, 96
  steps: the wire's worst one-way delay and the send budget's longest queue);
  the slack is counted by step, so a stream that stalls before the end still
  owes its last sends, and it never excuses more than the earlier rule, the
  newest 20 sequences, did;
- an endpoint (the server or a client) that can make fair progress does not remain wedged;
- no session ends unless the run scripted its end (`no_unscheduled_end`): a
  drawn churn, an outage past the timeout, a dark client's connect the outage
  window overlapped, or, on a run that opts into it, the reliable timeout. A
  client that leaves CONNECTED or fails a connect, a slot the server frees
  while its client still holds the session, and any message-layer kick
  (`rocketnet_dst_endpoint_disconnect`, for any reason) is judged; the first
  connect ends nothing;
- both message objects' counters (the clients' summed over every session a
  client held) are conserved and explain every refusal they count, as the
  transport's do; a send the library refuses for want of room or a connection
  is one the harness saw refused (`refusals_agree`: the harness's refused
  sends equal `MESSAGE_SEND_QUEUE_FULL + MESSAGE_SEND_STORE_FULL +
  MESSAGE_SEND_NOT_CONNECTED`), and at least half the reliable sends the
  harness offered outside an outage window are accepted (`send_floor`).

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

The same seeds and options must print the same trace on Linux, macOS and
Windows, so a seed that fails on one OS replays on the others. The reference is

    ./bin/dst --seeds 1-50
    PASS seeds=1-50 runs=50 trace=7a207ea303f1dbc2

measured on Linux (x64, gcc, Release) at W2-115, the change that set it.
Whether macOS and Windows print the same value is first compared by the
nightly (`.github/workflows/nightly-dst.yml`), whose legs on those OSes hold
their PASS line to the same pin. The last reference measured equal on all
three OSes was the one at executable source 9045dc02c (the v1.1.2 send pass,
fix/v1.1.2/no-aborted-frames), Release:

    Linux   (x64, gcc)     PASS seeds=1-50 runs=50 trace=255b8ae11ec099cb
    macOS   (arm64, clang) PASS seeds=1-50 runs=50 trace=255b8ae11ec099cb
    Windows (x64, MSVC)    PASS seeds=1-50 runs=50 trace=255b8ae11ec099cb

The trace was f5b51aa640524001 at main 010c8167, 510908e5051b7f68 at 968673ea
and 17999bcc78a7254d at e385557a (each equal on the three OSes; seeding the
message resend delay from the transport's measured RTT, 93eaebd06, gave the
last); the v1.0.1 audit fixes between e385557a and 27b56da21 moved it to
cb02374fb45015e4. The v1.1.2 send pass moved it to 255b8ae11ec099cb: a client
packet its send budget refuses is now refused before a frame is started, so
it no longer takes a session sequence that is never sent (with the
write-first path limited to the server the trace is cb02374fb45015e4 again).
W2-51's handshake pacing (e04c7f459) moved it to 4872414ac933a2a7, measured
on Linux (x64, gcc, Release): the client sends the packet of a state the
cookie or the challenge just advanced it to in that update, and a send
interval counts as elapsed one microsecond early. With both reverted on the
same source the trace is 255b8ae11ec099cb again; with only the early send
reverted it is e6ad356616a575e5, with only the tolerance reverted
4feb17ac1b822961. The fixed range and the nine schedules moved with it
(`.github/workflows/nightly-dst.yml`, measured on Linux at e04c7f459).
W2-115 moved it to 7a207ea303f1dbc2, measured on Linux (x64, gcc, Release):
a standalone ack now stamps the client's send clock as any session packet
does, so the next keep-alive is due a send interval after the ack rather
than after the packet before it. The fixed range and the nine schedules
moved with it again (measured on Linux at W2-115).
A change that moves this
trace changes what a run does; it updates the reference here and
`DST_REFERENCE_TRACE` in `.github/workflows/nightly-dst.yml` together (the
per-commit Release unit job reads its pin from there). The nightly pins the
fixed range 1-2050 (`DST_FIXED_TRACE`, 11e290abdf932865) and each fault schedule
over seeds 1-50 (`DST_TRACE_<SCHEDULE>`) beside it, and compares each step's
whole PASS line; a change that moves one moves its pin with it.

The nightly holds the fixed range and every fault schedule the same way:
`DST_FIXED_TRACE` (`--seeds 1-2050`) and one `DST_TRACE_<SCHEDULE>` per
schedule (`--seeds 1-50 --schedule <name>`) in the same workflow. The current
values, like the reference, are measured on Linux at W2-115; the nightly's
macOS and Windows legs are the first comparison of them on those OSes. A step
whose PASS line differs is red with `TRACE-MISMATCH`. The nightly sweep's
trace is printed and not compared: its range rolls each night and differs per
OS.

## The nightly

`.github/workflows/nightly-dst.yml` runs every night (and by
`workflow_dispatch`) on the fleet's Linux, macOS and Windows runners, for each
ref in `ROCKETNET_NIGHTLY_REFS`, with `fail-fast: false`. A fourth leg,
Linux with Clang, builds Release with `ROCKETNET_DEV=ON` and runs only the
whole unit suite. Each DST leg (Linux with GCC, macOS, Windows) builds
Release `dst` and `test`, runs the `test_dst_` unit cases by `--run` (the
Linux and macOS legs then run the whole unit suite, with no `--run` filter;
Windows does not), runs `--seeds 1-50` and fails unless its PASS line carries the reference trace,
requires every check to pass over the fixed range `--seeds 1-2050`, then runs
one named short step per fault schedule (`clock-back`, `seq-gap`,
`seq-wrap-31`, `seq-wrap-32`, `burst`, `dup-storm`, `outage`, `outage-past` and
`send-fail`, each over seeds 1-50)
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

The nightly legs are exempt from the two-minute rule: each job's
`timeout-minutes` is 7200, the self-hosted platform's ceiling. The seed
counts above size the work; they do not cap how long a night may take.

## Outage scenarios

`./bin/dst --outage <set> [--seeds a-b]` runs a second, smaller world
(`tests/dst_outage.h`, sets in `tests/dst_outage_grid.h`): one client and one
server through the public API on the loopback transport, each direction
through the development link simulator, a test clock from 1000.0 in 10 ms
steps. Both ends offer, every 100 ms, a small message cycling the eight
reliability modes and a two-piece message cycling the reliable ones, on their
own channels, checking `can_send_message` against every start; a 64 KiB
reliable-ordered message starts 50 ms before the outage on each end. Then
one direction, the other or both go dark (loss 100) for `D` seconds, where
`T` is the token's timeout. The seed sets the outage's start within the
second (`(seed - 1) % 50` steps of 20 ms) and the simulator's draws.

A surviving row requires: both ends still connected; every accepted reliable
message delivered once, intact and in order (ordered modes) or never older
(sequenced modes), and each channel's last reliable-sequenced message
delivered; every receipt-mode reliable send resolved ACKED; each end's
`send_refused` equal to the starts it refused; and the drain after the
outage inside a bound: a keep-alive, two round trips, a step, one initial
resend delay, the backlog (every reliable byte sent from the last
acknowledged round trip before the outage, and anything older still owed)
over the budget the offers leave, and one probe round, plus two more resend
rounds on a row with residual loss (the harness's allowance, not the
standard's). A row past `T` requires both ends to end by the timeout and
nothing else: the deaf end `T` after its last packet, the other `T` after the
deaf end went quiet (a timeout sends no notice), each within a keep-alive
below and a one-way trip and three steps above.

| set | rows per seed | what |
|---|---|---|
| `grid` | 120 | `D = T - 1` for `T` 2, 10, 30, 60; round trip 1, 150, 300, 600, 900 ms; loss 0 and 2%; each direction and both: survives |
| `complement` | 36 | `D = T + 2` for `T` 2, 10, 30, 60; round trip 1, 150, 600 ms: ends by the timeout only |
| `handshake` | 162 | `D = T - 1` for `T` 10, 30, 60, round trip 150, 600, 900 ms, a 30 s token, starting as the client enters states 1, 2 and 3, half a round trip into state 3, and when the server and the client first report connected: survives; states 1 and 2 at a longer round trip or `T` may instead fail as those states allow (COOKIE_REQUEST or CONNECTION_REQUEST timed out, or the token expired), never with the server holding the client; state 2 with only the client's direction dark: the first request leaves in the update that enters the state (W2-51), the challenge comes back, and the attempt fails in state 3 with CONNECTION_RESPONSE_TIMED_OUT `T` after it; state 3 with the client's direction dark: the first response leaves in the update that enters the state, the server holds the client, and it survives |
| `opted-in` | 27 | `reliable_message_timeout_ms` at half of `T` (at least 8000): an end with RELIABLE_TIMED_OUT at that time, as in v1.0.0 |
| `refusal` | 3 | `T = 60`, `D = 59` at the default buffers: refusals begin (about 42 s into the outage on the client) and nothing accepted is lost |
| `largest` | 1 | `T = 3600`, `D = 3599`, both directions: survives |

`all` runs every set: seeds 1-50 are 17450 rows, all PASS, in 365 s on
macOS (Release). The nightly runs `--outage all --seeds 1-50` on each leg
after the schedules. Five cases in `tests/test_unit_dst_outage.cpp` run a
few rows on every commit, each under 0.2 s.

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
  (`rocketnet_update_clock_advance`), so the protocol sees it. The skew check's
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
- `outage` and `outage-past`: draw a subset of the clients (at least one left
  bright), a direction (client to server, server to client or both;
  `outage-past` is always both) and a start. For `T - 1` s (`outage`, 4 s) or
  `T + 1` s (`outage-past`, 6 s), where `T` is the token's 5 s timeout, the
  wire drops the dark clients' packets in the dark direction. The message side
  is made small (`ROCKETNET_DST_OUTAGE_*` in `tests/dst_harness.h`: a 64-number
  window, 16 messages and 12288 bytes of send queue, a 250 ms resend cap) and a
  dark client is offered 16 reliable messages a step each way, so the
  library's no-room refusal is reached in the window. The checks:
  - `no_unscheduled_end`: under `outage` no dark session ends; under
    `outage-past` a dark session's end on either end is explained by the
    outage, counted in `outage_ends`, and every judged client connects again
    by the end of the run. A dark client's connect that the window overlapped
    may fail; that end is explained, and so are the cookie expiry and salt
    mismatch refusals its handshake meets, but only on a run where the
    harness dropped a handshake packet of a client that was not connected.
  - `probe_paced`: a sender whose window is held (no acknowledgement progress
    for a second or more) resends only its oldest unacknowledged number, at
    its resend delay, but may still send numbers it never sent. Over each
    second it is held it starts at most `floor(1 / delay) + 1` frames, plus
    the first sends it made in that second, plus `floor(1 / remote_delay) + 1`
    when a one-way outage leaves its remote endpoint held and answering through the
    bright direction. Frames started and then aborted because the send budget
    could not pay for the due reliable packet are counted apart
    (`budget_aborts` in the verbose line) and not judged. Reverting the
    library's probe slot fails this check on 49 of seeds 1-50.
  - `bright_unaffected`: each bright client's largest gap between deliveries
    inside the window, per direction, is at most the same gap in a control
    run plus 25 steps (the 250 ms resend cap). The control run is the same
    seed with the same draws and no client dark (`outage_control`); it runs
    first and is not judged. A bright client's gap is compared with itself,
    not with the gaps before the window, because loss and jitter draws make
    those gaps different whether or not an outage is open.
  - the in-flight slack for liveness is 216 steps on these runs
    (`ROCKETNET_DST_OUTAGE_IN_FLIGHT_STEPS`, derived from the constants, not
    measured): a reliable send can queue behind two windows' worth of round
    trips on top of the default 96.
- `send-fail`: draws a side (the client's sends or the server's), a window
  of 50 to 200 steps and its start. In the window that side's transport
  refuses one frame in three at finish (`finish_packet` returns 0), as a
  transport whose send failed does; the message layer takes the packet as
  failed. It reaches the failed-packet paths no other fault does: a reliable
  packet's numbers go back to be resent, and each type 5 message in a refused
  packet takes its LOSS at once, which `receipt_outcome_due` and
  `receipt_one_outcome` judge. `MESSAGE_UNRELIABLE_SEND_FAILED` is explained
  only on a run where the schedule refused a frame.

A schedule that fires on no seed of the range fails the run, even if every
check held: it prints `FAIL seeds=<a>-<b> schedule=<name> fired on no seed`
and exits non-zero.

`test_dst_schedule_same_seed_same_trace`, `test_dst_clock_back_reaches_endpoints`
and `test_dst_seq_gap_exceeds_history` (`tests/test_unit_dst_core.cpp`, run by
`./bin/test`) pin that each schedule replays and fires.
`test_dst_burst_releases_a_batch_in_one_step` pins that the burst sends at
least two hundred packets in one update and receives at least two hundred in
one release step, counted apart, and `test_dst_dup_storm_duplicates_payloads`
pins that the storm's copies reach the receivers as a payload taken twice,
with the duplicate fault off.
`test_dst_outage_holds_dark_sessions` and
`test_dst_outage_past_times_out_both_ends` run seed 1 for 1600 steps under
each outage schedule and require it to fire, to judge held seconds, to reach
the no-room refusal and (past) to end dark sessions on both ends;
`test_dst_witness_outage_bright_fails` is the reversed witness: it also drops
one bright client and requires `bright_unaffected` to fail. The check cases in
`tests/test_unit_dst_checks.cpp` drive each new check function directly, each
both ways.

At the default `reliable_message_timeout_ms`, 0 (off), the message layer does
not end a connection. `test_dst_reliable_timeout_ends_connections_cleanly`
first runs seed 3 at the default and requires every check to pass with no
message-layer kick, so `no_unscheduled_end` sees no session end that the seed
did not draw; it then runs seed 3 at an accepted 1500 ms, where the timeout
ends connections on both ends, and requires every check to hold across them. A client whose session ended
can reconnect while the server still holds the old session's slot; the
harness books each server send and receipt to the session its slot holds.
