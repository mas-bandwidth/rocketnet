# RNLiveFleetRun: the lifecycle of the live fleet run

`proofs/tla/RNLiveFleetRun.tla` models the live XDP fleet run of `docs/LIVE-XDP.md`.
One orchestrator drives the run over ssh: one server on the test server with
the xdp transport, and one client per bench. The model is written before the
code. The runner card (`rocketnet-live-xdp-fleet-runner`) builds
`scripts/rocketnet-live-xdp` and the remote steps under `scripts/live-xdp/`.
It names their steps after the actions below and cites this module. Until
then, the `live-fleet-run` row of `proofs/tla/COVERAGE.tsv` is `owed`.

`proofs/tla/MCRNLiveFleetRun.tla` and `proofs/tla/MCRNLiveFleetRun.cfg` are the small
instance: two clients, two reconnects each, a timeout of one tick (so the
reconnect wait is two), a watchdog bound of two, a dead-man timer of five,
`max_clients` equal to the client count, and one outside event per run.
`proofs/tla/MCRNLiveFleetRunBrokenXdpLeftAttached.cfg` and
`proofs/tla/MCRNLiveFleetRunBrokenServerOrphaned.cfg` are the reversed witnesses.
The cases are rows of `proofs/tla/CASES.tsv` (group `rnlivefleetrun`), with
measured records in `proofs/tla/RUNS.tsv`.

## What is modelled

- **The orchestrator** (`docs/LIVE-XDP.md` 9 and 12):
  - `ArmDeadman`
  - `LoadServer(h)`, only for `h = TestServer`
  - `SshCheckPasses` / `SshCheckFails`: a fresh ssh connection right after
    the load. The 10 s bound itself is not modelled; the check's place in
    the order (after the load, before the listen line) is.
  - `ListenSeen`: the listen line in the status file.
  - `BeginTeardown`, when a client failed, the server is gone, the dead-man
    timer fired, or every client is done.
  - `StopClients`: only the clients it started; a client never started
    stays `built`.
  - `StopServer`: SIGINT. The server's destroy detaches.
  - `DetachXdp`: `ip link set dev <nic> xdp off`, whatever the server left.
  - `Finish`: confirm no program is attached, collect, and only then stop
    the watchdog and cancel the dead-man timer.
- **The test server's side:**
  - `Listen`
  - `DisconnectArrives(c)`: the clean disconnect burst frees the slot early.
  - `ServerTimesOut(c)`: a slot is freed T ticks after its connection's last
    packet. This applies to a silent drop, and to a clean disconnect whose
    burst was lost.
  - `WatchdogFires`: the orchestrator's heartbeat is stale (ssh down, or the
    orchestrator dead), or the server died with XDP attached. It kills the
    server and detaches.
  - `DeadmanFires`
- **Each client's cycle** (`docs/LIVE-XDP.md` 6):
  - `StartClient`
  - `Connected`: a free slot, or a denial.
  - `EndWindow`: clean on even cycles, silent on odd ones.
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
    teardown runs and the heartbeat stops.
- **Time** is a discrete `Tick`. The server's slot timeout, the dead-man
  timer and the watchdog are urgent: `Tick` is disabled while one of them is
  due.

Left out: the message matrix (`RNChannel`, `RNChannelTypes`, `RNReceipts`),
the handshake (`ClientConnection`, `ServerConnection`), the slot machine's
internals (`RNSlots`) and the XDP program's verdicts (`RNXdpReplay`). A slot
here is only a count against `max_clients`.

## Properties

| Name | Kind | Statement |
|---|---|---|
| `XdpNeverOrphaned` | invariant | XDP is never attached while the server is gone, or the orchestrator's heartbeat is stopped, for more than `WatchdogBound` ticks. |
| `TeardownDetaches` | invariant | A finished run has no server running and no program attached. |
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
| `MCRNLiveFleetRun.cfg` | PASS, invariants and liveness | 60,111 | see `proofs/tla/RUNS.tsv` | 0 |
| `MCRNLiveFleetRunBrokenXdpLeftAttached.cfg` | `TeardownDetaches` violated, as declared | see `proofs/tla/RUNS.tsv` | see `proofs/tla/RUNS.tsv` | 12 |
| `MCRNLiveFleetRunBrokenServerOrphaned.cfg` | `XdpNeverOrphaned` violated, as declared | see `proofs/tla/RUNS.tsv` | see `proofs/tla/RUNS.tsv` | 12 |

## The counterexamples, checked by hand against the design

- **`BrokenXdpLeftAttached`**: ArmDeadman, LoadServer, ServerCrash while
  the server is still starting, BeginTeardown, then Finish with XDP
  attached. In the code, the detach runs only in
  `rocketnet_server_socket_destroy`
  (`sdk/rocketnet_server_socket_linux.cpp:1627-1641`), and a crashed server
  never reaches it. The native attach
  (`sdk/rocketnet_server_socket_linux.cpp:1337`) stays on the NIC after the
  process dies. A teardown that only stops the server leaves the uplink
  filtered by a program no process serves. That is why `docs/LIVE-XDP.md`
  section 9 step 7 runs `ip link set dev <nic> xdp off` on every path. With
  the witness switch, the timers still stand down only once no program is
  attached, so `XdpOnlyUnderDeadman` holds and the run fails on the declared
  invariant alone.
- **`BrokenServerOrphaned`**: ArmDeadman, LoadServer, then the heartbeat
  stops, by SshDrop right after the load or by OrchestratorDies after the
  ssh check (TLC with several workers reports either), then three ticks with
  XDP attached and nothing to detach it until the dead-man timer, far later.
  This is the shape of a machine going dark while the session that would
  tear it down is gone. The watchdog's heartbeat timeout is what bounds it.

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
