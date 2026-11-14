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
(*   Post      the I/O thread posts an idle receive (create, and          *)
(*             retry_idle for a receive a refused post left idle); the    *)
(*             kernel owns the buffer                                     *)
(*   Complete  the kernel finishes a posted receive and queues its        *)
(*             completion on the port: a datagram (queued), or an error,  *)
(*             or the abort closesocket gives every outstanding receive   *)
(*             (failed)                                                   *)
(*   Dequeue   the I/O thread dequeues completions (receive_batch, wait   *)
(*             and the drain all call GetQueuedCompletionStatusEx): a     *)
(*             datagram makes its buffer ready; a failed completion has   *)
(*             nothing to copy, so its buffer goes back to idle and is    *)
(*             re-posted at once unless the socket is closed              *)
(*   Drain     receive_batch copies a ready buffer into the caller's slot *)
(*             and re-posts it unless the socket is closed                *)
(*   Wake      any thread posts the wake key, at most one at a time       *)
(*   Close     destroy closes the socket: no further posts                *)
(*   Free      destroy frees the buffers once every posted receive has    *)
(*             completed and been dequeued; or, when its drain stalls     *)
(*             (ROCKETNET_CLIENT_IOCP_DRAIN_WAITS dequeues after which    *)
(*             nothing more completed), it gives up and leaks the         *)
(*             transport, never freeing a buffer the kernel may still     *)
(*             write                                                      *)
(*                                                                         *)
(* A re-post can be refused (post_receive fails and starts no I/O): the   *)
(* receive stays idle and the next receive_batch posts it again (Post).   *)
(* The stall count is abstracted: the give-up may come at any point the   *)
(* drain is not finished, which covers a slow kernel the bound runs out   *)
(* on as well as one that never completes.                                *)
(*                                                                         *)
(* FreeWaitsDrain = FALSE lets Free follow Close at once, the reversed    *)
(* witness MCRNClientIocpBrokenFreeBeforeDrain.cfg, which fails on        *)
(* FreedOnlyWhenDrained.  DrainOnlyReady = FALSE lets Drain copy a buffer *)
(* that was not dequeued ready (a receive_batch that walks the buffers    *)
(* instead of the ready ring), the reversed witness                       *)
(* MCRNClientIocpBrokenCopyPosted.cfg, which fails on                     *)
(* NeverCopiedWhilePosted.  KernelStalls = TRUE is a kernel that never    *)
(* delivers the aborts after Close; GiveUp = FALSE is a destroy that      *)
(* waits for them without bound, the reversed witnesses                   *)
(* MCRNClientIocpBrokenNoGiveUp.cfg (CloseEventuallyFrees) and            *)
(* MCRNClientIocpBrokenNoGiveUpDrain.cfg (PostedDrainAfterClose).         *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Buffers,          \* posted receives, allocated at create
          MaxDatagrams,     \* datagrams the kernel delivers, bounding the model
          FreeWaitsDrain,   \* Free waits until no receive is posted or queued
          DrainOnlyReady,   \* Drain copies only a buffer dequeued ready
          KernelStalls,     \* after Close the kernel never completes a posted receive
          GiveUp            \* destroy gives up a stalled drain and leaks (the code)

ASSUME Buffers \in Nat \ {0} /\ MaxDatagrams \in Nat /\ FreeWaitsDrain \in BOOLEAN
ASSUME DrainOnlyReady \in BOOLEAN /\ KernelStalls \in BOOLEAN /\ GiveUp \in BOOLEAN

BufferIds == 1..Buffers

\* idle: the I/O thread owns it, not posted; posted: the kernel owns it;
\* queued: completed with a datagram, its completion on the port, not yet
\* dequeued; failed: completed with an error or the abort, its completion on
\* the port; ready: dequeued, the I/O thread owns it and may copy it
States == {"idle", "posted", "queued", "failed", "ready"}

VARIABLES state,        \* state[b]
          copied,       \* buffers the I/O thread copied while it did not own them
          delivered,    \* datagrams the kernel has delivered
          wakePosted,   \* a wake completion is on the port
          woken,        \* a wait has consumed a wake
          closed,       \* destroy closed the socket
          freed,        \* destroy freed the buffers
          leaked        \* destroy gave up its drain and leaked the transport

vars == << state, copied, delivered, wakePosted, woken, closed, freed, leaked >>

TypeOK ==
    /\ state \in [BufferIds -> States]
    /\ copied \subseteq BufferIds
    /\ delivered \in 0..MaxDatagrams
    /\ wakePosted \in BOOLEAN
    /\ woken \in BOOLEAN
    /\ closed \in BOOLEAN
    /\ freed \in BOOLEAN
    /\ leaked \in BOOLEAN

\* create posts nothing yet; the first Posts are create's
Init ==
    /\ state = [b \in BufferIds |-> "idle"]
    /\ copied = {}
    /\ delivered = 0
    /\ wakePosted = FALSE
    /\ woken = FALSE
    /\ closed = FALSE
    /\ freed = FALSE
    /\ leaked = FALSE

\* destroy has returned, freeing or leaking: the I/O thread does nothing more
Gone == freed \/ leaked

\* the state a buffer the I/O thread gives back takes: posted, or idle when
\* the socket is closed or the post is refused (the refusal is the
\* platform's: either outcome while open)
RepostStates == IF closed THEN {"idle"} ELSE {"posted", "idle"}

Post(b) ==
    /\ ~closed /\ ~Gone
    /\ state[b] = "idle"
    /\ state' = [state EXCEPT ![b] = "posted"]
    /\ UNCHANGED << copied, delivered, wakePosted, woken, closed, freed, leaked >>

\* the kernel gives the buffer back through one completion: a datagram, or
\* an error or (after Close) the abort; a stalled kernel completes nothing
\* after Close
Complete(b) ==
    /\ state[b] = "posted"
    /\ ~(KernelStalls /\ closed)
    /\ \/ /\ delivered < MaxDatagrams
          /\ state' = [state EXCEPT ![b] = "queued"]
          /\ delivered' = delivered + 1
       \/ /\ state' = [state EXCEPT ![b] = "failed"]
          /\ UNCHANGED delivered
    /\ UNCHANGED << copied, wakePosted, woken, closed, freed, leaked >>

\* a datagram's completion makes the buffer ready; a failed one sends the
\* buffer straight back (idle, then the re-post)
Dequeue(b) ==
    /\ ~Gone
    /\ state[b] \in {"queued", "failed"}
    /\ IF state[b] = "queued"
       THEN state' = [state EXCEPT ![b] = "ready"]
       ELSE \E s \in RepostStates : state' = [state EXCEPT ![b] = s]
    /\ UNCHANGED << copied, delivered, wakePosted, woken, closed, freed, leaked >>

DequeueWake ==
    /\ ~Gone
    /\ wakePosted
    /\ wakePosted' = FALSE
    /\ woken' = TRUE
    /\ UNCHANGED << state, copied, delivered, closed, freed, leaked >>

\* copy into the caller's slot, then re-post (or keep it idle once closed,
\* or after a refused post); the code takes b from the ready ring and
\* asserts state[b] is ready
Drain(b) ==
    /\ ~Gone
    /\ IF DrainOnlyReady THEN state[b] = "ready" ELSE state[b] # "idle"
    /\ copied' = IF state[b] = "ready" THEN copied ELSE copied \cup {b}
    /\ \E s \in RepostStates : state' = [state EXCEPT ![b] = s]
    /\ UNCHANGED << delivered, wakePosted, woken, closed, freed, leaked >>

Wake ==
    /\ ~closed
    /\ ~wakePosted
    /\ wakePosted' = TRUE
    /\ UNCHANGED << state, copied, delivered, woken, closed, freed, leaked >>

Close ==
    /\ ~closed
    /\ closed' = TRUE
    /\ UNCHANGED << state, copied, delivered, wakePosted, woken, freed, leaked >>

Drained == \A b \in BufferIds : state[b] \notin {"posted", "queued", "failed"}

\* destroy: free once drained; a stalled drain gives up and leaks instead
Free ==
    /\ closed /\ ~Gone
    /\ \/ /\ FreeWaitsDrain => Drained
          /\ freed' = TRUE
          /\ UNCHANGED leaked
       \/ /\ GiveUp /\ FreeWaitsDrain /\ ~Drained
          /\ leaked' = TRUE
          /\ UNCHANGED freed
    /\ UNCHANGED << state, copied, delivered, wakePosted, woken, closed >>

\* destroy has returned: nothing more happens
Done ==
    /\ Gone
    /\ UNCHANGED vars

Next ==
    \/ \E b \in BufferIds : Post(b) \/ Complete(b) \/ Dequeue(b) \/ Drain(b)
    \/ DequeueWake \/ Wake \/ Close \/ Free \/ Done

\* the kernel completes every posted receive (unless it stalls after Close),
\* and the I/O thread keeps dequeuing until destroy has drained them
Fairness ==
    /\ \A b \in BufferIds : WF_vars(Complete(b)) /\ WF_vars(Dequeue(b)) /\ WF_vars(Drain(b))
    /\ WF_vars(Free)

Spec == Init /\ [][Next]_vars /\ Fairness

\* a buffer is never copied while the kernel owns it
NeverCopiedWhilePosted == copied = {}

\* the buffers are freed only when no receive is posted or its completion
\* queued
FreedOnlyWhenDrained == freed => Drained

\* after Close every posted receive completes and is dequeued, unless
\* destroy gave up on a stalled drain and leaked
PostedDrainAfterClose == closed ~> (Drained \/ leaked)

\* and destroy then returns: it frees, or it leaks
CloseEventuallyFrees == closed ~> (freed \/ leaked)
=============================================================================
