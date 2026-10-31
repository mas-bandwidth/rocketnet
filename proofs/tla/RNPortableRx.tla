---------------------------- MODULE RNPortableRx ----------------------------
(***************************************************************************)
(* The portable (and macOS msgx) server receive worker of                 *)
(* sdk/rocketnet_driver_builtin.cpp: portable_rx_worker,                  *)
(* portable_rx_publish, portable_rx_acquire, portable_receive and the RX   *)
(* half of portable_stop / portable_stopped.                              *)
(*                                                                         *)
(* One RX worker owns the socket reads.  It reads a batch into the one     *)
(* bank it owns (the producer) and publishes that bank onto a FIFO of      *)
(* ready banks in exchange for a free bank.  The main thread owns at most  *)
(* one consumer bank and a cursor into it.  portable_receive copies from   *)
(* the consumer at the cursor within the caller's budget; only when the    *)
(* consumer is completely copied does it try-lock, return it to the free   *)
(* FIFO and take the oldest ready bank.  Only bank indices change under    *)
(* the mutex; the socket read, the copies and the producer reset run       *)
(* outside it.  Main never waits: a failed try ends the call and the       *)
(* consumer, its cursor and every ready bank stay where they are.          *)
(*                                                                         *)
(* When no free bank exists the worker keeps its full producer, stops      *)
(* reading the socket and waits on its wake.  Main signals that wake when  *)
(* it returns a bank to an empty free FIFO.  Stop sets quit, then signals  *)
(* the wake; the worker sees quit at the top of its loop and exits.        *)
(*                                                                         *)
(* Datagrams are abstract ordinals 1..MaxDatagrams that enter the socket   *)
(* in order.  The checked claim is exact delivery: until stop, the         *)
(* datagrams main has copied, then the unread consumer tail, the ready     *)
(* banks in FIFO order, the producer and the socket, concatenated, are     *)
(* exactly 1..arrived.  Nothing is lost, duplicated or reordered.          *)
(*                                                                         *)
(* Rule switches, each FALSE in one reversed witness:                      *)
(*   HoldOnFull    a refused publish keeps the producer and waits; FALSE   *)
(*                 reads the socket into the full producer (overwrite).    *)
(*   ReturnWhenRead main returns the consumer only once fully copied;      *)
(*                 FALSE returns it as soon as a call's budget ends.       *)
(*   QuitBeforeWake stop sets quit before the wake; FALSE signals first.   *)
(*   WakeOnReturn  main signals the worker when it frees a bank; FALSE     *)
(*                 never does (the worker relies on its timeout).          *)
(*   Timeout       the worker's waits time out (0.1 s production bound).   *)
(*                 The liveness controls run with Timeout = FALSE, so      *)
(*                 progress is proved from the wake protocol alone.        *)
(*   BoundedRead   one socket read spends a read budget of Cap on every    *)
(*                 datagram it takes, kept or discarded; FALSE counts only *)
(*                 kept datagrams, so chaff (zero-length datagrams,        *)
(*                 refused sources) holds the worker inside the read.      *)
(*                                                                         *)
(* Chaff is the ordinal 0 in the socket: a datagram the receive helper     *)
(* takes and discards.  It is never delivered and never kept.              *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS NumBanks,        \* PORTABLE_RX_BANKS, at least 2
          Cap,             \* PORTABLE_RX_CAPACITY: datagrams per socket read
          Budget,          \* the count passed to one portable_receive call
          MaxDatagrams,    \* datagrams the network delivers to the socket
          MaxChaff,        \* discarded datagrams the network delivers
          HoldOnFull, ReturnWhenRead, QuitBeforeWake, WakeOnReturn, Timeout,
          BoundedRead

ASSUME NumBanks >= 2 /\ Cap >= 1 /\ Budget >= 1 /\ MaxDatagrams >= 1 /\ MaxChaff >= 0
ASSUME {HoldOnFull, ReturnWhenRead, QuitBeforeWake, WakeOnReturn, Timeout,
        BoundedRead} \subseteq BOOLEAN

Banks == 1..NumBanks
Ids == 1..MaxDatagrams
NoBank == 0
Chaff == 0

VARIABLES arrived,    \* datagrams that have entered the socket, 0..MaxDatagrams
          sock,       \* the socket receive buffer, oldest first
          data,       \* per bank, the datagrams it holds (its count prefix)
          prod,       \* rx_producer: the worker's bank
          cons,       \* rx_consumer: main's bank, NoBank for -1
          cursor,     \* rx_cursor
          free,       \* rx_free_index FIFO
          ready,      \* rx_ready_index FIFO
          delivered,  \* datagrams portable_receive returned, in order
          lock,       \* rx_mutex holder: "none", "worker" or "main"
          wake,       \* rx_wake: a pending signal
          quit,       \* server->quit as the RX worker reads it
          stopping,   \* server->stopping
          wpc,        \* worker program counter
          mpc,        \* main program counter
          budget,     \* the rest of the current call's count
          freed,      \* this acquire returned a bank to an empty free FIFO
          chaff,      \* chaff datagrams that have entered the socket
          taken       \* datagrams the current read has taken, kept or not

vars == <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
          lock, wake, quit, stopping, wpc, mpc, budget, freed, chaff, taken>>

Range(s) == {s[i] : i \in 1..Len(s)}
RECURSIVE Concat(_)
Concat(s) == IF s = <<>> THEN <<>> ELSE data[Head(s)] \o Concat(Tail(s))
Upto(n) == [i \in 1..n |-> i]
Kept(s) == SelectSeq(s, LAMBDA x : x # Chaff)

Init ==
    /\ arrived = 0
    /\ sock = <<>>
    /\ data = [b \in Banks |-> <<>>]
    /\ prod = 1
    /\ cons = NoBank
    /\ cursor = 0
    /\ free = [i \in 1..(NumBanks - 1) |-> i + 1]
    /\ ready = <<>>
    /\ delivered = <<>>
    /\ lock = "none"
    /\ wake = FALSE
    /\ quit = FALSE
    /\ stopping = FALSE
    /\ wpc = "top"
    /\ mpc = "idle"
    /\ budget = 0
    /\ freed = FALSE
    /\ chaff = 0
    /\ taken = 0

\* ------------------------------------------------------------------------
\* The network.  A datagram enters the socket buffer.

Arrive ==
    /\ arrived < MaxDatagrams
    /\ arrived' = arrived + 1
    /\ sock' = Append(sock, arrived + 1)
    /\ UNCHANGED <<data, prod, cons, cursor, free, ready, delivered, lock, wake,
                   quit, stopping, wpc, mpc, budget, freed, chaff, taken>>

\* A datagram the receive helper discards (zero-length, or a refused source)
\* enters the socket.
ArriveChaff ==
    /\ chaff < MaxChaff
    /\ chaff' = chaff + 1
    /\ sock' = Append(sock, Chaff)
    /\ UNCHANGED <<arrived, data, prod, cons, cursor, free, ready, delivered, lock,
                   wake, quit, stopping, wpc, mpc, budget, freed, taken>>

\* ------------------------------------------------------------------------
\* The RX worker, portable_rx_worker.

\* the loop test: quit ends the worker
WTop ==
    /\ wpc = "top"
    /\ wpc' = IF quit THEN "exited" ELSE "publish"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed, chaff, taken>>

\* portable_rx_publish: an empty producer needs no exchange
WPublishEmpty ==
    /\ wpc = "publish"
    /\ data[prod] = <<>>
    /\ wpc' = "read"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed, chaff, taken>>

\* portable_rx_publish: the worker's blocking acquire
WLock ==
    /\ wpc = "publish"
    /\ data[prod] # <<>>
    /\ lock = "none"
    /\ lock' = "worker"
    /\ wpc' = "swap"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   wake, quit, stopping, mpc, budget, freed, chaff, taken>>

\* under rx_mutex: queue the producer and take a free bank, or refuse
WSwap ==
    /\ wpc = "swap"
    /\ lock' = "none"
    /\ IF free = <<>>
       THEN /\ wpc' = IF HoldOnFull THEN "waitfull" ELSE "read"
            /\ UNCHANGED <<prod, free, ready, chaff, taken>>
       ELSE /\ ready' = Append(ready, prod)
            /\ prod' = Head(free)
            /\ free' = Tail(free)
            /\ wpc' = "reset"
    /\ UNCHANGED <<arrived, sock, data, cons, cursor, delivered, wake, quit,
                   stopping, mpc, budget, freed, chaff, taken>>

\* after the release: the new producer's count = 0
WReset ==
    /\ wpc = "reset"
    /\ data' = [data EXCEPT ![prod] = <<>>]
    /\ wpc' = "read"
    /\ UNCHANGED <<arrived, sock, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed, chaff, taken>>

\* rocketnet_platform_socket_receive_packets into the producer, one
\* datagram per step.  bank->count is assigned, so the read replaces
\* whatever the bank held; an empty socket returns 0 at once.
WRead ==
    /\ wpc = "read"
    /\ \/ /\ sock # <<>>
          /\ data' = [data EXCEPT ![prod] = <<>>]
          /\ taken' = 0
          /\ wpc' = "reading"
       \/ /\ sock = <<>>
          /\ wpc' = "waitidle"
          /\ UNCHANGED <<data, taken>>
    /\ UNCHANGED <<arrived, sock, prod, cons, cursor, free, ready, delivered, lock,
                   wake, quit, stopping, mpc, budget, freed, chaff>>

\* the read takes the oldest datagram: chaff is discarded, any other is kept.
\* With BoundedRead every datagram spends the read budget of Cap; without it
\* only a kept one counts against the bank's room.
WReadOne ==
    /\ wpc = "reading"
    /\ sock # <<>>
    /\ Len(data[prod]) < Cap
    /\ BoundedRead => taken < Cap
    /\ data' = IF Head(sock) = Chaff THEN data
               ELSE [data EXCEPT ![prod] = Append(@, Head(sock))]
    /\ sock' = Tail(sock)
    /\ taken' = taken + 1
    /\ UNCHANGED <<arrived, prod, cons, cursor, free, ready, delivered, lock,
                   wake, quit, stopping, wpc, mpc, budget, freed, chaff>>

\* the read returns: the socket is empty, the bank is full or the budget is
\* spent.  A read that kept nothing is an empty read.
WReadEnd ==
    /\ wpc = "reading"
    /\ \/ sock = <<>>
       \/ Len(data[prod]) = Cap
       \/ BoundedRead /\ taken = Cap
    /\ wpc' = IF data[prod] = <<>> THEN "waitidle" ELSE "top"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed, chaff, taken>>

\* full banks: wait on the wake alone
WWaitFull ==
    /\ wpc = "waitfull"
    /\ \/ /\ wake
          /\ wake' = FALSE
       \/ /\ Timeout
          /\ UNCHANGED wake
    /\ wpc' = "top"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, quit, stopping, mpc, budget, freed, chaff, taken>>

\* an empty read: wait on the wake or socket readability
WWaitIdle ==
    /\ wpc = "waitidle"
    /\ \/ /\ wake
          /\ wake' = FALSE
       \/ /\ sock # <<>>
          /\ UNCHANGED wake
       \/ /\ Timeout
          /\ UNCHANGED wake
    /\ wpc' = "top"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, quit, stopping, mpc, budget, freed, chaff, taken>>

\* ------------------------------------------------------------------------
\* Main, portable_receive and portable_rx_acquire.

MCall ==
    /\ mpc = "idle"
    /\ ~stopping
    /\ mpc' = "call"
    /\ budget' = Budget
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, wpc, freed, chaff, taken>>

Unread == IF cons = NoBank THEN 0 ELSE Len(data[cons]) - cursor

\* copy one datagram at the cursor
MCopy ==
    /\ mpc = "call"
    /\ budget > 0
    /\ Unread > 0
    /\ delivered' = Append(delivered, data[cons][cursor + 1])
    /\ cursor' = cursor + 1
    /\ budget' = budget - 1
    /\ UNCHANGED <<arrived, sock, data, prod, cons, free, ready, lock, wake,
                   quit, stopping, wpc, mpc, freed, chaff, taken>>

\* the budget is spent: the call returns.  The reversed rule returns a
\* partly copied consumer here.
MEnd ==
    /\ mpc = "call"
    /\ budget = 0
    /\ IF ~ReturnWhenRead /\ cons # NoBank /\ lock = "none"
       THEN /\ free' = Append(free, cons)
            /\ cons' = NoBank
       ELSE UNCHANGED <<free, cons, chaff, taken>>
    /\ mpc' = "idle"
    /\ UNCHANGED <<arrived, sock, data, prod, cursor, ready, delivered, lock,
                   wake, quit, stopping, wpc, budget, freed, chaff, taken>>

\* the consumer is completely copied (or absent): try the mutex
MTry ==
    /\ mpc = "call"
    /\ budget > 0
    /\ Unread = 0
    /\ lock = "none"
    /\ lock' = "main"
    /\ mpc' = "swap"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   wake, quit, stopping, wpc, budget, freed, chaff, taken>>

\* a failed try ends the call; main never waits
MTryFail ==
    /\ mpc = "call"
    /\ budget > 0
    /\ Unread = 0
    /\ lock # "none"
    /\ mpc' = "idle"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, wpc, budget, freed, chaff, taken>>

\* under rx_mutex: return the consumer, take the oldest ready bank
MSwap ==
    /\ mpc = "swap"
    /\ LET returned == cons # NoBank
           free1 == IF returned THEN Append(free, cons) ELSE free
       IN /\ freed' = (returned /\ free = <<>>)
          /\ IF ready # <<>>
             THEN /\ cons' = Head(ready)
                  /\ ready' = Tail(ready)
             ELSE /\ cons' = NoBank
                  /\ UNCHANGED ready
          /\ free' = free1
    /\ cursor' = 0
    /\ lock' = "none"
    /\ mpc' = "wake"
    /\ UNCHANGED <<arrived, sock, data, prod, delivered, wake, quit, stopping,
                   wpc, budget, chaff, taken>>

\* after the release: signal the worker if it may be waiting for a free bank
MWake ==
    /\ mpc = "wake"
    /\ wake' = (wake \/ (WakeOnReturn /\ freed))
    /\ freed' = FALSE
    /\ mpc' = IF cons = NoBank THEN "idle" ELSE "call"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, quit, stopping, wpc, budget, chaff, taken>>

\* portable_stop, between calls: stopping, then quit and the RX wake in the
\* order the rule says
MStop ==
    /\ mpc = "idle"
    /\ ~stopping
    /\ stopping' = TRUE
    /\ mpc' = "stop1"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, wpc, budget, freed, chaff, taken>>

MStop1 ==
    /\ mpc = "stop1"
    /\ IF QuitBeforeWake THEN quit' = TRUE /\ UNCHANGED wake
                         ELSE wake' = TRUE /\ UNCHANGED quit
    /\ mpc' = "stop2"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, stopping, wpc, budget, freed, chaff, taken>>

MStop2 ==
    /\ mpc = "stop2"
    /\ IF QuitBeforeWake THEN wake' = TRUE /\ UNCHANGED quit
                         ELSE quit' = TRUE /\ UNCHANGED wake
    /\ mpc' = "stopped"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, stopping, wpc, budget, freed, chaff, taken>>

\* after stop main polls portable_stopped between frames; a poll changes
\* nothing until the worker has exited (then it is the terminal state)
MPollStopped ==
    /\ mpc = "stopped"
    /\ UNCHANGED vars

Next ==
    \/ Arrive \/ ArriveChaff
    \/ WTop \/ WPublishEmpty \/ WLock \/ WSwap \/ WReset \/ WRead
    \/ WReadOne \/ WReadEnd
    \/ WWaitFull \/ WWaitIdle
    \/ MCall \/ MCopy \/ MEnd \/ MTry \/ MTryFail \/ MSwap \/ MWake
    \/ MStop \/ MStop1 \/ MStop2
    \/ MPollStopped

Spec == Init /\ [][Next]_vars

WorkerStep == WTop \/ WPublishEmpty \/ WLock \/ WSwap \/ WReset \/ WRead \/
              WReadOne \/ WReadEnd \/ WWaitFull \/ WWaitIdle

\* The worker runs whenever it can, and its blocking acquire is granted if
\* the mutex is free infinitely often (main holds it for an index swap
\* only).  Main keeps calling portable_receive and completes its calls; its
\* try succeeds if it is enabled infinitely often.
\* Stop is not forced: StopCompletes is conditional on stop starting.
SpecFair ==
    /\ Spec
    /\ WF_vars(WorkerStep) /\ SF_vars(WLock)
    /\ WF_vars(MCall) /\ WF_vars(MCopy) /\ WF_vars(MEnd) /\ WF_vars(MSwap)
    /\ WF_vars(MWake) /\ WF_vars(MTryFail) /\ SF_vars(MTry)
    /\ WF_vars(MStop1) /\ WF_vars(MStop2)

-----------------------------------------------------------------------------

TypeOK ==
    /\ arrived \in 0..MaxDatagrams
    /\ chaff \in 0..MaxChaff
    /\ sock \in Seq(Ids \cup {Chaff})
    /\ taken \in 0..(MaxDatagrams + MaxChaff)
    /\ data \in [Banks -> Seq(Ids)]
    /\ \A b \in Banks : Len(data[b]) <= Cap
    /\ prod \in Banks
    /\ cons \in Banks \cup {NoBank}
    /\ cursor \in 0..Cap
    /\ free \in Seq(Banks)
    /\ ready \in Seq(Banks)
    /\ delivered \in Seq(Ids)
    /\ lock \in {"none", "worker", "main"}
    /\ {wake, quit, stopping, freed} \subseteq BOOLEAN
    /\ budget \in 0..Budget

\* Every bank has exactly one owner: the worker's producer, main's consumer,
\* the free FIFO or the ready FIFO.  No bank is reachable by two threads.
ExclusiveOwnership ==
    LET owners == <<prod>> \o (IF cons = NoBank THEN <<>> ELSE <<cons>>) \o free \o ready
    IN /\ Len(owners) = NumBanks
       /\ Range(owners) = Banks

\* Main never reads past the consumer's count.
CursorInBank ==
    cons = NoBank \/ cursor <= Len(data[cons])

\* The producer's content, invisible while its stale count awaits the reset.
Produced == IF wpc = "reset" THEN <<>> ELSE data[prod]

\* Exact FIFO delivery: nothing lost, duplicated or reordered.
ExactDelivery ==
    delivered
      \o (IF cons = NoBank THEN <<>> ELSE SubSeq(data[cons], cursor + 1, Len(data[cons])))
      \o Concat(ready)
      \o Produced
      \o Kept(sock)
    = Upto(arrived)

\* The read budget: one socket read takes at most Cap datagrams, kept or
\* discarded, so the worker is back at its quit test after a bounded read
\* however much chaff the socket holds.
ReadBudget ==
    wpc = "reading" => taken <= Cap

\* Main's only lock is a try: it never blocks (MTryFail ends the call).
\* Stated as an action property: main never holds the lock across a call
\* boundary.
MainLockIsBrief ==
    mpc \in {"idle", "call", "stop1", "stop2", "stopped"} => lock # "main"

\* Liveness: every datagram that reaches the socket is delivered unless stop
\* begins.
EveryDatagramDelivered ==
    \A i \in Ids : (arrived >= i) ~> (stopping \/ \E j \in 1..Len(delivered) : delivered[j] = i)

\* Liveness: once stop begins the worker exits.
StopCompletes ==
    stopping ~> (wpc = "exited")

=============================================================================
