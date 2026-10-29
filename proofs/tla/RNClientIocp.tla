----------------------------- MODULE RNClientIocp -----------------------------
(***************************************************************************)
(* The iocp client transport's posted receives, as designed for           *)
(* sdk/rocketnet_client_iocp_windows.cpp (sdk/rocketnet_client_iocp.h):   *)
(* one socket associated with its own completion port, Buffers overlapped *)
(* WSARecvFrom receives posted at create, one I/O thread draining them.   *)
(*                                                                         *)
(* The state this adds to RNClientIo: between Post and its completion the *)
(* kernel owns a receive buffer, so the I/O thread must never read it and *)
(* destroy must never free it.                                            *)
(*                                                                         *)
(*   Post      the I/O thread posts a receive; the kernel owns the buffer *)
(*   Complete  the kernel finishes a posted receive (a datagram, or the   *)
(*             abort closesocket gives every outstanding receive) and     *)
(*             queues its completion on the port                          *)
(*   Dequeue   the I/O thread dequeues completions (receive_batch, wait   *)
(*             and the drain all call GetQueuedCompletionStatusEx): a     *)
(*             receive completion makes its buffer ready, a wake          *)
(*             completion sets woken                                      *)
(*   Drain     receive_batch copies a ready buffer into the caller's slot *)
(*             and re-posts it (Post) unless the socket is closed         *)
(*   Wake      any thread posts the wake key, at most one at a time       *)
(*   Close     destroy closes the socket: no further posts                *)
(*   Free      destroy frees the buffers once every posted receive has    *)
(*             completed and been dequeued                                *)
(*                                                                         *)
(* FreeWaitsDrain = FALSE lets Free follow Close at once, the reversed    *)
(* witness MCRNClientIocpBrokenFreeBeforeDrain.cfg, which fails on        *)
(* FreedOnlyWhenDrained.                                                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Buffers,          \* posted receives, allocated at create
          MaxDatagrams,     \* datagrams the kernel delivers, bounding the model
          FreeWaitsDrain    \* Free waits until no receive is posted or queued

ASSUME Buffers \in Nat \ {0} /\ MaxDatagrams \in Nat /\ FreeWaitsDrain \in BOOLEAN

BufferIds == 1..Buffers

\* idle: the I/O thread owns it, not posted; posted: the kernel owns it;
\* queued: completed, its completion on the port, not yet dequeued;
\* ready: dequeued, the I/O thread owns it and may copy it
States == {"idle", "posted", "queued", "ready"}

VARIABLES state,        \* state[b]
          copied,       \* buffers the I/O thread copied while it did not own them
          delivered,    \* datagrams the kernel has delivered
          wakePosted,   \* a wake completion is on the port
          woken,        \* a wait has consumed a wake
          closed,       \* destroy closed the socket
          freed         \* destroy freed the buffers

vars == << state, copied, delivered, wakePosted, woken, closed, freed >>

TypeOK ==
    /\ state \in [BufferIds -> States]
    /\ copied \subseteq BufferIds
    /\ delivered \in 0..MaxDatagrams
    /\ wakePosted \in BOOLEAN
    /\ woken \in BOOLEAN
    /\ closed \in BOOLEAN
    /\ freed \in BOOLEAN

\* create posts nothing yet; the first Posts are create's
Init ==
    /\ state = [b \in BufferIds |-> "idle"]
    /\ copied = {}
    /\ delivered = 0
    /\ wakePosted = FALSE
    /\ woken = FALSE
    /\ closed = FALSE
    /\ freed = FALSE

Post(b) ==
    /\ ~closed /\ ~freed
    /\ state[b] = "idle"
    /\ state' = [state EXCEPT ![b] = "posted"]
    /\ UNCHANGED << copied, delivered, wakePosted, woken, closed, freed >>

\* a datagram arrives, or (after Close) the receive is aborted; either way
\* the kernel gives the buffer back through one completion
Complete(b) ==
    /\ state[b] = "posted"
    /\ \/ closed
       \/ delivered < MaxDatagrams
    /\ state' = [state EXCEPT ![b] = "queued"]
    /\ delivered' = IF closed THEN delivered ELSE delivered + 1
    /\ UNCHANGED << copied, wakePosted, woken, closed, freed >>

Dequeue(b) ==
    /\ ~freed
    /\ state[b] = "queued"
    /\ state' = [state EXCEPT ![b] = "ready"]
    /\ UNCHANGED << copied, delivered, wakePosted, woken, closed, freed >>

DequeueWake ==
    /\ ~freed
    /\ wakePosted
    /\ wakePosted' = FALSE
    /\ woken' = TRUE
    /\ UNCHANGED << state, copied, delivered, closed, freed >>

\* copy into the caller's slot, then re-post (or keep it idle once closed)
Drain(b) ==
    /\ ~freed
    /\ state[b] = "ready"
    /\ copied' = IF state[b] = "ready" THEN copied ELSE copied \cup {b}
    /\ state' = [state EXCEPT ![b] = IF closed THEN "idle" ELSE "posted"]
    /\ UNCHANGED << delivered, wakePosted, woken, closed, freed >>

Wake ==
    /\ ~closed
    /\ ~wakePosted
    /\ wakePosted' = TRUE
    /\ UNCHANGED << state, copied, delivered, woken, closed, freed >>

Close ==
    /\ ~closed
    /\ closed' = TRUE
    /\ UNCHANGED << state, copied, delivered, wakePosted, woken, freed >>

Drained == \A b \in BufferIds : state[b] \notin {"posted", "queued"}

Free ==
    /\ closed /\ ~freed
    /\ FreeWaitsDrain => Drained
    /\ freed' = TRUE
    /\ UNCHANGED << state, copied, delivered, wakePosted, woken, closed >>

\* destroy has returned: nothing more happens
Done ==
    /\ freed
    /\ UNCHANGED vars

Next ==
    \/ \E b \in BufferIds : Post(b) \/ Complete(b) \/ Dequeue(b) \/ Drain(b)
    \/ DequeueWake \/ Wake \/ Close \/ Free \/ Done

\* the kernel completes every posted receive once the socket closes, and the
\* I/O thread keeps dequeuing until destroy has drained them
Fairness ==
    /\ \A b \in BufferIds : WF_vars(Complete(b)) /\ WF_vars(Dequeue(b)) /\ WF_vars(Drain(b))
    /\ WF_vars(Free)

Spec == Init /\ [][Next]_vars /\ Fairness

\* a buffer is never copied while the kernel owns it
NeverCopiedWhilePosted == copied = {}

\* the buffers are freed only when no receive is posted or queued
FreedOnlyWhenDrained == freed => Drained

\* after Close every posted receive completes, is dequeued and drained
PostedDrainAfterClose == closed ~> Drained

\* and destroy then frees
CloseEventuallyFrees == closed ~> freed
=============================================================================
