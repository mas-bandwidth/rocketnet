----------------------------- MODULE RNRioDriver -----------------------------
(***************************************************************************)
(* The Windows Registered I/O (RIO) server driver, as designed for        *)
(* sdk/rocketnet_server_rio_windows.cpp: one UDP socket, one registered   *)
(* region carved by sdk/rocketnet_rio_layout.h, one polled send           *)
(* completion queue, one polled receive completion queue, one request     *)
(* queue, all on the main thread.                                         *)
(*                                                                         *)
(* SEND.  Two staging phases of Slots frame slots each.  Fill threads     *)
(* reserve the next slot of the fill phase (one fetch_add), finish it     *)
(* with its bytes or abort it to zero bytes.  send_packets, with the fill *)
(* threads joined, posts every finished slot of the fill phase with       *)
(* RIO_MSG_DEFER and one commit, records the phase's outstanding count,   *)
(* and switches the fill phase to the other phase.  A phase opens to fill *)
(* threads only when its outstanding count is zero: send_packets polls    *)
(* the send completion queue until it is.  Between post and completion    *)
(* the kernel owns the slot's frame and address slot.                     *)
(*                                                                         *)
(* RECEIVE.  Every receive slice is posted at create.  receive_packets    *)
(* first re-posts each slice a refused re-post left in the retry list;    *)
(* then it dequeues at most one bank of receive completions, copies each  *)
(* datagram into the fill bank of two banks (or counts it dropped), and   *)
(* re-posts the slice.  A refused re-post waits in the retry list for the *)
(* next receive_packets and is never retired, so a transient refusal      *)
(* never shrinks the receive depth.  It swaps the banks when the queue is *)
(* empty or the per-call bank limit is reached; the old fill bank becomes *)
(* the exposed bank whose views process_packets returns until the next    *)
(* receive_packets.                                                       *)
(*                                                                         *)
(* DESTROY.  closesocket completes every outstanding request with an      *)
(* error; both queues are dequeued until every posted send and receive    *)
(* is accounted for; only then are the queues closed, the buffer          *)
(* deregistered and the region freed.                                     *)
(*                                                                         *)
(* USO.  Post groups the finished slots of the phase into runs of        *)
(* consecutive slots (rocketnet_coalesce_send_run decides which; here     *)
(* any grouping); each run is one RIOSendEx, so outstanding counts posts,  *)
(* not slots, and head[p][s] names the first slot of the post that       *)
(* carries s.  The kernel completes a post once, and RetireSend frees     *)
(* every slot the post carries.  A run of one is the per-datagram post.   *)
(*                                                                         *)
(* Datagrams are abstract ids 1..MaxSteps, which bounds the receive side. *)
(* SwitchWaitsDrain = FALSE lets SwitchPhase ignore the other phase's     *)
(* outstanding count; FreeWaitsDrain = FALSE lets Free follow Close at    *)
(* once; RetryRefused = FALSE restores the old rule that a refused        *)
(* re-post retires its slice; ReceiveBounded = FALSE lets a call dequeue   *)
(* past one bank; RunRetiresAll = FALSE lets a run's completion free only *)
(* its first slot.  TLC rejects each reversed witness.                     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Slots,             \* frame slots per send phase
          Slices,            \* receive slices; also the capacity of a bank
          MaxSteps,          \* datagrams the kernel delivers: ids 1..MaxSteps
          SwitchWaitsDrain,  \* SwitchPhase waits for the other phase to drain
          FreeWaitsDrain,    \* Free waits until every request is accounted for
          RetryRefused,      \* a refused re-post waits in the retry list
          ReceiveBounded,   \* receive_packets dequeues at most one bank
          RunRetiresAll     \* a USO run's one completion frees every slot it carries

ASSUME Slots \in Nat \ {0} /\ Slices \in Nat \ {0} /\ MaxSteps \in Nat
ASSUME SwitchWaitsDrain \in BOOLEAN /\ FreeWaitsDrain \in BOOLEAN
ASSUME RetryRefused \in BOOLEAN /\ ReceiveBounded \in BOOLEAN
ASSUME RunRetiresAll \in BOOLEAN

Phases     == {0, 1}
SlotIds    == 1..Slots
SliceIds   == 1..Slices
Datagrams  == 1..MaxSteps
Status     == {"ok", "fail"}
SlotStates == {"Free", "Reserved", "Finished", "Posted"}
SliceStates == {"Posted", "Completed", "Copied"}
BankCapacity == Slices
ReceiveCallLimit == IF ReceiveBounded THEN Slices ELSE MaxSteps

VARIABLES slot,         \* slot[p][s]: the state of slot s of send phase p
          outstanding,  \* outstanding[p]: posts (datagrams or USO runs) from phase p, not retired
          head,         \* head[p][s]: the first slot of the post carrying slot s; 0 when not posted
          next,         \* next[p]: phase p's reservation index (slots taken)
          fill,         \* the fill phase, open to fill threads
          sending,      \* send_packets is between its post and its switch
          sendCQ,       \* send completions <<phase, slot, status>> not dequeued
          slice,        \* slice[i]: the state of receive slice i
          held,         \* held[i]: the datagram in slice i's buffer; 0 for none
          received,     \* the kernel has delivered datagrams 1..received
          consumed,     \* datagrams copied into a bank or counted dropped
          fillBank,     \* the fill bank: datagrams copied since the last swap
          exposed,      \* the exposed bank, whose views process_packets returns
          swapped,      \* the exposed bank as the last Swap left it
          view,         \* the datagram the transport last read; 0 for none
          recvCQ,       \* receive completions <<slice, status>> not dequeued
          retry,        \* slices whose re-post was refused, waiting in the retry list
          receiveCount, \* completions dequeued by the current receive_packets call
          socket        \* Running, Closing (closesocket called) or Freed

sendVars == <<slot, outstanding, head, next, fill, sending, sendCQ>>
recvVars == <<slice, held, received, consumed, fillBank, exposed, swapped,
              view, recvCQ, retry, receiveCount>>
vars == <<sendVars, recvVars, socket>>

Init ==
    /\ slot = [p \in Phases |-> [s \in SlotIds |-> "Free"]]
    /\ outstanding = [p \in Phases |-> 0]
    /\ head = [p \in Phases |-> [s \in SlotIds |-> 0]]
    /\ next = [p \in Phases |-> 0]
    /\ fill = 0
    /\ sending = FALSE
    /\ sendCQ = {}
    /\ slice = [i \in SliceIds |-> "Posted"]
    /\ held = [i \in SliceIds |-> 0]
    /\ received = 0
    /\ consumed = {}
    /\ fillBank = {}
    /\ exposed = {}
    /\ swapped = {}
    /\ view = 0
    /\ recvCQ = {}
    /\ retry = {}
    /\ receiveCount = 0
    /\ socket = "Running"

\* the fill threads run while the main thread is outside send_packets
FillOpen == socket = "Running" /\ ~sending

-----------------------------------------------------------------------------
(* SEND *)

\* start_packet: one fetch_add reserves the next slot of the fill phase;
\* past the last slot it returns NULL (SEND_FULL) and changes nothing
Reserve ==
    /\ FillOpen
    /\ next[fill] < Slots
    /\ slot' = [slot EXCEPT ![fill][next[fill] + 1] = "Reserved"]
    /\ next' = [next EXCEPT ![fill] = @ + 1]
    /\ UNCHANGED <<outstanding, head, fill, sending, sendCQ, recvVars, socket>>

\* finish: the slot records its bytes
Finish(s) ==
    /\ FillOpen
    /\ slot[fill][s] = "Reserved"
    /\ slot' = [slot EXCEPT ![fill][s] = "Finished"]
    /\ UNCHANGED <<outstanding, head, next, fill, sending, sendCQ, recvVars, socket>>

\* abort: the slot's bytes are zeroed and it is never posted
Abort(s) ==
    /\ FillOpen
    /\ slot[fill][s] = "Reserved"
    /\ slot' = [slot EXCEPT ![fill][s] = "Free"]
    /\ UNCHANGED <<outstanding, head, next, fill, sending, sendCQ, recvVars, socket>>

\* the first slot of the run carrying s when the slots in joins join the
\* run of the slot before them (a joining slot needs a posted slot before it)
RunHead(posted, joins) ==
    LET h[s \in SlotIds] ==
            IF s > 1 /\ s \in joins /\ (s - 1) \in posted THEN h[s - 1] ELSE s
    IN  h

\* send_packets, fill threads joined: post every finished slot of the fill
\* phase by RIOSendEx with RIO_MSG_DEFER, grouped into runs of consecutive
\* slots (USO: one RIOSendEx per run, rocketnet_rio_post_run; a run of one
\* is rocketnet_rio_post_datagram), one RIO_MSG_COMMIT_ONLY, and record the
\* phase's outstanding count of posts
Post ==
    /\ FillOpen
    /\ \A s \in SlotIds : slot[fill][s] # "Reserved"
    /\ LET posted == {s \in SlotIds : slot[fill][s] = "Finished"}
       IN  \E joins \in SUBSET posted :
           LET h == RunHead(posted, joins)
           IN  /\ slot' = [slot EXCEPT ![fill] =
                              [s \in SlotIds |->
                                  IF s \in posted THEN "Posted" ELSE slot[fill][s]]]
               /\ head' = [head EXCEPT ![fill] =
                              [s \in SlotIds |-> IF s \in posted THEN h[s] ELSE 0]]
               /\ outstanding' = [outstanding EXCEPT ![fill] =
                              @ + Cardinality({s \in posted : h[s] = s})]
    /\ sending' = TRUE
    /\ UNCHANGED <<next, fill, sendCQ, recvVars, socket>>

\* the kernel completes a posted send (a datagram or a whole USO run, named
\* by its first slot) into the send completion queue, once
KernelCompleteSend(p, s) ==
    /\ socket = "Running"
    /\ slot[p][s] = "Posted"
    /\ head[p][s] = s
    /\ \A st \in Status : <<p, s, st>> \notin sendCQ
    /\ \E st \in Status : sendCQ' = sendCQ \cup {<<p, s, st>>}
    /\ UNCHANGED <<slot, outstanding, head, next, fill, sending, recvVars, socket>>

\* the slots the post whose first slot is s carries
RunSlots(p, s) == {t \in SlotIds : head[p][t] = s}

\* retire one send completion: every slot the post carries is the driver's
\* again (a failed status counts SEND_FAILED once per packet).  With
\* RunRetiresAll = FALSE only the first slot is freed.
RetireSend(p, s, st) ==
    /\ <<p, s, st>> \in sendCQ
    /\ sendCQ' = sendCQ \ {<<p, s, st>>}
    /\ LET freed == IF RunRetiresAll THEN RunSlots(p, s) ELSE {s}
       IN  /\ slot' = [slot EXCEPT ![p] =
                          [t \in SlotIds |-> IF t \in freed THEN "Free" ELSE slot[p][t]]]
           /\ head' = [head EXCEPT ![p] =
                          [t \in SlotIds |-> IF t \in freed THEN 0 ELSE head[p][t]]]
    /\ outstanding' = [outstanding EXCEPT ![p] = @ - 1]

