------------------------- MODULE RNGsoQueueBankLayout -------------------------
(***************************************************************************)
(* The gso server's per-queue send staging at the code's layout (layer L2  *)
(* of the gso push; sdk/rocketnet_server_gso_linux.cpp                     *)
(* rocketnet_server_gso_start_packet, _finish_packet, _abort_packet,       *)
(* rocketnet_server_gso_send_slot and the producer turn-over in            *)
(* rocketnet_server_gso_publish_send).                                     *)
(*                                                                         *)
(* Q queues, each with NB banks of S slots of Stride bytes, laid end to    *)
(* end: bank b of queue q at byte address Addr(q, b), so a pointer one     *)
(* byte before a bank is inside its neighbour (the worst case for a slot   *)
(* computed from a pointer).                                               *)
(*                                                                         *)
(* A game thread's start_packet picks the destination's queue (QueueOf,    *)
(* the address-and-port hash the XDP transport uses) and claims the next  *)
(* index of the queue's producer bank with one fetch-add on its 64-bit    *)
(* counter (rocketnet_send_queue_claim, as the io_uring and XDP banks      *)
(* claim): an index at or past S is refused (NULL) and leaves the counter  *)
(* past S until the bank is reset, so every reader takes min(counter, S).  *)
(* A destination d stands for an address the hash puts on queue d % Q     *)
(* (the trace walk picks such addresses); Start and StartFull take the    *)
(* queue's destination and what start_packet returned, so the trace check *)
(* compares both with the model's.                                         *)
(* The packet id is (turn * S + slot) * Q + q, where turn is the queue's   *)
(* producer turn-over count, written only by main while no fill runs: no   *)
(* shared id counter.  finish_packet and abort_packet take the queue from  *)
(* the id (id mod Q) and the slot from the pointer's byte offset past that *)
(* queue's producer bank: refused when the offset is negative, not a whole *)
(* number of strides or past the bank, all tested before the division, so *)
(* a refused pointer writes nothing.  A caller may hand finish a pointer   *)
(* Skew bytes off the one start returned (a contract violation): it must   *)
(* fail closed.                                                            *)
(*                                                                         *)
(* The fetch-add is one step (Start, StartFull).  Only the reversed        *)
(* witness ClaimRule "store" splits a claim in two, Load (read the         *)
(* counter) and Claim (store load + 1), so another thread's claim can fall *)
(* between them.                                                           *)
(*                                                                         *)
(* Flush is send_packets with the game threads joined (the game threads' call  *)
(* contract): each queue that staged anything hands its producer to its   *)
(* worker and takes a free bank as the new producer, its counter reset by  *)
(* the worker when it returned it.  A queue with no free bank keeps its    *)
(* producer (main never waits).  WorkerReturn is the queue's worker giving *)
(* a sent bank back.  The FIFO order of the hand-off is layer L3           *)
(* (RNGsoHandoff over RNBankFifoLayout, per queue); here only ownership    *)
(* matters.                                                                *)
(*                                                                         *)
(* The checked claims (each a flag the step that breaks it sets):          *)
(*   InBounds        no claim hands out a slot outside 0..S-1.             *)
(*   NoSharedSlot    no slot of a producer bank is handed to two claims in *)
(*                   one fill.                                             *)
(*   NoForeignWrite  a finish or abort writes only the slot its own claim  *)
(*                   holds, or nothing.                                    *)
(*   RouteMatches    finish and abort find the queue start chose.          *)
(*   NoWorkerWrite   no claim or finish writes a bank the worker owns.     *)
(*   StagedMatches   the claimed slots of each producer bank are exactly   *)
(*                   0..min(counter, S) - 1, the slots the worker reads.   *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   ClaimCmp   "ge" refuses an index >= S (the code); "gt" hands out S.   *)
(*   ClaimRule  "add" is the one-step fetch-add (the code); "store" reads  *)
(*              the counter and later writes load + 1, so two threads that *)
(*              loaded the same index both get it (NoSharedSlot).          *)
(*   CounterWrap 0 is the code's 64-bit counter, which no fill wraps; k > 0 *)
(*              counts modulo k, so a counter too narrow for the refused   *)
(*              claims wraps back to slot 0 while it is claimed            *)
(*              (NoSharedSlot).                                            *)
(*   SlotRule   "check" tests the offset before dividing (the code);       *)
(*              "divide" divides first with C's truncation toward zero, so *)
(*              an offset of -Stride+1..-1 becomes slot 0 (NoForeignWrite). *)
(*   IdRule     "mul" is (turn * S + slot) * Q + q (the code); "add" is    *)
(*              turn * S + slot + q, whose remainder is not the queue      *)
(*              (RouteMatches).                                            *)
(*   FlushRule  "joined" turns over only with every fill finished and only *)
(*              onto a free bank (the code); "open" turns over while a     *)
(*              claim is held and onto the next bank whatever its owner    *)
(*              (NoWorkerWrite).                                           *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Q,            \* queues (num_queues)
          NB,           \* banks per queue (ROCKETNET_GSO_TX_BANKS)
          S,            \* slots per bank (the transport's send_slots: ROCKETNET_GSO_SEND_QUEUE_SIZE, fewer on the test fixture)
          Stride,       \* bytes per slot (ROCKETNET_FRAME_STRIDE)
          Threads,      \* game threads
          Dests,        \* destinations the fill sends to: d is on queue QueueOf(d)
          Demand,      \* start_packet calls one thread makes in one fill
          MaxFlush,     \* bound on flushes
          SkewSpan,     \* a caller may add -SkewSpan..SkewSpan bytes to the pointer it finishes (Skew)
          ClaimCmp,     \* "ge" (the code) or "gt"
          ClaimRule,    \* "add" (the code) or "store"
          CounterWrap,  \* 0: no wrap (the code's uint64); k > 0: counter modulo k
          SlotRule,     \* "check" (the code) or "divide"
          IdRule,       \* "mul" (the code) or "add"
          FlushRule     \* "joined" (the code) or "open"

ASSUME Q >= 1 /\ NB >= 2 /\ S >= 1 /\ Stride >= 2 /\ Demand >= 1 /\ MaxFlush >= 0
ASSUME SkewSpan \in Nat
ASSUME Dests \subseteq Nat
ASSUME ClaimCmp \in {"ge", "gt"} /\ ClaimRule \in {"add", "store"} /\ CounterWrap \in Nat
ASSUME SlotRule \in {"check", "divide"} /\ IdRule \in {"mul", "add"}
ASSUME FlushRule \in {"joined", "open"}

Qs == 0..(Q - 1)
Bs == 0..(NB - 1)
None == [none |-> TRUE]

\* the destination's queue (rocketnet_server_gso_send_queue): destination d
\* stands for an address whose hash is d % Q
QueueOf(d) == d % Q

\* the byte offsets a caller may add to the pointer it finishes (a cfg cannot
\* write a negative number, so the constant is the span)
Skew == (-SkewSpan)..SkewSpan

\* bank b of queue q starts here; slot s of it at Addr(q, b) + s * Stride
Addr(q, b) == (q * NB + b) * S * Stride

\* C's division: truncation toward zero
Trunc(a, b) == IF a >= 0 THEN a \div b ELSE -((-a) \div b)

\* the slot a pointer names in a bank, or -1 (rocketnet_server_gso_send_slot)
SlotFrom(off) ==
    IF SlotRule = "check"
      THEN IF off < 0 \/ off % Stride # 0 \/ off \div Stride >= S THEN -1 ELSE off \div Stride
      ELSE LET s == Trunc(off, Stride) IN IF s < 0 \/ s >= S THEN -1 ELSE s

Granted(i) == IF ClaimCmp = "ge" THEN i < S ELSE i <= S
PidOf(turn, slot, q) == IF IdRule = "mul" THEN (turn * S + slot) * Q + q ELSE turn * S + slot + q
Bump(c) == IF CounterWrap = 0 THEN c + 1 ELSE (c + 1) % CounterWrap
Min(a, b) == IF a < b THEN a ELSE b

VARIABLES prod,       \* each queue's producer bank
          own,        \* each bank's owner: "prod", "worker" or "free"
          cnt,        \* each bank's claim counter (num_packets)
          turn,       \* each queue's producer turn-overs (the id's high part)
          load,       \* each thread's loaded counter value, its queue and bank, or None
          held,       \* each thread's claim: its queue, bank, slot, pointer and id, or None
          owner,      \* the thread a slot of a producer bank was handed to in this fill, or -1
          demand,     \* start_packet calls each thread made in this fill
          flushes,    \* flushes so far
          oob, shared, foreign, routeBad, workerWrite   \* the flags the broken steps set

vars == <<prod, own, cnt, turn, load, held, owner, demand, flushes, oob, shared, foreign, routeBad, workerWrite>>

SlotKeys == Qs \X Bs \X (0..S)

Init ==
    /\ prod = [q \in Qs |-> 0]
    /\ own = [q \in Qs |-> [b \in Bs |-> IF b = 0 THEN "prod" ELSE "free"]]
    /\ cnt = [q \in Qs |-> [b \in Bs |-> 0]]
    /\ turn = [q \in Qs |-> 0]
    /\ load = [t \in Threads |-> None]
    /\ held = [t \in Threads |-> None]
    /\ owner = [k \in SlotKeys |-> -1]
    /\ demand = [t \in Threads |-> 0]
    /\ flushes = 0
    /\ oob = FALSE /\ shared = FALSE /\ foreign = FALSE /\ routeBad = FALSE /\ workerWrite = FALSE

\* what start_packet(to = d) returns: the pointer and id of the slot the
\* fetch-add hands out, or None when the index it took is refused
StartResult(d) ==
    LET q == QueueOf(d)
        b == prod[q]
        i == cnt[q][b]
    IN IF Granted(i)
         THEN [ptr |-> Addr(q, b) + i * Stride, pid |-> PidOf(turn[q], i, q)]
         ELSE None

\* the slot i of queue q's producer bank b handed to thread t, and the flags
\* a slot outside the bank, a slot already handed out this fill or a bank the
\* worker owns sets
Grant(t, q, b, i) ==
    /\ held' = [held EXCEPT ![t] = [q |-> q, b |-> b, slot |-> i,
                                    ptr |-> Addr(q, b) + i * Stride,
                                    pid |-> PidOf(turn[q], i, q)]]
    /\ owner' = [owner EXCEPT ![<<q, b, i>>] = t]
    /\ oob' = (oob \/ i >= S)
    /\ shared' = (shared \/ owner[<<q, b, i>>] # -1)
    /\ workerWrite' = (workerWrite \/ own[q][b] # "prod")

\* start_packet(to = d) on thread t (the code, ClaimRule "add"): one
\* fetch-add on the counter of the destination queue's producer bank,
\* returning r. The counter moves whether or not the index it took is
\* granted: a refused start leaves it past S
Start(t, d, r) ==
    /\ ClaimRule = "add"
    /\ held[t] = None /\ load[t] = None
    /\ demand[t] < Demand
    /\ r = StartResult(d)
    /\ LET q == QueueOf(d)
           b == prod[q]
           i == cnt[q][b]
       IN /\ cnt' = [cnt EXCEPT ![q][b] = Bump(i)]
          /\ IF Granted(i)
               THEN Grant(t, q, b, i)
               ELSE UNCHANGED <<held, owner, oob, shared, workerWrite>>
    /\ demand' = [demand EXCEPT ![t] = @ + 1]
    /\ UNCHANGED <<prod, own, turn, load, flushes, foreign, routeBad>>

\* the reversed witness ClaimRule "store", first half: read the producer and
\* its counter
Load(t, d, q) ==
    /\ ClaimRule = "store"
    /\ held[t] = None /\ load[t] = None
    /\ demand[t] < Demand
    /\ q = QueueOf(d)
    /\ load' = [load EXCEPT ![t] = [q |-> q, b |-> prod[q], i |-> cnt[q][prod[q]]]]
    /\ demand' = [demand EXCEPT ![t] = @ + 1]
    /\ UNCHANGED <<prod, own, cnt, turn, held, owner, flushes, oob, shared, foreign, routeBad, workerWrite>>

\* what the second half gives the thread back: the pointer and id of the
\* slot it loaded, or None when the bound refuses it
ClaimResult(t) ==
    LET l == load[t]
    IN IF l = None \/ ~Granted(l.i)
         THEN None
         ELSE [ptr |-> Addr(l.q, l.b) + l.i * Stride, pid |-> PidOf(turn[l.q], l.i, l.q)]

\* second half: store load + 1 whatever the counter now holds, returning r
Claim(t, r) ==
    /\ load[t] # None
    /\ r = ClaimResult(t)
    /\ LET l == load[t]
           q == l.q
           b == l.b
           i == l.i
       IN IF ~Granted(i)
            THEN UNCHANGED <<cnt, held, owner, oob, shared, workerWrite>>
          ELSE /\ cnt' = [cnt EXCEPT ![q][b] = Bump(i)]
               /\ Grant(t, q, b, i)
    /\ load' = [load EXCEPT ![t] = None]
    /\ UNCHANGED <<prod, own, turn, demand, flushes, foreign, routeBad>>

\* finish_packet or abort_packet on thread t, with the pointer k bytes off
\* the one start returned (k = 0 is the contract)
Finish(t, k) ==
    /\ held[t] # None
    /\ LET h == held[t]
           qd == h.pid % Q
           b == prod[qd]
           s == SlotFrom(h.ptr + k - Addr(qd, b))
       IN /\ held' = [held EXCEPT ![t] = None]
          /\ routeBad' = (routeBad \/ qd # h.q)
          /\ foreign' = (foreign \/ (s # -1 /\ owner[<<qd, b, s>>] # t)
                                 \/ (s # -1 /\ s # h.slot))
          /\ workerWrite' = (workerWrite \/ (s # -1 /\ own[qd][b] # "prod"))
    /\ UNCHANGED <<prod, own, cnt, turn, load, owner, demand, flushes, oob, shared>>

FreeBank(q) == IF FlushRule = "joined"
                 THEN {b \in Bs : own[q][b] = "free"}
                 ELSE {(prod[q] + 1) % NB}

\* send_packets: every queue that staged hands its producer to its worker and
\* takes a free bank; a queue with none keeps its producer
Flush ==
    /\ FlushRule = "open" \/ \A t \in Threads : held[t] = None /\ load[t] = None
    /\ flushes < MaxFlush
    /\ \E pick \in [Qs -> Bs \cup {-1}] :
          /\ \A q \in Qs : IF cnt[q][prod[q]] # 0 /\ FreeBank(q) # {}
                             THEN pick[q] \in FreeBank(q)
                             ELSE pick[q] = -1
          /\ prod' = [q \in Qs |-> IF pick[q] = -1 THEN prod[q] ELSE pick[q]]
          /\ own' = [q \in Qs |-> [b \in Bs |->
                        IF pick[q] = -1 THEN own[q][b]
                        ELSE IF b = pick[q] THEN "prod"
                        ELSE IF b = prod[q] THEN "worker"
                        ELSE own[q][b]]]
          /\ turn' = [q \in Qs |-> IF pick[q] = -1 THEN turn[q] ELSE turn[q] + 1]
          /\ cnt' = [q \in Qs |-> [b \in Bs |->
                        IF FlushRule = "open" /\ pick[q] # -1 /\ b = pick[q] THEN 0 ELSE cnt[q][b]]]
          /\ owner' = [k \in SlotKeys |-> IF pick[k[1]] = -1 THEN owner[k] ELSE -1]
    /\ demand' = [t \in Threads |-> 0]
    /\ flushes' = flushes + 1
    /\ UNCHANGED <<load, held, oob, shared, foreign, routeBad, workerWrite>>

\* the queue's worker sent a bank and gives it back, its counter reset
WorkerReturn(q, b) ==
    /\ own[q][b] = "worker"
    /\ own' = [own EXCEPT ![q][b] = "free"]
    /\ cnt' = [cnt EXCEPT ![q][b] = 0]
    /\ UNCHANGED <<prod, turn, load, held, owner, demand, flushes, oob, shared, foreign, routeBad, workerWrite>>

\* the bound is reached and every claim finished: the run ends here
Done ==
    /\ \A t \in Threads : held[t] = None /\ load[t] = None
    /\ \A t \in Threads : demand[t] = Demand
    /\ flushes = MaxFlush
    /\ UNCHANGED vars

Next ==
    \/ \E t \in Threads, d \in Dests : Start(t, d, StartResult(d))
    \/ \E t \in Threads, d \in Dests : Load(t, d, QueueOf(d))
    \/ \E t \in Threads : Claim(t, ClaimResult(t))
    \/ \E t \in Threads, k \in Skew : Finish(t, k)
    \/ Flush
    \/ \E q \in Qs, b \in Bs : WorkerReturn(q, b)
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

\* the most a counter reaches: every start of every fill a producer is kept
\* through lands on it (a refused start still moves it)
CntMax == (MaxFlush + 1) * Cardinality(Threads) * Demand

TypeOK ==
    /\ prod \in [Qs -> Bs]
    /\ \A q \in Qs, b \in Bs : own[q][b] \in {"prod", "worker", "free"} /\ cnt[q][b] \in 0..CntMax
    /\ \A q \in Qs : turn[q] \in 0..MaxFlush
    /\ \A t \in Threads : held[t] = None \/ held[t].slot \in 0..S
    /\ owner \in [SlotKeys -> Threads \cup {-1}]
    /\ demand \in [Threads -> 0..Demand]
    /\ flushes \in 0..MaxFlush

\* one producer per queue, and it is the bank prod names
OneProducer ==
    \A q \in Qs : {b \in Bs : own[q][b] = "prod"} = {prod[q]}

InBounds == ~oob
NoSharedSlot == ~shared
NoForeignWrite == ~foreign
RouteMatches == ~routeBad
NoWorkerWrite == ~workerWrite

\* the claimed slots of each producer bank are exactly the first
\* min(counter, S) ones (the worker reads min(num_packets, send_slots)
\* slots), once every claim in flight landed
StagedMatches ==
    (\A t \in Threads : load[t] = None) =>
        \A q \in Qs : {s \in 0..(S - 1) : owner[<<q, prod[q], s>>] # -1} = 0..(Min(cnt[q][prod[q]], S) - 1)

=============================================================================
