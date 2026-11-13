-------------------------- MODULE RNReceiveHandoff --------------------------
(***************************************************************************)
(* The receive handoff a server transport's worker and main thread share   *)
(* (sdk/rocketnet_receive_handoff.h), over three buffers.  The worker      *)
(* banks datagrams into its buffer outside the mutex (Bank) and, at the    *)
(* end of each pass with results, commits its count under the mutex       *)
(* (Commit, rocketnet_receive_handoff_commit); when main has started on    *)
(* its buffer it moves to the free one in the same step.  Main's receive   *)
(* call (Call) resets a finished tail, then try-acquires the mutex: on     *)
(* success (rocketnet_receive_handoff_begin), when the worker has left     *)
(* main's buffer and main has finished its tail, main frees the tail,      *)
(* makes its own buffer the tail with the worker's final count and starts  *)
(* on the worker's buffer; then it reads up to its buffer's committed      *)
(* count.  A busy mutex leaves main with the limits it holds.  Main then   *)
(* returns the tail's unread datagrams and its own buffer's up to the      *)
(* limit, within the call's budget.                                        *)
(*                                                                         *)
(* A call is one step: what main reads lies below counts the worker no     *)
(* longer changes (the tail's final count, its own limit), and the         *)
(* worker's steps touch only its buffer past its committed count, the      *)
(* roles and the counts under the mutex, so the worker's steps during a   *)
(* call commute with main's reads.  A buffer's datagrams are consecutive   *)
(* (the worker banks the next ones into its own buffer until it moves), so *)
(* a buffer is its first datagram and its count.                           *)
(*                                                                         *)
(* The property the handoff exists for is DeliveredByNextCall: a datagram  *)
(* the worker committed before main's call took the mutex is returned by   *)
(* that call, unless the mutex was busy, the budget cut the call, or the   *)
(* call began with datagrams left from a cut call (main reads in order, so *)
(* a backlog delays what follows it, as a full budget always has).         *)
(* committedTop is the newest datagram committed, and late records a call  *)
(* that broke the property.                                                *)
(*                                                                         *)
(* Safety: the roles are distinct and a free buffer exists exactly while   *)
(* main and the worker share one (FreeIffShared); main reads only below    *)
(* the committed count, the worker writes only at or past it, and a       *)
(* buffer the worker left keeps its final count while main reads it        *)
(* (NoSharedSlot); the worker moves only to an empty buffer (FreeIsEmpty); *)
(* every datagram is returned once and in order (InOrder, and each Call    *)
(* returns the run that follows delivered).                                *)
(*                                                                         *)
(* Defect switch ReadOnlyLeft, FALSE in the passing run: main reads only   *)
(* a buffer the worker has left, never the one it is filling, as the old   *)
(* ready-slot rule did (the worker published a filled buffer, main         *)
(* claimed it at its next call), so a datagram committed before a call     *)
(* waits for the next one.  MCRNReceiveHandoffBrokenReadOnlyLeft.cfg sets  *)
(* it and violates DeliveredByNextCall.                                    *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Cap,           \* datagrams one buffer holds
          MaxRx,         \* datagrams that arrive in one behaviour
          Budgets,       \* the budgets a call may have
          ReadOnlyLeft   \* defect: main never reads the worker's buffer

ASSUME Cap >= 1 /\ MaxRx >= 1 /\ Budgets \subseteq Nat \ { 0 } /\ ReadOnlyLeft \in BOOLEAN

NONE == 3
Buffers == 0 .. 2

VARIABLES
    worker,        \* the buffer the worker fills
    main,          \* the buffer main reads
    tail,          \* the buffer the worker left that main reads before main's
    free,          \* the buffer nobody holds, or NONE
    first,         \* each buffer's first datagram, 0 while it holds none
    count,         \* the datagrams banked in each buffer
    committed,     \* each buffer's committed count (under the mutex)
    mainCursor,    \* main's alone: read position in main's buffer
    mainLimit,     \* main's alone: the committed count main last saw
    tailCursor,    \* main's alone: read position in the tail
    tailEnd,       \* main's alone: the tail's final count
    arrived,       \* datagrams banked so far (the next one is arrived + 1)
    delivered,     \* datagrams main returned (they are 1 .. delivered)
    committedTop,  \* history: the newest datagram committed
    late           \* ghost: a call broke DeliveredByNextCall

vars == << worker, main, tail, free, first, count, committed, mainCursor,
           mainLimit, tailCursor, tailEnd, arrived, delivered, committedTop,
           late >>

(* rocketnet_receive_handoff_init: main and the worker share buffer 0,     *)
(* buffer 1 is free, buffer 2 is a finished, empty tail                    *)
Init ==
    /\ worker = 0 /\ main = 0 /\ tail = 2 /\ free = 1
    /\ first = [ b \in Buffers |-> 0 ]
    /\ count = [ b \in Buffers |-> 0 ]
    /\ committed = [ b \in Buffers |-> 0 ]
    /\ mainCursor = 0 /\ mainLimit = 0 /\ tailCursor = 0 /\ tailEnd = 0
    /\ arrived = 0 /\ delivered = 0
    /\ committedTop = 0 /\ late = FALSE

TailFinished == tailCursor >= tailEnd

Min(a, b) == IF a < b THEN a ELSE b

(* the worker banks n datagrams in one pass, outside the mutex; a full     *)
(* buffer stops the worker reading until it can move                       *)
Bank(n) ==
    /\ n >= 1 /\ count[worker] + n <= Cap /\ arrived + n <= MaxRx
    /\ first' = IF count[worker] = 0 THEN [ first EXCEPT ![worker] = arrived + 1 ] ELSE first
    /\ count' = [ count EXCEPT ![worker] = @ + n ]
    /\ arrived' = arrived + n
    /\ UNCHANGED << worker, main, tail, free, committed, mainCursor, mainLimit,
                    tailCursor, tailEnd, delivered, committedTop, late >>

(* rocketnet_receive_handoff_commit: the end of a pass with results (the   *)
(* model's results are banked datagrams); under the mutex                  *)
Commit ==
    /\ count[worker] > 0
    /\ committed' = IF main = worker
                       THEN [ committed EXCEPT ![worker] = count[worker], ![free] = 0 ]
                       ELSE [ committed EXCEPT ![worker] = count[worker] ]
    /\ committedTop' = first[worker] + count[worker] - 1
    /\ IF main = worker
          THEN /\ worker' = free /\ free' = NONE
          ELSE UNCHANGED << worker, free >>
    /\ UNCHANGED << main, tail, first, count, mainCursor, mainLimit, tailCursor,
                    tailEnd, arrived, delivered, late >>

(* main's plan for a call (busy: the try-acquire found the mutex held):   *)
(* whether it moves on to the worker's buffer, the tail and main's buffer  *)
(* it then reads and from where to where, and what a budget of b returns   *)
Advance(busy) == ~busy /\ main # worker /\ TailFinished
PlanT(busy) == IF Advance(busy) THEN main ELSE tail
PlanM(busy) == IF Advance(busy) THEN worker ELSE main
PlanTC(busy) == IF Advance(busy) THEN mainCursor ELSE tailCursor
PlanTE(busy) == IF Advance(busy) THEN committed[main] ELSE tailEnd
PlanMC(busy) == IF Advance(busy) THEN 0 ELSE mainCursor
PlanML(busy) ==
    IF busy THEN mainLimit
    ELSE IF ReadOnlyLeft /\ PlanM(busy) = worker
         THEN ( IF Advance(busy) THEN 0 ELSE mainLimit )
         ELSE committed[PlanM(busy)]
FromTail(busy, b) == Min( b, PlanTE(busy) - PlanTC(busy) )
FromMain(busy, b) == Min( b - FromTail(busy, b), PlanML(busy) - PlanMC(busy) )
CallSize(busy, b) == FromTail(busy, b) + FromMain(busy, b)

(* one receive call (rocketnet_server_gso_receive_from_queue) with budget  *)
(* b: a finished tail is reset, the try-acquire succeeds or finds the      *)
(* mutex busy, and main returns the tail's unread datagrams, then its own  *)
(* buffer's up to the limit, within b.  r1 and rn are what the call        *)
(* returned, its first datagram and how many: the run that follows         *)
(* delivered                                                               *)
Call(busy, b, r1, rn) ==
    /\ b \in Budgets
    /\ LET t2 == PlanT(busy)
           m2 == PlanM(busy)
           tc == PlanTC(busy)
           te == PlanTE(busy)
           mc == PlanMC(busy)
           ml == PlanML(busy)
           fromTail == FromTail(busy, b)
           fromMain == FromMain(busy, b)
           n == fromTail + fromMain
           backlog == ~( TailFinished /\ mainCursor >= mainLimit )
           visible == ( te - tc ) + ( ml - mc )
       IN /\ rn = n
          /\ ( n > 0 => r1 = delivered + 1 )
          /\ ( fromTail > 0 => first[t2] + tc = delivered + 1 )
          /\ ( fromTail = 0 /\ fromMain > 0 => first[m2] + mc = delivered + 1 )
          /\ count' = IF TailFinished THEN [ count EXCEPT ![tail] = 0 ] ELSE count
          /\ first' = IF TailFinished THEN [ first EXCEPT ![tail] = 0 ] ELSE first
          /\ free' = IF Advance(busy) THEN tail ELSE free
          /\ tail' = t2 /\ main' = m2
          /\ tailCursor' = tc + fromTail /\ tailEnd' = te
          /\ mainCursor' = mc + fromMain /\ mainLimit' = ml
          /\ delivered' = delivered + n
          /\ IF ~busy /\ ~backlog /\ visible <= b /\ delivered + n < committedTop
                THEN late' = TRUE
                ELSE UNCHANGED late
    /\ UNCHANGED << worker, committed, arrived, committedTop >>

Next ==
    \/ \E n \in 1 .. Cap : Bank(n)
    \/ Commit
    \/ \E busy \in BOOLEAN, b \in Budgets : Call(busy, b, delivered + 1, CallSize(busy, b))

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ worker \in Buffers /\ main \in Buffers /\ tail \in Buffers
    /\ free \in Buffers \cup { NONE }
    /\ \A b \in Buffers : count[b] \in 0 .. Cap /\ committed[b] \in 0 .. Cap
    /\ delivered \in 0 .. MaxRx /\ arrived \in 0 .. MaxRx

(* the roles: main and the worker may share a buffer; the tail and the     *)
(* free buffer are nobody else's, and one is free exactly while they share *)
FreeIffShared ==
    /\ tail # main /\ tail # worker
    /\ ( free # NONE ) <=> ( main = worker )
    /\ free # NONE => free \notin { main, worker, tail }

(* main reads below the committed count, the worker writes at or past it, *)
(* and a buffer the worker has left keeps its final count while main reads *)
NoSharedSlot ==
    /\ mainCursor <= mainLimit
    /\ mainLimit <= committed[main]
    /\ committed[worker] <= count[worker]
    /\ ( main # worker => count[main] = committed[main] )
    /\ tailCursor <= tailEnd
    /\ ( TailFinished \/ tailEnd = count[tail] )

(* the worker moves only to an empty buffer *)
FreeIsEmpty == free # NONE => count[free] = 0

(* every datagram is returned once, in order: what main has not returned  *)
(* yet starts at delivered + 1, in the tail, else in main's own buffer    *)
InOrder ==
    /\ ( ~TailFinished => first[tail] + tailCursor = delivered + 1 )
    /\ ( TailFinished /\ mainCursor < count[main] => first[main] + mainCursor = delivered + 1 )
    /\ delivered <= arrived

DeliveredByNextCall == ~late

=============================================================================