\* send_packets dequeues the send completion queue without blocking, at its
\* start and in its wait for the other phase to drain
DequeueSend(p, s) ==
    /\ socket = "Running"
    /\ \E st \in Status : RetireSend(p, s, st)
    /\ UNCHANGED <<next, fill, sending, recvVars, socket>>

\* send_packets switches the fill phase to the other phase, opening it to
\* fill threads only when its outstanding count is zero (each empty poll
\* while it waits counts SEND_WAITS)
SwitchPhase ==
    /\ socket = "Running"
    /\ sending
    /\ SwitchWaitsDrain => outstanding[1 - fill] = 0
    /\ fill' = 1 - fill
    /\ next' = [next EXCEPT ![1 - fill] = 0]
    /\ sending' = FALSE
    /\ UNCHANGED <<slot, outstanding, head, sendCQ, recvVars, socket>>

-----------------------------------------------------------------------------
(* RECEIVE *)

\* the kernel fills a posted slice: a datagram, or a failed status
KernelFillSlice(i) ==
    /\ socket = "Running"
    /\ slice[i] = "Posted"
    /\ \/ /\ received < MaxSteps
          /\ received' = received + 1
          /\ held' = [held EXCEPT ![i] = received + 1]
          /\ recvCQ' = recvCQ \cup {<<i, "ok">>}
       \/ /\ recvCQ' = recvCQ \cup {<<i, "fail">>}
          /\ UNCHANGED <<received, held>>
    /\ slice' = [slice EXCEPT ![i] = "Completed"]
    /\ UNCHANGED <<sendVars, consumed, fillBank, exposed, swapped, view,
                   retry, receiveCount, socket>>

