------------------------------ MODULE SplitStart ------------------------------
(***************************************************************************)
(* The load_split start barrier (load_split.c, tools/load_split_barrier.h) *)
(* with discrete time.  Shards connect at any time; each shard stands for  *)
(* all of its clients.  The server waits ConnectWindow from its first      *)
(* connection for every shard, then sends ARM and waits PhaseWindow for    *)
(* every ACK, sends GO and waits PhaseWindow for every GO ACK, then sends  *)
(* RUN.  Every start message is reliable ordered: a loss is the SDK's      *)
(* resend, so each message arrives 1..Delay ticks after it is sent, and   *)
(* Delay <= RunTimeout, the reliable message timeout after which the SDK   *)
(* gives up on a connection.  A server that fails ends every connection;  *)
(* the end reaches each shard 1..Delay ticks later.                        *)
(*                                                                         *)
(* Rule "one" is the shard rule: one deadline from the shard's own         *)
(* connection, ConnectWindow + 2 * PhaseWindow + RunTimeout.  Rule         *)
(* "per_phase" is the rule it replaced: PhaseWindow from the shard's own   *)
(* connection for ARM, then PhaseWindow from each local step for GO and   *)
(* RUN.  Rule "no_run" is the one rule without the RunTimeout term.        *)
(*                                                                         *)
(* ServerIsTheAuthority: a shard gives up on its own clock only after the  *)
(* server has failed the start.  BoundedWait: no shard waits past its      *)
(* deadline.  RunOnlyAfterGo: a running shard was released by the server.  *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Shards, ConnectWindow, PhaseWindow, RunTimeout, Delay, MaxTime, Rule

ASSUME Delay >= 1 /\ Delay <= RunTimeout
ASSUME Rule \in {"one", "per_phase", "no_run"}

Kinds == {"arm", "ack", "go", "go_ack", "run", "end"}
None == -1

VARIABLES now, firstConnect, connectedAt, server, phaseStart,
          shard, step, deadline, reason, acked, goAcked, msg

vars == <<now, firstConnect, connectedAt, server, phaseStart,
          shard, step, deadline, reason, acked, goAcked, msg>>

Times == None .. MaxTime + Delay

TypeOK ==
    /\ now \in 0 .. MaxTime
    /\ firstConnect \in Times
    /\ connectedAt \in [Shards -> Times]
    /\ server \in {"connect", "arm", "go", "released", "failed"}
    /\ phaseStart \in Times
    /\ shard \in [Shards -> {"connecting", "waiting", "running", "failed"}]
    /\ step \in [Shards -> {"arm", "go", "run", "running"}]
    /\ deadline \in [Shards -> Int]
    /\ reason \in [Shards -> {"none", "timed_out", "disconnected"}]
    /\ acked \in [Shards -> BOOLEAN]
    /\ goAcked \in [Shards -> BOOLEAN]
    /\ msg \in [Kinds -> [Shards -> Times]]

Init ==
    /\ now = 0
    /\ firstConnect = None
    /\ connectedAt = [s \in Shards |-> None]
    /\ server = "connect"
    /\ phaseStart = None
    /\ shard = [s \in Shards |-> "connecting"]
    /\ step = [s \in Shards |-> "arm"]
    /\ deadline = [s \in Shards |-> 0]
    /\ reason = [s \in Shards |-> "none"]
    /\ acked = [s \in Shards |-> FALSE]
    /\ goAcked = [s \in Shards |-> FALSE]
    /\ msg = [k \in Kinds |-> [s \in Shards |-> None]]

\* Send one kind to every shard; each copy picks its own delay.
SendAll(kind) ==
    \E d \in [Shards -> 1 .. Delay] :
        msg' = [msg EXCEPT ![kind] = [s \in Shards |-> now + d[s]]]

Send(kind, s) ==
    \E d \in 1 .. Delay : msg' = [msg EXCEPT ![kind][s] = now + d]

AllConnected == \A s \in Shards : connectedAt[s] # None

\* A shard connects while the server still admits connections.
Connect(s) ==
    /\ server = "connect"
    /\ shard[s] = "connecting"
    /\ shard' = [shard EXCEPT ![s] = "waiting"]
    /\ connectedAt' = [connectedAt EXCEPT ![s] = now]
    /\ firstConnect' = IF firstConnect = None THEN now ELSE firstConnect
    /\ deadline' = [deadline EXCEPT ![s] =
           CASE Rule = "one" -> now + ConnectWindow + 2 * PhaseWindow + RunTimeout
             [] Rule = "no_run" -> now + ConnectWindow + 2 * PhaseWindow
             [] OTHER -> now + PhaseWindow]
    /\ UNCHANGED <<now, server, phaseStart, step, reason, acked, goAcked, msg>>

\* The population check comes before the window check, as in the server loop.
ServerArm ==
    /\ server = "connect"
    /\ AllConnected
    /\ server' = "arm"
    /\ phaseStart' = now
    /\ SendAll("arm")
    /\ UNCHANGED <<now, firstConnect, connectedAt, shard, step, deadline, reason, acked, goAcked>>

ServerGo ==
    /\ server = "arm"
    /\ \A s \in Shards : acked[s]
    /\ server' = "go"
    /\ phaseStart' = now
    /\ SendAll("go")
    /\ UNCHANGED <<now, firstConnect, connectedAt, shard, step, deadline, reason, acked, goAcked>>

ServerRun ==
    /\ server = "go"
    /\ \A s \in Shards : goAcked[s]
    /\ server' = "released"
    /\ SendAll("run")
    /\ UNCHANGED <<now, firstConnect, connectedAt, phaseStart, shard, step, deadline, reason, acked, goAcked>>

ServerExpired ==
    \/ server = "connect" /\ firstConnect # None /\ ~AllConnected /\ now - firstConnect >= ConnectWindow
    \/ server = "arm" /\ ~(\A s \in Shards : acked[s]) /\ now - phaseStart >= PhaseWindow
    \/ server = "go" /\ ~(\A s \in Shards : goAcked[s]) /\ now - phaseStart >= PhaseWindow

\* The server fails the start and ends every connection.
ServerFail ==
    /\ ServerExpired
    /\ server' = "failed"
    /\ SendAll("end")
    /\ UNCHANGED <<now, firstConnect, connectedAt, phaseStart, shard, step, deadline, reason, acked, goAcked>>

Arrives(kind, s) == msg[kind][s] = now

Consumed(kind, s) == msg' = [msg EXCEPT ![kind][s] = None]

\* The per-phase rule re-arms the shard deadline at each local step.
Rearm(s) ==
    deadline' = IF Rule = "per_phase" THEN [deadline EXCEPT ![s] = now + PhaseWindow] ELSE deadline

ShardArm(s) ==
    /\ Arrives("arm", s)
    /\ shard[s] = "waiting" /\ step[s] = "arm"
    /\ step' = [step EXCEPT ![s] = "go"]
    /\ Rearm(s)
    /\ \E d \in 1 .. Delay : msg' = [msg EXCEPT !["arm"][s] = None, !["ack"][s] = now + d]
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, shard, reason, acked, goAcked>>

ShardGo(s) ==
    /\ Arrives("go", s)
    /\ shard[s] = "waiting" /\ step[s] = "go"
    /\ step' = [step EXCEPT ![s] = "run"]
    /\ Rearm(s)
    /\ \E d \in 1 .. Delay : msg' = [msg EXCEPT !["go"][s] = None, !["go_ack"][s] = now + d]
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, shard, reason, acked, goAcked>>

ShardRun(s) ==
    /\ Arrives("run", s)
    /\ shard[s] = "waiting" /\ step[s] = "run"
    /\ step' = [step EXCEPT ![s] = "running"]
    /\ shard' = [shard EXCEPT ![s] = "running"]
    /\ Consumed("run", s)
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, deadline, reason, acked, goAcked>>

ServerAck(s) ==
    /\ Arrives("ack", s)
    /\ acked' = [acked EXCEPT ![s] = TRUE]
    /\ Consumed("ack", s)
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, shard, step, deadline, reason, goAcked>>

ServerGoAck(s) ==
    /\ Arrives("go_ack", s)
    /\ goAcked' = [goAcked EXCEPT ![s] = TRUE]
    /\ Consumed("go_ack", s)
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, shard, step, deadline, reason, acked>>

\* The end of a connection the server failed: a waiting shard stops on it.
ShardEnded(s) ==
    /\ Arrives("end", s)
    /\ shard' = [shard EXCEPT ![s] = IF shard[s] = "waiting" THEN "failed" ELSE shard[s]]
    /\ reason' = [reason EXCEPT ![s] = IF shard[s] = "waiting" THEN "disconnected" ELSE reason[s]]
    /\ Consumed("end", s)
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, step, deadline, acked, goAcked>>

\* ARM, GO or RUN arriving at a shard not waiting for it (a shard that
\* already failed) is dropped, as a destroyed client drops what reaches it.
Unwanted(kind, s) ==
    /\ kind \in {"arm", "go", "run"}
    /\ ~(shard[s] = "waiting" /\ step[s] = kind)

Stale(kind, s) ==
    /\ Arrives(kind, s)
    /\ Unwanted(kind, s)
    /\ Consumed(kind, s)
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, shard, step, deadline, reason, acked, goAcked>>

PendingFor(s) == \E k \in Kinds : msg[k][s] = now

ShardTimeout(s) ==
    /\ shard[s] = "waiting"
    /\ now >= deadline[s]
    /\ ~PendingFor(s)
    /\ shard' = [shard EXCEPT ![s] = "failed"]
    /\ reason' = [reason EXCEPT ![s] = "timed_out"]
    /\ UNCHANGED <<now, firstConnect, connectedAt, server, phaseStart, step, deadline, acked, goAcked, msg>>

\* Time passes only when nothing is due now: every arrival is handled, every
\* expired window has been acted on, and the server has sent what it can.
Urgent ==
    \/ \E k \in Kinds, s \in Shards : msg[k][s] = now
    \/ \E s \in Shards : shard[s] = "waiting" /\ now >= deadline[s]
    \/ ServerExpired
    \/ server = "connect" /\ AllConnected
    \/ server = "arm" /\ \A s \in Shards : acked[s]
    \/ server = "go" /\ \A s \in Shards : goAcked[s]

Tick ==
    /\ now < MaxTime
    /\ ~Urgent
    /\ now' = now + 1
    /\ UNCHANGED <<firstConnect, connectedAt, server, phaseStart, shard, step, deadline, reason, acked, goAcked, msg>>

Next ==
    \/ \E s \in Shards :
        \/ Connect(s) \/ ShardArm(s) \/ ShardGo(s) \/ ShardRun(s)
        \/ ServerAck(s) \/ ServerGoAck(s) \/ ShardEnded(s) \/ ShardTimeout(s)
        \/ \E k \in Kinds : Stale(k, s)
    \/ ServerArm \/ ServerGo \/ ServerRun \/ ServerFail
    \/ Tick

Spec == Init /\ [][Next]_vars

ServerIsTheAuthority ==
    \A s \in Shards : reason[s] = "timed_out" => server = "failed"

BoundedWait ==
    \A s \in Shards : shard[s] = "waiting" => now <= deadline[s]

RunOnlyAfterGo ==
    \A s \in Shards : shard[s] = "running" => server = "released"

\* Reversed witness: a start does complete for every shard.
NotAllRunning ==
    ~(\A s \in Shards : shard[s] = "running")

=============================================================================
