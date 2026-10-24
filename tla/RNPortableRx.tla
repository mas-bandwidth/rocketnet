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
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS NumBanks,        \* PORTABLE_RX_BANKS, at least 2
          Cap,             \* PORTABLE_RX_CAPACITY: datagrams per socket read
          Budget,          \* the count passed to one portable_receive call
          MaxDatagrams,    \* datagrams the network delivers to the socket
          HoldOnFull, ReturnWhenRead, QuitBeforeWake, WakeOnReturn, Timeout

ASSUME NumBanks >= 2 /\ Cap >= 1 /\ Budget >= 1 /\ MaxDatagrams >= 1
ASSUME {HoldOnFull, ReturnWhenRead, QuitBeforeWake, WakeOnReturn, Timeout} \subseteq BOOLEAN

Banks == 1..NumBanks
Ids == 1..MaxDatagrams
NoBank == 0

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
          freed       \* this acquire returned a bank to an empty free FIFO

vars == <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
          lock, wake, quit, stopping, wpc, mpc, budget, freed>>

Range(s) == {s[i] : i \in 1..Len(s)}
RECURSIVE Concat(_)
Concat(s) == IF s = <<>> THEN <<>> ELSE data[Head(s)] \o Concat(Tail(s))
Upto(n) == [i \in 1..n |-> i]

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

\* ------------------------------------------------------------------------
\* The network.  A datagram enters the socket buffer.

Arrive ==
    /\ arrived < MaxDatagrams
    /\ arrived' = arrived + 1
    /\ sock' = Append(sock, arrived + 1)
    /\ UNCHANGED <<data, prod, cons, cursor, free, ready, delivered, lock, wake,
                   quit, stopping, wpc, mpc, budget, freed>>

\* ------------------------------------------------------------------------
\* The RX worker, portable_rx_worker.

\* the loop test: quit ends the worker
WTop ==
    /\ wpc = "top"
    /\ wpc' = IF quit THEN "exited" ELSE "publish"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed>>

\* portable_rx_publish: an empty producer needs no exchange
WPublishEmpty ==
    /\ wpc = "publish"
    /\ data[prod] = <<>>
    /\ wpc' = "read"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed>>

\* portable_rx_publish: the worker's blocking acquire
WLock ==
    /\ wpc = "publish"
    /\ data[prod] # <<>>
    /\ lock = "none"
    /\ lock' = "worker"
    /\ wpc' = "swap"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   wake, quit, stopping, mpc, budget, freed>>

\* under rx_mutex: queue the producer and take a free bank, or refuse
WSwap ==
    /\ wpc = "swap"
    /\ lock' = "none"
    /\ IF free = <<>>
       THEN /\ wpc' = IF HoldOnFull THEN "waitfull" ELSE "read"
            /\ UNCHANGED <<prod, free, ready>>
       ELSE /\ ready' = Append(ready, prod)
            /\ prod' = Head(free)
            /\ free' = Tail(free)
            /\ wpc' = "reset"
    /\ UNCHANGED <<arrived, sock, data, cons, cursor, delivered, wake, quit,
                   stopping, mpc, budget, freed>>

\* after the release: the new producer's count = 0
WReset ==
    /\ wpc = "reset"
    /\ data' = [data EXCEPT ![prod] = <<>>]
    /\ wpc' = "read"
    /\ UNCHANGED <<arrived, sock, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, mpc, budget, freed>>

\* rocketnet_platform_socket_receive_packets into the producer: bank->count
\* is assigned, so the read replaces whatever the bank held
WRead ==
    /\ wpc = "read"
    /\ \/ /\ sock # <<>>
          /\ \E k \in 1..Cap :
                /\ k <= Len(sock)
                /\ data' = [data EXCEPT ![prod] = SubSeq(sock, 1, k)]
                /\ sock' = SubSeq(sock, k + 1, Len(sock))
          /\ wpc' = "top"
       \/ /\ sock = <<>>
          /\ wpc' = "waitidle"
          /\ UNCHANGED <<sock, data>>
    /\ UNCHANGED <<arrived, prod, cons, cursor, free, ready, delivered, lock,
                   wake, quit, stopping, mpc, budget, freed>>