\* receive_packets, per completion: a failed status counts RECEIVE_FAILED;
\* a datagram is copied into the fill bank when it has room, else counted
\* dropped (RECEIVE_DROPPED_SIZE, RECEIVE_DROPPED_OVERFLOW); then the slice
\* is re-posted.  A refused re-post counts RECEIVE_REPOST_FAILED: with
\* RetryRefused it waits in the retry list, without it the slice is retired.
DequeueReceive(i) ==
    /\ FillOpen
    /\ receiveCount < ReceiveCallLimit
    /\ \E st \in Status :
          /\ <<i, st>> \in recvCQ
          /\ recvCQ' = recvCQ \ {<<i, st>>}
          /\ IF st = "ok"
             THEN /\ consumed' = consumed \cup {held[i]}
                  /\ \/ /\ Cardinality(fillBank) < BankCapacity
                        /\ fillBank' = fillBank \cup {held[i]}
                     \/ UNCHANGED fillBank
             ELSE UNCHANGED <<consumed, fillBank>>
    /\ held' = [held EXCEPT ![i] = 0]
    /\ receiveCount' = receiveCount + 1
    /\ \/ /\ slice' = [slice EXCEPT ![i] = "Posted"]
          /\ retry' = retry \ {i}
       \/ /\ slice' = [slice EXCEPT ![i] = "Copied"]
          /\ retry' = IF RetryRefused THEN retry \cup {i} ELSE retry
    /\ UNCHANGED <<sendVars, received, exposed, swapped, view, socket>>

