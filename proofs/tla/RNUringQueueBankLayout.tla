------------------------ MODULE RNUringQueueBankLayout ------------------------
(***************************************************************************)
(* The io_uring server's per-queue send banks at the code's layout (layer  *)
(* L0 of the per-queue send; sdk/rocketnet_server_uring_linux.cpp          *)
(* rocketnet_server_uring_start_packet, _finish_packet, _abort_packet and  *)
(* the slot arithmetic they share).                                        *)
(*                                                                         *)
(* Q queues, each with NB banks of S slots of Stride bytes.  The banks of  *)
(* every queue are laid end to end, bank b of queue q at byte address      *)
(* Addr(q, b), so a pointer one byte before a bank is inside its           *)
(* neighbour: the worst case for a slot computed from a pointer.           *)
(*                                                                         *)
(* A fill thread's start_packet picks the destination's queue (QueueOf,    *)
(* the address-and-port hash the XDP transport uses: destinations are      *)
(* numbered so that d mod Q is the queue the hash gives, so any assignment *)
(* of destinations to queues is a choice of Dests), takes the packet id    *)
(* gid * Q + q from one fetch_add on the server's id counter, and claims   *)
(* the next index of the queue's producer bank with one fetch_add on that  *)
(* bank's counter; an index at or past S is refused (NULL, counted) and    *)
(* the counter stays past S until the bank is reset.  finish_packet and    *)
(* abort_packet take the queue from the id (id mod Q) and the slot from    *)
(* the pointer's byte offset past the queue's producer bank: refused       *)
(* (-1) when the offset is negative, not a whole number of strides or past *)
(* the bank, all tested before the division, so a refused pointer writes   *)
(* nothing.  A caller may hand finish a pointer up to MaxSkew bytes off,   *)
(* either way, the one start returned (a contract violation): it must      *)
(* fail closed.                                                            *)
(*                                                                         *)
(* What each call gives back is an argument of its action, so the trace    *)
(* check binds it (README-TraceCheck.md): start's pointer and id (Start,   *)
(* or StartFull when it returns NULL, with the id it still writes),        *)
(* finish's and abort's result (Finish when true, FinishRefused when       *)
(* false), and the queues a turn turned (Flush(n)).                        *)
(*                                                                         *)
(* Flush is send_packets with the fill threads joined (the phase           *)
(* contract): each queue that staged anything moves its producer to its    *)
(* next bank, whose counter is reset before any fill sees it.  The         *)
(* handoff of the published bank to the queue's worker is layer L1, the    *)
(* io_uring transport's own (a later piece); here a flush only turns the   *)
(* producer over, which is all the claim arithmetic depends on.            *)
(*                                                                         *)
(* The checked claims (each a flag the step that breaks it sets):          *)
(*   InBounds       no claim hands out a slot outside 0..S-1.              *)
(*   NoSharedSlot   no slot of a producer bank is handed to two claims in  *)
(*                  one fill.                                              *)
(*   NoForeignWrite a finish or abort writes only the slot its own claim   *)
(*                  holds, or nothing.                                     *)
(*   RouteMatches   finish and abort find the queue start chose.           *)
(*   StagedMatches  the claimed slots of each producer bank are exactly    *)
(*                  0..min(counter, S) - 1: the worker reads min(num, S).  *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   ClaimCmp     "ge" refuses an index >= S (the code); "gt" refuses only *)
(*                past S and hands out slot S (InBounds).                  *)
(*   CounterWrap  0 is the code's 64-bit counter, which no fill wraps; k   *)
(*                > 0 counts modulo k, so a narrow counter wraps back to   *)
(*                slot 0 while it is claimed (NoSharedSlot).               *)
(*   SlotRule     "check" tests the offset before dividing (the code);     *)
(*                "divide" divides first with C's truncation toward zero,  *)
(*                so an offset of -Stride+1..-1 becomes slot 0             *)
(*                (NoForeignWrite).                                        *)
(*   IdRule       "mul" is gid * Q + q (the code); "add" is gid + q, whose *)
(*                remainder is not the queue (RouteMatches).               *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Q,            \* queues (num_xdp_queues)
          NB,           \* banks per queue (ROCKETNET_URING_SEND_BANKS)
          S,            \* slots per bank (ROCKETNET_URING_SEND_QUEUE_SIZE)
          Stride,       \* bytes per slot (ROCKETNET_FRAME_STRIDE)
          Threads,      \* fill threads
          Dests,        \* destinations the fill sends to, numbered by their queue (QueueOf)
          Demand,       \* start_packet calls one thread makes in one fill
          MaxFlush,     \* bound on flushes
          MaxSkew,      \* the most bytes a caller's finish pointer is off, either way
          ClaimCmp,     \* "ge" (the code) or "gt"
          CounterWrap,  \* 0: no wrap (the code's uint64); k > 0: counter modulo k
          SlotRule,     \* "check" (the code) or "divide"
          IdRule        \* "mul" (the code) or "add"

ASSUME Q >= 1 /\ NB >= 2 /\ S >= 1 /\ Stride >= 2 /\ Demand >= 1 /\ MaxFlush >= 0
ASSUME MaxSkew \in Nat
ASSUME Dests \subseteq Nat
ASSUME ClaimCmp \in {"ge", "gt"} /\ CounterWrap \in Nat
ASSUME SlotRule \in {"check", "divide"} /\ IdRule \in {"mul", "add"}

Qs == 0..(Q - 1)
Bs == 0..(NB - 1)
None == [none |-> TRUE]

\* the byte offsets a caller may add to the pointer it finishes
Skew == (-MaxSkew)..MaxSkew

\* the destination's queue (rocketnet_send_queue_of): destination d is one
\* whose address and port hash to queue d mod Q
QueueOf(d) == d % Q

\* bank b of queue q starts here; slot s of it at Addr(q, b) + s * Stride
Addr(q, b) == (q * NB + b) * S * Stride

\* C's division: truncation toward zero
Trunc(a, b) == IF a >= 0 THEN a \div b ELSE -((-a) \div b)

\* the slot a pointer names in a bank, or -1 (rocketnet_server_uring_slot)
SlotFrom(off) ==
    IF SlotRule = "check"
      THEN IF off < 0 \/ off % Stride # 0 \/ off \div Stride >= S THEN -1 ELSE off \div Stride
      ELSE LET s == Trunc(off, Stride) IN IF s < 0 \/ s >= S THEN -1 ELSE s

Bump(c) == IF CounterWrap = 0 THEN c + 1 ELSE (c + 1) % CounterWrap
Granted(i) == IF ClaimCmp = "ge" THEN i < S ELSE i <= S
PidOf(g, q) == IF IdRule = "mul" THEN g * Q + q ELSE g + q

VARIABLES prod,       \* each queue's producer bank (the flow index)
          cnt,        \* each bank's claim counter (num_packets)
          gid,        \* the server's packet id counter
          held,       \* each thread's claim: its queue, bank, slot, pointer and id, or None
          owner,      \* the thread a slot of a producer bank was handed to in this fill, or -1
          demand,     \* start_packet calls each thread made in this fill
          flushes,    \* flushes so far
          oob, shared, foreign, routeBad   \* the flags the broken steps set

vars == <<prod, cnt, gid, held, owner, demand, flushes, oob, shared, foreign, routeBad>>

SlotKeys == Qs \X Bs \X (0..S)

Init ==
    /\ prod = [q \in Qs |-> 0]
    /\ cnt = [q \in Qs |-> [b \in Bs |-> 0]]
    /\ gid = 0
    /\ held = [t \in Threads |-> None]
    /\ owner = [k \in SlotKeys |-> -1]
    /\ demand = [t \in Threads |-> 0]
    /\ flushes = 0
    /\ oob = FALSE /\ shared = FALSE /\ foreign = FALSE /\ routeBad = FALSE

\* what start returns to destination d: the pointer to the slot it claims
\* and the packet id
StartPtr(d) == Addr(QueueOf(d), prod[QueueOf(d)]) + cnt[QueueOf(d)][prod[QueueOf(d)]] * Stride
StartPid(d) == PidOf(gid, QueueOf(d))

\* start_packet(to = d) on thread t, granted: it returns the pointer ptr and
\* writes the id pid
Start(t, d, ptr, pid) ==
    /\ held[t] = None
    /\ demand[t] < Demand
    /\ LET q == QueueOf(d)
           b == prod[q]
           i == cnt[q][b]
       IN /\ Granted(i)
          /\ ptr = Addr(q, b) + i * Stride
          /\ pid = PidOf(gid, q)
          /\ gid' = gid + 1
          /\ cnt' = [cnt EXCEPT ![q][b] = Bump(i)]
          /\ demand' = [demand EXCEPT ![t] = @ + 1]
          /\ held' = [held EXCEPT ![t] = [q |-> q, b |-> b, slot |-> i, ptr |-> ptr, pid |-> pid]]
          /\ owner' = [owner EXCEPT ![<<q, b, i>>] = t]
          /\ oob' = (oob \/ i >= S)
          /\ shared' = (shared \/ owner[<<q, b, i>>] # -1)
    /\ UNCHANGED <<prod, flushes, foreign, routeBad>>

\* start_packet(to = d) on thread t, refused: the producer bank is full, it
\* returns NULL and still writes the id pid
StartFull(t, d, pid) ==
    /\ held[t] = None
    /\ demand[t] < Demand
    /\ LET q == QueueOf(d)
           b == prod[q]
           i == cnt[q][b]
       IN /\ ~Granted(i)
          /\ pid = PidOf(gid, q)
          /\ gid' = gid + 1
          /\ cnt' = [cnt EXCEPT ![q][b] = Bump(i)]
          /\ demand' = [demand EXCEPT ![t] = @ + 1]
    /\ UNCHANGED <<prod, held, owner, flushes, oob, shared, foreign, routeBad>>

\* the slot finish or abort on thread t finds for the pointer k bytes off the
\* one start returned (k = 0 is the contract), or -1
FinishSlot(t, k) ==
    LET h == held[t]
        qd == h.pid % Q
    IN SlotFrom(h.ptr + k - Addr(qd, prod[qd]))

\* finish_packet or abort_packet on thread t with the pointer k bytes off,
\* returning true: it wrote slot FinishSlot(t, k)
Finish(t, k) ==
    /\ held[t] # None
    /\ LET h == held[t]
           qd == h.pid % Q
           b == prod[qd]
           s == FinishSlot(t, k)
       IN /\ s # -1
          /\ held' = [held EXCEPT ![t] = None]
          /\ routeBad' = (routeBad \/ qd # h.q)
          /\ foreign' = (foreign \/ owner[<<qd, b, s>>] # t \/ s # h.slot)
    /\ UNCHANGED <<prod, cnt, gid, owner, demand, flushes, oob, shared>>

\* finish_packet or abort_packet on thread t with the pointer k bytes off,
\* returning false: the pointer names no slot, nothing is written
FinishRefused(t, k) ==
    /\ held[t] # None
    /\ FinishSlot(t, k) = -1
    /\ held' = [held EXCEPT ![t] = None]
    /\ routeBad' = (routeBad \/ held[t].pid % Q # held[t].q)
    /\ UNCHANGED <<prod, cnt, gid, owner, demand, flushes, oob, shared, foreign>>

\* send_packets, the fill joined: every queue that staged turns its producer
\* over to its next bank, reset before any fill sees it; the turn returns n,
\* the queues it turned
Flush(n) ==
    /\ \A t \in Threads : held[t] = None
    /\ flushes < MaxFlush
    /\ LET moved == {q \in Qs : cnt[q][prod[q]] # 0}
           next == [q \in Qs |-> IF q \in moved THEN (prod[q] + 1) % NB ELSE prod[q]]
       IN /\ n = Cardinality(moved)
          /\ prod' = next
          /\ cnt' = [q \in Qs |-> [b \in Bs |-> IF q \in moved /\ b = next[q] THEN 0 ELSE cnt[q][b]]]
    /\ owner' = [k \in SlotKeys |-> -1]
    /\ demand' = [t \in Threads |-> 0]
    /\ flushes' = flushes + 1
    /\ UNCHANGED <<gid, held, oob, shared, foreign, routeBad>>

\* the bound is reached and every claim finished: the run ends here
Done ==
    /\ \A t \in Threads : held[t] = None
    /\ \A t \in Threads : demand[t] = Demand
    /\ flushes = MaxFlush
    /\ UNCHANGED vars

Next ==
    \/ \E t \in Threads, d \in Dests : Start(t, d, StartPtr(d), StartPid(d))
    \/ \E t \in Threads, d \in Dests : StartFull(t, d, StartPid(d))
    \/ \E t \in Threads, k \in Skew : Finish(t, k)
    \/ \E t \in Threads, k \in Skew : FinishRefused(t, k)
    \/ \E n \in 0..Q : Flush(n)
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ prod \in [Qs -> Bs]
    /\ \A q \in Qs, b \in Bs : cnt[q][b] \in Nat
    /\ gid \in Nat
    /\ \A t \in Threads : held[t] = None \/ held[t].slot \in 0..S
    /\ owner \in [SlotKeys -> Threads \cup {-1}]
    /\ demand \in [Threads -> 0..Demand]
    /\ flushes \in 0..MaxFlush

InBounds == ~oob
NoSharedSlot == ~shared
NoForeignWrite == ~foreign
RouteMatches == ~routeBad

Min(a, b) == IF a < b THEN a ELSE b

\* the claimed slots of each producer bank are exactly the first min(counter, S)
StagedMatches ==
    \A q \in Qs : {s \in 0..(S - 1) : owner[<<q, prod[q], s>>] # -1} = 0..(Min(cnt[q][prod[q]], S) - 1)

=============================================================================
