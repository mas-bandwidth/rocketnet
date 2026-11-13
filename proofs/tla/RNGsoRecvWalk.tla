--------------------------- MODULE RNGsoRecvWalk ---------------------------
(***************************************************************************)
(* Main's walk over the GSO receive queues, one rocketnet_server_gso_     *)
(* receive_packets call per Receive step (sdk/rocketnet_server_gso_linux.  *)
(* cpp).  Each queue's hand-off is RNGsoHandoff; this module abstracts it  *)
(* to the packets left in the buffer main holds for the queue (left).     *)
(*                                                                         *)
(* A call first lets every queue whose held buffer is exhausted claim a    *)
(* ready buffer of any size up to QueueSize, or none (the worker's         *)
(* publishes are the environment).  It then walks the queues from start,   *)
(* each taking what it has up to the room left in the per-update budget    *)
(* (rocketnet_server_gso_receive_from_queue keeps the unread tail).  The   *)
(* next call starts after the queue on which the budget ran out, or one    *)
(* after this call's start when it did not run out                         *)
(* (rocketnet_xdp_drain_queue).                                            *)
(*                                                                         *)
(* Rule switch StartAtZero, FALSE in the passing run: every call starts    *)
(* at queue 0.  MCRNGsoRecvWalkBrokenStartAtZero.cfg sets it TRUE and      *)
(* violates NoQueueStarves: with two full queues filling the budget, the   *)
(* later queues never deliver.                                             *)
(*                                                                         *)
(* The XDP server's walk (rocketnet_server_socket_receive_packets in       *)
(* sdk/rocketnet_server_socket_linux.cpp) is the same walk: each queue     *)
(* returns its staged datagrams from main's cursor within the budget's    *)
(* room, and the queue on which the budget ended, its own last datagram   *)
(* included, is filled (rocketnet_server_socket_take_range).              *)
(* MCRNGsoRecvWalkXdp.cfg runs it at three queues and                     *)
(* MCRNGsoRecvWalkXdpTwo.cfg at the witness's two, budget two.            *)
(* Rule switch FillAtNextQueue, FALSE in the passing runs: the XDP walk   *)
(* before 2026-11-13, whose range said the budget ended only when it met  *)
(* a datagram it could not take, so a queue whose last datagram ended the *)
(* budget exactly was not filled and the next queue holding datagrams     *)
(* was.  MCRNGsoRecvWalkBrokenFillAtNextQueue.cfg sets it TRUE and        *)
(* violates NoQueueStarves: with two queues, queue 0 filling the budget    *)
(* exactly on every call starts every call at queue 0 again.              *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS NumQueues,   \* receive queues
          QueueSize,   \* packets one receive buffer holds
          Budget       \* packets one receive call delivers at most

ASSUME NumQueues >= 1 /\ QueueSize >= 1 /\ Budget >= 1

StartAtZero == FALSE

FillAtNextQueue == FALSE

Queues == 0 .. NumQueues - 1

Min(a, b) == IF a < b THEN a ELSE b

VARIABLES
    start,   \* the queue the next call starts at
    left,    \* packets left in the buffer main holds, per queue
    waits    \* calls in a row on which the queue had packets and delivered none (saturates at NumQueues)

vars == << start, left, waits >>

Init ==
    /\ start = 0
    /\ left = [ q \in Queues |-> 0 ]
    /\ waits = [ q \in Queues |-> 0 ]

(* the walk from step i with room packets of budget left: what each queue *)
(* takes, and the first queue on which the room ran out (-1 if none); an  *)
(* earlier queue's filled replaces a later one's, so the first stands.    *)
(* Under FillAtNextQueue the filled queue is instead the first that keeps *)
(* packets it could not take.                                             *)
RECURSIVE Walk(_, _, _, _, _)
Walk(s, i, room, l, taken) ==
    IF i = NumQueues THEN [ left |-> l, taken |-> taken, filled |-> -1 ]
    ELSE LET q    == ( s + i ) % NumQueues
             take == Min( room, l[q] )
             rest == Walk( s, i + 1, room - take,
                           [ l EXCEPT ![q] = @ - take ],
                           [ taken EXCEPT ![q] = take ] )
             ends == IF FillAtNextQueue THEN l[q] > take
                                        ELSE room > 0 /\ room - take = 0
         IN  IF ends
             THEN [ rest EXCEPT !.filled = q ]
             ELSE rest

Receive ==
    \E fresh \in [ Queues -> 0 .. QueueSize ] :
        LET claimed == [ q \in Queues |-> IF left[q] = 0 THEN fresh[q] ELSE left[q] ]
            w       == Walk( start, 0, Budget, claimed, [ q \in Queues |-> 0 ] )
            origin  == IF w.filled >= 0 THEN w.filled ELSE start
        IN  /\ left' = w.left
            /\ waits' = [ q \in Queues |->
                            IF claimed[q] > 0 /\ w.taken[q] = 0
                            THEN Min( waits[q] + 1, NumQueues )
                            ELSE 0 ]
            /\ start' = IF StartAtZero THEN 0 ELSE ( origin + 1 ) % NumQueues

Next == Receive

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ start \in Queues
    /\ left \in [ Queues -> 0 .. QueueSize ]
    /\ waits \in [ Queues -> 0 .. NumQueues ]

(* a queue holding packets delivers some within NumQueues calls, however   *)
(* full the other queues stay                                              *)
NoQueueStarves == \A q \in Queues : waits[q] < NumQueues

=============================================================================