\* receive_packets, before it dequeues: RetryRepost re-posts each refused
\* slice while the fill side is open and the socket is Running (after
\* Close the code skips the retry).  A slice refused again stays waiting.
RetryRepost(i) ==
    /\ FillOpen
    /\ i \in retry
    /\ slice[i] = "Copied"
    /\ \/ /\ slice' = [slice EXCEPT ![i] = "Posted"]
          /\ retry' = retry \ {i}
       \/ /\ UNCHANGED slice
          /\ UNCHANGED retry
    /\ UNCHANGED <<sendVars, held, received, consumed, fillBank, exposed,
                   swapped, view, recvCQ, receiveCount, socket>>

\* receive_packets, once the receive queue is empty: swap the banks
Swap ==
    /\ FillOpen
    /\ \/ recvCQ = {}
       \/ receiveCount = ReceiveCallLimit
    /\ exposed' = fillBank
    /\ swapped' = fillBank
    /\ fillBank' = {}
    /\ view' = 0
    /\ receiveCount' = 0
    /\ UNCHANGED <<sendVars, slice, held, received, consumed, recvCQ,
                   retry, socket>>

\* process_packets: the transport reads a view of the exposed bank
TransportReads ==
    /\ FillOpen
    /\ \E d \in exposed : view' = d
    /\ UNCHANGED <<sendVars, slice, held, received, consumed, fillBank,
                   exposed, swapped, recvCQ, retry, receiveCount, socket>>

