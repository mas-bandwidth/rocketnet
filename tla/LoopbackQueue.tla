---------------------------- MODULE LoopbackQueue ----------------------------
(***************************************************************************)
(* One endpoint queue of the loopback transport                           *)
(* (sdk/rocketnet_driver_loopback.cpp, rocketnet_loopback_endpoint_t): a   *)
(* ring of Capacity datagram slots, allocated whole at create, with        *)
(* free-running head and tail indices and a count, every operation under   *)
(* the one state mutex, so each action here is atomic.                     *)
(*                                                                         *)
(* Send is a send into the queue (loopback_send and                       *)
(* loopback_server_send_batch_impl through loopback_queue_tail): the       *)
(* datagram is written at slot tail mod Capacity, or, on a full queue,     *)
(* dropped and counted.  Receive is a dequeue that hands the datagram to   *)
(* the caller (loopback_client_receive, loopback_server_receive_batch);    *)
(* Drop is the dequeue that discards a datagram larger than the caller's   *)
(* buffer.  Both dequeues go through loopback_queue_pop.                   *)
(*                                                                         *)
(* RestartAtEmpty selects the rule: a dequeue that leaves the queue empty  *)
(* sets head and tail back to 0.  Without it the indices only advance, so *)
(* a queue that never holds more than one datagram still writes every one *)
(* of its slots in turn, and each first write touches a fresh page.        *)
(*                                                                         *)
(* Datagrams carry their send order as a ghost id (1, 2, ...); slots hold *)
(* the id last written there (0: never written).  run counts the           *)
(* datagrams taken in since the queue was last empty and deepest is the    *)
(* largest run seen: the backlog TouchedWithinBacklog bounds the written   *)
(* slots by.  A queue drained every update has a run of one update's       *)
(* traffic at most.                                                        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Capacity,           \* the queue's slots (2048 for a client in the code)
    Bound,              \* the instance's bound on send attempts
    RestartAtEmpty      \* TRUE: a dequeue that empties the queue restarts it at slot 0

ASSUME Capacity >= 2 /\ Bound >= 1
ASSUME RestartAtEmpty \in BOOLEAN

Slots == 0..(Capacity - 1)

VARIABLES
    head,               \* free-running index of the oldest queued datagram
    tail,               \* free-running index of the next slot to write
    count,              \* datagrams queued
    slots,              \* slot -> the id of the datagram last written there
    touched,            \* the slots ever written
    attempts,           \* send attempts so far (the instance bound)
    sent,               \* datagrams taken into the queue (ids 1..sent)
    out,                \* the id of the last datagram dequeued
    fullDrops,          \* sends dropped on a full queue
    oversizeDrops,      \* dequeues dropped as larger than the caller's buffer
    run,                \* datagrams taken in since the queue was last empty
    deepest,            \* the largest run seen
    reused              \* ghost: a slot was written while it held an undelivered datagram

vars == << head, tail, count, slots, touched, attempts, sent, out,
           fullDrops, oversizeDrops, run, deepest, reused >>

Max(a, b) == IF a >= b THEN a ELSE b

\* the slots that hold undelivered datagrams
Live == { (head + i) % Capacity : i \in 0..(count - 1) }

Init ==
    /\ head = 0
    /\ tail = 0
    /\ count = 0
    /\ slots = [ s \in Slots |-> 0 ]
    /\ touched = {}
    /\ attempts = 0
    /\ sent = 0
    /\ out = 0
    /\ fullDrops = 0
    /\ oversizeDrops = 0
    /\ run = 0
    /\ deepest = 0
    /\ reused = FALSE

\* loopback_queue_tail, then the write and ++tail, ++count; a full queue
\* drops the datagram and counts it.
Send ==
    /\ attempts < Bound
    /\ attempts' = attempts + 1
    /\ IF count < Capacity
         THEN LET s == tail % Capacity IN
              /\ slots' = [ slots EXCEPT ![s] = sent + 1 ]
              /\ touched' = touched \cup { s }
              /\ reused' = (reused \/ s \in Live)
              /\ tail' = tail + 1
              /\ count' = count + 1
              /\ sent' = sent + 1
              /\ run' = run + 1
              /\ deepest' = Max(deepest, run + 1)
              /\ UNCHANGED << head, out, fullDrops >>
         ELSE /\ fullDrops' = fullDrops + 1
              /\ UNCHANGED << head, tail, count, slots, touched, sent, out, run, deepest, reused >>
    /\ UNCHANGED oversizeDrops

\* loopback_queue_pop: ++head, --count, and at empty the restart.
Pop ==
    /\ count' = count - 1
    /\ IF count = 1 /\ RestartAtEmpty
         THEN /\ head' = 0
              /\ tail' = 0
         ELSE /\ head' = head + 1
              /\ UNCHANGED tail
    /\ run' = IF count = 1 THEN 0 ELSE run

\* a dequeue that hands the head datagram to the caller
Receive ==
    /\ count > 0
    /\ out' = slots[head % Capacity]
    /\ Pop
    /\ UNCHANGED << slots, touched, attempts, sent, fullDrops, oversizeDrops, deepest, reused >>

\* a dequeue that drops the head datagram, larger than the caller's buffer
Drop ==
    /\ count > 0
    /\ out' = slots[head % Capacity]
    /\ oversizeDrops' = oversizeDrops + 1
    /\ Pop
    /\ UNCHANGED << slots, touched, attempts, sent, fullDrops, deepest, reused >>

Next == Send \/ Receive \/ Drop

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ head \in 0..Bound
    /\ tail \in 0..Bound
    /\ count \in 0..Capacity
    /\ slots \in [ Slots -> 0..Bound ]
    /\ touched \subseteq Slots
    /\ attempts \in 0..Bound
    /\ sent \in 0..Bound
    /\ out \in 0..Bound
    /\ fullDrops \in 0..Bound
    /\ oversizeDrops \in 0..Bound
    /\ run \in 0..Bound
    /\ deepest \in 0..Bound
    /\ reused \in BOOLEAN

\* a slot is written only when it holds no undelivered datagram
NoSlotReusedInFlight == ~reused

\* the queued datagrams are the next ids after the last one out, in order
\* from head: datagrams leave in the order they were sent
FifoOrder ==
    /\ tail - head = count
    /\ out + count = sent
    /\ \A i \in 0..(count - 1) : slots[(head + i) % Capacity] = out + 1 + i

\* an empty queue has head = tail; with the restart, both are 0
EmptyIsRestarted ==
    count = 0 => (head = tail /\ (RestartAtEmpty => head = 0))

\* every slot ever written is below the deepest backlog seen
TouchedWithinBacklog == \A s \in touched : s < deepest

=============================================================================
