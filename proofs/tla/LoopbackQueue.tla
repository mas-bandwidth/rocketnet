-------------------------- MODULE LoopbackQueue ---------------------------
(***************************************************************************)
(* The loopback endpoint queue of sdk/rocketnet_driver_loopback.cpp: one   *)
(* ring of Capacity datagrams with free-running head and tail indices.     *)
(* Send writes at tail mod Capacity and advances tail; Receive takes the   *)
(* datagram at head; Drop is the oversize dequeue path, which removes the  *)
(* head datagram without delivering it.  When the queue empties,          *)
(* RestartAtEmpty makes every dequeue restart head and tail at slot zero,  *)
(* so a queue that empties writes only the slots its deepest backlog       *)
(* reached.  The code cites this module's Send, Receive and Drop where     *)
(* they happen.                                                            *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Capacity,           \* queue slots, a power of two in the code
    MaxSent,            \* exploration bound: datagrams the harness may send
    BacklogMax,         \* the deepest backlog the harness admits
    RestartAtEmpty      \* the fix: restart head and tail at zero when empty

VARIABLES
    head, tail, count,
    slots,              \* datagram at each physical slot, 0 when empty
    written,            \* physical slots ever written
    enqueued,           \* datagrams accepted, in send order
    departed,           \* datagrams that left the head, in order (received or dropped)
    delivered,          \* datagrams handed out by Receive, in order
    nextMessage,
    dropped

vars == <<head, tail, count, slots, written, enqueued, departed, delivered, nextMessage, dropped>>

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

\* Send: enqueue at tail mod Capacity when count < Capacity; a full queue
\* drops and counts.  The harness admits a fresh send while the backlog is
\* empty, or while the free-running tail is still inside the bound.
Send ==
    /\ Len(enqueued) < MaxSent
    /\ (count = 0 \/ tail < BacklogMax)
    /\ \/ /\ count < Capacity
          /\ slots' = [ slots EXCEPT ![tail % Capacity] = nextMessage ]
          /\ written' = written \cup { tail % Capacity }
          /\ enqueued' = Append(enqueued, nextMessage)
          /\ nextMessage' = nextMessage + 1
          /\ head' = head
          /\ tail' = tail + 1
          /\ count' = count + 1
          /\ UNCHANGED <<departed, delivered, dropped>>
       \/ /\ count >= Capacity
          /\ dropped' = dropped + 1
          /\ UNCHANGED <<head, tail, count, slots, written, enqueued, departed, delivered, nextMessage>>

\* Receive: dequeue at head.  The restart rule resets both indices when the
\* count reaches zero.
Receive ==
    /\ count > 0
    /\ \E d \in 1..nextMessage-1 :
         /\ slots[head % Capacity] = d
         /\ delivered' = Append(delivered, d)
         /\ departed' = Append(departed, d)
         /\ slots' = [ slots EXCEPT ![head % Capacity] = 0 ]
         /\ head' = IF count = 1 /\ RestartAtEmpty THEN 0 ELSE head + 1
         /\ tail' = IF count = 1 /\ RestartAtEmpty THEN 0 ELSE tail
         /\ count' = count - 1
    /\ UNCHANGED <<written, enqueued, nextMessage, dropped>>

\* Drop: the oversize dequeue path removes the head datagram without
\* delivering it.
Drop ==
    /\ count > 0
    /\ \E d \in 1..nextMessage-1 :
         /\ slots[head % Capacity] = d
         /\ departed' = Append(departed, d)
         /\ slots' = [ slots EXCEPT ![head % Capacity] = 0 ]
         /\ head' = IF count = 1 /\ RestartAtEmpty THEN 0 ELSE head + 1
         /\ tail' = IF count = 1 /\ RestartAtEmpty THEN 0 ELSE tail
         /\ count' = count - 1
    /\ UNCHANGED <<written, enqueued, delivered, nextMessage, dropped>>

\* The harness is done once it has sent its bound and drained the queue.
Done ==
    /\ Len(enqueued) = MaxSent
    /\ count = 0
    /\ UNCHANGED vars

Next == Send \/ Receive \/ Drop \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ head \in 0..MaxSent+Capacity
    /\ tail \in 0..MaxSent+Capacity
    /\ count \in 0..Capacity
    /\ slots \in [0..Capacity-1 -> 0..MaxSent]
    /\ written \subseteq 0..Capacity-1
    /\ nextMessage \in 1..MaxSent+1
    /\ Len(enqueued) <= MaxSent
    /\ dropped \in 0..MaxSent

\* A slot is written only when it holds no undelivered datagram.
NoSlotReusedInFlight ==
    count < Capacity => slots[tail % Capacity] = 0

\* Datagrams leave (by Receive or Drop) in the order they were sent.
FifoOrder ==
    departed = SubSeq(enqueued, 1, Len(departed))

\* An empty queue has equal indices; with the restart rule both are zero.
EmptyIsRestarted ==
    count = 0 => /\ head = tail
                 /\ (RestartAtEmpty => head = 0)

\* Every slot ever written is below the deepest backlog the bound admits.
TouchedWithinBacklog ==
    written \subseteq 0..BacklogMax-1

=============================================================================
