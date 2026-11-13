-------------------------- MODULE RNSendReserveCalls --------------------------
(***************************************************************************)
(* The protocol send reserve of each send queue, one whole call at a time  *)
(* (sdk/rocketnet_server_transport.cpp: rocketnet_server_transport_        *)
(* send_packets, _start_packet, _finish_packet, _abort_packet,             *)
(* _send_capacity_available and _send_capacity_to; README-RNSendReserve.md).*)
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
(* state where every thread is idle to another such state (one thread, so  *)
(* one fill line; a line's tail there is tail here).                       *)
(*   Open(used, acked)   OpenFill, its choices used and Cardinality(acked) *)
(*   StartStaged(c)      own[c]:  OwnClaim, OwnIo (room), OwnCount         *)
(*                       tail:    Continue, Stage (room), Count            *)
(*                       else:    Size, Claim, Check [Read1, Read2, Read3, *)
(*                                Decide], Stage (room), Count             *)
(*   StartIoRefused(c)   own[c]:  OwnClaim, OwnIo (refused)                *)
(*                       tail:    Continue, Stage (refused), End           *)
(*                       else:    Size, Claim, Check [Read1-3, Decide],    *)
(*                                Stage (refused), End                     *)
(*   StartRefused(c)     Size, Claim, Check, Read1-3, Decide (refused),    *)
(*                       TakeBack, Retest (refused, then the counter)      *)
(*   Retire(q, n)        Retire, n times                                   *)
(*   Finish, Abort, Capacity(n), CapacityTo(c, n), CapacityAll(n): no      *)
(*                       step; they leave every word alone                 *)
(* A claim is a chunk of Chunk frames while the budget less the claims     *)
(* leaves Guard frames (the code: a chunk for every line), and one frame    *)
(* otherwise; the frames of a chunk not yet taken are the tail the calling  *)
(* thread's line holds, taken one a start with no write to the claims, and *)
(* given back when the transport refuses a frame and when main opens the   *)
(* budget again.  With one thread the single claim is never refused while  *)
(* a tail is held, so SEND_RESERVE_REFUSED_TAILS_HELD does not move here.  *)
(* The variables map by name (free, started, ownUsed, ownLeft, claims,     *)
(* budget, tail; started is the one line's count); ownTaken there is       *)
(* ownUsed here (an own frame the transport refused is given back before   *)
(* the call ends); own is which clients hold the ownLeft frames, and       *)
(* refused is the code's counter SEND_RESERVE_REFUSED.  RNSendReserve is   *)
(* one queue: each queue here is one copy of it, the own frames shared,    *)
(* and its assumption MaxOwn <= Reserve is the invariant OwnWithinReserve  *)
(* here (an open hands out no more own frames than the smallest queue's    *)
(* reserve; witness OwnUnbounded).  One thing differs, on the side of the  *)
(* code: Retire there is one frame.  The refinement is argued, not checked *)
(* by TLC; this module's own invariants are checked (MCRNSendReserveCalls) *)
(* and are RNSendReserve's two properties at the points between calls.     *)
(*                                                                         *)
(* Send queues: NQ queues, each with its own staging of MaxFree frames,    *)
(* budget, claims, tail, started and own frames used; client c stages on   *)
(* queue QueueOf(c).  NQ = 1 is the transport with one queue, the module   *)
(* before queues.  The own frames and their count are the server's.  The   *)
(* queues meet only in the capacity questions: Capacity is the fewest room *)
(* of any queue against the whole budget left; CapacityTo(c) is c's own    *)
(* queue's room against what that queue's budget leaves and c's own frame; *)
(* CapacityAll is the room summed over the queues against the whole budget *)
(* left.  The witness MinRoom answers CapacityTo with the fewest room of   *)
(* any queue, the send pass's question before queues were judged one by    *)
(* one: a full queue hides the room of every other (NoHiddenRoom).         *)
(*                                                                         *)
(* The reserve: each open computes it again, the formula Reserve cut to    *)
(* half the largest room the queue has shown at an open (seen), so a queue *)
(* whose first opens saw a stalled staging has its reserve back once an    *)
(* open sees the staging.  The witness StickyReserve keeps a reserve once  *)
(* cut (ReserveRestored).                                                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Clients,        \* the connected clients
    NQ,             \* send queues (send_queues)
    Reserve,        \* a queue's protocol reserve by the formula, in frames
    MaxFree,        \* each queue's staging, in frames
    SeenInit,       \* the largest room each queue showed at the opens before Init
    Chunk,          \* the frames one chunk claim takes (ROCKETNET_SERVER_FILL_CHUNK)
    Guard,          \* a chunk is claimed while the budget less the claims leaves this many frames (a chunk a line)
    \* reversed witnesses, each TRUE breaks one rule of the code
    RoomOnly,       \* the budget is the whole room, no reserve      (ReserveKept)
    NoRefresh,      \* the budget stays as main opened it            (NeverStarved)
    AbortReturns,   \* an aborted packet gives a claim back          (ReserveKept)
    TailLeak,       \* a refused frame gives back its own claim only (NeverStarved)
    MinRoom,        \* CapacityTo reads the fewest room of any queue (NoHiddenRoom)
    StickyReserve,  \* a reserve once cut is never computed again    (ReserveRestored)
    OwnUnbounded    \* an open hands every acknowledged client its own frame (OwnWithinReserve)

ASSUME Chunk \in Nat /\ Chunk >= 1 /\ Guard \in Nat /\ Guard >= Chunk
ASSUME NQ \in Nat /\ NQ >= 1 /\ SeenInit \in Nat

Queues == 1..NQ

\* send_queue_of: the queue a client's frames are staged on
QueueOf(c) == (c % NQ) + 1

VARIABLES
    free,           \* each queue's free frames, as the transport reports them
    budget,         \* each queue's grant cap (fill_budget)
    claims,         \* each queue's claims counter (fill_claims)
    tail,           \* frames of a granted chunk not yet taken, each queue (the calling thread's line)
    started,        \* game frames staged on each queue since the budget opened (fill_started)
    ownUsed,        \* own frames staged on each queue since then (fill_own_used)
    own,            \* the clients whose own frame is not yet taken (fill_own_frame)
    ownLeft,        \* how many those are (fill_own_frames_left)
    reserve,        \* each queue's protocol reserve (send_reserve)
    seen,           \* the largest room each queue showed at an open (send_room_seen)
    refused,        \* game packet starts refused for the reserve (SEND_RESERVE_REFUSED)
    starved,        \* a start was refused with a frame above the reserve (history)
    hidden          \* a CapacityTo answered 0 while a start to that client would stage (history)

vars == <<free, budget, claims, tail, started, ownUsed, own, ownLeft, reserve, seen, refused, starved, hidden>>

Max(a, b) == IF a > b THEN a ELSE b
Min(a, b) == IF a < b THEN a ELSE b

RECURSIVE SumOf(_, _)
SumOf(f, Q) == IF Q = {} THEN 0 ELSE LET q == CHOOSE x \in Q : TRUE IN f[q] + SumOf(f, Q \ {q})
RECURSIVE MinOf(_, _)
MinOf(f, Q) == LET q == CHOOSE x \in Q : TRUE
               IN IF Q = {q} THEN f[q] ELSE Min(f[q], MinOf(f, Q \ {q}))

\* the reserve an open computes from the largest room a queue has shown
ReserveFor(s) == Min(Reserve, s \div 2)

\* the room game packets may take above the reserve r still held back
Above(r, room, o) == LET held == IF r > o THEN r - o ELSE 0
                     IN IF RoomOnly THEN room ELSE IF room > held THEN room - held ELSE 0

\* a queue's budget as its state allows it now, never lowered
Raised(q) == IF NoRefresh THEN budget[q] ELSE Max(budget[q], started[q] + Above(reserve[q], free[q], ownUsed[q]))

\* the budget a claim leaves behind: untouched when the claim is under it
AfterClaim(q) == IF claims[q] < budget[q] THEN budget[q] ELSE Raised(q)

\* a claim is granted under the budget, or under the budget raised
Granted(q) == claims[q] < budget[q] \/ claims[q] + 1 <= Raised(q)

\* where a start's frame comes from: the tail when one is held; else a chunk
\* while the guard holds (a chunk under the guard always fits the budget:
\* Guard >= Chunk); else one frame
UseTail(q) == tail[q] > 0
UseChunk(q) == tail[q] = 0 /\ budget[q] >= claims[q] /\ budget[q] - claims[q] >= Guard

\* what a queue's budget, raised now, leaves the caller, its tail with it
Left(q) == tail[q] + (IF Raised(q) > claims[q] THEN Raised(q) - claims[q] ELSE 0)
BudgetLeft == ownLeft + SumOf([q \in Queues |-> Left(q)], Queues)

\* a start to c would stage a frame now
CanStage(c) == LET q == QueueOf(c)
               IN free[q] > 0 /\ (c \in own \/ UseTail(q) \/ UseChunk(q) \/ Granted(q))

\* the own frames an open hands out: the clients acknowledged, in slot
\* order, no more than the smallest queue's reserve holds
OwnCap(acked, most) == {c \in acked : Cardinality({d \in acked : d <= c}) <= most}

TypeOK ==
    /\ free \in [Queues -> 0..MaxFree]
    /\ budget \in [Queues -> Nat]
    /\ claims \in [Queues -> Nat]
    /\ tail \in [Queues -> 0..(Chunk - 1)]
    /\ started \in [Queues -> Nat]
    /\ ownUsed \in [Queues -> Nat]
    /\ own \subseteq Clients
    /\ ownLeft \in Nat
    /\ reserve \in [Queues -> Nat]
    /\ seen \in [Queues -> Nat]
    /\ refused \in Nat
    /\ starved \in BOOLEAN
    /\ hidden \in BOOLEAN

\* the state just after main opens the budget, no keep-alive acknowledged
\* yet, the opens before having shown each queue a room of SeenInit at
\* most: the queues are empty (MaxFree free) when SeenInit is at least the
\* staging, else that open found SeenInit free, the rest still in flight
\* (one state: the trace check starts here)
InitFree == Min(SeenInit, MaxFree)
Init ==
    /\ free = [q \in Queues |-> InitFree]
    /\ seen = [q \in Queues |-> SeenInit]
    /\ reserve = [q \in Queues |-> ReserveFor(SeenInit)]
    /\ budget = [q \in Queues |-> Above(ReserveFor(SeenInit), InitFree, 0)]
    /\ claims = [q \in Queues |-> 0]
    /\ tail = [q \in Queues |-> 0]
    /\ started = [q \in Queues |-> 0]
    /\ ownUsed = [q \in Queues |-> 0]
    /\ own = {}
    /\ ownLeft = 0
    /\ refused = 0
    /\ starved = FALSE
    /\ hidden = FALSE

\* send_packets, after an update whose protocol frames took used[q] frames
\* of each queue's room: the largest room seen grows to the room now, the
\* reserve is computed again (the formula, cut to half of that), the tail
\* goes back, the counts start over, each budget is the room above its
\* reserve, and each client whose keep-alive is acknowledged holds its own
\* frame
Open(used, acked) ==
    /\ \A q \in Queues : used[q] <= Min(free[q], reserve[q])
    /\ LET room == [q \in Queues |-> free[q] - used[q]]
           seen1 == [q \in Queues |-> Max(seen[q], room[q])]
           res1 == [q \in Queues |-> IF StickyReserve THEN Min(reserve[q], ReserveFor(seen1[q]))
                                      ELSE ReserveFor(seen1[q])]
           own1 == IF OwnUnbounded THEN acked ELSE OwnCap(acked, MinOf(res1, Queues))
       IN /\ free' = room
          /\ seen' = seen1
          /\ reserve' = res1
          /\ budget' = [q \in Queues |-> Above(res1[q], room[q], 0)]
          /\ own' = own1
          /\ ownLeft' = Cardinality(own1)
    /\ claims' = [q \in Queues |-> 0]
    /\ tail' = [q \in Queues |-> 0]
    /\ started' = [q \in Queues |-> 0]
    /\ ownUsed' = [q \in Queues |-> 0]
    /\ UNCHANGED <<refused, starved, hidden>>

\* start_packet gave a frame: the client's own frame, a frame of the tail,
\* the first frame of a chunk claimed now, or one claim granted, and the
\* transport staged it on the client's queue
StartStaged(c) ==
    LET q == QueueOf(c) IN
    /\ free[q] > 0
    /\ free' = [free EXCEPT ![q] = @ - 1]
    /\ IF c \in own
          THEN /\ own' = own \ {c}
               /\ ownLeft' = ownLeft - 1
               /\ ownUsed' = [ownUsed EXCEPT ![q] = @ + 1]
               /\ UNCHANGED <<budget, claims, tail, started>>
          ELSE /\ IF UseTail(q)
                     THEN /\ tail' = [tail EXCEPT ![q] = @ - 1]
                          /\ UNCHANGED <<budget, claims>>
                     ELSE IF UseChunk(q)
                             THEN /\ claims' = [claims EXCEPT ![q] = @ + Chunk]
                                  /\ tail' = [tail EXCEPT ![q] = Chunk - 1]
                                  /\ UNCHANGED budget
                             ELSE /\ Granted(q)
                                  /\ budget' = [budget EXCEPT ![q] = AfterClaim(q)]
                                  /\ claims' = [claims EXCEPT ![q] = @ + 1]
                                  /\ UNCHANGED tail
               /\ started' = [started EXCEPT ![q] = @ + 1]
               /\ UNCHANGED <<own, ownLeft, ownUsed>>
    /\ UNCHANGED <<reserve, seen, refused, starved, hidden>>

\* start_packet gave nothing because the transport refused the frame: an
\* own frame stays taken (its client's keep-alive is still due, and its
\* frame stays in the reserve); a claim is given back, and the tail with it:
\* the frame's claim and every frame of the tail still held leave the claims
\* in one add (the witness TailLeak gives back the frame's claim alone)
StartIoRefused(c) ==
    LET q == QueueOf(c) IN
    /\ IF c \in own
          THEN /\ own' = own \ {c}
               /\ ownLeft' = ownLeft - 1
               /\ UNCHANGED <<budget, claims, tail>>
          ELSE /\ IF UseTail(q)
                     THEN /\ claims' = [claims EXCEPT ![q] = IF TailLeak THEN @ - 1 ELSE @ - tail[q]]
                          /\ tail' = [tail EXCEPT ![q] = 0]
                          /\ UNCHANGED budget
                     ELSE IF UseChunk(q)
                             THEN /\ claims' = [claims EXCEPT ![q] = IF TailLeak THEN @ + Chunk - 1 ELSE @]
                                  /\ tail' = [tail EXCEPT ![q] = 0]
                                  /\ UNCHANGED budget
                             ELSE /\ Granted(q)
                                  /\ budget' = [budget EXCEPT ![q] = AfterClaim(q)]
                                  /\ UNCHANGED <<claims, tail>>
               /\ UNCHANGED <<own, ownLeft>>
    /\ UNCHANGED <<free, started, ownUsed, reserve, seen, refused, starved, hidden>>

\* start_packet gave nothing because the claim is past the budget, raised
\* to what the queue allows now: counted, and the transport never asked
StartRefused(c) ==
    LET q == QueueOf(c) IN
    /\ c \notin own
    /\ ~UseTail(q)
    /\ ~UseChunk(q)
    /\ ~Granted(q)
    /\ budget' = [budget EXCEPT ![q] = AfterClaim(q)]
    /\ refused' = refused + 1
    /\ IF free[q] + ownUsed[q] > reserve[q] THEN starved' = TRUE ELSE UNCHANGED starved
    /\ UNCHANGED <<free, claims, tail, started, ownUsed, own, ownLeft, reserve, seen, hidden>>

\* finish_packet: the frame is the transport's; the reserve's words stay
Finish == UNCHANGED <<free, budget, claims, tail, started, ownUsed, own, ownLeft, reserve, seen, refused, starved, hidden>>

\* abort_packet: the frame's claim is not given back (the frame left the
\* room when it was staged)
Abort ==
    /\ claims' = IF AbortReturns /\ claims[1] > 0 THEN [claims EXCEPT ![1] = @ - 1] ELSE claims
    /\ UNCHANGED <<free, budget, tail, started, ownUsed, own, ownLeft, reserve, seen, refused, starved, hidden>>

\* send_capacity_available gave n: the room (the fewest free frames of any
\* queue, the transport's send_room), and no more than the own frames not
\* taken, the tails the caller holds, and what each budget, as it would be
\* raised now, leaves
Capacity(n) ==
    /\ n = Min(MinOf(free, Queues), BudgetLeft)
    /\ UNCHANGED <<free, budget, claims, tail, started, ownUsed, own, ownLeft, reserve, seen, refused, starved, hidden>>

\* send_capacity_to(c) gave n: with one queue, Capacity; else the room of
\* c's own queue, and no more than what that queue's budget leaves and c's
\* own frame. MinRoom answers from the fewest room of any queue.
CapacityTo(c, n) ==
    LET q == QueueOf(c)
        answer == IF NQ = 1 \/ MinRoom THEN Min(MinOf(free, Queues), BudgetLeft)
                  ELSE IF free[q] = 0 THEN 0
                  ELSE Min(free[q], Left(q) + (IF c \in own THEN 1 ELSE 0))
    IN /\ n = answer
       /\ IF n = 0 /\ CanStage(c) THEN hidden' = TRUE ELSE UNCHANGED hidden
       /\ UNCHANGED <<free, budget, claims, tail, started, ownUsed, own, ownLeft, reserve, seen, refused, starved>>

\* send_capacity_to(-1) gave n: with one queue 0 (no other queue to take a
\* frame); else the room summed over the queues, within the whole budget left
CapacityAll(n) ==
    /\ n = IF NQ = 1 THEN 0 ELSE Min(SumOf(free, Queues), BudgetLeft)
    /\ UNCHANGED <<free, budget, claims, tail, started, ownUsed, own, ownLeft, reserve, seen, refused, starved, hidden>>

\* the transport retires n staged frames of queue q (asynchronous sends complete)
Retire(q, n) ==
    /\ free[q] + n <= MaxFree
    /\ free' = [free EXCEPT ![q] = @ + n]
    /\ UNCHANGED <<budget, claims, tail, started, ownUsed, own, ownLeft, reserve, seen, refused, starved, hidden>>

\* Open's room is matched before its acknowledged set is chosen: the same
\* steps, but a trace step (TraceRNSendReserveCalls) tries the acknowledged
\* sets only for the used frames that match the recorded room, not for every
\* used frames: with two queues of a reserve near 80, every pair was most of
\* the trace check's time
Next ==
    \/ \E used \in [Queues -> 0..Reserve] :
          /\ free' = [q \in Queues |-> free[q] - used[q]]
          /\ \E acked \in SUBSET Clients : Open(used, acked)
    \/ \E c \in Clients : StartStaged(c)
    \/ \E c \in Clients : StartIoRefused(c)
    \/ \E c \in Clients : StartRefused(c)
    \/ Finish
    \/ Abort
    \/ \E n \in 0..(NQ * MaxFree + Cardinality(Clients)) : Capacity(n)
    \/ \E c \in Clients, n \in 0..(NQ * MaxFree + Cardinality(Clients)) : CapacityTo(c, n)
    \/ \E n \in 0..(NQ * MaxFree + Cardinality(Clients)) : CapacityAll(n)
    \/ \E q \in Queues, n \in 1..MaxFree : Retire(q, n)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties                                                              *)
(***************************************************************************)

\* once a game frame is granted on a queue, the room left there after the
\* tail its thread holds has staged covers its reserve, less the own frames
\* used: the next update's protocol frames are
\* never refused for the game packets' sake (RNSendReserve ReserveKept
\* between calls)
ReserveKept == \A q \in Queues : started[q] + tail[q] > 0 => free[q] + ownUsed[q] >= reserve[q] + tail[q]

\* no start is refused for the reserve while its queue's room holds a frame
\* above it (RNSendReserve NeverStarved between calls)
NeverStarved == ~starved

\* with one thread every claim outstanding is a frame staged or a frame of
\* the tail the thread holds
ClaimsAreStarted == \A q \in Queues : claims[q] = started[q] + tail[q]

\* the count of own frames left is the clients that hold one
OwnLeftCounts == ownLeft = Cardinality(own)

\* a client's capacity is 0 only when a start to it would stage nothing:
\* a full queue hides no other queue's room
NoHiddenRoom == ~hidden

\* the own frames an open handed out, less those the transport refused, are
\* never more than the smallest queue's reserve: each comes out of the
\* reserve of the queue its client stages on (RNSendReserve's MaxOwn <=
\* Reserve, for every queue)
OwnWithinReserve == ownLeft + SumOf(ownUsed, Queues) <= MinOf(reserve, Queues)

\* each queue's reserve is the formula cut to half the largest room it has
\* shown at an open: a reserve cut while a queue was stalled comes back once
\* an open sees its staging
ReserveRestored == \A q \in Queues : reserve[q] = ReserveFor(seen[q])

=============================================================================
