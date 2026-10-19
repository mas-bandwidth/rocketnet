---------------------------- MODULE RNSimFaults ----------------------------
(***************************************************************************)
(* rocketnet: the deterministic simulation test's fault injector for       *)
(* crash, restart and clock skew (docs/DST.md sections 3 and 7).           *)
(*                                                                         *)
(* The injector owns the world around the peers: each peer's phase (up,    *)
(* crashed, restarting), each client's session generation, the server's    *)
(* incarnation, each peer's skewed clock, the true time, the fault window  *)
(* flag, and the packets in the wire tagged with the fields the payload    *)
(* scheme stamps (docs/DST.md section 2): the session generation (bytes    *)
(* 6-8) and the server incarnation (byte 9).  The two are separate         *)
(* counters with separate checks: a receiver accepts a packet only when    *)
(* its generation equals the receiving session's generation and,           *)
(* separately, its incarnation equals the receiving session's              *)
(* incarnation.                                                            *)
(*                                                                         *)
(* Each session also carries a ghost tag (sid), a number no other session  *)
(* ever holds: it is the checker's ghost state of docs/DST.md section 3    *)
(* ("what was sent and delivered on its old sessions"), never on the       *)
(* wire.  A delivery whose packet tag differs from the receiving           *)
(* session's tag is a payload of another session reaching this one, which  *)
(* is what DeliverOnlyToLiveGeneration refuses.                            *)
(*                                                                         *)
(* The handshake is one step (Connect begins it, Establish ends it); the   *)
(* handshake packet by packet is ClientConnection and ServerConnection,   *)
(* the session's per-connection state ClientSession and ServerSession,    *)
(* and the message layer RNChannel.  They are referred to, not restated.   *)
(* The server learns of a client crash only by its timeout                 *)
(* (ServerTimeout); clients see a server restart as silence                *)
(* (ClientTimeout) and reconnect under a fresh generation.  Timeouts are   *)
(* untimed here: each is enabled exactly when its session is dead and is   *)
(* weakly fair.                                                            *)
(*                                                                         *)
(* Clocks: true time t counts steps from 0 (the DST's 1000.0).  A peer's   *)
(* clock is c_p = Offset[p] + Rate[p] * t in units of 1/Scale step, the    *)
(* integer form of c_p(k) = 1000.0 + o_p + r_p * (t(k) - 1000.0).  The     *)
(* bound of docs/DST.md section 3, |c_p - t| <= 0.25 + 0.005 * (t - 1000), *)
(* is |clock[p] - Scale * t| <= OffsetBound + DriftBound * t here.  The    *)
(* injector keeps a peer's clock across its crash and restart.             *)
(*                                                                         *)
(* Rule switches, each overridden by one reversed witness:                 *)
(*   FreshOnRestart (design TRUE): a restarted client connects under a     *)
(*     generation never used; FALSE keeps its old generation.              *)
(*   CrashSilences (design TRUE): a crash destroys the peer's objects at   *)
(*     once, so it sends and receives nothing until it is rebuilt; FALSE   *)
(*     flips the phase but leaves the peer running.                        *)
(***************************************************************************)
EXTENDS Naturals, Integers, FiniteSets

CONSTANTS
    Clients,          \* the client peers
    Server,           \* the server peer (peer 0)
    MaxSessions,      \* client sessions over the whole seed
    MaxClientCrashes, \* client crashes per seed
    MaxServerCrashes, \* server crashes per seed
    MaxDisconnects,   \* clean client disconnects in the fault window
    MaxTime,          \* steps of true time modelled
    MaxInFlight,      \* packets the wire holds at once
    Scale,            \* clock units per step
    Offset,           \* [Peers -> Int], each peer's clock offset, in clock units
    Rate,             \* [Peers -> Int], each peer's clock units per step
    OffsetBound,      \* the design's offset bound, in clock units
    DriftBound        \* the design's rate bound: |Rate[p] - Scale| <= DriftBound

Peers == Clients \cup {Server}

ASSUME /\ Clients # {}
       /\ Server \notin Clients
       /\ MaxClientCrashes \in Nat /\ MaxServerCrashes \in Nat
       /\ MaxDisconnects \in Nat /\ MaxTime \in Nat
       /\ MaxInFlight \in Nat \ {0}
       /\ Scale \in Nat \ {0}
       /\ OffsetBound \in Nat /\ DriftBound \in Nat
       /\ Offset \in [Peers -> Int] /\ Rate \in [Peers -> Int]
       \* Enough sessions for every reconnect the faults can force: one per
       \* client, one per disconnect and client crash, and one per client for
       \* each server crash.
       /\ MaxSessions \in Nat
       /\ MaxSessions >= Cardinality(Clients) * (1 + MaxServerCrashes)
                         + MaxClientCrashes + MaxDisconnects

FreshOnRestart == TRUE
CrashSilences  == TRUE

Phases   == {"up", "crashed", "restarting"}
CStates  == {"idle", "connecting", "connected"}
Gens     == 1..MaxSessions
Sids     == 1..MaxSessions

\* A server session record: one per client_id the server holds.
SrvRecs  == [c : Clients, gen : Gens, sid : Sids, inc : Nat]
\* A packet in the wire: direction, the client end, the stamped fields, and
\* the ghost tag of the sending session.
Packets  == [dir : {"up", "down"}, c : Clients, gen : Gens, inc : Nat, sid : Sids]

VARIABLES
    phase,        \* [Peers -> Phases]
    cst,          \* [Clients -> CStates], the client's session state
    gen,          \* [Clients -> 0..MaxSessions], the generation of its latest session
    sid,          \* [Clients -> 0..MaxSessions], ghost tag of its latest session
    cinc,         \* [Clients -> Nat], the incarnation its session was admitted under
    restarted,    \* [Clients -> BOOLEAN], rebuilt and not yet connected since
    nextGen,      \* the global generation counter (starts at 1)
    nextSid,      \* the ghost tag counter
    genFirst,     \* [Gens -> 0..MaxSessions], the tag of the first session to take each generation
    incarnation,  \* the server incarnation counter (starts at 1)
    srv,          \* the server's session records
    net,          \* packets in the wire
    t,            \* true time, in steps
    clock,        \* [Peers -> Int], each peer's skewed clock
    faults,       \* TRUE in the fault window
    clientCrashes, serverCrashes, disconnects,
    lastAct,      \* the phase of the peer at the latest send or receive (ghost)
    lastDel       \* the latest delivery: did the packet's ghost tag equal the
                  \* receiving session's, and its incarnation the session's (ghost)

vars == <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid, genFirst,
          incarnation, srv, net, t, clock, faults, clientCrashes, serverCrashes,
          disconnects, lastAct, lastDel>>

\* A peer that runs: up, or any phase when a crash leaves it running.
Running(p) == phase[p] = "up" \/ ~CrashSilences

Init ==
    /\ phase = [p \in Peers |-> "up"]
    /\ cst = [c \in Clients |-> "idle"]
    /\ gen = [c \in Clients |-> 0]
    /\ sid = [c \in Clients |-> 0]
    /\ cinc = [c \in Clients |-> 0]
    /\ restarted = [c \in Clients |-> FALSE]
    /\ nextGen = 1
    /\ nextSid = 1
    /\ genFirst = [g \in Gens |-> 0]
    /\ incarnation = 1
    /\ srv = {}
    /\ net = {}
    /\ t = 0
    /\ clock = [p \in Peers |-> Offset[p]]
    /\ faults = TRUE
    /\ clientCrashes = 0 /\ serverCrashes = 0 /\ disconnects = 0
    /\ lastAct = "none"
    /\ lastDel = [session |-> TRUE, inc |-> TRUE]

-----------------------------------------------------------------------------
(* The injector's own actions.                                             *)

\* A crash destroys the peer's objects at once and sends nothing.  A crash
\* due while the peer is down is skipped (not enabled).
Crash(p) ==
    /\ faults
    /\ phase[p] = "up"
    /\ IF p = Server
         THEN /\ serverCrashes < MaxServerCrashes
              /\ serverCrashes' = serverCrashes + 1
              /\ srv' = IF CrashSilences THEN {} ELSE srv
              /\ UNCHANGED <<clientCrashes, cst>>
         ELSE /\ clientCrashes < MaxClientCrashes
              /\ clientCrashes' = clientCrashes + 1
              /\ cst' = IF CrashSilences THEN [cst EXCEPT ![p] = "idle"] ELSE cst
              /\ UNCHANGED <<serverCrashes, srv>>
    /\ phase' = [phase EXCEPT ![p] = "crashed"]
    /\ UNCHANGED <<gen, sid, cinc, restarted, nextGen, nextSid, genFirst,
                   incarnation, net, t, clock, faults, disconnects, lastAct, lastDel>>

\* The down time ends: the injector starts rebuilding the peer.
Restart(p) ==
    /\ phase[p] = "crashed"
    /\ phase' = [phase EXCEPT ![p] = "restarting"]
    /\ UNCHANGED <<cst, gen, sid, cinc, restarted, nextGen, nextSid, genFirst,
                   incarnation, srv, net, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

\* The peer's objects are built fresh.  The server takes a new incarnation
\* and holds no session; a client starts idle and connects next.
Rejoin(p) ==
    /\ phase[p] = "restarting"
    /\ phase' = [phase EXCEPT ![p] = "up"]
    /\ IF p = Server
         THEN /\ incarnation' = incarnation + 1
              /\ srv' = {}
              /\ UNCHANGED <<cst, restarted>>
         ELSE /\ cst' = [cst EXCEPT ![p] = "idle"]
              /\ restarted' = [restarted EXCEPT ![p] = TRUE]
              /\ UNCHANGED <<incarnation, srv>>
    /\ UNCHANGED <<gen, sid, cinc, nextGen, nextSid, genFirst, net, t, clock,
                   faults, clientCrashes, serverCrashes, disconnects, lastAct, lastDel>>

\* True time advances one step; each peer's clock moves by its rate.  The
\* wire runs on true time; clocks run whatever the peer's phase.
Tick ==
    /\ t < MaxTime
    /\ t' = t + 1
    /\ clock' = [p \in Peers |-> clock[p] + Rate[p]]
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, srv, net, faults, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

\* The fault window closes; every fault has ended inside it.
EndFaults ==
    /\ faults
    /\ \A p \in Peers : phase[p] = "up"
    /\ faults' = FALSE
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, srv, net, t, clock, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

\* Loss inside the fault window.
Lose(m) ==
    /\ faults
    /\ m \in net
    /\ net' = net \ {m}
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, srv, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

-----------------------------------------------------------------------------
(* The outside events: the session protocols as the injector sees them.    *)

\* A client mints a token for a new session and begins its handshake.
Connect(c) ==
    /\ Running(c)
    /\ cst[c] = "idle"
    /\ nextSid <= MaxSessions
    /\ LET keep == ~FreshOnRestart /\ restarted[c] /\ gen[c] # 0
           g    == IF keep THEN gen[c] ELSE nextGen
       IN /\ g \in Gens
          /\ gen' = [gen EXCEPT ![c] = g]
          /\ nextGen' = IF keep THEN nextGen ELSE nextGen + 1
          /\ genFirst' = IF genFirst[g] = 0 THEN [genFirst EXCEPT ![g] = nextSid]
                                            ELSE genFirst
    /\ sid' = [sid EXCEPT ![c] = nextSid]
    /\ nextSid' = nextSid + 1
    /\ cst' = [cst EXCEPT ![c] = "connecting"]
    /\ restarted' = [restarted EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<phase, cinc, incarnation, srv, net, t, clock, faults,
                   clientCrashes, serverCrashes, disconnects, lastAct, lastDel>>

\* The handshake completes: the server admits the client_id (generation and
\* client index) under its current incarnation.  A client_id the server
\* already holds is not admitted again until that session times out.
Establish(c) ==
    /\ Running(c) /\ phase[Server] = "up"
    /\ cst[c] = "connecting"
    /\ ~\E r \in srv : r.c = c /\ r.gen = gen[c]
    /\ srv' = srv \cup {[c |-> c, gen |-> gen[c], sid |-> sid[c], inc |-> incarnation]}
    /\ cinc' = [cinc EXCEPT ![c] = incarnation]
    /\ cst' = [cst EXCEPT ![c] = "connected"]
    /\ UNCHANGED <<phase, gen, sid, restarted, nextGen, nextSid, genFirst,
                   incarnation, net, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

\* A clean disconnect inside the fault window: both ends drop the session.
Disconnect(c) ==
    /\ faults
    /\ disconnects < MaxDisconnects
    /\ phase[c] = "up" /\ phase[Server] = "up"
    /\ cst[c] = "connected"
    /\ disconnects' = disconnects + 1
    /\ cst' = [cst EXCEPT ![c] = "idle"]
    /\ srv' = {r \in srv : r.sid # sid[c]}
    /\ UNCHANGED <<phase, gen, sid, cinc, restarted, nextGen, nextSid, genFirst,
                   incarnation, net, t, clock, faults, clientCrashes,
                   serverCrashes, lastAct, lastDel>>

\* A client whose session the server no longer holds hears silence and
\* times out (5 s); it reconnects under a new generation.
ClientTimeout(c) ==
    /\ phase[c] = "up"
    /\ cst[c] = "connected"
    /\ ~\E r \in srv : r.sid = sid[c]
    /\ cst' = [cst EXCEPT ![c] = "idle"]
    /\ UNCHANGED <<phase, gen, sid, cinc, restarted, nextGen, nextSid, genFirst,
                   incarnation, srv, net, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

\* The server times out a session whose client no longer runs it.
ClientLive(r) == phase[r.c] = "up" /\ cst[r.c] = "connected" /\ sid[r.c] = r.sid
ServerTimeout(r) ==
    /\ phase[Server] = "up"
    /\ r \in srv
    /\ ~ClientLive(r)
    /\ srv' = srv \ {r}
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, net, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

\* A connected client sends a payload stamped with its generation and the
\* incarnation it was admitted under.
ClientSend(c) ==
    /\ Running(c)
    /\ cst[c] = "connected"
    /\ Cardinality(net) < MaxInFlight
    /\ net' = net \cup {[dir |-> "up", c |-> c, gen |-> gen[c], inc |-> cinc[c], sid |-> sid[c]]}
    /\ lastAct' = phase[c]
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, srv, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastDel>>

\* The server sends a payload on a session it holds.
ServerSend(r) ==
    /\ Running(Server)
    /\ r \in srv
    /\ Cardinality(net) < MaxInFlight
    /\ net' = net \cup {[dir |-> "down", c |-> r.c, gen |-> r.gen, inc |-> r.inc, sid |-> r.sid]}
    /\ lastAct' = phase[Server]
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, srv, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastDel>>

\* The receiver's session a packet matches, by the two separate field
\* compares: the generation against the session's generation, and the
\* incarnation against the session's incarnation.
ServerMatch(m) ==
    {r \in srv : r.c = m.c /\ r.gen = m.gen /\ m.inc = incarnation}
ClientMatches(m) ==
    cst[m.c] = "connected" /\ m.gen = gen[m.c] /\ m.inc = cinc[m.c]

Receiver(m) == IF m.dir = "up" THEN Server ELSE m.c

\* A packet reaches a running receiver and is delivered to the session it
\* matches.
Deliver(m) ==
    /\ m \in net
    /\ Running(Receiver(m))
    /\ net' = net \ {m}
    /\ IF m.dir = "up"
         THEN \E r \in ServerMatch(m) :
                lastDel' = [session |-> m.sid = r.sid, inc |-> m.inc = r.inc]
         ELSE /\ ClientMatches(m)
              /\ lastDel' = [session |-> m.sid = sid[m.c], inc |-> m.inc = cinc[m.c]]
    /\ lastAct' = phase[Receiver(m)]
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, srv, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects>>

\* A packet no session takes: refused by the receiver (a stale generation
\* or incarnation), or received and discarded for a peer that is down.
Refuse(m) ==
    /\ m \in net
    /\ \/ ~Running(Receiver(m))
       \/ m.dir = "up" /\ ServerMatch(m) = {}
       \/ m.dir = "down" /\ ~ClientMatches(m)
    /\ net' = net \ {m}
    /\ UNCHANGED <<phase, cst, gen, sid, cinc, restarted, nextGen, nextSid,
                   genFirst, incarnation, srv, t, clock, faults, clientCrashes,
                   serverCrashes, disconnects, lastAct, lastDel>>

Next ==
    \/ \E p \in Peers : Crash(p) \/ Restart(p) \/ Rejoin(p)
    \/ Tick \/ EndFaults
    \/ \E m \in net : Lose(m) \/ Deliver(m) \/ Refuse(m)
    \/ \E c \in Clients : Connect(c) \/ Establish(c) \/ Disconnect(c)
                          \/ ClientTimeout(c) \/ ClientSend(c)
    \/ \E r \in srv : ServerTimeout(r) \/ ServerSend(r)

Spec == Init /\ [][Next]_vars

\* Every fault ends (down times are finite and the window closes); the
\* session protocols make progress.  No fairness on crashes, disconnects,
\* loss, sends or deliveries.
Fairness ==
    /\ \A p \in Peers : WF_vars(Restart(p)) /\ WF_vars(Rejoin(p))
    /\ \A c \in Clients : WF_vars(Connect(c)) /\ WF_vars(Establish(c))
                          /\ WF_vars(ClientTimeout(c))
    /\ WF_vars(\E r \in srv : ServerTimeout(r))
    /\ WF_vars(EndFaults)

LiveSpec == Spec /\ Fairness

-----------------------------------------------------------------------------
(* Invariants and properties.                                              *)

TypeOK ==
    /\ phase \in [Peers -> Phases]
    /\ cst \in [Clients -> CStates]
    /\ gen \in [Clients -> 0..MaxSessions]
    /\ sid \in [Clients -> 0..MaxSessions]
    /\ cinc \in [Clients -> Nat]
    /\ restarted \in [Clients -> BOOLEAN]
    /\ nextGen \in 1..(MaxSessions + 1)
    /\ nextSid \in 1..(MaxSessions + 1)
    /\ genFirst \in [Gens -> 0..MaxSessions]
    /\ incarnation \in 1..(1 + MaxServerCrashes)
    /\ srv \subseteq SrvRecs
    /\ net \subseteq Packets
    /\ t \in 0..MaxTime
    /\ clock \in [Peers -> Int]
    /\ faults \in BOOLEAN
    /\ lastAct \in {"none"} \cup Phases
    /\ lastDel \in [session : BOOLEAN, inc : BOOLEAN]

\* A peer that is not up sends nothing and receives nothing.
CrashedPeerSilent ==
    lastAct \in {"none", "up"}

\* Every live session's generation was first taken by that session: a
\* restarted (or reconnecting) client's generation is fresh, never an
\* earlier one.
RestartIsFresh ==
    \A c \in Clients : cst[c] # "idle" => genFirst[gen[c]] = sid[c]

\* No delivery reaches a session other than the one that sent it: the
\* receiving session's ghost tag equals the packet's, and the packet's
\* incarnation equals the receiving session's.
DeliverOnlyToLiveGeneration ==
    lastDel.session /\ lastDel.inc

\* The incarnation is a separate counter: every session the server holds
\* was admitted under its current incarnation.
LiveSessionOnCurrentIncarnation ==
    \A r \in srv : r.inc = incarnation

\* Every clock stays inside the design's bound of the true clock.
SkewBounded ==
    \A p \in Peers : LET d == clock[p] - Scale * t
                     IN -(OffsetBound + DriftBound * t) <= d
                        /\ d <= OffsetBound + DriftBound * t

\* No clock runs backwards.
ClockMonotone ==
    [][\A p \in Peers : clock'[p] >= clock[p]]_vars

\* Once the faults stop, every client is up, connected, and held by the
\* server.
AllConnected ==
    \A c \in Clients : /\ phase[c] = "up"
                       /\ cst[c] = "connected"
                       /\ \E r \in srv : r.sid = sid[c]
EveryLiveClientConnects ==
    <>[](~faults /\ AllConnected)
=============================================================================
