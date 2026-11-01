--------------------------- MODULE RNBankFifoLayout ---------------------------
(***************************************************************************)
(* The hand-written bank queues of the server and client transports at    *)
(* their layout: N banks, a producer that holds one bank (or, for the      *)
(* iocp client, a kernel that holds the posted ones), a free queue, a      *)
(* ready FIFO and a consumer that holds the banks it took.  Only bank      *)
(* indices move, under the caller's mutex.  The instances:                 *)
(*                                                                         *)
(*   portable_rx_publish, portable_rx_acquire, portable_tx_publish and     *)
(*   portable_tx_worker (sdk/rocketnet_driver_builtin.cpp): free FIFO and  *)
(*   ready FIFO of three, free initialized from the literal { 1, 2, 0 }.   *)
(*   rocketnet_server_gso_publish_send, rocketnet_server_gso_take_send and *)
(*   rocketnet_server_gso_return_send (sdk/rocketnet_server_gso_linux.cpp): *)
(*   the same shape and literal.                                           *)
(*   rocketnet_xdp_buffer_exchange_t publish, claim and recycle            *)
(*   (sdk/rocketnet_server_socket_linux.cpp): free FIFO and ready FIFO of  *)
(*   N, free initialized by a loop.                                        *)
(*   rocketnet_buffer_handoff_publish, _take, _release and _init           *)
(*   (sdk/rocketnet_buffer_handoff.h): the same with the max_ready test.   *)
(*   rocketnet_server_iocp_send_packets and rocketnet_iocp_send_worker     *)
(*   (sdk/rocketnet_server_iocp_windows.cpp): free LIFO stack              *)
(*   send_free[--send_free_count] from the literal, ready FIFO of three.   *)
(*   rocketnet_iocp_publish_receive_bank and                               *)
(*   rocketnet_server_iocp_receive_packets: free LIFO, ready FIFO of four  *)
(*   and an exposed list main holds until its next call.                   *)
(*   rocketnet_rio_worker_pass and rocketnet_server_rio_receive_packets    *)
(*   (sdk/rocketnet_server_rio_windows.cpp): the same as the iocp receive. *)
(*   rocketnet_client_iocp_dequeue and rocketnet_client_iocp_receive_batch *)
(*   (sdk/rocketnet_client_iocp_windows.cpp): no free queue (a buffer not  *)
(*   ready or being copied is posted to the kernel, which completes them   *)
(*   in any order); a ready FIFO of num_receives in an array of            *)
(*   ROCKETNET_CLIENT_IOCP_RECEIVES.                                       *)
(*                                                                         *)
(* The arrays are the code's: free and ready each an array of ArrLen       *)
(* entries; a FIFO is a head and a count, its tail ( head + count ) mod N, *)
(* a head advance ( head + 1 ) mod N; a LIFO is a count, pushed at count   *)
(* and popped at count - 1.  A position at or past ArrLen (or below 0) is  *)
(* not written: it sets oob, which InBounds refuses, and stops the model.  *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   ExactlyOnePlace  read through the arrays, every bank is in exactly    *)
(*                    one place: the producer, a free position, a ready    *)
(*                    position, the consumer's list or posted.             *)
(*   ReadyMatches     the ready positions from the head, read through the  *)
(*                    array, are the banks published and not yet taken, in *)
(*                    publish order.                                       *)
(*   InBounds         no position outside the arrays is read or written.   *)
(*   CountsInRange    the free and ready counts stay in 0..N.              *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   FreeSlack   0 is the code's test (no exchange when free_count = 0);   *)
(*               1 tests one late and pops from an empty free queue.       *)
(*   HeadMod, TailMod  the modulo of a head advance and of the tail        *)
(*               position; 0 is no modulo.                                 *)
(*   PushKind, PopKind  "fifo" or "lifo" for the free queue's push and     *)
(*               pop; the code uses one discipline for both.               *)
(*   InitKind    "literal" is { 1, 2, 0 } and zeros past it, count N - 1;  *)
(*               "loop" is 1..N-1 in order, count N - 1 (the loops of     *)
(*               rocketnet_xdp_buffer_exchange_t, rocketnet_buffer_handoff *)
(*               _init with producer 0, and the iocp and rio creates);     *)
(*               "posted" is every buffer posted (the iocp client).        *)
(*   MaxReady    rocketnet_buffer_handoff_publish's max_ready; N where the *)
(*               code has no such test.                                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS N,          \* banks (or posted receives)
          ArrLen,     \* entries in each index array (N except the iocp client)
          ConsMax,    \* banks the consumer may hold at once
          MaxReady,   \* max_ready, or N for no such test
          MaxPub,     \* publishes the harness makes in all
          FreeSlack, HeadMod, TailMod, PushKind, PopKind, InitKind

ASSUME N >= 3 /\ ArrLen >= N /\ ConsMax \in 1..N /\ MaxReady \in 1..N /\ MaxPub >= 1
ASSUME FreeSlack \in 0..1 /\ HeadMod \in 0..ArrLen /\ TailMod \in 0..ArrLen
ASSUME PushKind \in {"fifo", "lifo"} /\ PopKind \in {"fifo", "lifo"}
ASSUME InitKind \in {"literal", "loop", "posted"}

Banks == 0..N - 1
NoBank == -1
Pos == 0..ArrLen - 1
Posted == InitKind = "posted"      \* the kernel, not a producer, fills buffers

VARIABLES producer,     \* the producer's bank (NoBank for the iocp client)
          freeArr,      \* free_index / free_banks / send_free
          freeHead,     \* free_head (FIFO only)
          freeCount,    \* free_count
          readyArr,     \* ready_index / ready_banks / send_ready / ready
          readyHead,    \* ready_head
          readyCount,   \* ready_count
          exposed,      \* the consumer's banks: exposed_banks, or the one it holds
          exposedCount, \* exposed_count (0 or 1 for a single consumer)
          posted,       \* the iocp client: posted[b], the kernel holds buffer b
          oob,          \* a position outside an array was read or written
          absReady,     \* ghost: banks published and not taken, in order
          pubs          \* ghost: publishes so far

vars == <<producer, freeArr, freeHead, freeCount, readyArr, readyHead, readyCount,
          exposed, exposedCount, posted, oob, absReady, pubs>>

Red(x) == IF HeadMod = 0 THEN x ELSE x % HeadMod     \* ( head + 1 ) % N
Tl(x) == IF TailMod = 0 THEN x ELSE x % TailMod      \* ( head + count ) % N
In(p) == p >= 0 /\ p < ArrLen
Rd(arr, p) == IF In(p) THEN arr[p] ELSE NoBank
Wr(arr, p, v) == IF In(p) THEN [arr EXCEPT ![p] = v] ELSE arr

\* the free queue as a record, so a push and a pop compose
FreeRec == [arr |-> freeArr, head |-> freeHead, count |-> freeCount, bad |-> FALSE]

\* free_index[( free_head + free_count ) % N] = b, or free_banks[free_count++] = b
Push(f, b) ==
    LET p == IF PushKind = "fifo" THEN Tl(f.head + f.count) ELSE f.count
    IN [arr |-> Wr(f.arr, p, b), head |-> f.head, count |-> f.count + 1,
        bad |-> f.bad \/ ~In(p)]

\* free_index[free_head], free_head = ( free_head + 1 ) % N, or free_banks[--free_count]
PopBank(f) == IF PopKind = "fifo" THEN Rd(f.arr, f.head) ELSE Rd(f.arr, f.count - 1)
Pop(f) ==
    IF PopKind = "fifo"
      THEN [arr |-> f.arr, head |-> Red(f.head + 1), count |-> f.count - 1,
            bad |-> f.bad \/ ~In(f.head)]
      ELSE [arr |-> f.arr, head |-> f.head, count |-> f.count - 1,
            bad |-> f.bad \/ ~In(f.count - 1)]

RECURSIVE PushAll(_, _, _)
PushAll(f, j, n) == IF j >= n THEN f ELSE PushAll(Push(f, exposed[j]), j + 1, n)

LiteralInit == [p \in Pos |-> CASE p = 0 -> 1 [] p = 1 -> 2 [] OTHER -> 0]
LoopInit == [p \in Pos |-> IF p < N - 1 THEN p + 1 ELSE 0]

Init ==
    /\ producer = IF Posted THEN NoBank ELSE 0
    /\ freeArr = CASE InitKind = "literal" -> LiteralInit
                   [] InitKind = "loop" -> LoopInit
                   [] OTHER -> [p \in Pos |-> 0]
    /\ freeHead = 0
    /\ freeCount = IF Posted THEN 0 ELSE N - 1
    /\ readyArr = [p \in Pos |-> 0]
    /\ readyHead = 0
    /\ readyCount = 0
    /\ exposed = [j \in 0..ConsMax - 1 |-> 0]
    /\ exposedCount = 0
    /\ posted = [b \in Banks |-> Posted]
    /\ oob = FALSE
    /\ absReady = <<>>
    /\ pubs = 0

\* The ready tail write shared by both publishes.
ReadyTail == Tl(readyHead + readyCount)

\* The producer publishes: its bank onto the ready tail, the oldest (or top)
\* free bank becomes the producer.  Refused, with nothing changed, when no
\* bank is free or max_ready banks are ready.  portable_rx_publish,
\* portable_tx_publish, rocketnet_server_gso_publish_send,
\* rocketnet_xdp_buffer_exchange_t::publish, rocketnet_buffer_handoff_publish,
\* rocketnet_server_iocp_send_packets, rocketnet_iocp_publish_receive_bank,
\* rocketnet_rio_worker_pass (WPublish).
Publish ==
    /\ ~Posted
    /\ ~oob
    /\ pubs < MaxPub
    /\ freeCount >= 1 - FreeSlack
    /\ readyCount < MaxReady
    /\ LET f == Pop(FreeRec)
       IN /\ producer' = PopBank(FreeRec)
          /\ freeHead' = f.head
          /\ freeCount' = f.count
          /\ readyArr' = Wr(readyArr, ReadyTail, producer)
          /\ readyCount' = readyCount + 1
          /\ oob' = (f.bad \/ ~In(ReadyTail))
    /\ absReady' = Append(absReady, producer)
    /\ pubs' = pubs + 1
    /\ UNCHANGED <<freeArr, readyHead, exposed, exposedCount, posted>>

\* The iocp client: a posted receive completes with a datagram and its
\* buffer joins the ready tail, in completion order.  rocketnet_client_iocp_dequeue.
Complete(b) ==
    /\ Posted
    /\ ~oob
    /\ pubs < MaxPub
    /\ posted[b]
    /\ posted' = [posted EXCEPT ![b] = FALSE]
    /\ readyArr' = Wr(readyArr, ReadyTail, b)
    /\ readyCount' = readyCount + 1
    /\ oob' = ~In(ReadyTail)
    /\ absReady' = Append(absReady, b)
    /\ pubs' = pubs + 1
    /\ UNCHANGED <<producer, freeArr, freeHead, freeCount, readyHead, exposed, exposedCount>>

\* The consumer takes the oldest ready bank onto its list.  portable_rx_acquire,
\* portable_tx_worker, rocketnet_server_gso_take_send,
\* rocketnet_xdp_buffer_exchange_t::claim, rocketnet_buffer_handoff_take,
\* rocketnet_iocp_send_worker, rocketnet_server_iocp_receive_packets,
\* rocketnet_server_rio_receive_packets, rocketnet_client_iocp_receive_batch.
Take ==
    /\ ~oob
    /\ readyCount > 0
    /\ exposedCount < ConsMax
    /\ exposed' = [exposed EXCEPT ![exposedCount] = Rd(readyArr, readyHead)]
    /\ exposedCount' = exposedCount + 1
    /\ readyHead' = Red(readyHead + 1)
    /\ readyCount' = readyCount - 1
    /\ oob' = ~In(readyHead)
    /\ absReady' = Tail(absReady)
    /\ UNCHANGED <<producer, freeArr, freeHead, freeCount, readyArr, posted, pubs>>

\* The consumer returns every bank it holds, in order, to the free queue (or,
\* for the iocp client, posts it again).  portable_rx_acquire,
\* portable_tx_worker, rocketnet_server_gso_return_send,
\* rocketnet_xdp_buffer_exchange_t::recycle, rocketnet_buffer_handoff_release,
\* rocketnet_iocp_send_worker, rocketnet_server_iocp_receive_packets,
\* rocketnet_server_rio_receive_packets, rocketnet_client_iocp_receive_batch.
Release ==
    /\ ~oob
    /\ exposedCount > 0
    /\ IF Posted
         THEN /\ posted' = [b \in Banks |->
                              posted[b] \/ \E j \in 0..exposedCount - 1 : exposed[j] = b]
              /\ UNCHANGED <<freeArr, freeHead, freeCount, oob>>
         ELSE LET f == PushAll(FreeRec, 0, exposedCount)
              IN /\ freeArr' = f.arr
                 /\ freeHead' = f.head
                 /\ freeCount' = f.count
                 /\ oob' = f.bad
                 /\ UNCHANGED posted
    /\ exposedCount' = 0
    /\ UNCHANGED <<producer, readyArr, readyHead, readyCount, exposed, absReady, pubs>>

Next == Publish \/ (\E b \in Banks : Complete(b)) \/ Take \/ Release

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ producer \in Banks \cup {NoBank}
    /\ freeArr \in [Pos -> Banks \cup {NoBank}]
    /\ readyArr \in [Pos -> Banks \cup {NoBank}]
    /\ freeHead \in 0..2 * ArrLen + MaxPub
    /\ readyHead \in 0..2 * ArrLen + MaxPub
    /\ freeCount \in -1..N + 1
    /\ readyCount \in 0..N + 1
    /\ exposedCount \in 0..ConsMax
    /\ oob \in BOOLEAN
    /\ pubs \in 0..MaxPub

InBounds == ~oob

CountsInRange == freeCount \in 0..N /\ readyCount \in 0..N

FreeSeq ==
    IF Posted \/ freeCount <= 0 THEN <<>>
    ELSE IF PopKind = "fifo"
      THEN [i \in 1..freeCount |-> Rd(freeArr, Red(freeHead + i - 1))]
      ELSE [i \in 1..freeCount |-> Rd(freeArr, i - 1)]
ReadySeq == [i \in 1..readyCount |-> Rd(readyArr, Red(readyHead + i - 1))]
ExposedSeq == [i \in 1..exposedCount |-> exposed[i - 1]]
ProducerSeq == IF producer = NoBank THEN <<>> ELSE <<producer>>
PostedSeq == IF Posted THEN [i \in 1..Cardinality({b \in Banks : posted[b]}) |->
                               CHOOSE b \in Banks : posted[b] /\
                                 Cardinality({c \in Banks : posted[c] /\ c < b}) = i - 1]
             ELSE <<>>
Places == ProducerSeq \o FreeSeq \o ReadySeq \o ExposedSeq \o PostedSeq

Occurrences(b) == Cardinality({i \in 1..Len(Places) : Places[i] = b})

\* Every bank is in exactly one place, read through the arrays.
ExactlyOnePlace ==
    /\ \A b \in Banks : Occurrences(b) = 1
    /\ Len(Places) = N

\* The ready positions are the published banks not yet taken, in order.
ReadyMatches == ReadySeq = absReady

=============================================================================
