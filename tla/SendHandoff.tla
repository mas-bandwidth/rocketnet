------------------------------ MODULE SendHandoff ------------------------------
(***************************************************************************)
(* The transport-owned send handoff generations of                        *)
(* sdk/rocketnet_send_handoff.h: acquire, publish, reopen and retire.  *)
(*                                                                      *)
(* The producer acquires a free generation, fills its packets, and      *)
(* publishes it onto the tail of the queue.  The worker takes only the  *)
(* front, copies its packets, and retires it.                           *)
(*                                                                      *)
(* When the producer needs storage and every generation is queued,      *)
(* start_packet's slow path reopens the newest queued generation.  The  *)
(* newest is not the front, so the worker has not started it.  reopen   *)
(* removes that generation from the queue and hands it back to the      *)
(* producer, which stages its next packets after the ones already       *)
(* published.  reopen never takes the front.                            *)
(*                                                                      *)
(* A reopened generation keeps the packets it published before the      *)
(* reopen.  The server's finish and abort refuse a packet index below   *)
(* finish_base, the count already published when reopen returned the    *)
(* generation, so a pointer from before that publish cannot rewrite a   *)
(* published packet.  ReopenFront = FALSE revisits taking the front;    *)
(* FinishBase = FALSE revisits letting a stale pointer through.  Both   *)
(* reversed witnesses must make TLC reject the model.                   *)
(*                                                                      *)
(* One generation is filled at a time.  The worker owns one front at a  *)
(* time.  The queue is a sequence in publish order and holds each       *)
(* generation at most once.  Packets are abstract ordinal slots; a      *)
(* packet's bytes are not modelled, only whether a stale finish         *)
(* rewrites one already published.                                      *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS NumBuffers,              \* the generation count, at least two
          MaxPackets,              \* packets one generation may publish
          ReopenNewest,            \* TRUE takes the newest, FALSE the front
          FinishBase               \* TRUE bounds finish at finish_base

ASSUME NumBuffers \in Nat \ {0}
ASSUME MaxPackets \in Nat \ {0}
ASSUME ReopenNewest \in BOOLEAN
ASSUME FinishBase \in BOOLEAN

Gens == 1..NumBuffers

VARIABLES free,                    \* generations the producer may acquire
          queued,                  \* published generations, oldest first
          filling,                 \* the generation the producer holds, 0 none
          inflight,                \* the front the worker took, 0 none
          base,                    \* per generation, its finish_base
          packets,                 \* per generation, packets filled so far
          rewritten                \* a stale finish rewrote a published packet

vars == <<free, queued, filling, inflight, base, packets, rewritten>>

QueuedSet == {queued[i] : i \in 1..Len(queued)}

Init ==
    /\ free = Gens
    /\ queued = <<>>
    /\ filling = 0
    /\ inflight = 0
    /\ base = [g \in Gens |-> 0]
    /\ packets = [g \in Gens |-> 0]
    /\ rewritten = FALSE

\* rocketnet_send_handoff_t::acquire: take a free generation and reset it
Acquire(g) ==
    /\ filling = 0
    /\ g \in free
    /\ free' = free \ {g}
    /\ filling' = g
    /\ base' = [base EXCEPT ![g] = 0]
    /\ packets' = [packets EXCEPT ![g] = 0]
    /\ UNCHANGED <<queued, inflight, rewritten>>

\* rocketnet_send_handoff_t::publish: append the filling generation
Publish ==
    /\ filling # 0
    /\ queued' = Append(queued, filling)
    /\ filling' = 0
    /\ UNCHANGED <<free, inflight, base, packets, rewritten>>

\* a finish after the acquire or reopen: the next packet slot
Finish(g, i) ==
    /\ filling # 0
    /\ g = filling
    /\ i \in 1..MaxPackets
    /\ i = packets[g] + 1
    /\ packets' = [packets EXCEPT ![g] = i]
    /\ UNCHANGED <<free, queued, filling, inflight, base, rewritten>>

\* a finish through a pointer from before the reopen: below finish_base
StaleFinish(g, i) ==
    /\ filling # 0
    /\ g = filling
    /\ i \in 1..MaxPackets
    /\ i <= base[g]
    /\ IF FinishBase
       THEN UNCHANGED <<free, queued, filling, inflight, base, packets, rewritten>>
       ELSE /\ rewritten' = TRUE
            /\ UNCHANGED <<free, queued, filling, inflight, base, packets>>

\* rocketnet_send_handoff_t::reopen: hand the newest queued generation back,
\* never the front.  ReopenNewest = FALSE is the reversed rule that takes the
\* front while it stays published.
Reopen ==
    /\ filling = 0
    /\ \/ /\ ReopenNewest
          /\ Len(queued) >= 2
          /\ LET newest == queued[Len(queued)] IN
             /\ filling' = newest
             /\ queued' = SubSeq(queued, 1, Len(queued) - 1)
             /\ base' = [base EXCEPT ![newest] = packets[newest]]
       \/ /\ ~ReopenNewest
          /\ Len(queued) >= 1
          /\ LET front == Head(queued) IN
             /\ filling' = front
             /\ queued' = queued
             /\ base' = [base EXCEPT ![front] = packets[front]]
    /\ UNCHANGED <<free, inflight, packets, rewritten>>

\* the worker takes the front
TakeFront ==
    /\ inflight = 0
    /\ Len(queued) > 0
    /\ inflight' = Head(queued)
    /\ UNCHANGED <<free, queued, filling, base, packets, rewritten>>

\* rocketnet_send_handoff_t::retire: the worker returns the copied front
Retire ==
    /\ inflight # 0
    /\ Len(queued) > 0
    /\ Head(queued) = inflight
    /\ free' = free \cup {inflight}
    /\ queued' = Tail(queued)
    /\ inflight' = 0
    /\ UNCHANGED <<filling, base, packets, rewritten>>

Next ==
    \/ \E g \in Gens : Acquire(g)
    \/ Publish
    \/ \E g \in Gens, i \in 1..MaxPackets : Finish(g, i)
    \/ \E g \in Gens, i \in 1..MaxPackets : StaleFinish(g, i)
    \/ Reopen
    \/ TakeFront
    \/ Retire

Spec == Init /\ [][Next]_vars

\* the producer publishes, the worker takes the front, and the worker retires
SpecFair == Spec /\ WF_vars(Publish) /\ WF_vars(TakeFront) /\ WF_vars(Retire)

-----------------------------------------------------------------------------

TypeOK ==
    /\ free \subseteq Gens
    /\ queued \in Seq(Gens)
    /\ \A i, j \in 1..Len(queued) : i # j => queued[i] # queued[j]
    /\ filling \in Gens \cup {0}
    /\ inflight \in Gens \cup {0}
    /\ base \in [Gens -> 0..MaxPackets]
    /\ packets \in [Gens -> 0..MaxPackets]
    /\ rewritten \in BOOLEAN
    /\ \A g \in Gens : base[g] <= packets[g]
    /\ free \cup QueuedSet \cup (IF filling = 0 THEN {} ELSE {filling}) = Gens
    /\ free \cap QueuedSet = {}
    /\ inflight = 0 \/ (Len(queued) > 0 /\ inflight = Head(queued))

\* the producer's generation is private: it is neither published nor in flight
ReopenedPrivate ==
    filling = 0 \/ (filling \notin QueuedSet /\ filling # inflight)

\* the front the worker may be draining is never handed back to the producer
FrontNeverReopened ==
    filling = 0 \/ Len(queued) = 0 \/ filling # Head(queued)

\* a packet published before a reopen is never rewritten by a stale finish
PublishedPacketsStable ==
    rewritten = FALSE

\* every generation that reaches the queue is eventually retired
EveryQueuedGenerationCompletes ==
    \A g \in Gens : (g \in QueuedSet) ~> (g \in free)

=============================================================================
