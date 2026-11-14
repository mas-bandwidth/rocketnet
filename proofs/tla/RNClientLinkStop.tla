--------------------------- MODULE RNClientLinkStop ---------------------------
(***************************************************************************)
(* The caller stages the disconnect burst in the development simulator.   *)
(* On the legacy path Stop only publishes quit and transfers the queue to *)
(* the worker; WorkerComplete drains every surviving copy, including held *)
(* latency, before publishing stopped. Explicit step-driven tests run the *)
(* same completion action. On the I/O-core path the simulator is never    *)
(* transferred (the I/O worker does not know it), so Stop itself sends    *)
(* every copy it holds, ahead of its latency, before publishing quit: on  *)
(* that path the request does drain, synchronously, on the caller's       *)
(* thread (rocketnet_client_link_drain at DBL_MAX).                       *)
(* Defect restores a missing or due-only completion drain.                *)
(*                                                                         *)
(* wire is the copies sent and not yet read or lost. A copy leaves the    *)
(* simulator through the backend's send (operations->send), which returns *)
(* nothing: the send can fail, and UDP can lose the datagram on the way.  *)
(* ServerReceive is the server reading a copy, or the copy never reaching *)
(* it (dropped). Delivery is therefore claimed only for a burst some copy *)
(* of which survives both the loss policy and the network.                *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Copies,   \* ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS, small
          Defect    \* "none", "nodrain" or "dueonly"

ASSUME Copies \in Nat \ {0}
ASSUME Defect \in {"none", "nodrain", "dueonly"}

VARIABLES path,     \* "legacy" or "iocore", fixed at create
          phase,    \* "running", "stopping" (stop requested), "stopped"
          sealed,   \* the burst was sealed by disconnect
          due,      \* staged copies whose latency has elapsed
          held,     \* staged copies still inside their latency
          wire,     \* copies sent, not yet read by the server or lost
          lost,     \* copies the loss policy removed at send
          received, \* copies the server read
          freed,    \* the server freed the slot on a copy
          dropped   \* copies sent that never reached the server: a send that
                    \* failed, or a datagram the network lost

vars == <<path, phase, sealed, due, held, wire, lost, received, freed, dropped>>

Init ==
    /\ path \in {"legacy", "iocore"}
    /\ phase = "running" /\ sealed = FALSE
    /\ due = 0 /\ held = 0 /\ wire = 0 /\ lost = 0 /\ received = 0
    /\ freed = FALSE /\ dropped = 0

\* rocketnet_client_transport_disconnect, link simulator branch: each copy is
\* lost, staged due (no latency), or staged held (inside its latency).
Disconnect ==
    /\ phase = "running" /\ ~sealed
    /\ \E d \in 0..Copies : \E h \in 0..(Copies - d) :
         /\ due' = d /\ held' = h /\ lost' = Copies - d - h
    /\ sealed' = TRUE
    /\ UNCHANGED <<path, phase, wire, received, freed, dropped>>

\* A running update advances the clock: a held copy comes due.
Tick ==
    /\ phase = "running" /\ held > 0
    /\ held' = held - 1 /\ due' = due + 1
    /\ UNCHANGED <<path, phase, sealed, wire, lost, received, freed, dropped>>

\* A running update drains a due copy (rocketnet_client_link_drain).
Drain ==
    /\ phase = "running" /\ due > 0
    /\ due' = due - 1 /\ wire' = wire + 1
    /\ UNCHANGED <<path, phase, sealed, held, lost, received, freed, dropped>>

\* rocketnet_client_transport_stop. Legacy path: the caller publishes quit
\* without draining or invoking the backend. I/O-core path: the caller keeps
\* the simulator, so it sends every copy it holds (rocketnet_client_link_drain
\* at DBL_MAX) and then publishes quit.
Stop ==
    /\ phase = "running" /\ sealed
    /\ phase' = "stopping"
    /\ IF path = "legacy"
         THEN UNCHANGED <<due, held, wire>>
         ELSE due' = 0 /\ held' = 0 /\ wire' = wire + due + held
    /\ UNCHANGED <<path, sealed, lost, received, freed, dropped>>

\* The worker acquires quit, drains the transferred queue, then completes.
WorkerComplete ==
    /\ phase = "stopping"
    /\ phase' = "stopped"
    /\ CASE Defect = "nodrain" -> UNCHANGED <<due, held, wire>>
         [] Defect = "dueonly" -> due' = 0 /\ wire' = wire + due /\ UNCHANGED held
         [] OTHER              -> due' = 0 /\ held' = 0 /\ wire' = wire + due + held
    /\ UNCHANGED <<path, sealed, lost, received, freed, dropped>>

\* A sent copy's fate, outside the code: the server reads it and frees the
\* slot (the first one does it), or it never arrives: the backend's send
\* failed (operations->send returns nothing; the platform counts the
\* failure) or the network lost the datagram.
ServerReceive ==
    /\ wire > 0
    /\ wire' = wire - 1
    /\ \/ /\ received' = received + 1 /\ freed' = TRUE
          /\ UNCHANGED dropped
       \/ /\ dropped' = dropped + 1
          /\ UNCHANGED <<received, freed>>
    /\ UNCHANGED <<path, phase, sealed, due, held, lost>>

Next == Disconnect \/ Tick \/ Drain \/ Stop \/ WorkerComplete \/ ServerReceive

Spec == Init /\ [][Next]_vars
\* The caller keeps updating while running, the worker completes, and the
\* server keeps reading. Stop itself is the caller's choice: not fair.
FairSpec == Spec /\ WF_vars(Tick) /\ WF_vars(Drain) /\ WF_vars(WorkerComplete) /\ WF_vars(ServerReceive)

TypeOK ==
    /\ path \in {"legacy", "iocore"}
    /\ phase \in {"running", "stopping", "stopped"} /\ sealed \in BOOLEAN
    /\ due \in 0..Copies /\ held \in 0..Copies /\ wire \in 0..Copies
    /\ lost \in 0..Copies /\ received \in 0..Copies /\ freed \in BOOLEAN
    /\ dropped \in 0..Copies

\* Every copy is staged, sent, lost by the policy, dropped on the way or
\* read: none vanishes.
BurstAccounted == sealed => due + held + wire + lost + received + dropped = Copies

\* Completion, not the request, guarantees the transferred queue is empty.
StopLeavesNothingStaged == phase = "stopped" => due + held = 0

\* What the request does to the simulator's queue. On the legacy path it
\* does no backend work and the queue is unchanged (the worker's completion
\* drains it). On the I/O-core path it drains: it sends every staged copy
\* itself and leaves nothing staged. (The name is the legacy path's.)
StopDoesNotDrain == [][(phase = "running" /\ phase' = "stopping") =>
                        IF path = "legacy" THEN UNCHANGED <<due, held, wire>>
                                           ELSE due' + held' = 0]_vars

\* A burst with any copy surviving the loss policy reaches the server,
\* unless every surviving copy is dropped on the way (a failed send or a
\* lost datagram): what the code can promise over a send that returns
\* nothing.
BurstArrives == (sealed /\ lost < Copies) ~> (freed \/ lost + dropped = Copies)

\* Not a property of the code: the reach case
\* MCRNClientLinkStopReachAllDropped.cfg breaks it, showing that a burst
\* every sent copy of which is dropped never reaches the server.
BurstReachesServer == (sealed /\ lost < Copies) ~> freed
=============================================================================
