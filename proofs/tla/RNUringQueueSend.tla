---------------------------- MODULE RNUringQueueSend ----------------------------
(***************************************************************************)
(* One queue of the io_uring server's per-queue send (layer L1): the bank  *)
(* handoff between the fill, main's send_packets and the queue's worker,   *)
(* and the kernel's ownership of a submitted run until its completion.     *)
(* The queues share nothing but main's loop, so one queue is the machine;  *)
(* RNUringQueueBankLayout checks the claim and slot arithmetic across      *)
(* queues and RNBankFifoLayout the exchange's index arrays.                *)
(*                                                                         *)
(* NB banks.  The fill stages into the producer bank: a claim takes the    *)
(* next of its S slots or is refused when the bank is full; a claimed slot *)
(* is finished (staged, with its destination and a packet id) or aborted.  *)
(* Publish is send_packets with the fill joined: a producer that staged    *)
(* anything goes to the back of the ready FIFO and the producer becomes    *)
(* the head of the free queue, or, with no free bank, the producer stays   *)
(* and keeps its slots for the next flush.  The worker takes the oldest    *)
(* ready bank, submits its staged slots as runs (each a set of staged      *)
(* slots to one destination, the earliest of that destination first, at   *)
(* most MaxSeg of them, one SENDMSG and one completion), and reaps         *)
(* completions in the order the kernel posts them: "ok" counts the run's   *)
(* packets sent, "err" counts them dropped, "eagain" (the socket buffer    *)
(* full under MSG_DONTWAIT) hands the run's slots back to be submitted     *)
(* again.  The worker retires its bank to the back of the free queue only  *)
(* when no slot of it is staged or in a run and none of its runs is        *)
(* pending.  The kernel completes a submitted run whenever it likes; it    *)
(* answers "eagain" at most MaxEagain times in all (a socket that stays    *)
(* full is the stop's bounded retry, layer L3).                            *)
(*                                                                         *)
(* A segmented run its destination refuses is resent plain by the reap     *)
(* (RNSegmentRefusal, RNUringSendPhases ReapRefused); here that is an      *)
(* "eagain" that hands the slots back, the same ownership step.            *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   NoWriteWhileKernelOwned  the fill never writes a slot of a run the    *)
(*                  kernel has not completed and the worker not reaped.    *)
(*   PendingMatches each bank's pending count is its runs in the kernel or *)
(*                  in the completion queue.                               *)
(*   BanksPartition every bank is in exactly one place: the producer, the  *)
(*                  ready FIFO, the worker or the free queue.              *)
(*   Accounted      every finished packet is staged, in the kernel, sent   *)
(*                  or dropped; none is lost without a count.              *)
(*   Drains         (liveness) once the flushes stop, every published bank *)
(*                  is sent and back in the free queue.                    *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   RetireRule  "cqe" retires a bank once its runs are reaped (the        *)
(*               design); "submitted" once every slot is submitted, runs   *)
(*               still in the kernel (NoWriteWhileKernelOwned).            *)
(*   EagainRule  "retry" hands an eagain run's slots back (the design);    *)
(*               "lost" marks them done with no count (Accounted);         *)
(*               "stuck" hands them back but leaves the run pending        *)
(*               (Drains).                                                 *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS NB,         \* banks per queue (ROCKETNET_URING_SEND_BANKS)
          S,          \* slots per bank (ROCKETNET_URING_SEND_QUEUE_SIZE)
          Dests,      \* destinations
          MaxSeg,     \* slots one run may carry (ROCKETNET_URING_MAX_SEGMENTS)
          Demand,     \* claims one fill makes
          MaxFlush,   \* bound on flushes
          MaxEagain,  \* bound on eagain completions
          RetireRule, \* "cqe" (the design) or "submitted"
          EagainRule  \* "retry" (the design), "lost" or "stuck"

ASSUME NB >= 3 /\ S >= 1 /\ MaxSeg >= 1 /\ Demand >= 1 /\ MaxFlush \in Nat /\ MaxEagain \in Nat
ASSUME RetireRule \in {"cqe", "submitted"} /\ EagainRule \in {"retry", "lost", "stuck"}

Bs == 0..(NB - 1)
Sl == 0..(S - 1)
NoBank == -1
Results == {"ok", "err", "eagain"}

Range(q) == {q[i] : i \in 1..Len(q)}

VARIABLES producer,   \* the bank the fill stages into
          ready,      \* published banks, oldest first
          worker,     \* the bank the worker holds, or NoBank
          free,       \* free banks, oldest first
          num,        \* each bank's claim counter
          st,         \* each slot: "empty", "claimed", "staged", "aborted", "run" or "done"
          dest,       \* each slot's destination (0 when empty)
          pkt,        \* each slot's packet id (0 when empty)
          runs,       \* runs submitted, completion not yet posted: [b, slots, ids]
          cq,         \* posted completions, oldest first: [run, res]
          pend,       \* each bank's runs pending (submitted, not reaped)
          status,     \* each packet id: "staged", "kernel", "sent", "dropped" or "lost"
          nextId,     \* the next packet id
          eagains,    \* eagain completions so far
          demand,     \* claims in this fill
          flushes,    \* flushes so far
          kernelWrite \* the fill wrote a kernel-owned slot

vars == <<producer, ready, worker, free, num, st, dest, pkt, runs, cq, pend,
          status, nextId, eagains, demand, flushes, kernelWrite>>

Ids == 1..(MaxFlush + 1) * Demand

\* the slots the kernel owns: every slot of a run not yet reaped
Kern == UNION {{<<r.b, s>> : s \in r.slots} : r \in runs \cup {c.run : c \in Range(cq)}}

Init ==
    /\ producer = 0
    /\ ready = <<>>
    /\ worker = NoBank
    /\ free = [i \in 1..(NB - 1) |-> i]
    /\ num = [b \in Bs |-> 0]
    /\ st = [b \in Bs |-> [s \in Sl |-> "empty"]]
    /\ dest = [b \in Bs |-> [s \in Sl |-> 0]]
    /\ pkt = [b \in Bs |-> [s \in Sl |-> 0]]
    /\ runs = {}
    /\ cq = <<>>
    /\ pend = [b \in Bs |-> 0]
    /\ status = [i \in Ids |-> "none"]
    /\ nextId = 1
    /\ eagains = 0
    /\ demand = 0
    /\ flushes = 0
    /\ kernelWrite = FALSE

\* start_packet then finish_packet (staged) or abort_packet (aborted) on
\* one slot of the producer; a claim past the bank is refused and stages
\* nothing
Stage(d, finished) ==
    /\ demand < Demand
    /\ demand' = demand + 1
    /\ LET b == producer
           s == num[b]
       IN IF s < S
            THEN /\ num' = [num EXCEPT ![b] = @ + 1]
                 /\ kernelWrite' = (kernelWrite \/ <<b, s>> \in Kern)
                 /\ IF finished
                      THEN /\ st' = [st EXCEPT ![b][s] = "staged"]
                           /\ dest' = [dest EXCEPT ![b][s] = d]
                           /\ pkt' = [pkt EXCEPT ![b][s] = nextId]
                           /\ status' = [status EXCEPT ![nextId] = "staged"]
                           /\ nextId' = nextId + 1
                      ELSE /\ st' = [st EXCEPT ![b][s] = "aborted"]
                           /\ UNCHANGED <<dest, pkt, status, nextId>>
            ELSE UNCHANGED <<num, st, dest, pkt, status, nextId, kernelWrite>>
    /\ UNCHANGED <<producer, ready, worker, free, runs, cq, pend, eagains, flushes>>

\* send_packets with the fill joined: publish the producer if it staged
\* anything and a free bank can replace it; otherwise keep it
Publish ==
    /\ flushes < MaxFlush
    /\ flushes' = flushes + 1
    /\ demand' = 0
    /\ IF num[producer] # 0 /\ free # <<>>
         THEN /\ ready' = Append(ready, producer)
              /\ producer' = Head(free)
              /\ free' = Tail(free)
              /\ num' = [num EXCEPT ![Head(free)] = 0]
         ELSE UNCHANGED <<ready, producer, free, num>>
    /\ UNCHANGED <<worker, st, dest, pkt, runs, cq, pend, status, nextId, eagains, kernelWrite>>

\* the worker takes the oldest ready bank
WorkerClaim ==
    /\ worker = NoBank
    /\ ready # <<>>
    /\ worker' = Head(ready)
    /\ ready' = Tail(ready)
    /\ UNCHANGED <<producer, free, num, st, dest, pkt, runs, cq, pend, status, nextId, eagains, demand, flushes, kernelWrite>>

\* the staged slots of destination d in the worker's bank
StagedOf(d) == {s \in Sl : st[worker][s] = "staged" /\ dest[worker][s] = d}

\* one run: the earliest k staged slots of one destination, one SENDMSG
WorkerSubmit(d, k) ==
    /\ worker # NoBank
    /\ k \in 1..MaxSeg
    /\ Cardinality(StagedOf(d)) >= k
    /\ LET R == {s \in StagedOf(d) : Cardinality({u \in StagedOf(d) : u < s}) < k}
           b == worker
       IN /\ st' = [st EXCEPT ![b] = [s \in Sl |-> IF s \in R THEN "run" ELSE st[b][s]]]
          /\ runs' = runs \cup {[b |-> b, slots |-> R, ids |-> {pkt[b][s] : s \in R}]}
          /\ pend' = [pend EXCEPT ![b] = @ + 1]
          /\ status' = [i \in Ids |-> IF \E s \in R : pkt[b][s] = i THEN "kernel" ELSE status[i]]
    /\ UNCHANGED <<producer, ready, worker, free, num, dest, pkt, cq, nextId, eagains, demand, flushes, kernelWrite>>

\* the kernel posts a run's completion (environment)
KernelComplete(r, res) ==
    /\ r \in runs
    /\ res = "eagain" => eagains < MaxEagain
    /\ runs' = runs \ {r}
    /\ cq' = Append(cq, [run |-> r, res |-> res])
    /\ eagains' = IF res = "eagain" THEN eagains + 1 ELSE eagains
    /\ UNCHANGED <<producer, ready, worker, free, num, st, dest, pkt, pend, status, nextId, demand, flushes, kernelWrite>>

SetSt(b, R, v) == [st EXCEPT ![b] = [s \in Sl |-> IF s \in R THEN v ELSE st[b][s]]]
SetStatus(I, v) == [i \in Ids |-> IF i \in I THEN v ELSE status[i]]

\* the worker reaps the oldest completion
WorkerReap ==
    /\ cq # <<>>
    /\ LET c == Head(cq)
           r == c.run
       IN /\ cq' = Tail(cq)
          /\ CASE c.res = "ok" ->
                    /\ st' = SetSt(r.b, r.slots, "done")
                    /\ status' = SetStatus(r.ids, "sent")
                    /\ pend' = [pend EXCEPT ![r.b] = @ - 1]
               [] c.res = "err" ->
                    /\ st' = SetSt(r.b, r.slots, "done")
                    /\ status' = SetStatus(r.ids, "dropped")
                    /\ pend' = [pend EXCEPT ![r.b] = @ - 1]
               [] c.res = "eagain" /\ EagainRule = "retry" ->
                    /\ st' = SetSt(r.b, r.slots, "staged")
                    /\ status' = SetStatus(r.ids, "staged")
                    /\ pend' = [pend EXCEPT ![r.b] = @ - 1]
               [] c.res = "eagain" /\ EagainRule = "lost" ->
                    /\ st' = SetSt(r.b, r.slots, "done")
                    /\ status' = SetStatus(r.ids, "lost")
                    /\ pend' = [pend EXCEPT ![r.b] = @ - 1]
               [] c.res = "eagain" /\ EagainRule = "stuck" ->
                    /\ st' = SetSt(r.b, r.slots, "staged")
                    /\ status' = SetStatus(r.ids, "staged")
                    /\ UNCHANGED pend
    /\ UNCHANGED <<producer, ready, worker, free, num, dest, pkt, runs, nextId, eagains, demand, flushes, kernelWrite>>

\* the worker retires its bank: nothing staged or in a run, and (RetireRule
\* "cqe") no run of it pending
Retirable(b) ==
    /\ \A s \in Sl : st[b][s] \notin {"staged", "run"} \/ (RetireRule = "submitted" /\ st[b][s] = "run")
    /\ (RetireRule = "cqe" => pend[b] = 0)

WorkerRetire ==
    /\ worker # NoBank
    /\ Retirable(worker)
    /\ free' = Append(free, worker)
    /\ worker' = NoBank
    /\ st' = [st EXCEPT ![worker] = [s \in Sl |-> "empty"]]
    /\ dest' = [dest EXCEPT ![worker] = [s \in Sl |-> 0]]
    /\ pkt' = [pkt EXCEPT ![worker] = [s \in Sl |-> 0]]
    /\ UNCHANGED <<producer, ready, num, runs, cq, pend, status, nextId, eagains, demand, flushes, kernelWrite>>

\* the bound on flushes is reached and the worker has nothing left: stutter
Done ==
    /\ flushes = MaxFlush
    /\ demand = Demand
    /\ ready = <<>> /\ worker = NoBank /\ runs = {} /\ cq = <<>>
    /\ UNCHANGED vars

WorkerStep ==
    \/ WorkerClaim
    \/ \E d \in Dests, k \in 1..MaxSeg : WorkerSubmit(d, k)
    \/ WorkerReap
    \/ WorkerRetire

Next ==
    \/ \E d \in Dests, f \in BOOLEAN : Stage(d, f)
    \/ Publish
    \/ WorkerStep
    \/ \E r \in runs, res \in Results : KernelComplete(r, res)
    \/ Done

Spec == Init /\ [][Next]_vars

\* the worker and the kernel keep going; the fill and main may stop
Fairness ==
    /\ WF_vars(WorkerClaim)
    /\ WF_vars(\E d \in Dests, k \in 1..MaxSeg : WorkerSubmit(d, k))
    /\ WF_vars(WorkerReap)
    /\ WF_vars(WorkerRetire)
    /\ WF_vars(\E r \in runs, res \in Results : KernelComplete(r, res))

LiveSpec == Spec /\ Fairness

-----------------------------------------------------------------------------

TypeOK ==
    /\ producer \in Bs
    /\ Range(ready) \subseteq Bs /\ Range(free) \subseteq Bs
    /\ worker \in Bs \cup {NoBank}
    /\ num \in [Bs -> 0..S]
    /\ \A b \in Bs, s \in Sl : st[b][s] \in {"empty", "claimed", "staged", "aborted", "run", "done"}
    /\ pend \in [Bs -> Nat]
    /\ status \in [Ids -> {"none", "staged", "kernel", "sent", "dropped", "lost"}]
    /\ eagains \in 0..MaxEagain
    /\ demand \in 0..Demand
    /\ flushes \in 0..MaxFlush

NoWriteWhileKernelOwned == ~kernelWrite

PendingMatches ==
    \A b \in Bs : pend[b] = Cardinality({r \in runs : r.b = b}) +
                            Cardinality({i \in 1..Len(cq) : cq[i].run.b = b})

BanksPartition ==
    /\ Len(ready) + Len(free) + 1 + (IF worker = NoBank THEN 0 ELSE 1) = NB
    /\ Range(ready) \cup Range(free) \cup {producer} \cup (IF worker = NoBank THEN {} ELSE {worker}) = Bs

Accounted ==
    \A i \in Ids : i < nextId => status[i] \in {"staged", "kernel", "sent", "dropped"}

\* once the flushes stop, every published bank is sent and free again
Drains == <>[](ready = <<>> /\ worker = NoBank /\ runs = {} /\ cq = <<>>)

=============================================================================
