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
(* A fill thread's start_packet picks the destination's queue (QueueOf,    *)
(* the address-and-port hash the XDP transport uses) and claims the next  *)
(* index of the queue's producer bank with a compare-and-swap that never   *)
(* moves the counter past S: an index at S is refused (NULL, counted).     *)
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
(* A claim is two steps, Load (read the counter) and Claim (the CAS), so   *)
(* another thread's claim can fall between them; the CAS then fails and    *)
(* the thread loads again.                                                 *)
(*                                                                         *)
(* Flush is send_packets with the fill threads joined (the fill phase      *)
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
(*                   0..counter - 1, the slots the worker reads.           *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   ClaimCmp   "ge" refuses an index >= S (the code); "gt" hands out S.   *)
(*   ClaimRule  "cas" (the code); "store" writes load + 1 without the      *)
(*              compare, so two threads that loaded the same index both    *)
(*              get it (NoSharedSlot).                                     *)
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
          S,            \* slots per bank (ROCKETNET_GSO_SEND_QUEUE_SIZE)
          Stride,       \* bytes per slot (ROCKETNET_FRAME_STRIDE)
          Threads,      \* fill threads
          Dests,        \* destinations the fill sends to
          QueueOf(_),   \* the destination's queue (rocketnet_server_gso_send_queue)
          Demand,       \* start_packet calls one thread makes in one fill
          MaxFlush,     \* bound on flushes
          Skew,         \* byte offsets a caller may add to the pointer it finishes
          ClaimCmp,     \* "ge" (the code) or "gt"
          ClaimRule,    \* "cas" (the code) or "store"
          SlotRule,     \* "check" (the code) or "divide"
          IdRule,       \* "mul" (the code) or "add"
          FlushRule     \* "joined" (the code) or "open"

ASSUME Q >= 1 /\ NB >= 2 /\ S >= 1 /\ Stride >= 2 /\ Demand >= 1 /\ MaxFlush >= 0
ASSUME Skew \subseteq Int /\ 0 \in Skew
ASSUME \A d \in Dests : QueueOf(d) \in 0..(Q - 1)
ASSUME ClaimCmp \in {"ge", "gt"} /\ ClaimRule \in {"cas", "store"}
ASSUME SlotRule \in {"check", "divide"} /\ IdRule \in {"mul", "add"}
ASSUME FlushRule \in {"joined", "open"}

Qs == 0..(Q - 1)
Bs == 0..(NB - 1)
None == [none |-> TRUE]

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

\* start_packet(to = d) on thread t, first half: pick the queue, read the
\* producer and its counter
Load(t, d) ==
    /\ held[t] = None /\ load[t] = None
    /\ demand[t] < Demand
    /\ LET q == QueueOf(d) IN
       load' = [load EXCEPT ![t] = [q |-> q, b |-> prod[q], i |-> cnt[q][prod[q]]]]
    /\ demand' = [demand EXCEPT ![t] = @ + 1]
    /\ UNCHANGED <<prod, own, cnt, turn, held, owner, flushes, oob, shared, foreign, routeBad, workerWrite>>

\* second half: the CAS (or, under "store", a blind store). A loaded index
\* the bound refuses is a NULL start; a CAS that lost the race reloads
Claim(t) ==
    /\ load[t] # None
    /\ LET l == load[t]
           q == l.q
           b == l.b
           i == l.i
       IN IF ~Granted(i)
            THEN /\ load' = [load EXCEPT ![t] = None]
                 /\ UNCHANGED <<cnt, held, owner, oob, shared, workerWrite>>
          ELSE IF ClaimRule = "cas" /\ cnt[q][b] # i
            THEN /\ load' = [load EXCEPT ![t] = [q |-> q, b |-> prod[q], i |-> cnt[q][prod[q]]]]
                 /\ UNCHANGED <<cnt, held, owner, oob, shared, workerWrite>>
          ELSE /\ cnt' = [cnt EXCEPT ![q][b] = i + 1]
               /\ load' = [load EXCEPT ![t] = None]
               /\ held' = [held EXCEPT ![t] = [q |-> q, b |-> b, slot |-> i,
                                               ptr |-> Addr(q, b) + i * Stride,
                                               pid |-> PidOf(turn[q], i, q)]]
               /\ owner' = [owner EXCEPT ![<<q, b, i>>] = t]
               /\ oob' = (oob \/ i >= S)
               /\ shared' = (shared \/ owner[<<q, b, i>>] # -1)
               /\ workerWrite' = (workerWrite \/ own[q][b] # "prod")
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
    \/ \E t \in Threads, d \in Dests : Load(t, d)
    \/ \E t \in Threads : Claim(t)
    \/ \E t \in Threads, k \in Skew : Finish(t, k)
    \/ Flush
    \/ \E q \in Qs, b \in Bs : WorkerReturn(q, b)
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ prod \in [Qs -> Bs]
    /\ \A q \in Qs, b \in Bs : own[q][b] \in {"prod", "worker", "free"} /\ cnt[q][b] \in 0..(S + 1)
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

\* the claimed slots of each producer bank are exactly the first counter ones
\* (the worker reads num_packets slots), once every claim in flight landed
StagedMatches ==
    (\A t \in Threads : load[t] = None) =>
        \A q \in Qs : {s \in 0..(S - 1) : owner[<<q, prod[q], s>>] # -1} = 0..(cnt[q][prod[q]] - 1)

=============================================================================
