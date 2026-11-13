-------------------------- MODULE LoopbackQueue ---------------------------
(***************************************************************************)
(* The loopback endpoint queue of sdk/rocketnet_driver_loopback.cpp: one   *)
(* ring of Capacity datagrams with head and tail indices and a count.     *)
(* Send writes at tail mod Capacity and advances tail while count <       *)
(* Capacity, and a send to a full queue is dropped and counted            *)
(* (loopback_queue_tail).  Receive takes the datagram at head; Drop is the *)
(* oversize dequeue path, which removes the head datagram without          *)
(* delivering it and counts it in the same dropped counter                 *)
(* (loopback_server_receive_batch, loopback_client_receive).  Both go     *)
(* through loopback_queue_pop, which restarts head and tail at slot zero   *)
(* when the count reaches zero.  A popped slot is never cleared: the code  *)
(* leaves the old bytes there, and so does the model.                      *)
(*                                                                         *)
(* What the restart buys: the slots a queue writes are those below the     *)
(* longest run of sends accepted between two moments the queue was empty   *)
(* (TouchedWithinRun).  That bound is the run, not the backlog: sends A    *)
(* and B, receive A, send C writes slot 2 although at most two datagrams   *)
(* were ever queued, because the queue did not empty in between.          *)
(*                                                                         *)
(* The harness sends any time, up to MaxSent attempts, so the full branch, *)
(* the wrap of tail past Capacity and the restart are all reached.        *)
(* Mutation selects one code defect for a reversed witness; "none" is the  *)
(* code:                                                                   *)
(*   "no_restart"      pop never restarts the indices (TouchedWithinRun,  *)
(*                     EmptyIsRestarted)                                   *)
(*   "no_full_check"   a send to a full queue writes over the head         *)
(*                     (NoSlotReusedInFlight)                              *)
(*   "early_restart"   pop restarts one datagram too early, with one left *)
(*                     (FifoOrder)                                         *)
(*   "uncounted_drop"  the oversize Drop does not count (DropsCounted)     *)
(*                                                                         *)
(* The code cites this module's Send, Receive and Drop where they happen. *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Capacity,           \* queue slots, a power of two in the code
    MaxSent,            \* exploration bound: send attempts the harness makes
    Mutation            \* "none" (the code) or one defect, above

Mutations == {"none", "no_restart", "no_full_check", "early_restart", "uncounted_drop"}

ASSUME /\ Capacity \in Nat \ {0}
       /\ MaxSent \in Nat
       /\ Mutation \in Mutations

VARIABLES
    head, tail, count,
    slots,              \* datagram at each physical slot (stale after a pop), 0 if never written
    written,            \* physical slots ever written
    enqueued,           \* datagrams accepted, in send order
    departed,           \* datagrams that left the head, in order (received or dropped)
    delivered,          \* datagrams handed out by Receive, in order
    nextMessage,        \* the next send attempt's datagram id
    dropped,            \* the endpoint's dropped counter: full sends and oversize drops
    run,                \* ghost: sends accepted since the queue was last empty
    maxRun              \* ghost: the longest such run so far

vars == <<head, tail, count, slots, written, enqueued, departed, delivered,
          nextMessage, dropped, run, maxRun>>

Max(a, b) == IF a > b THEN a ELSE b

Init ==
    /\ head = 0
    /\ tail = 0
    /\ count = 0
    /\ slots = [ i \in 0..Capacity-1 |-> 0 ]
    /\ written = {}
    /\ enqueued = <<>>
    /\ departed = <<>>
    /\ delivered = <<>>
    /\ nextMessage = 1
    /\ dropped = 0
    /\ run = 0
    /\ maxRun = 0

\* Send: loopback_queue_tail and loopback_queue_push.  Enqueue at tail mod
\* Capacity when count < Capacity; a full queue drops and counts.
Send ==
    /\ nextMessage <= MaxSent
    /\ nextMessage' = nextMessage + 1
    /\ IF count < Capacity \/ Mutation = "no_full_check"
         THEN /\ slots' = [ slots EXCEPT ![tail % Capacity] = nextMessage ]
              /\ written' = written \cup { tail % Capacity }
              /\ enqueued' = Append(enqueued, nextMessage)
              /\ tail' = tail + 1
              /\ count' = count + 1
              /\ run' = run + 1
              /\ maxRun' = Max(maxRun, run + 1)
              /\ UNCHANGED <<head, departed, delivered, dropped>>
         ELSE /\ dropped' = dropped + 1
              /\ UNCHANGED <<head, tail, count, slots, written, enqueued,
                             departed, delivered, run, maxRun>>

\* loopback_queue_pop: advance head, shrink count, and restart both indices
\* at zero when the count reaches zero.  The slot is not cleared.
Restarts == CASE Mutation = "no_restart"    -> FALSE
              [] Mutation = "early_restart" -> count <= 2
              [] OTHER                      -> count = 1

Pop ==
    /\ head' = IF Restarts THEN 0 ELSE head + 1
    /\ tail' = IF Restarts THEN 0 ELSE tail
    /\ count' = count - 1
    /\ run' = IF count = 1 THEN 0 ELSE run

\* Receive: hand out the datagram at head.
Receive ==
    /\ count > 0
    /\ delivered' = Append(delivered, slots[head % Capacity])
    /\ departed' = Append(departed, slots[head % Capacity])
    /\ Pop
    /\ UNCHANGED <<slots, written, enqueued, nextMessage, dropped, maxRun>>

\* Drop: the oversize dequeue path removes the head datagram without
\* delivering it, and counts it.
Drop ==
    /\ count > 0
    /\ departed' = Append(departed, slots[head % Capacity])
    /\ dropped' = IF Mutation = "uncounted_drop" THEN dropped ELSE dropped + 1
    /\ Pop
    /\ UNCHANGED <<slots, written, enqueued, delivered, nextMessage, maxRun>>

\* The harness is done once it has made its attempts and drained the queue.
Done ==
    /\ nextMessage > MaxSent
    /\ count = 0
    /\ UNCHANGED vars

Next == Send \/ Receive \/ Drop \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ head \in 0..MaxSent
    /\ tail \in 0..MaxSent
    /\ count \in 0..MaxSent
    /\ slots \in [0..Capacity-1 -> 0..MaxSent]
    /\ written \subseteq 0..Capacity-1
    /\ nextMessage \in 1..MaxSent+1
    /\ Len(enqueued) <= MaxSent
    /\ dropped \in 0..MaxSent
    /\ run \in 0..MaxSent
    /\ maxRun \in 0..MaxSent

\* No slot holding an unread datagram is written again: the count slots
\* from head hold exactly the datagrams accepted and not yet departed, in
\* order, and the count never passes Capacity.
NoSlotReusedInFlight ==
    /\ count = Len(enqueued) - Len(departed)
    /\ count <= Capacity
    /\ \A k \in 1..count :
         slots[(head + k - 1) % Capacity] = enqueued[Len(departed) + k]

\* Datagrams leave (by Receive or Drop) in the order they were accepted.
FifoOrder ==
    departed = SubSeq(enqueued, 1, Len(departed))

\* An empty queue has restarted both indices at slot zero.
EmptyIsRestarted ==
    count = 0 => head = 0 /\ tail = 0

\* Every slot ever written is below the longest run of sends accepted
\* between two empties (the run, not the backlog: see the header).
TouchedWithinRun ==
    written \subseteq 0..maxRun-1

\* Every send attempt is queued, delivered or counted dropped.
DropsCounted ==
    nextMessage - 1 = count + Len(delivered) + dropped

=============================================================================
