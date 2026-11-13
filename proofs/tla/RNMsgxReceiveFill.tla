-------------------------- MODULE RNMsgxReceiveFill --------------------------
(***************************************************************************)
(* The macOS batched server transport's receive worker over its three     *)
(* banks (sdk/rocketnet_msgx_receive_fill.h, driven by                    *)
(* rocketnet_server_msgx_receive_worker in                                *)
(* sdk/rocketnet_server_msgx_apple.cpp), and main's receive calls         *)
(* (rocketnet_msgx_receive_call from rocketnet_server_msgx_receive_packets) *)
(* over the shared receive handoff (sdk/rocketnet_receive_handoff.h,      *)
(* RNReceiveHandoff).                                                     *)
(*                                                                         *)
(* The fill rule is RNGsoHandoff's RxFill: a datagram is read only into   *)
(* room.  Each round the worker drains every socket in turn, one step at  *)
(* a time as rocketnet_msgx_receive_drain_step takes them: a full bank is *)
(* committed and left for the free one (MoveFull) when main has started   *)
(* on it, and otherwise committed and kept, the drain holds (HoldFull),   *)
(* the round ends and every socket keeps what it holds (Sleep); a         *)
(* socket's reads in a round are bounded (BudgetSpent); a read asks for   *)
(* no more than the room and the budget leave (Want) and takes what the   *)
(* socket has (Read); an ICMP error queued for the client, EINTR or       *)
(* ENOBUFS is read past at one read of the budget (ReadWeather); a dead   *)
(* socket or a failed read ends that socket's drain (ReadFailed).  A      *)
(* round that read and was not held commits its bank's count, moving on   *)
(* when main has started on it (RoundCommit).  Datagrams arrive on a      *)
(* socket (Arrive) until its kernel buffer is full, and past it the       *)
(* kernel drops them (KernelDrop): the kernel's, never the server's.      *)
(*                                                                         *)
(* Main's receive call (Call) empties a finished tail, tries the mutex    *)
(* once and on success moves on to the worker's bank when the worker has  *)
(* left main's and main has finished its tail; then it hands the server   *)
(* the tail's unread datagrams and its own bank's up to the committed     *)
(* count, within the update's capacity.  A busy mutex leaves main with    *)
(* the limits it holds.  As in RNReceiveHandoff a call is one step: what  *)
(* main reads lies below counts the worker no longer changes.             *)
(*                                                                         *)
(* Counts, not datagrams: which datagram is where does not bear on the   *)
(* properties (RNReceiveHandoff carries the order), and arrival order     *)
(* within a socket is the kernel's.  committedTop counts every datagram    *)
(* committed so far, so a call that leaves one of them unreturned when it *)
(* could have returned it sets late.                                       *)
(*                                                                         *)
(* Properties: nothing read is dropped for want of room (NoFullDrop); a   *)
(* socket's reads in a round never pass the budget (BudgetKept); every    *)
(* datagram is in a socket, a bank, main's hands, a drop or counted       *)
(* stopped (Conserved); the roles are distinct and a free bank exists     *)
(* exactly while main and the worker share one (FreeIffShared); main      *)
(* reads only below what the worker committed and a bank the worker left *)
(* keeps its final count (NoSharedSlot); the worker moves only to an      *)
(* empty bank (FreeIsEmpty); a held worker's bank is full                 *)
(* (HeldOnlyWhenFull) and the drain holds only when main has not started  *)
(* on it (HoldOnlyWhenNowhere, an action property); a datagram committed  *)
(* before main's call took the mutex is returned by that call, unless the *)
(* mutex was busy, the capacity cut the call, or the call began with      *)
(* datagrams left from a cut call (DeliveredByNextCall); and with the     *)
(* worker and main fair, every datagram that arrives is eventually main's *)
(* or the kernel's drop (AllConsumed).                                     *)
(*                                                                         *)
(* The stop (R69): main stops (MainStop) and calls no more; the worker    *)
(* leaves its loop at the next top (Exit); main, once it finds the worker *)
(* exited, counts every datagram banked and never handed to the server    *)
(* (StopCount: the tail's unread, main's bank past its cursor, the        *)
(* worker's bank when it is another, committed or not).  StopCounted:     *)
(* after the count every datagram that arrived was consumed, dropped or   *)
(* counted stopped; StopEnds: a stop is counted in the end.               *)
(*                                                                         *)
(* Reversed witnesses: ReadPastRoom = TRUE asks for a whole batch         *)
(* whatever the room, and what lands past the bank's end is dropped        *)
(* (NoFullDrop; the trace check's plant).  ReadPastBudget = TRUE asks for *)
(* the batch whatever the budget leaves, the drain before Stella's review  *)
(* of 4119919c7 (BudgetKept).  HoldWithoutMove = TRUE holds a full bank   *)
(* even when main has started on it (HoldOnlyWhenNowhere).                *)
(* FreeNotReset = TRUE frees a finished tail without emptying it, so the  *)
(* worker appends after a stale count (Conserved).  ReadOnlyLeft = TRUE   *)
(* reads only a bank the worker has left, the ready-slot rule before this *)
(* fix (DeliveredByNextCall).  StopCountSkipsWorker = TRUE leaves the     *)
(* worker's bank out of the stop's count when the worker has moved on     *)
(* from main's bank (StopCounted).                                        *)
(***************************************************************************)
EXTENDS Integers, Sequences

CONSTANTS
    Sockets,                \* the sockets the worker's kqueue reports
    Capacity,               \* a bank: ROCKETNET_MSGX_RECV_QUEUE_SIZE
    Batch,                  \* one recvmsg_x: ROCKETNET_MSGX_RECV_BATCH
    Budget,                 \* one socket's reads a round: ROCKETNET_MSGX_RECV_SOCKET_BUDGET
    Buffer,                 \* datagrams a socket's receive buffer holds
    CallCaps,               \* what one receive call may hand the server (the update's capacity)
    MaxArrivals,            \* exploration bound: datagrams that arrive
    MaxErrors,              \* exploration bound: errors queued on sockets
    ReadPastRoom,           \* defect: a read asks for a whole batch whatever the room
    ReadPastBudget,         \* defect: a read asks for a whole batch whatever the budget leaves
    HoldWithoutMove,        \* defect: a full bank is held when main has started on it
    FreeNotReset,           \* defect: a finished tail is freed without being emptied
    ReadOnlyLeft,           \* defect: main reads only a bank the worker has left (the ready-slot rule)
    StopCountSkipsWorker  \* defect: the stop leaves the worker's bank out

ASSUME /\ Sockets >= 1 /\ Capacity >= 1 /\ Batch >= 1 /\ Budget >= 1 /\ Buffer >= 1
       /\ CallCaps \subseteq Nat \ { 0 } /\ CallCaps # {}
       /\ MaxArrivals \in Nat /\ MaxErrors \in Nat
       /\ ReadPastRoom \in BOOLEAN /\ ReadPastBudget \in BOOLEAN /\ HoldWithoutMove \in BOOLEAN
       /\ FreeNotReset \in BOOLEAN /\ ReadOnlyLeft \in BOOLEAN /\ StopCountSkipsWorker \in BOOLEAN

NoBank == -1
Banks == 0 .. 2
SocketIds == 1 .. Sockets
ErrorKinds == { "weather", "failed" }
MaxCap == CHOOSE c \in CallCaps : \A d \in CallCaps : d <= c

Min(a, b) == IF a < b THEN a ELSE b

VARIABLES
    kq,             \* datagrams queued in each socket's receive buffer
    errs,           \* errors each socket answers before its datagrams, in order
    cnt,            \* datagrams banked in each bank (num_packets)
    committed,      \* each bank's committed count (hand.committed, under the mutex)
    worker,         \* hand.worker
    main,           \* hand.main
    tail,           \* hand.tail
    free,           \* hand.free, or NoBank
    mainCursor,     \* main's alone: hand.main_cursor
    mainLimit,      \* main's alone: hand.main_limit
    tailCursor,     \* main's alone: hand.tail_cursor
    tailEnd,        \* main's alone: hand.tail_end
    pc,             \* the worker: "poll" (a round starts), "read" (draining cur), "end" (the round's sockets done), "held", "exit"
    cur,            \* the socket being drained, 0 between rounds
    reads,          \* reads spent on cur this round (drain.reads)
    roundRead,      \* the worker read this round (round_read)
    arrived,        \* datagrams that arrived
    kdrops,         \* dropped by the kernel on arrival, its buffer full
    consumed,       \* datagrams main handed the server
    fullDrops,      \* read and lost for want of room
    errors,         \* errors queued so far
    stopping,       \* main stopped (msgx->stopping)
    exited,         \* the worker left its loop
    counted,        \* main ran StopCount (stop_counted)
    stopped,        \* datagrams counted stopped (main_stopped)
    committedTop,   \* history: datagrams committed so far
    late            \* ghost: a call broke DeliveredByNextCall

vars == << kq, errs, cnt, committed, worker, main, tail, free, mainCursor, mainLimit, tailCursor, tailEnd,
           pc, cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, counted,
           stopped, committedTop, late >>

handVars == << committed, worker, main, tail, free, mainCursor, mainLimit, tailCursor, tailEnd, committedTop, late >>
stopVars == << stopping, exited, counted, stopped >>

\* rocketnet_receive_handoff_init: main and the worker share bank 0, bank 1
\* is free and bank 2 is main's finished, empty tail
Init ==
    /\ kq = [ s \in SocketIds |-> 0 ]
    /\ errs = [ s \in SocketIds |-> << >> ]
    /\ cnt = [ b \in Banks |-> 0 ]
    /\ committed = [ b \in Banks |-> 0 ]
    /\ worker = 0 /\ main = 0 /\ tail = 2 /\ free = 1
    /\ mainCursor = 0 /\ mainLimit = 0 /\ tailCursor = 0 /\ tailEnd = 0
    /\ pc = "poll"
    /\ cur = 0
    /\ reads = 0
    /\ roundRead = FALSE
    /\ arrived = 0
    /\ kdrops = 0
    /\ consumed = 0
    /\ fullDrops = 0
    /\ errors = 0
    /\ stopping = FALSE
    /\ exited = FALSE
    /\ counted = FALSE
    /\ stopped = 0
    /\ committedTop = 0
    /\ late = FALSE

TailFinished == tailCursor >= tailEnd

-----------------------------------------------------------------------------
\* the environment: datagrams and errors arrive

Arrive(s) ==
    /\ arrived < MaxArrivals
    /\ kq[s] < Buffer
    /\ kq' = [ kq EXCEPT ![s] = @ + 1 ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << errs, cnt, pc, cur, reads, roundRead, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

KernelDrop(s) ==
    /\ arrived < MaxArrivals
    /\ kq[s] >= Buffer
    /\ arrived' = arrived + 1
    /\ kdrops' = kdrops + 1
    /\ UNCHANGED << kq, errs, cnt, pc, cur, reads, roundRead, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

Error(s, e) ==
    /\ errors < MaxErrors
    /\ e \in ErrorKinds
    /\ errs' = [ errs EXCEPT ![s] = Append( @, e ) ]
    /\ errors' = errors + 1
    /\ UNCHANGED << kq, cnt, pc, cur, reads, roundRead, arrived, kdrops, consumed, fullDrops >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

-----------------------------------------------------------------------------
\* the worker

Full == cnt[worker] >= Capacity

\* rocketnet_receive_handoff_commit: the worker's count made visible; when
\* main has started on its bank (main = worker) the worker moves to the free
\* bank, whose committed count starts at 0
CommitMoves == main = worker

Commit ==
    /\ committedTop' = committedTop + ( cnt[worker] - committed[worker] )
    /\ IF CommitMoves
       THEN /\ committed' = [ committed EXCEPT ![worker] = cnt[worker], ![free] = 0 ]
            /\ worker' = free
            /\ free' = NoBank
       ELSE /\ committed' = [ committed EXCEPT ![worker] = cnt[worker] ]
            /\ UNCHANGED << worker, free >>

\* the drain's read size: rocketnet_msgx_receive_want (the batch, or the
\* room when less), then no more than the budget leaves (the step's cap)
RoomWant == IF ReadPastRoom THEN Batch ELSE Min( Batch, Capacity - cnt[worker] )
Want == IF ReadPastBudget THEN RoomWant ELSE Min( RoomWant, Budget - reads )

\* a socket's drain is done: the next socket, or the round's end
NextSocket ==
    /\ cur' = IF cur = Sockets THEN 0 ELSE cur + 1
    /\ reads' = 0
    /\ pc' = IF cur = Sockets THEN "end" ELSE "read"

\* kevent: a round over every socket, while main has not stopped (the loop
\* reads quit at its top)
Poll ==
    /\ pc = "poll"
    /\ ~stopping
    /\ pc' = "read"
    /\ cur' = 1
    /\ reads' = 0
    /\ roundRead' = FALSE
    /\ UNCHANGED << kq, errs, cnt, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* DRAIN_STEP_MOVE_FULL: the full bank is committed and, main having
\* started on it, left for the free one
MoveFull ==
    /\ pc = "read"
    /\ Full
    /\ ~HoldWithoutMove
    /\ CommitMoves
    /\ Commit
    /\ UNCHANGED << kq, errs, cnt, main, tail, mainCursor, mainLimit, tailCursor, tailEnd, late >>
    /\ UNCHANGED << pc, cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED stopVars

\* DRAIN_STEP_HOLD_FULL: the full bank is committed and kept, main not having
\* started on it; the round stops and every socket keeps what it holds
HoldFull ==
    /\ pc = "read"
    /\ Full
    /\ HoldWithoutMove \/ ~CommitMoves
    /\ committedTop' = committedTop + ( cnt[worker] - committed[worker] )
    /\ committed' = [ committed EXCEPT ![worker] = cnt[worker] ]
    /\ pc' = "held"
    /\ UNCHANGED << kq, errs, cnt, worker, main, tail, free, mainCursor, mainLimit, tailCursor, tailEnd, late >>
    /\ UNCHANGED << cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED stopVars

\* DRAIN_STEP_BUDGET
BudgetSpent ==
    /\ pc = "read"
    /\ ~Full
    /\ reads >= Budget
    /\ NextSocket
    /\ UNCHANGED << kq, errs, cnt, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* DRAIN_STEP_READ: n datagrams read, each banked while there is room (with
\* ReadPastRoom FALSE there always is); a short read drains the socket
Read(n) ==
    /\ pc = "read"
    /\ ~Full
    /\ reads < Budget
    /\ errs[cur] = << >>
    /\ kq[cur] >= 1
    /\ n = Min( Want, kq[cur] )
    /\ LET took == Min( n, Capacity - cnt[worker] )
       IN /\ cnt' = [ cnt EXCEPT ![worker] = @ + took ]
          /\ fullDrops' = fullDrops + ( n - took )
    /\ kq' = [ kq EXCEPT ![cur] = @ - n ]
    /\ roundRead' = TRUE
    /\ IF n < Want
       THEN NextSocket
       ELSE /\ reads' = reads + n
            /\ UNCHANGED << pc, cur >>
    /\ UNCHANGED << errs, arrived, kdrops, consumed, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* DRAIN_STEP_EMPTY: EAGAIN
ReadEmpty ==
    /\ pc = "read"
    /\ ~Full
    /\ reads < Budget
    /\ errs[cur] = << >>
    /\ kq[cur] = 0
    /\ roundRead' = TRUE
    /\ NextSocket
    /\ UNCHANGED << kq, errs, cnt, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* DRAIN_STEP_WEATHER: the error is cleared by the read that reports it
ReadWeather ==
    /\ pc = "read"
    /\ ~Full
    /\ reads < Budget
    /\ errs[cur] # << >>
    /\ Head( errs[cur] ) = "weather"
    /\ errs' = [ errs EXCEPT ![cur] = Tail( @ ) ]
    /\ reads' = reads + 1
    /\ roundRead' = TRUE
    /\ UNCHANGED << kq, cnt, pc, cur, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* DRAIN_STEP_FAILED: counted, the socket left for the round
ReadFailed ==
    /\ pc = "read"
    /\ ~Full
    /\ reads < Budget
    /\ errs[cur] # << >>
    /\ Head( errs[cur] ) = "failed"
    /\ errs' = [ errs EXCEPT ![cur] = Tail( @ ) ]
    /\ roundRead' = TRUE
    /\ NextSocket
    /\ UNCHANGED << kq, cnt, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* the round's end: a round that read commits its bank's count, moving on
\* when main has started on the bank
RoundCommit ==
    /\ pc = "end"
    /\ roundRead
    /\ Commit
    /\ pc' = "poll"
    /\ UNCHANGED << kq, errs, cnt, main, tail, mainCursor, mainLimit, tailCursor, tailEnd, late >>
    /\ UNCHANGED << cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED stopVars

RoundKeep ==
    /\ pc = "end"
    /\ ~roundRead
    /\ pc' = "poll"
    /\ UNCHANGED << kq, errs, cnt, cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* a held round: the worker sleeps, then polls again
Sleep ==
    /\ pc = "held"
    /\ pc' = "poll"
    /\ cur' = 0
    /\ reads' = 0
    /\ UNCHANGED << kq, errs, cnt, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED stopVars

\* the worker's loop sees quit at its top
Exit ==
    /\ pc = "poll"
    /\ stopping
    /\ ~exited
    /\ exited' = TRUE
    /\ pc' = "exit"
    /\ UNCHANGED << kq, errs, cnt, cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED << stopping, counted, stopped >>

-----------------------------------------------------------------------------
\* main

\* main's plan for a call (busy: the try-acquire found the mutex held):
\* whether it moves on to the worker's bank, the tail and main's bank it then
\* reads and from where to where, and what a capacity of c returns
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
FromTail(busy, c) == Min( c, PlanTE(busy) - PlanTC(busy) )
FromMain(busy, c) == Min( c - FromTail(busy, c), PlanML(busy) - PlanMC(busy) )

\* rocketnet_msgx_receive_call with capacity c: a finished tail is emptied
\* (FreeNotReset skips it), the try-acquire succeeds or finds the mutex busy,
\* and main hands the server the tail's unread datagrams, then its own bank's
\* up to the limit, within c. Main calls nothing after its stop
Call(busy, c) ==
    /\ ~stopping
    /\ c \in CallCaps
    /\ LET fromTail == FromTail(busy, c)
           fromMain == FromMain(busy, c)
           n == fromTail + fromMain
           backlog == ~( TailFinished /\ mainCursor >= mainLimit )
           visible == ( PlanTE(busy) - PlanTC(busy) ) + ( PlanML(busy) - PlanMC(busy) )
       IN /\ cnt' = IF TailFinished /\ ~FreeNotReset THEN [ cnt EXCEPT ![tail] = 0 ] ELSE cnt
          /\ free' = IF Advance(busy) THEN tail ELSE free
          /\ tail' = PlanT(busy)
          /\ main' = PlanM(busy)
          /\ tailCursor' = PlanTC(busy) + fromTail
          /\ tailEnd' = PlanTE(busy)
          /\ mainCursor' = PlanMC(busy) + fromMain
          /\ mainLimit' = PlanML(busy)
          /\ consumed' = consumed + n
          /\ IF ~busy /\ ~backlog /\ visible <= c /\ consumed + n < committedTop
             THEN late' = TRUE
             ELSE UNCHANGED late
    /\ UNCHANGED << kq, errs, committed, worker, committedTop, pc, cur, reads, roundRead, arrived, kdrops, fullDrops, errors >>
    /\ UNCHANGED stopVars

\* rocketnet_server_msgx_stop: main hands the server nothing more
MainStop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ UNCHANGED << kq, errs, cnt, pc, cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors >>
    /\ UNCHANGED handVars /\ UNCHANGED << exited, counted, stopped >>

\* rocketnet_msgx_receive_stop_count, once main finds the worker exited: the
\* tail's unread, main's bank past its cursor and the worker's bank when it
\* is another are counted stopped (StopCountSkipsWorker: the worker's bank
\* left out), and main's and the worker's banks are emptied. The worker exits
\* only at its loop's top, after its round's commit, so each bank's count is
\* its committed count here
StopCount ==
    /\ exited
    /\ ~counted
    /\ LET left == ( tailEnd - tailCursor )
                   + ( IF mainCursor < cnt[main] THEN cnt[main] - mainCursor ELSE 0 )
                   + ( IF worker # main /\ ~StopCountSkipsWorker THEN cnt[worker] ELSE 0 )
       IN stopped' = stopped + left
    /\ counted' = TRUE
    /\ tailCursor' = tailEnd
    /\ mainCursor' = 0
    /\ mainLimit' = 0
    /\ cnt' = [ cnt EXCEPT ![main] = 0, ![worker] = 0 ]
    /\ UNCHANGED << kq, errs, committed, worker, main, tail, free, tailEnd, committedTop, late >>
    /\ UNCHANGED << pc, cur, reads, roundRead, arrived, kdrops, consumed, fullDrops, errors, stopping, exited >>

\* the end: the stop is counted and nothing more arrives (the arrivals bound
\* spent), a stutter
Done ==
    /\ counted
    /\ arrived >= MaxArrivals
    /\ UNCHANGED vars

-----------------------------------------------------------------------------

Worker == \/ Poll
          \/ MoveFull
          \/ HoldFull
          \/ BudgetSpent
          \/ \E n \in 1 .. Batch : Read(n)
          \/ ReadEmpty
          \/ ReadWeather
          \/ ReadFailed
          \/ RoundCommit
          \/ RoundKeep
          \/ Sleep
          \/ Exit

\* the fair main: a call that takes the mutex with the whole capacity, and
\* the stop's count
MainStep == Call(FALSE, MaxCap) \/ StopCount

\* every action by name (tools/tla_trace_check.py reads Next's actions from
\* its text): the environment, Worker and main
Next == \/ \E s \in SocketIds : Arrive(s)
        \/ \E s \in SocketIds : KernelDrop(s)
        \/ \E s \in SocketIds : \E e \in ErrorKinds : Error(s, e)
        \/ Poll \/ MoveFull \/ HoldFull \/ BudgetSpent
        \/ \E n \in 1 .. Batch : Read(n)
        \/ ReadEmpty \/ ReadWeather \/ ReadFailed
        \/ RoundCommit \/ RoundKeep \/ Sleep
        \/ \E busy \in BOOLEAN : \E c \in CallCaps : Call(busy, c)
        \/ MainStop \/ Exit \/ StopCount \/ Done

Spec == Init /\ [][Next]_vars

FairSpec == Spec /\ WF_vars(Worker) /\ WF_vars(MainStep)

-----------------------------------------------------------------------------

TypeOK ==
    /\ kq \in [ SocketIds -> 0 .. Buffer ]
    /\ errs \in [ SocketIds -> Seq( ErrorKinds ) ]
    /\ cnt \in [ Banks -> 0 .. Capacity ]
    /\ committed \in [ Banks -> 0 .. Capacity ]
    /\ { worker, main, tail } \subseteq Banks
    /\ free \in Banks \cup { NoBank }
    /\ { mainCursor, mainLimit, tailCursor, tailEnd } \subseteq 0 .. Capacity
    /\ pc \in { "poll", "read", "end", "held", "exit" }
    /\ cur \in 0 .. Sockets
    /\ reads \in Nat
    /\ { arrived, kdrops, consumed, fullDrops, errors, stopped, committedTop } \subseteq Nat
    /\ { roundRead, stopping, exited, counted, late } \subseteq BOOLEAN

\* the roles: main and the worker may share a bank; the tail and the free
\* bank are nobody else's, and one is free exactly while they share
FreeIffShared ==
    /\ tail # main /\ tail # worker
    /\ ( free # NoBank ) <=> ( main = worker )
    /\ free # NoBank => free \notin { main, worker, tail }

\* main reads below the committed count, the worker writes at or past it,
\* and a bank the worker has left keeps its final count while main reads it
\* (until the stop's count empties the banks)
NoSharedSlot ==
    ~counted =>
        /\ mainCursor <= mainLimit
        /\ mainLimit <= committed[main]
        /\ committed[worker] <= cnt[worker]
        /\ ( main # worker => cnt[main] = committed[main] )
        /\ tailCursor <= tailEnd
        /\ ( TailFinished \/ tailEnd = cnt[tail] )

\* the worker moves only to an empty bank
FreeIsEmpty == free # NoBank => cnt[free] = 0

\* a read never asks past the bank's room, so nothing read is lost
NoFullDrop == fullDrops = 0

\* a socket's reads in a round never pass the budget: a read asks for no
\* more than the budget leaves, and a failed read spends one only below it
BudgetKept == reads <= Budget

RECURSIVE SumOver(_, _)
SumOver(f, S) == IF S = {} THEN 0 ELSE LET x == CHOOSE y \in S : TRUE IN f[x] + SumOver(f, S \ { x })

\* banked and not yet handed to the server: the tail's unread, main's bank
\* past its cursor, and the worker's bank when it is another
InBanks ==
    IF counted THEN 0
    ELSE ( tailEnd - tailCursor ) + ( cnt[main] - mainCursor ) + ( IF worker # main THEN cnt[worker] ELSE 0 )

\* every datagram that arrived is in a socket, a bank, main's hands, a drop,
\* or counted stopped
Conserved == arrived = SumOver( kq, SocketIds ) + InBanks + consumed + kdrops + fullDrops + stopped

\* once the stop is counted, every datagram the worker read was handed to
\* the server or counted stopped: none is left in a bank nobody reads (R69)
StopCounted == counted => arrived = SumOver( kq, SocketIds ) + consumed + kdrops + fullDrops + stopped

\* a held worker's bank is full (main may start on it while the worker
\* sleeps; the next round leaves it)
HeldOnlyWhenFull == pc = "held" => Full

\* the worker holds only when its bank is full and main has not started on
\* it: it never stalls with room or with a bank it could leave
HoldOnlyWhenNowhere == [][ ( pc' = "held" /\ pc # "held" ) => ( Full /\ ~CommitMoves ) ]_vars

\* a datagram committed before main's call took the mutex is returned by
\* that call, unless the mutex was busy, the capacity cut the call, or the
\* call began with datagrams left from a cut call
DeliveredByNextCall == ~late

\* with the worker and main fair, every datagram that arrives is main's or
\* the kernel's drop in the end, unless main stops first
AllConsumed == <>[]( consumed + kdrops = arrived ) \/ <>stopping

\* a stop is counted in the end: the worker leaves its loop and main counts
StopEnds == stopping ~> counted

=============================================================================