-----------------------------------------------------------------------------
(* DESTROY *)

PendingSends ==
    {q \in Phases \X SlotIds :
        /\ slot[q[1]][q[2]] = "Posted"
        /\ head[q[1]][q[2]] = q[2]
        /\ \A st \in Status : <<q[1], q[2], st>> \notin sendCQ}

\* closesocket: every outstanding request completes with an error
Close ==
    /\ FillOpen
    /\ socket' = "Closing"
    /\ sendCQ' = sendCQ \cup {<<q[1], q[2], "fail">> : q \in PendingSends}
    /\ recvCQ' = recvCQ \cup {<<i, "fail">> : i \in {j \in SliceIds : slice[j] = "Posted"}}
    /\ slice' = [i \in SliceIds |-> IF slice[i] = "Posted" THEN "Completed" ELSE slice[i]]
    /\ UNCHANGED <<slot, outstanding, head, next, fill, sending, held, received,
                   consumed, fillBank, exposed, swapped, view, retry, receiveCount>>

\* destroy dequeues both queues; nothing is re-posted
Drain ==
    /\ socket = "Closing"
    /\ \/ /\ \E p \in Phases, s \in SlotIds, st \in Status : RetireSend(p, s, st)
          /\ UNCHANGED <<next, fill, sending, recvVars>>
       \/ /\ \E i \in SliceIds, st \in Status :
                /\ <<i, st>> \in recvCQ
                /\ recvCQ' = recvCQ \ {<<i, st>>}
                /\ consumed' = IF st = "ok" THEN consumed \cup {held[i]} ELSE consumed
                /\ held' = [held EXCEPT ![i] = 0]
                /\ slice' = [slice EXCEPT ![i] = "Copied"]
          /\ UNCHANGED <<sendVars, received, fillBank, exposed, swapped,
                         view, retry, receiveCount>>
    /\ UNCHANGED socket

\* every posted send and receive is accounted for
Drained ==
    /\ sendCQ = {}
    /\ recvCQ = {}
    /\ \A p \in Phases, s \in SlotIds : slot[p][s] # "Posted"
    /\ \A i \in SliceIds : slice[i] \notin {"Posted", "Completed"}

\* RIOCloseCompletionQueue, RIODeregisterBuffer and VirtualFree
Free ==
    /\ socket = "Closing"
    /\ FreeWaitsDrain => Drained
    /\ socket' = "Freed"
    /\ UNCHANGED <<sendVars, recvVars>>

\* the driver is gone; the run stutters
Done ==
    /\ socket = "Freed"
    /\ UNCHANGED vars

Next ==
    \/ Reserve
    \/ \E s \in SlotIds : Finish(s) \/ Abort(s)
    \/ Post
    \/ \E p \in Phases, s \in SlotIds : KernelCompleteSend(p, s) \/ DequeueSend(p, s)
    \/ SwitchPhase
    \/ \E i \in SliceIds : KernelFillSlice(i) \/ DequeueReceive(i)
    \/ \E i \in SliceIds : RetryRepost(i)
    \/ Swap
    \/ TransportReads
    \/ Close
    \/ Drain
    \/ Free
    \/ Done

Spec == Init /\ [][Next]_vars

\* the liveness properties hold under weak fairness of the kernel's send
\* completions, the dequeues, Drain and Free
SpecFair ==
    /\ Spec
    /\ \A p \in Phases, s \in SlotIds :
          WF_vars(KernelCompleteSend(p, s)) /\ WF_vars(DequeueSend(p, s))
    /\ \A i \in SliceIds : WF_vars(DequeueReceive(i))
    /\ \A i \in SliceIds : WF_vars(RetryRepost(i))
    /\ WF_vars(Drain)
    /\ WF_vars(Free)

-----------------------------------------------------------------------------

