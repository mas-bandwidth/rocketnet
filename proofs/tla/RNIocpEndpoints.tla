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
(* shared retry queue, which any worker drains.  At stop each worker       *)
(* cancels its own endpoint's receives on its own socket, every pass, and  *)
(* exits once no operation is pending anywhere.                            *)
(*                                                                         *)
(* A post (rocketnet_iocp_post_receive) is steps of its worker, which     *)
(* does nothing else meanwhile: PostCount (pending.fetch_add), PostCheck   *)
(* (the stopping test: once stopping, the count is taken back and the      *)
(* operation retired) and PostAccept or PostRefuse (the kernel takes the   *)
(* receive, or refuses it into the retry queue and the count is taken      *)
(* back).  The count, the test, stop's store and the exit's loads are      *)
(* seq_cst in the code, one total order, which this interleaving is.  A    *)
(* worker posts the receive it dequeued and any receive it takes from the  *)
(* retry queue, whoever owns it.                                           *)
(*                                                                         *)
(* The datagram arrival is not fair: only cancellation ends a receive at   *)
(* stop.  Rule switches, each TRUE in the code:                            *)
(*   RepostOnOwner  a repost (and a retry) goes on the operation's own     *)
(*                  endpoint socket, not on the posting worker's;          *)
(*   CancelOwnSocket  worker e cancels on sockets[e], not on sockets[0]    *)
(*                  (the one socket before the endpoints);                 *)
(*   CountFirst     a post counts itself pending before it tests stopping, *)
(*                  not after (A01 of the v12 review: worker B, retrying   *)
(*                  endpoint A's receive, tests stopping false; stop is    *)
(*                  stored and A, seeing nothing pending, exits; B's post  *)
(*                  then lands on A's port, which has no drainer).         *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS E, R, RepostOnOwner, CancelOwnSocket, CountFirst

Eps == 0 .. E - 1
Ops == 0 .. E * R - 1
Owner(o) == o \div R
NONE == -1

VARIABLES st, on, cxl, stopping, exited, ph, cur

vars == << st, on, cxl, stopping, exited, ph, cur >>

Pending == { o \in Ops : st[o] \in { "posted", "queued" } }

\* a worker's post phases in order, from its first step
FirstPhase == IF CountFirst THEN "count" ELSE "check"
AfterCount == IF CountFirst THEN "check" ELSE "issue"
AfterCheck == IF CountFirst THEN "issue" ELSE "count"

\* the workers whose post is counted pending and not yet given to the kernel
Counted == { e \in Eps : ph[e] \in ( IF CountFirst THEN { "check", "issue" } ELSE { "issue" } ) }

\* where a worker posts operation o
PostOn(e, o) == IF RepostOnOwner THEN Owner(o) ELSE e

\* the socket worker e cancels its receives on
CancelOn(e) == IF CancelOwnSocket THEN e ELSE 0

TypeOK ==
    /\ st \in [ Ops -> { "posted", "queued", "retry", "held", "idle" } ]
    /\ on \in [ Ops -> Eps ]
    /\ cxl \in [ Ops -> BOOLEAN ]
    /\ stopping \in BOOLEAN
    /\ exited \in [ Eps -> BOOLEAN ]
    /\ ph \in [ Eps -> { "none", "count", "check", "issue" } ]
    /\ cur \in [ Eps -> Ops \cup { NONE } ]

\* create posts every receive on its own endpoint
Init ==
    /\ st = [ o \in Ops |-> "posted" ]
    /\ on = [ o \in Ops |-> Owner(o) ]
    /\ cxl = [ o \in Ops |-> FALSE ]
    /\ stopping = FALSE
    /\ exited = [ e \in Eps |-> FALSE ]
    /\ ph = [ e \in Eps |-> "none" ]
    /\ cur = [ e \in Eps |-> NONE ]

\* a datagram completes a posted receive: its completion is queued on the
\* port of the socket it was posted on
Arrive(o) ==
    /\ st[o] = "posted"
    /\ st' = [ st EXCEPT ![o] = "queued" ]
    /\ UNCHANGED << on, cxl, stopping, exited, ph, cur >>

\* CancelIoEx took effect: the receive completes as aborted
CancelDone(o) ==
    /\ st[o] = "posted"
    /\ cxl[o]
    /\ st' = [ st EXCEPT ![o] = "queued" ]
    /\ UNCHANGED << on, cxl, stopping, exited, ph, cur >>

\* worker e takes a completion from its own port and reposts the operation
Dequeue(e, o) ==
    /\ ~exited[e] /\ ph[e] = "none"
    /\ st[o] = "queued"
    /\ on[o] = e
    /\ st' = [ st EXCEPT ![o] = "held" ]
    /\ ph' = [ ph EXCEPT ![e] = FirstPhase ]
    /\ cur' = [ cur EXCEPT ![e] = o ]
    /\ UNCHANGED << on, cxl, stopping, exited >>

\* worker e's retry pass takes a refused receive, whoever owns it, and
\* posts it again; at exit the queue is abandoned with the transport (not
\* pending)
Retry(e, o) ==
    /\ ~exited[e] /\ ph[e] = "none"
    /\ st[o] = "retry"
    /\ st' = [ st EXCEPT ![o] = "held" ]
    /\ ph' = [ ph EXCEPT ![e] = FirstPhase ]
    /\ cur' = [ cur EXCEPT ![e] = o ]
    /\ UNCHANGED << on, cxl, stopping, exited >>

\* the post's pending.fetch_add
PostCount(e) ==
    /\ ph[e] = "count"
    /\ ph' = [ ph EXCEPT ![e] = AfterCount ]
    /\ UNCHANGED << st, on, cxl, stopping, exited, cur >>

\* the post's stopping test: stopping, the count is taken back (if taken)
\* and the operation retired
PostCheck(e) ==
    /\ ph[e] = "check"
    /\ IF stopping
       THEN /\ st' = [ st EXCEPT ![cur[e]] = "idle" ]
            /\ ph' = [ ph EXCEPT ![e] = "none" ]
            /\ cur' = [ cur EXCEPT ![e] = NONE ]
       ELSE /\ ph' = [ ph EXCEPT ![e] = AfterCheck ]
            /\ UNCHANGED << st, cur >>
    /\ UNCHANGED << on, cxl, stopping, exited >>

\* the kernel takes the receive, on the socket PostOn names
PostAccept(e) ==
    /\ ph[e] = "issue"
    /\ st' = [ st EXCEPT ![cur[e]] = "posted" ]
    /\ on' = [ on EXCEPT ![cur[e]] = PostOn(e, cur[e]) ]
    /\ cxl' = [ cxl EXCEPT ![cur[e]] = FALSE ]
    /\ ph' = [ ph EXCEPT ![e] = "none" ]
    /\ cur' = [ cur EXCEPT ![e] = NONE ]
    /\ UNCHANGED << stopping, exited >>

\* the kernel refuses the receive: the count is taken back and it waits on
\* the retry queue
PostRefuse(e) ==
    /\ ph[e] = "issue"
    /\ st' = [ st EXCEPT ![cur[e]] = "retry" ]
    /\ ph' = [ ph EXCEPT ![e] = "none" ]
    /\ cur' = [ cur EXCEPT ![e] = NONE ]
    /\ UNCHANGED << on, cxl, stopping, exited >>

Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ UNCHANGED << st, on, cxl, exited, ph, cur >>

\* worker e, stopping, cancels its endpoint's posted receives on the socket
\* it names; a cancel on a socket the receive is not posted on finds nothing
Cancel(e) ==
    /\ ~exited[e] /\ ph[e] = "none"
    /\ stopping
    /\ \E o \in Ops : st[o] = "posted" /\ Owner(o) = e /\ on[o] = CancelOn(e) /\ ~cxl[o]
    /\ cxl' = [ o \in Ops |-> IF st[o] = "posted" /\ Owner(o) = e /\ on[o] = CancelOn(e) THEN TRUE ELSE cxl[o] ]
    /\ UNCHANGED << st, on, stopping, exited, ph, cur >>

\* the exit test: stopping and the pending count zero (nothing in the
\* kernel or on a port, and no post counted)
Exit(e) ==
    /\ ~exited[e] /\ ph[e] = "none"
    /\ stopping
    /\ Pending = {}
    /\ Counted = {}
    /\ exited' = [ exited EXCEPT ![e] = TRUE ]
    /\ UNCHANGED << st, on, cxl, stopping, ph, cur >>

\* every worker has exited: the transport is destroyed
Done ==
    /\ \A e \in Eps : exited[e]
    /\ UNCHANGED vars

Next ==
    \/ \E o \in Ops : Arrive(o) \/ CancelDone(o)
    \/ \E e \in Eps, o \in Ops : Dequeue(e, o) \/ Retry(e, o)
    \/ \E e \in Eps : PostCount(e) \/ PostCheck(e) \/ PostAccept(e) \/ PostRefuse(e)
    \/ Stop
    \/ \E e \in Eps : Cancel(e) \/ Exit(e)
    \/ Done

Spec ==
    /\ Init
    /\ [][ Next ]_vars
    /\ \A o \in Ops : WF_vars( CancelDone(o) )
    /\ \A e \in Eps, o \in Ops : WF_vars( Dequeue(e, o) )
    /\ \A e \in Eps : WF_vars( PostCount(e) ) /\ WF_vars( PostCheck(e) ) /\ WF_vars( PostAccept(e) )
    /\ \A e \in Eps : WF_vars( Cancel(e) ) /\ WF_vars( Exit(e) )
    /\ WF_vars( Stop )

\* a receive in the kernel or on a port is on its owner's socket, so its
\* completion is drained by its owner, the worker that cancels it
OnOwnerSocket == \A o \in Pending : on[o] = Owner(o)

\* a worker exits only when nothing is pending anywhere, and after it
\* exits nothing is ever posted again
ExitedOnlyDrained == \A e \in Eps : exited[e] => Pending = {}

\* every worker exits after stop
EveryWorkerExits == stopping ~> \A e \in Eps : exited[e]
=============================================================================
