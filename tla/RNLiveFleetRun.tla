--------------------------- MODULE RNLiveFleetRun ---------------------------
(***************************************************************************)
(* rocketnet: the lifecycle of one live fleet run (docs/LIVE-XDP.md).      *)
(*                                                                         *)
(* One orchestrator on the coordinator's machine drives, over ssh, one     *)
(* server on the test server (the xdp transport, so loading the server        *)
(* attaches the XDP program to the NIC) and one client per bench.  Before  *)
(* the load it arms a dead-man timer on the test server; with the server   *)
(* it starts a watchdog that detaches XDP and kills the server when the    *)
(* orchestrator's heartbeat over ssh stops (ssh down, or the orchestrator  *)
(* dead without its trap) or when the server dies with XDP still attached.  Each client runs a cycle: connect, a stress        *)
(* window, a disconnect (clean, then a silent drop, alternating), a wait   *)
(* of 2 x the connection timeout, a reconnect with a fresh token, and so   *)
(* on; the last window ends with a clean disconnect.                       *)
(*                                                                         *)
(* Time is a discrete tick.  The timers that must fire on time (the        *)
(* server's slot timeout, the dead-man timer, the watchdog) are urgent:    *)
(* Tick is disabled while one of them is due, so the action fires before   *)
(* time moves on.  The outside events (ssh drop and return, a server      *)
(* crash, a client crash, the orchestrator interrupted with its trap, the  *)
(* orchestrator killed without it) are bounded by MaxFaults over the run   *)
(* and carry no fairness.  The 10 s bound on the ssh check after the load  *)
(* is not modelled; the check's place in the order is.                     *)
(*                                                                         *)
(* Left out: the message matrix itself (RNChannel, RNChannelTypes,         *)
(* RNReceipts), the handshake (ClientConnection, ServerConnection), the    *)
(* slot machine's internals (RNSlots), and the XDP program's verdicts      *)
(* (RNXdpReplay).  A slot here is a count against max_clients.             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Clients,        \* the benches, one client each
    TestServer,     \* the one host a server may run on
    Cycles,         \* stress windows per client before the final one
    T,              \* the connection timeout, in ticks
    WatchdogBound,  \* ticks the watchdog lets XDP outlive its server or ssh
    DeadmanBound,   \* ticks from arming until the dead-man timer fires
    MaxClients,     \* the server's max_clients
    MaxFaults       \* outside events over the whole run

ASSUME /\ Clients # {}
       /\ TestServer \notin Clients
       /\ Cycles \in Nat
       /\ T \in Nat \ {0}
       /\ WatchdogBound \in Nat \ {0}
       /\ DeadmanBound \in Nat /\ DeadmanBound > WatchdogBound + 1
       /\ MaxClients \in Nat \ {0}
       /\ MaxFaults \in Nat

(***************************************************************************)
(* Rule switches, definitions with the design's values, each overridden    *)
(* by one reversed witness configuration.                                  *)
(*   WatchdogPresent: the server starts with its watchdog (design TRUE).   *)
(*   TeardownDetachesOnCrash: teardown detaches XDP on the crash path too  *)
(*     (design TRUE); FALSE lets the run finish with XDP still attached    *)
(*     after a server crash, as a teardown that trusts the server's own    *)
(*     destroy would.                                                      *)
(***************************************************************************)
WatchdogPresent         == TRUE
TeardownDetachesOnCrash == TRUE

Hosts      == {TestServer} \cup Clients
NoHost     == "none"
OrchStates == {"preflight", "armed", "loading", "checked", "serving", "teardown", "done", "dead"}
ServerStates == {"absent", "starting", "listening", "stopped"}
Phases     == {"built", "connecting", "running", "disconnected_clean",
               "dropped_silent", "waiting", "failed", "done"}
OldSlots   == {"none", "freeing", "timing_out"}
DeadmanStates == {"unarmed", "armed", "fired", "cancelled"}

VARIABLES
    orch,        \* the orchestrator's step
    server,      \* the server process on the test server
    host,        \* where the server runs
    xdp,         \* the XDP program attached to the test server's NIC
    ssh,         \* the orchestrator's ssh path to the test server
    hbAge,       \* ticks since the orchestrator's last heartbeat reached the test server (capped)
    orphan,      \* ticks XDP has been attached with no live server (capped)
    watchdog,    \* the watchdog is running on the test server
    deadman,     \* the dead-man timer
    deadmanAge,  \* ticks since it was armed (capped)
    listened,    \* history: the server has reached listening
    crashed,     \* history: the server died outside teardown
    faults,      \* outside events so far
    phase,       \* each client's phase
    cycle,       \* each client's completed reconnects
    wait,        \* ticks each client has waited since its disconnect
    lastWait,    \* the wait each client's latest reconnect followed
    live,        \* each client holds a server slot for a live connection
    old,         \* each client's previous connection's slot on the server
    oldAge,      \* ticks since that previous connection's last packet
    denied,      \* history: some connect found no free slot
    collected    \* results collected

vars == << orch, server, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
           deadmanAge, listened, crashed, faults, phase, cycle, wait, lastWait,
           live, old, oldAge, denied, collected >>

ServerUp == server \in {"starting", "listening"}

UsedSlots == Cardinality({ c \in Clients : live[c] }) +
             Cardinality({ c \in Clients : old[c] # "none" })

\* the server's process ends: every slot it held goes with it
SlotsVanish == /\ live'   = [ c \in Clients |-> FALSE ]
               /\ old'    = [ c \in Clients |-> "none" ]
               /\ oldAge' = [ c \in Clients |-> 0 ]

TypeOK ==
    /\ orch \in OrchStates
    /\ server \in ServerStates
    /\ host \in Hosts \cup {NoHost}
    /\ xdp \in BOOLEAN
    /\ ssh \in {"up", "down"}
    /\ hbAge \in 0..(WatchdogBound + 1)
    /\ orphan \in 0..(WatchdogBound + 1)
    /\ watchdog \in BOOLEAN
    /\ deadman \in DeadmanStates
    /\ deadmanAge \in 0..DeadmanBound
    /\ listened \in BOOLEAN
    /\ crashed \in BOOLEAN
    /\ faults \in 0..MaxFaults
    /\ phase \in [ Clients -> Phases ]
    /\ cycle \in [ Clients -> 0..Cycles ]
    /\ wait \in [ Clients -> 0..(2 * T) ]
    /\ lastWait \in [ Clients -> 0..(2 * T) ]
    /\ live \in [ Clients -> BOOLEAN ]
    /\ old \in [ Clients -> OldSlots ]
    /\ oldAge \in [ Clients -> 0..T ]
    /\ denied \in BOOLEAN
    /\ collected \in BOOLEAN

Init ==
    /\ orch = "preflight"
    /\ server = "absent"
    /\ host = NoHost
    /\ xdp = FALSE
    /\ ssh = "up"
    /\ hbAge = 0
    /\ orphan = 0
    /\ watchdog = FALSE
    /\ deadman = "unarmed"
    /\ deadmanAge = 0
    /\ listened = FALSE
    /\ crashed = FALSE
    /\ faults = 0
    /\ phase = [ c \in Clients |-> "built" ]
    /\ cycle = [ c \in Clients |-> 0 ]
    /\ wait = [ c \in Clients |-> 0 ]
    /\ lastWait = [ c \in Clients |-> 0 ]
    /\ live = [ c \in Clients |-> FALSE ]
    /\ old = [ c \in Clients |-> "none" ]
    /\ oldAge = [ c \in Clients |-> 0 ]
    /\ denied = FALSE
    /\ collected = FALSE

-----------------------------------------------------------------------------
(* The orchestrator's steps, in order (docs/LIVE-XDP.md section 12).       *)

\* systemd-run arms the dead-man timer before anything touches the NIC
ArmDeadman ==
    /\ orch = "preflight" /\ ssh = "up"
    /\ orch' = "armed"
    /\ deadman' = "armed" /\ deadmanAge' = 0
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, watchdog, listened,
                    crashed, faults, phase, cycle, wait, lastWait, live, old,
                    oldAge, denied, collected >>

\* the server starts on host h with its watchdog; create attaches XDP
LoadServer(h) ==
    /\ orch = "armed" /\ ssh = "up" /\ deadman = "armed"
    /\ h = TestServer
    /\ orch' = "loading"
    /\ server' = "starting" /\ host' = h /\ xdp' = TRUE
    /\ watchdog' = WatchdogPresent
    /\ UNCHANGED << ssh, hbAge, orphan, deadman, deadmanAge, listened, crashed,
                    faults, phase, cycle, wait, lastWait, live, old, oldAge,
                    denied, collected >>

\* a fresh ssh connection inside 10 s of the load, before anything else
SshCheckPasses ==
    /\ orch = "loading" /\ ssh = "up"
    /\ orch' = "checked"
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, faults, phase, cycle, wait,
                    lastWait, live, old, oldAge, denied, collected >>

SshCheckFails ==
    /\ orch = "loading" /\ ssh = "down"
    /\ orch' = "teardown"
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, faults, phase, cycle, wait,
                    lastWait, live, old, oldAge, denied, collected >>

\* then the listen line in the status file
ListenSeen ==
    /\ orch = "checked" /\ server = "listening"
    /\ orch' = "serving"
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, faults, phase, cycle, wait,
                    lastWait, live, old, oldAge, denied, collected >>

TeardownDue ==
    \/ crashed
    \/ ~ServerUp
    \/ deadman = "fired"
    \/ \E c \in Clients : phase[c] = "failed"
    \/ \A c \in Clients : phase[c] = "done"

BeginTeardown ==
    /\ orch \in {"loading", "checked", "serving"} /\ TeardownDue
    /\ orch' = "teardown"
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, faults, phase, cycle, wait,
                    lastWait, live, old, oldAge, denied, collected >>

\* the trap: any exit of the orchestrator after arming runs teardown
Interrupted ==
    /\ faults < MaxFaults
    /\ orch \in {"armed", "loading", "checked", "serving"}
    /\ orch' = "teardown"
    /\ faults' = faults + 1
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, phase, cycle, wait, lastWait,
                    live, old, oldAge, denied, collected >>

\* teardown stops every client it started (ssh to each bench); a client
\* never started stays built
StopClients ==
    /\ orch = "teardown"
    /\ \E c \in Clients : phase[c] \notin {"built", "done", "failed"}
    /\ phase' = [ c \in Clients |-> IF phase[c] \in {"built", "done", "failed"} THEN phase[c] ELSE "done" ]
    /\ old' = [ c \in Clients |-> IF live[c] THEN "timing_out" ELSE old[c] ]
    /\ oldAge' = [ c \in Clients |-> IF live[c] THEN 0 ELSE oldAge[c] ]
    /\ live' = [ c \in Clients |-> FALSE ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, cycle, wait,
                    lastWait, denied, collected >>

\* SIGINT: the server stops, and its destroy detaches XDP
StopServer ==
    /\ orch = "teardown" /\ ssh = "up" /\ ServerUp
    /\ server' = "stopped" /\ xdp' = FALSE /\ orphan' = 0
    /\ SlotsVanish
    /\ UNCHANGED << orch, host, ssh, hbAge, watchdog, deadman, deadmanAge,
                    listened, crashed, faults, phase, cycle, wait, lastWait,
                    denied, collected >>

\* ip link set dev <nic> xdp off, whatever the server left
DetachXdp ==
    /\ orch = "teardown" /\ ssh = "up" /\ ~ServerUp /\ xdp
    /\ (crashed => TeardownDetachesOnCrash)
    /\ xdp' = FALSE /\ orphan' = 0
    /\ UNCHANGED << orch, server, host, ssh, hbAge, watchdog, deadman,
                    deadmanAge, listened, crashed, faults, phase, cycle, wait,
                    lastWait, live, old, oldAge, denied, collected >>

\* confirm no program is attached, collect, cancel the timers, done
Finish ==
    /\ orch = "teardown" /\ ssh = "up" /\ ~ServerUp
    /\ \/ ~xdp
       \/ crashed /\ ~TeardownDetachesOnCrash
    /\ \A c \in Clients : phase[c] \in {"built", "done", "failed"}
    /\ orch' = "done"
    /\ collected' = TRUE
    \* the timers stand down only once no program is attached
    /\ watchdog' = (watchdog /\ xdp)
    /\ deadman' = IF deadman = "armed" /\ ~xdp THEN "cancelled" ELSE deadman
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, deadmanAge,
                    listened, crashed, faults, phase, cycle, wait, lastWait,
                    live, old, oldAge, denied >>

-----------------------------------------------------------------------------
(* The server and its timers on the test server.                           *)

Listen ==
    /\ server = "starting"
    /\ server' = "listening" /\ listened' = TRUE
    /\ UNCHANGED << orch, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, crashed, faults, phase, cycle, wait, lastWait,
                    live, old, oldAge, denied, collected >>

\* the redundant disconnect burst of a clean disconnect arrives
DisconnectArrives(c) ==
    /\ server = "listening" /\ old[c] = "freeing"
    /\ old' = [ old EXCEPT ![c] = "none" ]
    /\ oldAge' = [ oldAge EXCEPT ![c] = 0 ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, phase, cycle,
                    wait, lastWait, live, denied, collected >>

\* the server's timeout path frees a slot T after its last packet
ServerTimesOut(c) ==
    /\ server = "listening" /\ old[c] # "none" /\ oldAge[c] = T
    /\ old' = [ old EXCEPT ![c] = "none" ]
    /\ oldAge' = [ oldAge EXCEPT ![c] = 0 ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, phase, cycle,
                    wait, lastWait, live, denied, collected >>

\* the heartbeat is stale, or the server is gone with XDP attached
WatchdogDue == watchdog /\ xdp /\ (orphan >= WatchdogBound \/ hbAge >= WatchdogBound)

HeartbeatStopped == ssh = "down" \/ orch = "dead"

\* the watchdog kills the server and runs ip link set dev <nic> xdp off
WatchdogFires ==
    /\ WatchdogDue
    /\ server' = IF ServerUp THEN "stopped" ELSE server
    /\ xdp' = FALSE /\ orphan' = 0
    /\ SlotsVanish
    /\ UNCHANGED << orch, host, ssh, hbAge, watchdog, deadman, deadmanAge,
                    listened, crashed, faults, phase, cycle, wait, lastWait,
                    denied, collected >>

DeadmanDue == deadman = "armed" /\ deadmanAge = DeadmanBound

DeadmanFires ==
    /\ DeadmanDue
    /\ deadman' = "fired"
    /\ server' = IF ServerUp THEN "stopped" ELSE server
    /\ xdp' = FALSE /\ orphan' = 0
    /\ SlotsVanish
    /\ UNCHANGED << orch, host, ssh, hbAge, watchdog, deadmanAge, listened,
                    crashed, faults, phase, cycle, wait, lastWait, denied,
                    collected >>

-----------------------------------------------------------------------------
(* Each client's cycle (docs/LIVE-XDP.md section 6).                       *)

StartClient(c) ==
    /\ orch = "serving" /\ phase[c] = "built"
    /\ phase' = [ phase EXCEPT ![c] = "connecting" ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, cycle, wait,
                    lastWait, live, old, oldAge, denied, collected >>

\* a fresh token, minted now; the server takes a free slot or denies
Connected(c) ==
    /\ phase[c] = "connecting" /\ server = "listening"
    /\ IF UsedSlots < MaxClients
       THEN /\ phase' = [ phase EXCEPT ![c] = "running" ]
            /\ live' = [ live EXCEPT ![c] = TRUE ]
            /\ UNCHANGED denied
       ELSE /\ phase' = [ phase EXCEPT ![c] = "failed" ]
            /\ denied' = TRUE
            /\ UNCHANGED live
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, cycle, wait,
                    lastWait, old, oldAge, collected >>

\* the window ends: even cycles disconnect clean, odd ones drop silently
EndWindow(c) ==
    /\ phase[c] = "running" /\ server = "listening" /\ cycle[c] < Cycles
    /\ phase' = [ phase EXCEPT ![c] = IF cycle[c] % 2 = 0 THEN "disconnected_clean" ELSE "dropped_silent" ]
    /\ live' = [ live EXCEPT ![c] = FALSE ]
    /\ old' = [ old EXCEPT ![c] = IF cycle[c] % 2 = 0 THEN "freeing" ELSE "timing_out" ]
    /\ oldAge' = [ oldAge EXCEPT ![c] = 0 ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, cycle, wait,
                    lastWait, denied, collected >>

BeginWait(c) ==
    /\ phase[c] \in {"disconnected_clean", "dropped_silent"}
    /\ phase' = [ phase EXCEPT ![c] = "waiting" ]
    /\ wait' = [ wait EXCEPT ![c] = 0 ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, cycle,
                    lastWait, live, old, oldAge, denied, collected >>

\* 2 x timeout after the disconnect, the client reconnects
Reconnect(c) ==
    /\ phase[c] = "waiting" /\ wait[c] = 2 * T
    /\ phase' = [ phase EXCEPT ![c] = "connecting" ]
    /\ cycle' = [ cycle EXCEPT ![c] = cycle[c] + 1 ]
    /\ lastWait' = [ lastWait EXCEPT ![c] = wait[c] ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, wait, live,
                    old, oldAge, denied, collected >>

\* the last window ends with a clean disconnect
FinalDisconnect(c) ==
    /\ phase[c] = "running" /\ server = "listening" /\ cycle[c] = Cycles
    /\ phase' = [ phase EXCEPT ![c] = "done" ]
    /\ live' = [ live EXCEPT ![c] = FALSE ]
    /\ old' = [ old EXCEPT ![c] = "freeing" ]
    /\ oldAge' = [ oldAge EXCEPT ![c] = 0 ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, cycle, wait,
                    lastWait, denied, collected >>

\* the server is gone under a live or connecting client: an unexpected
\* disconnect, recorded as a FAIL
LosesServer(c) ==
    /\ phase[c] \in {"connecting", "running"} /\ server # "listening"
    /\ listened
    /\ phase' = [ phase EXCEPT ![c] = "failed" ]
    /\ live' = [ live EXCEPT ![c] = FALSE ]
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, faults, cycle, wait,
                    lastWait, old, oldAge, denied, collected >>

-----------------------------------------------------------------------------
(* Outside events.                                                         *)

SshDrop ==
    /\ faults < MaxFaults /\ ssh = "up" /\ orch # "done"
    /\ ssh' = "down" /\ hbAge' = 0 /\ faults' = faults + 1
    /\ UNCHANGED << orch, server, host, xdp, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, phase, cycle, wait, lastWait,
                    live, old, oldAge, denied, collected >>

\* the orchestrator is killed without its trap (SIGKILL, the coordinator's
\* machine asleep or gone): no teardown, and no more heartbeats
OrchestratorDies ==
    /\ faults < MaxFaults
    /\ orch \in {"armed", "loading", "checked", "serving", "teardown"}
    /\ orch' = "dead" /\ faults' = faults + 1
    /\ UNCHANGED << server, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, phase, cycle, wait, lastWait,
                    live, old, oldAge, denied, collected >>

SshBack ==
    /\ ssh = "down"
    /\ ssh' = "up" /\ hbAge' = 0
    /\ UNCHANGED << orch, server, host, xdp, orphan, watchdog, deadman,
                    deadmanAge, listened, crashed, faults, phase, cycle, wait,
                    lastWait, live, old, oldAge, denied, collected >>

\* the server process dies; the XDP attachment outlives it
ServerCrash ==
    /\ faults < MaxFaults /\ ServerUp
    /\ server' = "stopped" /\ crashed' = TRUE /\ faults' = faults + 1
    /\ SlotsVanish
    /\ UNCHANGED << orch, host, xdp, ssh, hbAge, orphan, watchdog, deadman,
                    deadmanAge, listened, phase, cycle, wait, lastWait, denied,
                    collected >>

\* a client process dies; the server times its slot out
ClientCrash(c) ==
    /\ faults < MaxFaults
    /\ phase[c] \in {"connecting", "running", "waiting"}
    /\ phase' = [ phase EXCEPT ![c] = "failed" ]
    /\ live' = [ live EXCEPT ![c] = FALSE ]
    /\ old' = [ old EXCEPT ![c] = IF live[c] THEN "timing_out" ELSE old[c] ]
    /\ oldAge' = [ oldAge EXCEPT ![c] = IF live[c] THEN 0 ELSE oldAge[c] ]
    /\ faults' = faults + 1
    /\ UNCHANGED << orch, server, host, xdp, ssh, hbAge, orphan, watchdog,
                    deadman, deadmanAge, listened, crashed, cycle, wait,
                    lastWait, denied, collected >>

-----------------------------------------------------------------------------
(* Time.  Urgent timers block the tick until they fire.                    *)

Cap(n, m) == IF n < m THEN n + 1 ELSE m

TimeoutDue == \E c \in Clients : server = "listening" /\ old[c] # "none" /\ oldAge[c] = T

Tick ==
    /\ ~TimeoutDue
    /\ ~DeadmanDue
    /\ ~WatchdogDue
    /\ wait' = [ c \in Clients |-> IF phase[c] = "waiting" THEN Cap(wait[c], 2 * T) ELSE wait[c] ]
    /\ oldAge' = [ c \in Clients |-> IF old[c] # "none" THEN Cap(oldAge[c], T) ELSE oldAge[c] ]
    /\ hbAge' = IF HeartbeatStopped THEN Cap(hbAge, WatchdogBound + 1) ELSE 0
    /\ orphan' = IF xdp /\ ~ServerUp THEN Cap(orphan, WatchdogBound + 1) ELSE 0
    /\ deadmanAge' = IF deadman = "armed" THEN Cap(deadmanAge, DeadmanBound) ELSE deadmanAge
    /\ UNCHANGED << orch, server, host, xdp, ssh, watchdog, deadman, listened,
                    crashed, faults, phase, cycle, lastWait, live, old, denied,
                    collected >>

-----------------------------------------------------------------------------

Orchestrate ==
    \/ ArmDeadman
    \/ \E h \in Hosts : LoadServer(h)
    \/ SshCheckPasses \/ SshCheckFails \/ ListenSeen \/ BeginTeardown
    \/ StopClients \/ StopServer \/ DetachXdp \/ Finish

ServerSide ==
    \/ Listen \/ WatchdogFires \/ DeadmanFires
    \/ \E c \in Clients : DisconnectArrives(c) \/ ServerTimesOut(c)

ClientSide ==
    \E c \in Clients :
        \/ StartClient(c) \/ Connected(c) \/ EndWindow(c) \/ BeginWait(c)
        \/ Reconnect(c) \/ FinalDisconnect(c) \/ LosesServer(c)

Outside ==
    \/ Interrupted \/ OrchestratorDies \/ SshDrop \/ SshBack \/ ServerCrash
    \/ \E c \in Clients : ClientCrash(c)

Next == Orchestrate \/ ServerSide \/ ClientSide \/ Outside \/ Tick

Fairness ==
    /\ WF_vars(Orchestrate)
    /\ WF_vars(Listen) /\ WF_vars(WatchdogFires) /\ WF_vars(DeadmanFires)
    /\ \A c \in Clients :
        /\ WF_vars(ServerTimesOut(c))
        /\ WF_vars(StartClient(c)) /\ WF_vars(Connected(c))
        /\ WF_vars(EndWindow(c)) /\ WF_vars(BeginWait(c))
        /\ WF_vars(Reconnect(c)) /\ WF_vars(FinalDisconnect(c))
        /\ WF_vars(LosesServer(c))
    /\ WF_vars(Tick)

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
(* Invariants.                                                             *)

\* XDP is never attached while the server is gone, or while the
\* orchestrator's heartbeat is stopped, for longer than the watchdog bound
XdpNeverOrphaned == xdp => (orphan <= WatchdogBound /\ hbAge <= WatchdogBound)

\* a finished run leaves no server running and no program attached
TeardownDetaches == orch = "done" => (~xdp /\ ~ServerUp)

\* XDP is attached only under an armed dead-man timer
XdpOnlyUnderDeadman == xdp => deadman = "armed"

\* no client starts before the server has listened (structural: StartClient
\* needs serving, which needs the listen line)
NoClientBeforeListen == \A c \in Clients : phase[c] # "built" => listened

\* no bench ever holds the server (structural: LoadServer's guard)
NoBenchServer == host \notin Clients

\* a reconnect follows a wait of 2 x the timeout (structural: Reconnect's
\* guard; the timeout's consequence is ReconnectIntoFreedSlot)
ReconnectAfterTwoTimeouts ==
    \A c \in Clients : (cycle[c] > 0 /\ phase[c] \in {"connecting", "running"}) => lastWait[c] >= 2 * T

\* a live connection never shares the server with its client's previous one
ReconnectIntoFreedSlot == \A c \in Clients : phase[c] = "running" => old[c] = "none"

\* with max_clients at least the bench count, no connect is ever denied
NeverDenied == MaxClients >= Cardinality(Clients) => ~denied

(* Liveness: every run ends with the server stopped and XDP detached.     *)
EndsStoppedAndDetached == <>[](~xdp /\ ~ServerUp)

=============================================================================
