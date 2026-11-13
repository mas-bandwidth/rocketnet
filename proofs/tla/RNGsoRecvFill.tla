---------------------------- MODULE RNGsoRecvFill ----------------------------
(***************************************************************************)
(* The layout of one GSO receive drain (rocketnet_server_gso_drain in      *)
(* sdk/rocketnet_server_gso_linux.cpp): the worker's buffer is an array of *)
(* Cap slots filled from index num.  Each recvmmsg call asks for vlen      *)
(* messages, vlen fixed once before the call from the free slots           *)
(* (rocketnet_server_gso_recv_vlen: free \div Seg, at most Batch), and     *)
(* returns up to vlen of the messages queued at that moment.  Every        *)
(* message it returns is staged, one slot per segment from num; a segment  *)
(* with no slot left is dropped and counted (RECEIVE_DROPPED_OVERFLOW,     *)
(* the drop at num = Cap).  The model carries the same arithmetic: Vlen,   *)
(* the call of Min(Vlen, queued) messages, and the per-segment fill and    *)
(* drop, with no recount of the room between the messages of one call.     *)
(*                                                                         *)
(* A message is a GRO run of 1 .. MaxRun segments.  The drain makes room   *)
(* for Seg segments per message (ROCKETNET_GSO_RECV_MAX_SEGMENTS, 64: the  *)
(* kernel's UDP_GRO_CNT_MAX, NIC GRO's limit), but a local sender's        *)
(* segmented send can coalesce up to UDP_MAX_SEGMENTS (128 on recent       *)
(* kernels), so MaxRun may exceed Seg (sdk/rocketnet_server_gso.h).  Such  *)
(* a run can lose segments to a buffer the call's vlen said had room:      *)
(* the code stages what fits and counts the rest.  What the room rule      *)
(* guarantees is that nothing is dropped beyond the segments the calls'    *)
(* messages carried past Seg (DropOnlyPastSeg), and so nothing at all      *)
(* while no run is longer than Seg (NoOverflow).                            *)
(*                                                                         *)
(* Abstracted: a segment the worker's screen refuses (size, prefilter)     *)
(* takes no slot, which only lowers num; a truncated or unaddressable      *)
(* message is dropped whole before staging.  Without GRO a message is one  *)
(* datagram: Seg = MaxRun = 1.                                             *)
(*                                                                         *)
(* RNGsoHandoff is the abstract model of the same worker: it owns the      *)
(* buffer exchange, with RxFill one datagram at a time. This model owns    *)
(* the fill at call x message x segment granularity.                       *)
(*                                                                         *)
(* Rule switch VlenRule, the messages a call asks for:                     *)
(*   "room"     the code: Min((Cap - num) \div Seg, Batch); 0 ends the     *)
(*              drain with the socket kept.                                *)
(*   "anyslot"  a whole batch while one slot is free.                      *)
(*   "onefits"  a whole batch once one message of Seg segments fits: the   *)
(*              over-request a per-message recount of the room would hide, *)
(*              since the code stages every message the call returns.      *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Cap,            \* slots in the worker's buffer (ROCKETNET_GSO_RECV_QUEUE_SIZE)
    Seg,            \* segments the drain makes room for per message (ROCKETNET_GSO_RECV_MAX_SEGMENTS)
    MaxRun,         \* segments one queued message may carry (UDP_MAX_SEGMENTS from a local sender)
    Batch,          \* messages one recvmmsg may return (ROCKETNET_GSO_RECV_BATCH)
    MaxArrive,      \* messages the kernel queues in all
    SockCap,        \* messages the socket holds
    VlenRule        \* rule switch: "room" is the code

ASSUME Cap \in Nat \ {0} /\ Seg \in Nat \ {0} /\ Batch \in Nat \ {0}
ASSUME MaxRun \in Nat \ {0} /\ Cap >= Seg
ASSUME MaxArrive \in Nat /\ SockCap \in Nat \ {0}
ASSUME VlenRule \in {"room", "anyslot", "onefits"}

VARIABLES
    socket,         \* the queued messages, each its segment count
    num,            \* the fill index: slots 1 .. num are taken
    dropped,        \* segments read and dropped for want of a slot (RECEIVE_DROPPED_OVERFLOW)
    excess,         \* ghost: segments the read messages carried past Seg
    arrived         \* messages the kernel has queued

vars == << socket, num, dropped, excess, arrived >>

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

(* rocketnet_server_gso_recv_vlen, read once before the call *)
Vlen ==
    CASE VlenRule = "room"    -> Min( ( Cap - num ) \div Seg, Batch )
      [] VlenRule = "anyslot" -> IF num < Cap THEN Batch ELSE 0
      [] VlenRule = "onefits" -> IF Cap - num >= Seg THEN Batch ELSE 0

Init ==
    /\ socket = << >>
    /\ num = 0
    /\ dropped = 0
    /\ excess = 0
    /\ arrived = 0

(* the kernel queues one message of k segments *)
Arrive ==
    /\ arrived < MaxArrive
    /\ Len(socket) < SockCap
    /\ \E k \in 1 .. MaxRun : socket' = Append( socket, k )
    /\ arrived' = arrived + 1
    /\ UNCHANGED << num, dropped, excess >>

(* the inner loops over the call's messages and each message's segments:  *)
(* each segment takes slot n + 1 while one is left, else is dropped; the   *)
(* room is never recounted between messages                                *)
RECURSIVE StageAll(_, _, _, _)
StageAll(msgs, n, d, e) ==
    IF msgs = << >>
        THEN [num |-> n, dropped |-> d, excess |-> e]
        ELSE LET k   == Head(msgs)
                 fit == Min( k, Cap - n )
             IN StageAll( Tail(msgs), n + fit, d + ( k - fit ), e + Max( k - Seg, 0 ) )

(* one recvmmsg of the drain: vlen fixed from the room before the call,    *)
(* the call returns the first Min(vlen, queued) messages at once, and      *)
(* every one of them is staged                                             *)
Read ==
    /\ Vlen > 0
    /\ socket # << >>
    /\ LET c == Min( Vlen, Len(socket) )
           r == StageAll( SubSeq( socket, 1, c ), num, dropped, excess )
       IN /\ num' = r.num
          /\ dropped' = r.dropped
          /\ excess' = r.excess
          /\ socket' = SubSeq( socket, c + 1, Len(socket) )
    /\ UNCHANGED arrived

(* main returns a free buffer: the worker starts filling from slot 1 *)
Publish ==
    /\ num > 0
    /\ num' = 0
    /\ UNCHANGED << socket, dropped, excess, arrived >>

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
    /\ excess \in Nat
    /\ \A i \in 1 .. Len(socket) : socket[i] \in 1 .. MaxRun

(* the room rule's guarantee: a call asks only for messages whose Seg      *)
(* segments each have a slot, so a drop is never more than the segments    *)
(* the calls' messages carried past Seg.  It is not that no datagram is    *)
(* lost: while the worker cannot read, the kernel drops what its full      *)
(* socket cannot hold.  Here Arrive waits for room instead; the code       *)
(* counts those drops from SO_RXQ_OVFL in GSO_RECEIVE_DROPPED_OVERFLOW     *)
DropOnlyPastSeg == dropped <= excess

(* with no run longer than Seg (MaxRun <= Seg, every NIC GRO run), nothing *)
(* the worker reads is dropped for want of room.  A local sender's longer  *)
(* run can be dropped in part (MCRNGsoRecvFillReachLocalRunDrop.cfg)       *)
NoOverflow == dropped = 0

=============================================================================
