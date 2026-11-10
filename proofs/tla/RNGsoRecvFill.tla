---------------------------- MODULE RNGsoRecvFill ----------------------------
(***************************************************************************)
(* The layout of one GSO receive drain (rocketnet_server_gso_drain in      *)
(* sdk/rocketnet_server_gso_linux.cpp): the worker's buffer is an array of *)
(* Cap slots filled from index num, and each recvmmsg returns up to vlen   *)
(* messages, each a GRO run of 1 .. Seg segments, one slot per segment.    *)
(* A segment read with no slot left is dropped as overflow. The model      *)
(* carries the same arithmetic as the code: the free slots Cap - num, the  *)
(* vlen of rocketnet_server_gso_recv_vlen (free \div Seg, at most Batch),  *)
(* the fill index num advanced per staged segment, and the drop at         *)
(* num = Cap.                                                              *)
(*                                                                         *)
(* RNGsoHandoff is the abstract model of the same worker: it owns the      *)
(* buffer exchange, with RxFill one datagram at a time. This model owns    *)
(* the fill at batch x segment granularity.                                *)
(*                                                                         *)
(* Rule switch: BoundByRoom. TRUE is the code: vlen = (Cap - num) \div Seg *)
(* capped at Batch, and a vlen of 0 ends the drain with the socket kept.   *)
(* FALSE restores the defect: every call asks for a whole batch while one  *)
(* slot is free, so a run read into a nearly full buffer drops segments    *)
(* the socket would have held.                                             *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Cap,            \* slots in the worker's buffer (ROCKETNET_GSO_RECV_QUEUE_SIZE)
    Seg,            \* segments one GRO message may carry (ROCKETNET_GSO_RECV_MAX_SEGMENTS)
    Batch,          \* messages one recvmmsg may return (ROCKETNET_GSO_RECV_BATCH)
    MaxArrive,      \* messages the kernel queues in all
    SockCap,        \* messages the socket holds
    BoundByRoom     \* rule switch: TRUE is the code

ASSUME Cap \in Nat \ {0} /\ Seg \in Nat \ {0} /\ Batch \in Nat \ {0}
ASSUME MaxArrive \in Nat /\ SockCap \in Nat \ {0} /\ BoundByRoom \in BOOLEAN

VARIABLES
    socket,         \* the queued messages, each its segment count
    num,            \* the fill index: slots 1 .. num are taken
    dropped,        \* segments read and dropped for want of a slot
    arrived         \* messages the kernel has queued

vars == << socket, num, dropped, arrived >>

Min(a, b) == IF a < b THEN a ELSE b

(* rocketnet_server_gso_recv_vlen *)
Vlen ==
    IF BoundByRoom
        THEN Min( ( Cap - num ) \div Seg, Batch )
        ELSE IF num < Cap THEN Batch ELSE 0

Init ==
    /\ socket = << >>
    /\ num = 0
    /\ dropped = 0
    /\ arrived = 0

(* the kernel queues one message of k segments *)
Arrive ==
    /\ arrived < MaxArrive
    /\ Len(socket) < SockCap
    /\ \E k \in 1 .. Seg : socket' = Append( socket, k )
    /\ arrived' = arrived + 1
    /\ UNCHANGED << num, dropped >>

(* stage n segments from slot num + 1, dropping those past Cap: the inner *)
(* loop over one message's segments                                        *)
StageRun(n) ==
    LET fit == Min( n, Cap - num )
    IN /\ num' = num + fit
       /\ dropped' = dropped + ( n - fit )

(* one recvmmsg of the drain, taking one message: a call of vlen messages  *)
(* is vlen of these steps with no arrival between them, which the model    *)
(* over-approximates by letting arrivals interleave                        *)
Read ==
    /\ Vlen > 0
    /\ socket # << >>
    /\ StageRun( Head(socket) )
    /\ socket' = Tail(socket)
    /\ UNCHANGED arrived

(* main returns a free buffer: the worker starts filling from slot 1 *)
Publish ==
    /\ num > 0
    /\ num' = 0
    /\ UNCHANGED << socket, dropped, arrived >>

(* every message the kernel queues is read and published: the run ends    *)
Done ==
    /\ arrived = MaxArrive /\ socket = << >> /\ num = 0
    /\ UNCHANGED vars

Next == Arrive \/ Read \/ Publish \/ Done

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ num \in 0 .. Cap
    /\ dropped \in Nat
    /\ \A i \in 1 .. Len(socket) : socket[i] \in 1 .. Seg

(* nothing read is dropped for want of room: a message is read only when   *)
(* every segment it can carry has a slot                                   *)
NoOverflow == dropped = 0

=============================================================================