\* full banks: wait on the wake alone
WWaitFull ==
    /\ wpc = "waitfull"
    /\ \/ /\ wake
          /\ wake' = FALSE
       \/ /\ Timeout
          /\ UNCHANGED wake
    /\ wpc' = "top"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, quit, stopping, mpc, budget, freed>>

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
                   lock, quit, stopping, mpc, budget, freed>>

\* ------------------------------------------------------------------------
\* Main, portable_receive and portable_rx_acquire.

MCall ==
    /\ mpc = "idle"
    /\ ~stopping
    /\ mpc' = "call"
    /\ budget' = Budget
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, wpc, freed>>

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
                   quit, stopping, wpc, mpc, freed>>

\* the budget is spent: the call returns.  The reversed rule returns a
\* partly copied consumer here.
MEnd ==
    /\ mpc = "call"
    /\ budget = 0
    /\ IF ~ReturnWhenRead /\ cons # NoBank /\ lock = "none"
       THEN /\ free' = Append(free, cons)
            /\ cons' = NoBank
       ELSE UNCHANGED <<free, cons>>
    /\ mpc' = "idle"
    /\ UNCHANGED <<arrived, sock, data, prod, cursor, ready, delivered, lock,
                   wake, quit, stopping, wpc, budget, freed>>

\* the consumer is completely copied (or absent): try the mutex
MTry ==
    /\ mpc = "call"
    /\ budget > 0
    /\ Unread = 0
    /\ lock = "none"
    /\ lock' = "main"
    /\ mpc' = "swap"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   wake, quit, stopping, wpc, budget, freed>>

\* a failed try ends the call; main never waits
MTryFail ==
    /\ mpc = "call"
    /\ budget > 0
    /\ Unread = 0
    /\ lock # "none"
    /\ mpc' = "idle"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, stopping, wpc, budget, freed>>

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
                   wpc, budget>>

\* after the release: signal the worker if it may be waiting for a free bank
MWake ==
    /\ mpc = "wake"
    /\ wake' = (wake \/ (WakeOnReturn /\ freed))
    /\ freed' = FALSE
    /\ mpc' = IF cons = NoBank THEN "idle" ELSE "call"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, quit, stopping, wpc, budget>>

\* portable_stop, between calls: stopping, then quit and the RX wake in the
\* order the rule says
MStop ==
    /\ mpc = "idle"
    /\ ~stopping
    /\ stopping' = TRUE
    /\ mpc' = "stop1"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, wake, quit, wpc, budget, freed>>

MStop1 ==
    /\ mpc = "stop1"
    /\ IF QuitBeforeWake THEN quit' = TRUE /\ UNCHANGED wake
                         ELSE wake' = TRUE /\ UNCHANGED quit
    /\ mpc' = "stop2"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, stopping, wpc, budget, freed>>

MStop2 ==
    /\ mpc = "stop2"
    /\ IF QuitBeforeWake THEN wake' = TRUE /\ UNCHANGED quit
                         ELSE quit' = TRUE /\ UNCHANGED wake
    /\ mpc' = "stopped"
    /\ UNCHANGED <<arrived, sock, data, prod, cons, cursor, free, ready, delivered,
                   lock, stopping, wpc, budget, freed>>

\* after stop main polls portable_stopped between frames; a poll changes
\* nothing until the worker has exited (then it is the terminal state)
MPollStopped ==
    /\ mpc = "stopped"
    /\ UNCHANGED vars

Next ==
    \/ Arrive
    \/ WTop \/ WPublishEmpty \/ WLock \/ WSwap \/ WReset \/ WRead
    \/ WWaitFull \/ WWaitIdle
    \/ MCall \/ MCopy \/ MEnd \/ MTry \/ MTryFail \/ MSwap \/ MWake
    \/ MStop \/ MStop1 \/ MStop2
    \/ MPollStopped

Spec == Init /\ [][Next]_vars

WorkerStep == WTop \/ WPublishEmpty \/ WLock \/ WSwap \/ WReset \/ WRead \/
              WWaitFull \/ WWaitIdle

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
    /\ sock \in Seq(Ids)
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
      \o sock
    = Upto(arrived)

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
