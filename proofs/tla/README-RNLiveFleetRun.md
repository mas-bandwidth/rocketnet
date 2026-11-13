# RNLiveFleetRun: the lifecycle of the live fleet run

`proofs/tla/RNLiveFleetRun.tla` models the live XDP fleet run as
`scripts/live-xdp-fleet` (the orchestrator, on the coordinator's machine) and
`scripts/live-xdp-fleet-remote` (each host's steps, copied to the host) make
it. One orchestrator drives the run over ssh: one server on the test server
with the xdp transport, and one client per bench (`tests/client_server/client.c`
with `--matrix`). The scripts were written after the model; their comments name
the model's switches and actions where the order matters. The binding is not
checked: `tools/check_tla_coverage.py` reads only C-style comments, and no
trace check runs the scripts, so the `live-fleet-run` row of
`proofs/tla/COVERAGE.tsv` stays `owed`. `docs/LIVE-XDP.md` now keeps only the
verification evidence; the section numbers in the scripts' comments are the
design's.

`proofs/tla/MCRNLiveFleetRun.tla` and `proofs/tla/MCRNLiveFleetRun.cfg` are the small
instance: two clients, two reconnects each, a timeout of one tick (so the
reconnect wait is two), a watchdog bound of two, a dead-man timer of five,
`max_clients` equal to the client count, and one outside event per run.
The reversed witnesses are
`proofs/tla/MCRNLiveFleetRunBrokenXdpLeftAttached.cfg`,
`proofs/tla/MCRNLiveFleetRunBrokenServerOrphaned.cfg`,
`proofs/tla/MCRNLiveFleetRunBrokenHeartbeatLate.cfg` and
`proofs/tla/MCRNLiveFleetRunBrokenTeardownUnchecked.cfg`.
The cases are rows of `proofs/tla/CASES.tsv` (group `rnlivefleetrun`), with
measured records in `proofs/tla/RUNS.tsv`.

## What is modelled

- **The orchestrator** (`scripts/live-xdp-fleet`, steps 4 to 9):
  - `ArmDeadman`: `live-xdp-fleet-remote arm` truncates the heartbeat file and
    starts the dead-man timer and the watchdog. The orchestrator's heartbeat
    loop starts right after it (MM205).
  - `LoadServer(h)`, only for `h = TestServer`; with ssh down instead,
    `StartFails`: the start fails inside its 10 s connect bound and the trap
    runs teardown. The start is the next command after the arm, so no tick
    passes in `armed`.
  - `SshCheckPasses` / `SshCheckFails`: a fresh ssh connection right after
    the load. On failure the script stops its heartbeat and exits with no
    teardown; the watchdog and the dead-man timer detach. The 10 s bound
    itself is not modelled; the check's place in the order (after the load,
    before the listen line) is.
  - `ListenSeen`: the listen line in the status file. The script's 30 s
    bound on it is not modelled.
  - `BeginTeardown`, when a client failed, the server is gone, the dead-man
    timer fired, or every client is done. Teardown first stops the heartbeat
    loop (`LoopStopped`).
  - `StopClients`: only the clients it started; a client never started
    stays `built`.
  - `StopServer`: SIGINT. The server's destroy detaches.
  - `DetachXdp`: `ip link set dev <nic> xdp off`, whatever the server left.
  - `TeardownLosesSsh`: the next teardown step finds ssh down. "stop server"
    fails, restore is skipped, and the script fails the run and exits; the
    watchdog and the dead-man timer act.
  - `Finish`: confirm no program is attached, collect, and only then stop
    the watchdog and cancel the dead-man timer. The script reaches it only
    when both "stop server" and "restore" exited 0 (MM206).
- **The test server's side:**
  - `Listen`
  - `DisconnectArrives(c)`: the clean disconnect burst frees the slot early.
  - `ServerTimesOut(c)`: a slot is freed T ticks after its connection's last
    packet. This applies to a crashed or stopped client, and to a clean
    disconnect whose burst was lost.
  - `WatchdogFires`: the heartbeat is stale (ssh down, teardown begun, or
    the orchestrator gone), whatever is attached, or the server died with
    XDP attached. It kills the server, detaches, and exits.
  - `DeadmanFires`
- **Each client's cycle** (`tests/client_server/client.c`):
  - `StartClient`
  - `Connected`: a free slot, or a denial.
  - `EndWindow`: an orderly disconnect, every window. A silent loss is only
    a client crash.
  - `BeginWait`
  - `Reconnect`: after a wait of 2 x T.
  - `FinalDisconnect`: clean.
  - `LosesServer`: an unexpected disconnect, recorded as a FAIL.
- **Outside events,** at most `MaxFaults` per run and with no fairness:
  - `SshDrop` and `SshBack`
  - `ServerCrash`: the libxdp attachment outlives the process.
  - `ClientCrash(c)`
  - `Interrupted`: the orchestrator exits, and its trap runs teardown.
  - `OrchestratorDies`: the orchestrator is killed without its trap; no
    teardown runs and the heartbeat stops (the heartbeat loop checks that
    its parent is alive).
  - `TeardownStepFails`: "stop server" or "restore" exits non-zero over a
    working ssh. The script fails the run and exits with its heartbeat
    stopped. After a failed stop it skips restore, so the timers stay
    armed. A restore that fails only on its sweep has already stood the
    timers down; the model keeps them armed, an over-approximation.
- **Time** is a discrete `Tick`. The server's slot timeout, the dead-man
  timer and the watchdog are urgent: `Tick` is disabled while one of them is
  due, and in `armed`.

Left out: the message matrix (`RNChannel`, `RNChannelTypes`, `RNReceipts`),
the handshake (`ClientConnection`, `ServerConnection`), the slot machine's
internals (`RNSlots`) and the XDP program's verdicts (`RNXdpReplay`). A slot
here is only a count against `max_clients`. The orchestrator's watch loop is
abstracted: `BeginTeardown` may follow a server crash or the dead-man timer at
once, where the script waits for a client to fail or finish.

## Properties

| Name | Kind | Statement |
|---|---|---|
| `XdpNeverOrphaned` | invariant | XDP is never attached while the server is gone, or the orchestrator's heartbeat is stopped, for more than `WatchdogBound` ticks. |
| `TeardownDetaches` | invariant | A finished run has no server running and no program attached. |
| `WatchdogKillsOnlyOnFault` | invariant | In a run with no outside event so far (`faults = 0`), the watchdog never stops a live server between the arm and teardown. It says nothing about runs with a fault, where the watchdog firing is the design. |
| `XdpOnlyUnderDeadman` | invariant | XDP is attached only while the dead-man timer is armed. |
| `NoClientBeforeListen` | invariant, structural | No client starts before the server has listened. Held by StartClient's guard. |
| `NoBenchServer` | invariant, structural | The server never runs on a bench. Held by LoadServer's guard. |
| `ReconnectAfterTwoTimeouts` | invariant, structural | Every reconnect follows a wait of 2 x T. Held by Reconnect's guard; what the wait buys is `ReconnectIntoFreedSlot`. |
| `ReconnectIntoFreedSlot` | invariant | A client's live connection never shares the server with its previous connection's slot. |
| `NeverDenied` | invariant | With `max_clients` at least the client count, no connect is denied. |
| `EndsStoppedAndDetached` | liveness | Every run ends with the server stopped and XDP detached. |

## Runs

Pinned runner and JAR as `proofs/tla/README.md`, on a Linux bench, two workers for
the control and one for each witness:

| Case | Result | Distinct states | Seconds | Exit |
|---|---|---:|---:|---:|
| `MCRNLiveFleetRun.cfg` | PASS, invariants and liveness | see `proofs/tla/RUNS.tsv` | see `proofs/tla/RUNS.tsv` | 0 |
| `MCRNLiveFleetRunBrokenXdpLeftAttached.cfg` | `TeardownDetaches` violated, as declared | see `proofs/tla/RUNS.tsv` | see `proofs/tla/RUNS.tsv` | 12 |
| `MCRNLiveFleetRunBrokenServerOrphaned.cfg` | `XdpNeverOrphaned` violated, as declared | see `proofs/tla/RUNS.tsv` | see `proofs/tla/RUNS.tsv` | 12 |
| `MCRNLiveFleetRunBrokenHeartbeatLate.cfg` | `WatchdogKillsOnlyOnFault` violated, as declared | see `proofs/tla/RUNS.tsv` | see `proofs/tla/RUNS.tsv` | 12 |
| `MCRNLiveFleetRunBrokenTeardownUnchecked.cfg` | `TeardownDetaches` violated, as declared | see `proofs/tla/RUNS.tsv` | see `proofs/tla/RUNS.tsv` | 12 |

## The counterexamples, checked by hand against the code

- **`BrokenXdpLeftAttached`**: ArmDeadman, LoadServer, ServerCrash while
  the server is still starting, BeginTeardown, then Finish with XDP
  attached. In the code, the detach runs only in
  `rocketnet_server_socket_destroy`
  through `rocketnet_xdp_program_detach` in
  `sdk/rocketnet_server_socket_linux.cpp`, and a crashed server never
  reaches it. The native attach by `rocketnet_xdp_program_attach` in the
  same file stays on the NIC after the
  process dies. A teardown that only stops the server leaves the uplink
  filtered by a program no process serves. That is why
  `live-xdp-fleet-remote restore` runs `ip link set dev <nic> xdp off` on
  every path. With the witness switch, the timers still stand down only once
  no program is attached, so `XdpOnlyUnderDeadman` holds and the run fails on
  the declared invariant alone.
- **`BrokenServerOrphaned`**: ArmDeadman, LoadServer, then the heartbeat
  stops, by SshDrop right after the load or by OrchestratorDies after the
  ssh check (TLC with several workers reports either), then three ticks with
  XDP attached and nothing to detach it until the dead-man timer, far later.
  This is the shape of a machine going dark while the session that would
  tear it down is gone. The watchdog's heartbeat timeout is what bounds it.
- **`BrokenHeartbeatLate`** (MM205): ArmDeadman, LoadServer, two ticks with
  the server still starting, then WatchdogFires with no outside event. In
  the script before MM205, `arm` touched the heartbeat once
  (`live-xdp-fleet-remote`, `: > "$BASE/heartbeat"`), and the loop that
  touches it every 5 s started only after the server start, up to 30 s of
  polling for the listen line and one ssh per client start; the watchdog
  fires when the heartbeat is more than 30 s old. A local run of those
  scripts, with the hosts stood in and a listen line at 29 s, showed the
  watchdog firing at heartbeat age 31 s with the server up.
- **`BrokenTeardownUnchecked`** (MM206): a fault-free run to every client
  done, BeginTeardown, then TeardownStepFails, and the run is done with the
  server listening and XDP attached. In the script before MM206, teardown
  sent "stop server" and "restore" to `/dev/null` and ignored both exit
  statuses, then printed "run complete" and exited 0; a local run with the
  detach failing showed exactly that.

The first draft's control failed `NoClientBeforeListen` in four states:
Interrupted straight after ArmDeadman, then StopClients marked the
never-started clients `done`. That was a modelling error, not a design one.
Teardown stops only the clients it started, so `StopClients` and `Finish`
now leave `built` clients alone. The design text already said so.

The cold read of the first version found the orchestrator killed without its
trap unmodelled (the watchdog then watched only ssh), and the ssh check
ordered after the listen line where the design puts it before. Both are in
the model now: `OrchestratorDies` stops the heartbeat, and `SshCheckPasses`
precedes `ListenSeen`.

The models review of 2026-11-13 (MM207) found the model describing a run the
scripts do not make: alternating silent drops (the client disconnects
orderly every window), a failed ssh check entering teardown (the script exits
without one), and the watchdog started at the load (arm starts it). The model
follows the scripts now; the watchdog also exits after firing, as
`cmd_watchdog` does.
