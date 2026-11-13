--------------------------- MODULE RNCreateUnwind ---------------------------
(***************************************************************************)
(* Who owns a transport whose create fails after its I/O worker started.  *)
(* The create holds the transport while it builds; on success it hands    *)
(* it to the caller as the returned handle. A failure unwinds: the        *)
(* transport is told to stop and the create waits for the worker to exit  *)
(* before freeing it, because the worker still reads it.                   *)
(*                                                                         *)
(* Two places fail after the worker started:                               *)
(* - the transport's own create (rocketnet_server_transport_abort_create,  *)
(*   sdk/rocketnet_server_transport.cpp). A caller with a retained slot    *)
(*   (rocketnet_server_start's failed_start, sdk/rocketnet_api.cpp) gets   *)
(*   a bounded wait: on its deadline the transport is handed to that slot, *)
(*   which frees it once the worker has exited (RetainedFrees). A caller   *)
(*   with no slot gets a wait with no deadline.                            *)
(* - the message object's create after its transport was created           *)
(*   (rn_message_link_abort_create, sdk/rocketnet_message_server.cpp). It  *)
(*   has no slot to hand the transport to, so its wait has no deadline.    *)
(*                                                                         *)
(* A wait with no deadline returns only once the worker exits: the create  *)
(* blocks for as long as the worker runs. That is the price of the rules   *)
(* this module checks. LiveTransportOwned: a transport not freed always    *)
(* has an owner, the create, the caller's handle or the retained slot.     *)
(* FreedOnlyAfterExit: it is never freed under its running worker.    *)
(*                                                                         *)
(* The wait is counted in polls: Budget polls stand for the 1 s deadline.  *)
(* Reversed witnesses, one constant: Defect = "boundedunwind" gives the    *)
(* message create's unwind the deadline (the code before this rule: the    *)
(* create returned with the transport live and no owner); "noretain"     *)
(* makes the deadline drop the transport even with a slot (the server     *)
(* start before failed_start); "freeatdeadline" frees the transport at   *)
(* the deadline under the running worker.                                  *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Budget, Defect

ASSUME Budget \in Nat \ {0}
ASSUME Defect \in {"none", "boundedunwind", "noretain", "freeatdeadline"}

VARIABLES step,     \* "idle", "built", "unwinding", "returned"
          layer,    \* where the create failed: "none", "transport", "message"
          retained, \* the caller passed a retained slot (fixed at Init)
          worker,   \* "up", "stopping", "exited"
          polls,    \* polls of the unwind's wait, saturating at Budget
          owner,    \* "create", "caller", "retained", "none"
          freed     \* the transport was destroyed

vars == <<step, layer, retained, worker, polls, owner, freed>>

Init ==
    /\ step = "idle" /\ layer = "none" /\ retained \in BOOLEAN
    /\ worker = "up" /\ polls = 0 /\ owner = "none" /\ freed = FALSE

\* The wait at this failure has the deadline.
Bounded ==
    \/ layer = "message" /\ Defect = "boundedunwind"
    \/ layer = "transport" /\ retained

\* The transport is created and its worker runs; the create holds it.
Build ==
    /\ step = "idle"
    /\ step' = "built" /\ owner' = "create"
    /\ UNCHANGED <<layer, retained, worker, polls, freed>>

\* The create returns the handle: the caller owns the transport.
Succeed ==
    /\ step = "built"
    /\ step' = "returned" /\ owner' = "caller"
    /\ UNCHANGED <<layer, retained, worker, polls, freed>>

\* The transport's own create fails after its worker started, and asks the
\* worker to stop.
FailInTransport ==
    /\ step = "built"
    /\ step' = "unwinding" /\ layer' = "transport" /\ worker' = "stopping"
    /\ UNCHANGED <<retained, polls, owner, freed>>

\* The message object's create fails after its transport was created, and
\* stops the transport.
FailInMessage ==
    /\ step = "built"
    /\ step' = "unwinding" /\ layer' = "message" /\ worker' = "stopping"
    /\ UNCHANGED <<retained, polls, owner, freed>>

\* The worker exits, at any time after it was told to stop. Spec makes it
\* exit eventually, for the liveness rules; the invariants hold on every
\* prefix, so they cover a worker that is still running at any length.
WorkerExits ==
    /\ worker = "stopping"
    /\ worker' = "exited"
    /\ UNCHANGED <<step, layer, retained, polls, owner, freed>>

\* One poll of the unwind's wait while the worker runs. A wait with no
\* deadline keeps polling past Budget (the count saturates).
UnwindWaits ==
    /\ step = "unwinding" /\ worker # "exited"
    /\ (~Bounded \/ polls < Budget)
    /\ polls' = IF polls < Budget THEN polls + 1 ELSE polls
    /\ UNCHANGED <<step, layer, retained, worker, owner, freed>>

\* The worker has exited: the unwind frees the transport and the create
\* returns no handle.
UnwindFrees ==
    /\ step = "unwinding" /\ worker = "exited"
    /\ step' = "returned" /\ freed' = TRUE /\ owner' = "none"
    /\ UNCHANGED <<layer, retained, worker, polls>>

\* A bounded wait's deadline with the worker still running: the transport
\* is handed to the retained slot, or, with no slot, left with no owner.
UnwindDeadline ==
    /\ step = "unwinding" /\ worker # "exited" /\ Bounded /\ polls = Budget
    /\ step' = "returned"
    /\ owner' = IF retained /\ layer = "transport" /\ Defect \notin {"noretain", "freeatdeadline"}
                THEN "retained" ELSE "none"
    /\ freed' = (Defect = "freeatdeadline")
    /\ UNCHANGED <<layer, retained, worker, polls>>

\* The next start, or the destroy, finds the retained worker exited and
\* frees the transport.
RetainedFrees ==
    /\ owner = "retained" /\ worker = "exited"
    /\ owner' = "none" /\ freed' = TRUE
    /\ UNCHANGED <<step, layer, retained, worker, polls>>

Next ==
    \/ Build \/ Succeed \/ FailInTransport \/ FailInMessage \/ WorkerExits
    \/ UnwindWaits \/ UnwindFrees \/ UnwindDeadline \/ RetainedFrees

Spec == Init /\ [][Next]_vars /\ WF_vars(WorkerExits) /\ WF_vars(UnwindFrees)
             /\ WF_vars(UnwindDeadline) /\ WF_vars(RetainedFrees)

TypeOK ==
    /\ step \in {"idle", "built", "unwinding", "returned"}
    /\ layer \in {"none", "transport", "message"}
    /\ retained \in BOOLEAN
    /\ worker \in {"up", "stopping", "exited"}
    /\ polls \in 0..Budget
    /\ owner \in {"create", "caller", "retained", "none"}
    /\ freed \in BOOLEAN

\* A transport that was created and not freed has an owner.
LiveTransportOwned == (step # "idle" /\ ~freed) => owner # "none"

\* A transport is freed only once its worker has exited.
FreedOnlyAfterExit == freed => worker = "exited"

\* The create owns the transport only while it runs.
CreateOwnsOnlyWhileCreating == owner = "create" => step \in {"built", "unwinding"}

\* Once the worker exits, every failed create's transport is freed.
UnwindEnds == (step = "unwinding") ~> (step = "returned")
FailedCreateFreed == (layer # "none" /\ worker = "exited") ~> freed
=============================================================================