TypeOK ==
    /\ slot \in [Phases -> [SlotIds -> SlotStates]]
    /\ outstanding \in [Phases -> 0..Slots]
    /\ head \in [Phases -> [SlotIds -> 0..Slots]]
    /\ next \in [Phases -> 0..Slots]
    /\ fill \in Phases
    /\ sending \in BOOLEAN
    /\ sendCQ \subseteq Phases \X SlotIds \X Status
    /\ slice \in [SliceIds -> SliceStates]
    /\ held \in [SliceIds -> 0..MaxSteps]
    /\ received \in 0..MaxSteps
    /\ consumed \subseteq Datagrams
    /\ fillBank \subseteq Datagrams
    /\ exposed \subseteq Datagrams
    /\ swapped \subseteq Datagrams
    /\ view \in 0..MaxSteps
    /\ recvCQ \subseteq SliceIds \X Status
    /\ retry \subseteq SliceIds
    /\ receiveCount \in 0..(Slices + MaxSteps)
    /\ socket \in {"Running", "Closing", "Freed"}

\* Reserve never returns a slot of a phase with outstanding sends: a slot a
\* fill thread holds is always in a phase the kernel owns nothing of
NoReserveIntoPostedPhase ==
    \A p \in Phases :
        (\E s \in SlotIds : slot[p][s] \in {"Reserved", "Finished"})
            => outstanding[p] = 0

\* the outstanding count is the number of the phase's posts the kernel owns
\* (a USO run is one post)
OutstandingCounts ==
    \A p \in Phases :
        outstanding[p] = Cardinality({s \in SlotIds : slot[p][s] = "Posted" /\ head[p][s] = s})

\* a slot is posted exactly while the post that carries it is: its head is
\* a posted slot at or before it, and a slot that is not posted names no post
RunPostedTogether ==
    \A p \in Phases, s \in SlotIds :
        IF slot[p][s] = "Posted"
        THEN head[p][s] \in 1..s /\ slot[p][head[p][s]] = "Posted" /\ head[p][head[p][s]] = head[p][s]
        ELSE head[p][s] = 0

\* every queued completion names a request the kernel still owns
CompletionsOwned ==
    /\ \A e \in sendCQ : slot[e[1]][e[2]] = "Posted" /\ head[e[1]][e[2]] = e[2]
    /\ \A i \in SliceIds :
          (slice[i] = "Completed") <=> (\E st \in Status : <<i, st>> \in recvCQ)

\* the exposed bank never changes between two Swaps, and a view the
\* transport holds is in it
ExposedStable ==
    /\ exposed = swapped
    /\ view # 0 => view \in exposed

\* a slice is re-posted only after its datagram is in a bank or counted
\* dropped: a posted slice holds no datagram, and every delivered datagram
\* is still in its slice or consumed
RepostOnlyAfterCopy ==
    /\ \A i \in SliceIds : slice[i] = "Posted" => held[i] = 0
    /\ \A d \in 1..received : d \in consumed \/ \E i \in SliceIds : held[i] = d
    /\ fillBank \cup exposed \subseteq consumed

\* a receive_packets call dequeues at most one bank of completions
ReceiveCallBound == receiveCount <= Slices

\* a transient refusal never removes a slice from the receive depth: while
\* the socket runs every slice is posted, completed, or waiting in the
\* retry list (with RetryRefused = FALSE a refused slice is retired)
ReceiveDepthNeverShrinks ==
    socket = "Running" =>
        \A i \in SliceIds : slice[i] \in {"Posted", "Completed"} \/ i \in retry

\* Freed holds only with no posted slot, no posted or completed slice and
\* both completion queues empty; a slice waiting in the retry list is not
\* posted, so the kernel does not own it and it blocks neither Drained nor
\* Free
FreedOnlyWhenDrained ==
    socket = "Freed" => Drained

\* every posted slot is eventually free
PostedEventuallyFree ==
    \A p \in Phases, s \in SlotIds : (slot[p][s] = "Posted") ~> (slot[p][s] = "Free")

\* a close eventually frees the region
CloseEventuallyFrees ==
    (socket = "Closing") ~> (socket = "Freed")

=============================================================================
