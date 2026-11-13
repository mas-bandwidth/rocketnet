--------------------------- MODULE RNIocpEndpoints ---------------------------
(***************************************************************************)
(* The IOCP server transport's endpoints (sdk/rocketnet_server_iocp_       *)
(* windows.cpp, rocketnet_server_iocp_t sockets and ports).  Several       *)
(* workers are as many endpoints: endpoint e has its own socket, given     *)
(* its processor by SIO_CPU_AFFINITY, and its own completion port, which  *)
(* only worker e drains.  Receive operation o belongs to endpoint          *)
(* o \div R (rocketnet_iocp_op_endpoint) and is always posted on that      *)
(* endpoint's socket, whoever posts it (rocketnet_iocp_op_socket), so its  *)
(* completion lands on its owner's port.  A refused post goes to the one   *)
(* shared retry queue, which any worker drains while running.  At stop     *)
(* each worker cancels its own endpoint's receives on its own socket,      *)
(* every pass, and exits once no operation is pending anywhere.           *)
(*                                                                         *)
(* The datagram arrival is not fair: only cancellation ends a receive at   *)
(* stop.  Rule switches, each TRUE in the code:                            *)
(*   RepostOnOwner  a repost (and a retry) goes on the operation's own     *)
(*                  endpoint socket, not on the posting worker's;          *)
(*   CancelOwnSocket  worker e cancels on sockets[e], not on sockets[0]    *)
(*                  (the one socket before the endpoints).                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS E, R, RepostOnOwner, CancelOwnSocket

Eps == 0 .. E - 1
Ops == 0 .. E * R - 1
Owner(o) == o \div R

VARIABLES st, on, cxl, stopping, exited

vars == << st, on, cxl, stopping, exited >>

Pending == { o \in Ops : st[o] \in { "posted", "queued" } }

\* where a worker posts operation o
PostOn(e, o) == IF RepostOnOwner THEN Owner(o) ELSE e

\* the socket worker e cancels its receives on
CancelOn(e) == IF CancelOwnSocket THEN e ELSE 0

TypeOK ==
    /\ st \in [ Ops -> { "posted", "queued", "retry", "idle" } ]
    /\ on \in [ Ops -> Eps ]
    /\ cxl \in [ Ops -> BOOLEAN ]
    /\ stopping \in BOOLEAN
    /\ exited \in [ Eps -> BOOLEAN ]

\* create posts every receive on its own endpoint
Init ==
    /\ st = [ o \in Ops |-> "posted" ]
    /\ on = [ o \in Ops |-> Owner(o) ]
    /\ cxl = [ o \in Ops |-> FALSE ]
    /\ stopping = FALSE
    /\ exited = [ e \in Eps |-> FALSE ]

\* a datagram completes a posted receive: its completion is queued on the
\* port of the socket it was posted on
Arrive(o) ==
    /\ st[o] = "posted"
    /\ st' = [ st EXCEPT ![o] = "queued" ]
    /\ UNCHANGED << on, cxl, stopping, exited >>

\* CancelIoEx took effect: the receive completes as aborted
CancelDone(o) ==
    /\ st[o] = "posted"
    /\ cxl[o]
    /\ st' = [ st EXCEPT ![o] = "queued" ]
    /\ UNCHANGED << on, cxl, stopping, exited >>

\* worker e takes a completion from its own port: running, it reposts (or
\* the post is refused into the retry queue); stopping, it retires it
Dequeue(e, o) ==
    /\ ~exited[e]
    /\ st[o] = "queued"
    /\ on[o] = e
    /\ IF stopping
       THEN /\ st' = [ st EXCEPT ![o] = "idle" ]
            /\ UNCHANGED << on, cxl >>
       ELSE \/ /\ st' = [ st EXCEPT ![o] = "posted" ]
               /\ on' = [ on EXCEPT ![o] = PostOn(e, o) ]
               /\ cxl' = [ cxl EXCEPT ![o] = FALSE ]
            \/ /\ st' = [ st EXCEPT ![o] = "retry" ]
               /\ UNCHANGED << on, cxl >>
    /\ UNCHANGED << stopping, exited >>

\* any running worker reposts a refused receive; at stop the queue is
\* abandoned with the transport (not pending)
Retry(e, o) ==
    /\ ~exited[e]
    /\ ~stopping
    /\ st[o] = "retry"
    /\ st' = [ st EXCEPT ![o] = "posted" ]
    /\ on' = [ on EXCEPT ![o] = PostOn(e, o) ]
    /\ cxl' = [ cxl EXCEPT ![o] = FALSE ]
    /\ UNCHANGED << stopping, exited >>

Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ UNCHANGED << st, on, cxl, exited >>

\* worker e, stopping, cancels its endpoint's posted receives on the socket
\* it names; a cancel on a socket the receive is not posted on finds nothing
Cancel(e) ==
    /\ ~exited[e]
    /\ stopping
    /\ \E o \in Ops : st[o] = "posted" /\ Owner(o) = e /\ on[o] = CancelOn(e) /\ ~cxl[o]
    /\ cxl' = [ o \in Ops |-> IF st[o] = "posted" /\ Owner(o) = e /\ on[o] = CancelOn(e) THEN TRUE ELSE cxl[o] ]
    /\ UNCHANGED << st, on, stopping, exited >>

Exit(e) ==
    /\ ~exited[e]
    /\ stopping
    /\ Pending = {}
    /\ exited' = [ exited EXCEPT ![e] = TRUE ]
    /\ UNCHANGED << st, on, cxl, stopping >>

\* every worker has exited: the transport is destroyed
Done ==
    /\ \A e \in Eps : exited[e]
    /\ UNCHANGED vars

Next ==
    \/ \E o \in Ops : Arrive(o) \/ CancelDone(o)
    \/ \E e \in Eps, o \in Ops : Dequeue(e, o) \/ Retry(e, o)
    \/ Stop
    \/ \E e \in Eps : Cancel(e) \/ Exit(e)
    \/ Done

Spec ==
    /\ Init
    /\ [][ Next ]_vars
    /\ \A o \in Ops : WF_vars( CancelDone(o) )
    /\ \A e \in Eps, o \in Ops : WF_vars( Dequeue(e, o) )
    /\ \A e \in Eps : WF_vars( Cancel(e) ) /\ WF_vars( Exit(e) )
    /\ WF_vars( Stop )

\* a receive in the kernel or on a port is on its owner's socket, so its
\* completion is drained by its owner, the worker that cancels it
OnOwnerSocket == \A o \in Pending : on[o] = Owner(o)

\* a worker exits only when nothing is pending anywhere
ExitedOnlyDrained == \A e \in Eps : exited[e] => Pending = {}

\* every worker exits after stop
EveryWorkerExits == stopping ~> \A e \in Eps : exited[e]
=============================================================================
