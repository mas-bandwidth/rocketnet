--------------------------- MODULE RNClientLinkStop ---------------------------
(***************************************************************************)
(* The caller stages the disconnect burst in the development simulator.   *)
(* Stop only publishes quit and transfers the queue to the worker.        *)
(* WorkerComplete drains every surviving copy, including held latency,    *)
(* before publishing stopped. Explicit step-driven tests run the same     *)
(* completion action. Defect restores a missing or due-only drain.        *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Copies,   \* ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS, small
          Defect    \* "none", "nodrain" or "dueonly"

ASSUME Copies \in Nat \ {0}
ASSUME Defect \in {"none", "nodrain", "dueonly"}

VARIABLES phase,    \* "running", "stopping" (stop requested), "stopped"
          sealed,   \* the burst was sealed by disconnect
          due,      \* staged copies whose latency has elapsed
          held,     \* staged copies still inside their latency
          wire,     \* copies in the server's receive queue
          lost,     \* copies the loss policy removed at send
          received, \* copies the server read
          freed     \* the server freed the slot on a copy

vars == <<phase, sealed, due, held, wire, lost, received, freed>>

Init ==
    /\ phase = "running" /\ sealed = FALSE
    /\ due = 0 /\ held = 0 /\ wire = 0 /\ lost = 0 /\ received = 0
    /\ freed = FALSE

\* rocketnet_client_transport_disconnect, link simulator branch: each copy is
\* lost, staged due (no latency), or staged held (inside its latency).
Disconnect ==
    /\ phase = "running" /\ ~sealed
    /\ \E d \in 0..Copies : \E h \in 0..(Copies - d) :
         /\ due' = d /\ held' = h /\ lost' = Copies - d - h
    /\ sealed' = TRUE
    /\ UNCHANGED <<phase, wire, received, freed>>

\* A running update advances the clock: a held copy comes due.
Tick ==
    /\ phase = "running" /\ held > 0
    /\ held' = held - 1 /\ due' = due + 1
    /\ UNCHANGED <<phase, sealed, wire, lost, received, freed>>

\* A running update drains a due copy (rocketnet_client_link_drain).
Drain ==
    /\ phase = "running" /\ due > 0
    /\ due' = due - 1 /\ wire' = wire + 1
    /\ UNCHANGED <<phase, sealed, held, lost, received, freed>>

\* The caller publishes quit without draining or invoking the backend.
Stop ==
    /\ phase = "running" /\ sealed
    /\ phase' = "stopping"
    /\ UNCHANGED <<sealed, due, held, wire, lost, received, freed>>

\* The worker acquires quit, drains the transferred queue, then completes.
WorkerComplete ==
    /\ phase = "stopping"
    /\ phase' = "stopped"
    /\ CASE Defect = "nodrain" -> UNCHANGED <<due, held, wire>>
         [] Defect = "dueonly" -> due' = 0 /\ wire' = wire + due /\ UNCHANGED held
         [] OTHER              -> due' = 0 /\ held' = 0 /\ wire' = wire + due + held
    /\ UNCHANGED <<sealed, lost, received, freed>>

\* The server reads a copy and frees the slot (the first one does it).
ServerReceive ==
    /\ wire > 0
    /\ wire' = wire - 1 /\ received' = received + 1 /\ freed' = TRUE
    /\ UNCHANGED <<phase, sealed, due, held, lost>>

Next == Disconnect \/ Tick \/ Drain \/ Stop \/ WorkerComplete \/ ServerReceive

Spec == Init /\ [][Next]_vars
\* The caller keeps updating while running, the worker completes, and the
\* server keeps reading. Stop itself is the caller's choice: not fair.
FairSpec == Spec /\ WF_vars(Tick) /\ WF_vars(Drain) /\ WF_vars(WorkerComplete) /\ WF_vars(ServerReceive)

TypeOK ==
    /\ phase \in {"running", "stopping", "stopped"} /\ sealed \in BOOLEAN
    /\ due \in 0..Copies /\ held \in 0..Copies /\ wire \in 0..Copies
    /\ lost \in 0..Copies /\ received \in 0..Copies /\ freed \in BOOLEAN

\* Every copy is staged, on the wire, lost or read: none vanishes.
BurstAccounted == sealed => due + held + wire + lost + received = Copies

\* Completion, not the request, guarantees the transferred queue is empty.
StopLeavesNothingStaged == phase = "stopped" => due + held = 0

\* Requesting stop does no backend work; the queue is unchanged.
StopDoesNotDrain == [][(phase = "running" /\ phase' = "stopping") => UNCHANGED <<due, held, wire>>]_vars

\* A burst with any copy surviving the loss policy reaches the server.
BurstArrives == (sealed /\ lost < Copies) ~> freed
=============================================================================
