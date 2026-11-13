-------------------------- MODULE RNMsgxReceiveFill --------------------------
(***************************************************************************)
(* The macOS batched server transport's receive worker over its three     *)
(* banks (sdk/rocketnet_msgx_receive_fill.h, driven by                    *)
(* rocketnet_server_msgx_receive_worker in                                *)
(* sdk/rocketnet_server_msgx_apple.cpp), and main's claim of a ready bank *)
(* (rocketnet_server_msgx_receive_packets).                               *)
(*                                                                         *)
(* The rule is RNGsoHandoff's RxFill and RxPublish: a datagram is read    *)
(* only into room.  Each round the worker drains every socket in turn,    *)
(* one step at a time as rocketnet_msgx_receive_drain_step takes them:    *)
(* a full bank is handed to main (PublishFull) when none is ready and a   *)
(* free bank exists, and otherwise the drain holds (HoldFull), the round  *)
(* ends and every socket keeps what it holds (Sleep); a socket's reads in *)
(* a round are bounded (BudgetSpent); a read asks for no more than the    *)
(* room (Want) and takes what the socket has (Read); an ICMP error queued *)
(* for the client, EINTR or ENOBUFS is read past at one read of the       *)
(* budget (ReadWeather); a dead socket or a failed read ends that         *)
(* socket's drain (ReadFailed).  A round that was not held hands its bank *)
(* to main when it can (RoundPublish).  Main consumes its bank (Consume)  *)
(* and then claims the ready one (Claim).  Datagrams arrive on a socket   *)
(* (Arrive) until its kernel buffer is full, and past it the kernel drops *)
(* them (KernelDrop): the kernel's, never the server's.                   *)
(*                                                                         *)
(* Counts, not datagrams: which datagram is where does not bear on the   *)
(* properties, and arrival order within a socket is the kernel's.         *)
(*                                                                         *)
(* Properties: nothing read is dropped for want of room (NoFullDrop);     *)
(* every datagram is in a socket, a bank, main's hands or a kernel drop   *)
(* (Conserved); the three banks are always the worker's, main's and one   *)
(* ready or free (HandOK); a held worker's bank is full                   *)
(* (HeldOnlyWhenFull), and the drain holds only when main has not taken   *)
(* the last bank (HoldOnlyWhenNowhere, an action property: the worker     *)
(* never stalls with room or with a bank it could hand over); and with    *)
(* the worker and main fair, every datagram that arrives is eventually    *)
(* main's or the kernel's drop (AllConsumed).                             *)
(*                                                                         *)
(* Reversed witnesses: ReadPastRoom = TRUE asks for a whole batch         *)
(* whatever the room, the drain before this fix, and what lands past the  *)
(* bank's end is dropped (NoFullDrop; the trace check's plant).           *)
(* PublishOverReady = TRUE hands a full bank over a ready one main has    *)
(* not taken, discarding it (Conserved).  HoldWithoutPublish = TRUE holds *)
(* a full bank even when main has taken the last (HoldOnlyWhenNowhere).   *)
(*                                                                         *)
(* The stop (R69): main stops (MainStop: its bank's tail counted stopped, *)
(* and it claims nothing more), the worker leaves its loop at the next    *)
(* top (Exit: the bank it fills and a ready bank counted stopped), and    *)
(* main stops calling once the worker has exited.  StopCounted: after the *)
(* exit no bank holds a datagram; StopEnds: a stop ends the worker.       *)
(* ClaimAfterStop = TRUE, the code before R69, lets main claim a ready    *)
(* bank after its stop: the bank's datagrams are then in main's hands at  *)
(* the exit, read by no later update and counted by nobody (StopCounted).  *)
(***************************************************************************)
EXTENDS Integers, Sequences

CONSTANTS
    Sockets,            \* the sockets the worker's kqueue reports
    Capacity,           \* a bank: ROCKETNET_MSGX_RECV_QUEUE_SIZE
    Batch,              \* one recvmsg_x: ROCKETNET_MSGX_RECV_BATCH
    Budget,             \* one socket's reads a round: ROCKETNET_MSGX_RECV_SOCKET_BUDGET
    Buffer,             \* datagrams a socket's receive buffer holds
    MaxArrivals,        \* exploration bound: datagrams that arrive
    MaxErrors,          \* exploration bound: errors queued on sockets
    ReadPastRoom,       \* defect: a read asks for a whole batch whatever the room
    PublishOverReady,   \* defect: a full bank is handed over a ready one
    HoldWithoutPublish, \* defect: a full bank is held when it could be handed over
    ClaimAfterStop      \* defect: main claims a ready bank after it stopped (R69)

ASSUME /\ Sockets >= 1 /\ Capacity >= 1 /\ Batch >= 1 /\ Budget >= 1 /\ Buffer >= 1
       /\ MaxArrivals \in Nat /\ MaxErrors \in Nat
       /\ ReadPastRoom \in BOOLEAN /\ PublishOverReady \in BOOLEAN /\ HoldWithoutPublish \in BOOLEAN
       /\ ClaimAfterStop \in BOOLEAN

NoBank == -1
Banks == 0 .. 2
SocketIds == 1 .. Sockets
ErrorKinds == { "weather", "failed" }

Min(a, b) == IF a < b THEN a ELSE b

VARIABLES
    kq,         \* datagrams queued in each socket's receive buffer
    errs,       \* errors each socket answers before its datagrams, in order
    cnt,        \* datagrams in each bank (num_packets)
    worker,     \* hand.worker
    ready,      \* hand.ready, or NoBank
    main,       \* hand.main
    free,       \* hand.free, or NoBank
    pc,         \* the worker: "poll" (a round starts), "read" (draining cur), "end" (the round's sockets done), "held"
    cur,        \* the socket being drained, 0 between rounds
    reads,      \* reads spent on cur this round (drain.reads)
    arrived,    \* datagrams that arrived
    kdrops,     \* dropped by the kernel on arrival, its buffer full
    consumed,   \* datagrams main took
    fullDrops,  \* read and lost for want of room
    errors,     \* errors queued so far
    stopping,   \* main stopped (hand.stopped, rocketnet_msgx_receive_stop)
    exited,     \* the worker left its loop and counted what it held
    stopped     \* datagrams read and counted stopped (main_stopped and DROP_STOPPED)

vars == << kq, errs, cnt, worker, ready, main, free, pc, cur, reads, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

Init ==
    /\ kq = [ s \in SocketIds |-> 0 ]
    /\ errs = [ s \in SocketIds |-> << >> ]
    /\ cnt = [ b \in Banks |-> 0 ]
    /\ worker = 0
    /\ main = 1
    /\ free = 2
    /\ ready = NoBank
    /\ pc = "poll"
    /\ cur = 0
    /\ reads = 0
    /\ arrived = 0
    /\ kdrops = 0
    /\ consumed = 0
    /\ fullDrops = 0
    /\ errors = 0
    /\ stopping = FALSE
    /\ exited = FALSE
    /\ stopped = 0

-----------------------------------------------------------------------------
\* the environment: datagrams and errors arrive

Arrive(s) ==
    /\ arrived < MaxArrivals
    /\ kq[s] < Buffer
    /\ kq' = [ kq EXCEPT ![s] = @ + 1 ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << errs, cnt, worker, ready, main, free, pc, cur, reads, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

KernelDrop(s) ==
    /\ arrived < MaxArrivals
    /\ kq[s] >= Buffer
    /\ arrived' = arrived + 1
    /\ kdrops' = kdrops + 1
    /\ UNCHANGED << kq, errs, cnt, worker, ready, main, free, pc, cur, reads, consumed, fullDrops, errors, stopping, exited, stopped >>

Error(s, e) ==
    /\ errors < MaxErrors
    /\ e \in ErrorKinds
    /\ errs' = [ errs EXCEPT ![s] = Append( @, e ) ]
    /\ errors' = errors + 1
    /\ UNCHANGED << kq, cnt, worker, ready, main, free, pc, cur, reads, arrived, kdrops, consumed, fullDrops, stopping, exited, stopped >>

-----------------------------------------------------------------------------
\* the worker

\* rocketnet_msgx_receive_publish: none ready and one free
CanPublish == ready = NoBank /\ free # NoBank

Full == cnt[worker] >= Capacity

\* the drain's read size, rocketnet_msgx_receive_want
Want == IF ReadPastRoom THEN Batch ELSE Min( Batch, Capacity - cnt[worker] )

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
    /\ UNCHANGED << kq, errs, cnt, worker, ready, main, free, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* DRAIN_STEP_PUBLISH_FULL: the full bank goes to main and the worker fills
\* the free one; under PublishOverReady a ready bank main has not taken is
\* emptied and becomes the worker's
PublishFull ==
    /\ pc = "read"
    /\ Full
    /\ ~HoldWithoutPublish
    /\ CanPublish \/ PublishOverReady
    /\ IF CanPublish
       THEN /\ ready' = worker
            /\ worker' = free
            /\ free' = NoBank
            /\ UNCHANGED cnt
       ELSE /\ ready' = worker
            /\ worker' = ready
            /\ cnt' = [ cnt EXCEPT ![ready] = 0 ]
            /\ UNCHANGED free
    /\ UNCHANGED << kq, errs, main, pc, cur, reads, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* DRAIN_STEP_HOLD_FULL: the bank is full and main has not taken the last;
\* the round stops and every socket keeps what it holds
HoldFull ==
    /\ pc = "read"
    /\ Full
    /\ HoldWithoutPublish \/ ~( CanPublish \/ PublishOverReady )
    /\ pc' = "held"
    /\ UNCHANGED << kq, errs, cnt, worker, ready, main, free, cur, reads, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* DRAIN_STEP_BUDGET
BudgetSpent ==
    /\ pc = "read"
    /\ ~Full
    /\ reads >= Budget
    /\ NextSocket
    /\ UNCHANGED << kq, errs, cnt, worker, ready, main, free, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

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
    /\ IF n < Want
       THEN NextSocket
       ELSE /\ reads' = reads + n
            /\ UNCHANGED << pc, cur >>
    /\ UNCHANGED << errs, worker, ready, main, free, arrived, kdrops, consumed, errors, stopping, exited, stopped >>

\* DRAIN_STEP_EMPTY: EAGAIN
ReadEmpty ==
    /\ pc = "read"
    /\ ~Full
    /\ reads < Budget
    /\ errs[cur] = << >>
    /\ kq[cur] = 0
    /\ NextSocket
    /\ UNCHANGED << kq, errs, cnt, worker, ready, main, free, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* DRAIN_STEP_WEATHER: the error is cleared by the read that reports it
ReadWeather ==
    /\ pc = "read"
    /\ ~Full
    /\ reads < Budget
    /\ errs[cur] # << >>
    /\ Head( errs[cur] ) = "weather"
    /\ errs' = [ errs EXCEPT ![cur] = Tail( @ ) ]
    /\ reads' = reads + 1
    /\ UNCHANGED << kq, cnt, worker, ready, main, free, pc, cur, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* DRAIN_STEP_FAILED: counted, the socket left for the round
ReadFailed ==
    /\ pc = "read"
    /\ ~Full
    /\ reads < Budget
    /\ errs[cur] # << >>
    /\ Head( errs[cur] ) = "failed"
    /\ errs' = [ errs EXCEPT ![cur] = Tail( @ ) ]
    /\ NextSocket
    /\ UNCHANGED << kq, cnt, worker, ready, main, free, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* the round's end: its bank goes to main when main has taken the last and
\* it holds datagrams, or only counts (the worker loop publishes a bank that
\* carries reads or refusals with no datagram: either choice with none)
RoundPublish ==
    /\ pc = "end"
    /\ CanPublish
    /\ ready' = worker
    /\ worker' = free
    /\ free' = NoBank
    /\ pc' = "poll"
    /\ UNCHANGED << kq, errs, cnt, main, cur, reads, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

RoundKeep ==
    /\ pc = "end"
    /\ ~( cnt[worker] > 0 /\ CanPublish )
    /\ pc' = "poll"
    /\ UNCHANGED << kq, errs, cnt, worker, ready, main, free, cur, reads, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* a held round: the worker sleeps, then polls again
Sleep ==
    /\ pc = "held"
    /\ pc' = "poll"
    /\ cur' = 0
    /\ reads' = 0
    /\ UNCHANGED << kq, errs, cnt, worker, ready, main, free, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

-----------------------------------------------------------------------------
\* main

Consume ==
    /\ ~exited
    /\ cnt[main] > 0
    /\ consumed' = consumed + cnt[main]
    /\ cnt' = [ cnt EXCEPT ![main] = 0 ]
    /\ UNCHANGED << kq, errs, worker, ready, main, free, pc, cur, reads, arrived, kdrops, fullDrops, errors, stopping, exited, stopped >>

\* rocketnet_msgx_receive_claim, once main has consumed its bank, and never
\* after main stopped (ClaimAfterStop: the code before R69). Main stops calling
\* once the worker has exited (the server sees the transport stopped)
Claim ==
    /\ ~exited
    /\ ~stopping \/ ClaimAfterStop
    /\ cnt[main] = 0
    /\ ready # NoBank
    /\ main' = ready
    /\ free' = main
    /\ ready' = NoBank
    /\ UNCHANGED << kq, errs, cnt, worker, pc, cur, reads, arrived, kdrops, consumed, fullDrops, errors, stopping, exited, stopped >>

\* rocketnet_server_msgx_stop: what main has not consumed of its bank is
\* counted stopped (main_stopped), and main claims nothing more
\* (rocketnet_msgx_receive_stop)
MainStop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ stopped' = stopped + cnt[main]
    /\ cnt' = [ cnt EXCEPT ![main] = 0 ]
    /\ UNCHANGED << kq, errs, worker, ready, main, free, pc, cur, reads, arrived, kdrops, consumed, fullDrops, errors, exited >>

\* the worker's loop sees quit at its top: the bank it fills and a ready bank
\* main has not claimed are counted stopped (RECEIVE_DROP_STOPPED)
Exit ==
    /\ pc = "poll"
    /\ stopping
    /\ ~exited
    /\ exited' = TRUE
    /\ stopped' = stopped + cnt[worker] + ( IF ready # NoBank THEN cnt[ready] ELSE 0 )
    /\ cnt' = [ b \in Banks |-> IF b = worker \/ b = ready THEN 0 ELSE cnt[b] ]
    /\ pc' = "exit"
    /\ UNCHANGED << kq, errs, worker, ready, main, free, cur, reads, arrived, kdrops, consumed, fullDrops, errors, stopping >>

\* the end: the worker has exited and nothing more arrives (the arrivals
\* bound spent), a stutter
Done ==
    /\ exited
    /\ arrived >= MaxArrivals
    /\ UNCHANGED vars

-----------------------------------------------------------------------------

Worker == \/ Poll
          \/ PublishFull
          \/ HoldFull
          \/ BudgetSpent
          \/ \E n \in 1 .. Batch : Read(n)
          \/ ReadEmpty
          \/ ReadWeather
          \/ ReadFailed
          \/ RoundPublish
          \/ RoundKeep
          \/ Sleep
          \/ Exit

MainStep == Consume \/ Claim

\* every action by name (tools/tla_trace_check.py reads Next's actions from
\* its text): the environment, Worker and MainStep
Next == \/ \E s \in SocketIds : Arrive(s)
        \/ \E s \in SocketIds : KernelDrop(s)
        \/ \E s \in SocketIds : \E e \in ErrorKinds : Error(s, e)
        \/ Poll \/ PublishFull \/ HoldFull \/ BudgetSpent
        \/ \E n \in 1 .. Batch : Read(n)
        \/ ReadEmpty \/ ReadWeather \/ ReadFailed
        \/ RoundPublish \/ RoundKeep \/ Sleep
        \/ Consume \/ Claim \/ MainStop \/ Exit \/ Done

Spec == Init /\ [][Next]_vars

FairSpec == Spec /\ WF_vars(Worker) /\ WF_vars(MainStep)

-----------------------------------------------------------------------------

TypeOK ==
    /\ kq \in [ SocketIds -> 0 .. Buffer ]
    /\ errs \in [ SocketIds -> Seq( ErrorKinds ) ]
    /\ cnt \in [ Banks -> 0 .. Capacity ]
    /\ { worker, main } \subseteq Banks
    /\ ready \in Banks \cup { NoBank }
    /\ free \in Banks \cup { NoBank }
    /\ pc \in { "poll", "read", "end", "held", "exit" }
    /\ cur \in 0 .. Sockets
    /\ reads \in Nat
    /\ { arrived, kdrops, consumed, fullDrops, errors, stopped } \subseteq Nat
    /\ stopping \in BOOLEAN /\ exited \in BOOLEAN

\* the three banks are the worker's, main's, and one ready or free
HandOK ==
    /\ ( ready = NoBank ) # ( free = NoBank )
    /\ { worker, main, ready, free } \ { NoBank } = Banks

\* a read never asks past the bank's room, so nothing read is lost
NoFullDrop == fullDrops = 0

RECURSIVE SumOver(_, _)
SumOver(f, S) == IF S = {} THEN 0 ELSE LET x == CHOOSE y \in S : TRUE IN f[x] + SumOver(f, S \ { x })

\* every datagram that arrived is in a socket, a bank, main's hands, a drop,
\* or counted stopped
Conserved == arrived = SumOver( kq, SocketIds ) + SumOver( cnt, Banks ) + consumed + kdrops + fullDrops + stopped

\* once the worker has exited no bank holds a datagram: every one read was
\* consumed or counted stopped, none left in a bank nobody reads (R69)
StopCounted == exited => SumOver( cnt, Banks ) = 0

\* a held worker's bank is full (main may take the ready bank while the
\* worker sleeps; the next round hands the full bank over)
HeldOnlyWhenFull == pc = "held" => Full

\* the worker holds only when its bank is full and main has not taken the
\* last: it never stalls with room or with a bank it could hand over
HoldOnlyWhenNowhere == [][ ( pc' = "held" /\ pc # "held" ) => ( Full /\ ~CanPublish ) ]_vars

\* with the worker and main fair, every datagram that arrives is main's or
\* the kernel's drop in the end, unless main stops first
AllConsumed == <>[]( consumed + kdrops = arrived ) \/ <>stopping

\* a stop ends the worker: it leaves its loop once main has stopped
StopEnds == stopping ~> exited

=============================================================================
