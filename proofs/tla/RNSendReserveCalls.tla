-------------------------- MODULE RNSendReserveCalls --------------------------
(***************************************************************************)
(* The protocol send reserve of one send queue, one whole call at a time   *)
(* (sdk/rocketnet_server_transport.cpp: rocketnet_server_transport_        *)
(* send_packets, _start_packet, _finish_packet, _abort_packet and          *)
(* _send_capacity_available; README-RNSendReserve.md).                     *)
(*                                                                         *)
(* RNSendReserve.tla is the design: every read and write of a shared word  *)
(* is a step, so threads may interleave inside a call.  This module is the *)
(* same rules as one thread sees them, each call run to its end, which is  *)
(* what the trace check can record (tests/tla_trace_send_reserve.cpp,      *)
(* TraceRNSendReserveCalls.tla): the code's own calls must be these        *)
(* actions, with these numbers.                                            *)
(*                                                                         *)
(* The refinement, stated: each action here is a run of RNSendReserve      *)
(* steps by one thread f with no other thread's step between them, from a  *)
(* state where every thread is idle to another such state.                 *)
(*   Open(used, acked)   OpenFill, its choices used and Cardinality(acked) *)
(*   StartStaged(c)      own[c]:  OwnClaim, OwnIo (room), OwnCount         *)
(*                       else:    Claim, Check [Read1, Read2, Read3],      *)
(*                                Stage (room), Count                      *)
(*   StartIoRefused(c)   own[c]:  OwnClaim, OwnIo (refused)                *)
(*                       else:    Claim, Check [Read1-3], Stage (refused)  *)
(*   StartRefused(c)     Claim, Check, Read1, Read2, Read3 (Refuse),       *)
(*                       TakeBack, Retest (FinalRefuse)                    *)
(*   Retire(n)           Retire, n times                                   *)
(*   Finish, Abort, Capacity(n): no step; they leave every word alone      *)
(* The variables map by name (free, started, ownUsed, ownLeft, claims,     *)
(* budget); ownTaken there is ownUsed here (an own frame the transport     *)
(* refused is given back before the call ends); own is which clients hold  *)
(* the ownLeft frames, and refused is the code's counter                   *)
(* SEND_RESERVE_REFUSED.  Two things differ, both on the side of the code: *)
(* RNSendReserve's Stage refuses a frame only when the room is 0, where a  *)
(* transport here may refuse any start (StartIoRefused has no room guard); *)
(* and Retire there is one frame.  The refinement is argued, not checked   *)
(* by TLC; this module's own invariants are checked (MCRNSendReserveCalls) *)
(* and are RNSendReserve's two properties at the points between calls.     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Clients,        \* the connected clients
    Reserve,        \* the queue's protocol reserve, in frames
    MaxFree,        \* the queue's staging, in frames
    \* reversed witnesses, each TRUE breaks one rule of the code
    RoomOnly,       \* the budget is the whole room, no reserve      (ReserveKept)
    NoRefresh,      \* the budget stays as main opened it            (NeverStarved)
    AbortReturns    \* an aborted packet gives a claim back          (ReserveKept)

VARIABLES
    free,           \* the queue's free frames, as the transport reports them
    budget,         \* the grant cap (fill_budget)
    claims,         \* the claims counter (fill_claims)
    started,        \* game frames staged since the budget opened (fill_started)
    ownUsed,        \* own frames staged since then (fill_own_used)
    own,            \* the clients whose own frame is not yet taken (fill_own_frame)
    ownLeft,        \* how many those are (fill_own_frames_left)
    refused,        \* game packet starts refused for the reserve (SEND_RESERVE_REFUSED)
    starved         \* a start was refused with a frame above the reserve (history)

vars == <<free, budget, claims, started, ownUsed, own, ownLeft, refused, starved>>

Max(a, b) == IF a > b THEN a ELSE b
Min(a, b) == IF a < b THEN a ELSE b

\* the room game packets may take above the reserve still held back
Above(r, o) == LET held == IF Reserve > o THEN Reserve - o ELSE 0
               IN IF RoomOnly THEN r ELSE IF r > held THEN r - held ELSE 0

\* the budget as the queue's state allows it now, never lowered
Raised == IF NoRefresh THEN budget ELSE Max(budget, started + Above(free, ownUsed))

\* the budget a claim leaves behind: untouched when the claim is under it
AfterClaim == IF claims < budget THEN budget ELSE Raised

\* a claim is granted under the budget, or under the budget raised
Granted == claims < budget \/ claims + 1 <= Raised

TypeOK ==
    /\ free \in 0..MaxFree
    /\ budget \in Nat
    /\ claims \in Nat
    /\ started \in Nat
    /\ ownUsed \in Nat
    /\ own \subseteq Clients
    /\ ownLeft \in Nat
    /\ refused \in Nat
    /\ starved \in BOOLEAN

\* the state just after main first opens the budget
Init ==
    /\ free \in 0..MaxFree
    /\ budget = Above(free, 0)
    /\ claims = 0
    /\ started = 0
    /\ ownUsed = 0
    /\ own \in SUBSET Clients
    /\ ownLeft = Cardinality(own)
    /\ refused = 0
    /\ starved = FALSE

\* send_packets, after an update whose protocol frames took used frames of
\* the room: the counts start over, the budget is the room above the
\* reserve, and each client whose keep-alive is acknowledged holds its own
\* frame
Open(used, acked) ==
    /\ used <= Min(free, Reserve)
    /\ free' = free - used
    /\ budget' = Above(free - used, 0)
    /\ claims' = 0
    /\ started' = 0
    /\ ownUsed' = 0
    /\ own' = acked
    /\ ownLeft' = Cardinality(acked)
    /\ UNCHANGED <<refused, starved>>

\* start_packet gave a frame: the client's own frame, or a claim granted,
\* and the transport staged it
StartStaged(c) ==
    /\ free > 0
    /\ free' = free - 1
    /\ IF c \in own
          THEN /\ own' = own \ {c}
               /\ ownLeft' = ownLeft - 1
               /\ ownUsed' = ownUsed + 1
               /\ UNCHANGED <<budget, claims, started>>
          ELSE /\ Granted
               /\ budget' = AfterClaim
               /\ claims' = claims + 1
               /\ started' = started + 1
               /\ UNCHANGED <<own, ownLeft, ownUsed>>
    /\ UNCHANGED <<refused, starved>>

\* start_packet gave nothing because the transport refused the frame: an
\* own frame stays taken (its client's keep-alive is still due, and its
\* frame stays in the reserve); a claim is given back
StartIoRefused(c) ==
    /\ IF c \in own
          THEN /\ own' = own \ {c}
               /\ ownLeft' = ownLeft - 1
               /\ UNCHANGED budget
          ELSE /\ Granted
               /\ budget' = AfterClaim
               /\ UNCHANGED <<own, ownLeft>>
    /\ UNCHANGED <<free, claims, started, ownUsed, refused, starved>>

\* start_packet gave nothing because the claim is past the budget, raised
\* to what the queue allows now: counted, and the transport never asked
StartRefused(c) ==
    /\ c \notin own
    /\ ~Granted
    /\ budget' = AfterClaim
    /\ refused' = refused + 1
    /\ IF free + ownUsed > Reserve THEN starved' = TRUE ELSE UNCHANGED starved
    /\ UNCHANGED <<free, claims, started, ownUsed, own, ownLeft>>

\* finish_packet: the frame is the transport's; the reserve's words stay
Finish == UNCHANGED vars

\* abort_packet: the frame's claim is not given back (the frame left the
\* room when it was staged)
Abort ==
    /\ claims' = IF AbortReturns /\ claims > 0 THEN claims - 1 ELSE claims
    /\ UNCHANGED <<free, budget, started, ownUsed, own, ownLeft, refused, starved>>

\* send_capacity_available gave n: the room, and no more than the own
\* frames not taken plus what the budget, as it would be raised now, leaves
Capacity(n) ==
    /\ n = Min(free, ownLeft + (IF Raised > claims THEN Raised - claims ELSE 0))
    /\ UNCHANGED vars

\* the transport retires n staged frames (asynchronous sends complete)
Retire(n) ==
    /\ free + n <= MaxFree
    /\ free' = free + n
    /\ UNCHANGED <<budget, claims, started, ownUsed, own, ownLeft, refused, starved>>

Next ==
    \/ \E used \in 0..Reserve, acked \in SUBSET Clients : Open(used, acked)
    \/ \E c \in Clients : StartStaged(c)
    \/ \E c \in Clients : StartIoRefused(c)
    \/ \E c \in Clients : StartRefused(c)
    \/ Finish
    \/ Abort
    \/ \E n \in 0..MaxFree : Capacity(n)
    \/ \E n \in 1..MaxFree : Retire(n)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* once a game frame is granted, the room left covers the reserve, less
\* the own frames used: the next update's protocol frames are never
\* refused for the game packets' sake (RNSendReserve ReserveKept between
\* calls)
ReserveKept == started > 0 => free + ownUsed >= Reserve

\* no start is refused for the reserve while the room holds a frame above
\* it (RNSendReserve NeverStarved between calls)
NeverStarved == ~starved

\* with one thread every claim outstanding is a frame staged
ClaimsAreStarted == claims = started

\* the count of own frames left is the clients that hold one
OwnLeftCounts == ownLeft = Cardinality(own)

=============================================================================
