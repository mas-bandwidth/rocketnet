------------------------ MODULE MCRNUringQueueSend ------------------------
(***************************************************************************)
(* The small instance of RNUringQueueSend: three banks of two slots, two   *)
(* destinations, runs of up to two slots, three claims a fill (so a bank   *)
(* refuses one), three flushes (so every bank is reused) and one eagain.   *)
(* MCRNUringQueueSend.cfg (safety) and MCRNUringQueueSendLive.cfg (Drains) *)
(* are the passing controls; each MCRNUringQueueSendBroken*.cfg must       *)
(* violate the property it names (README-RNUringQueueSend.md).             *)
(***************************************************************************)
EXTENDS RNUringQueueSend

MCSpec == Spec
MCLiveSpec == LiveSpec

\* the safety quotient: every variable but pids. pids is the server's id
\* counter, a trace argument: no guard but Stage's and StageFull's pid = pids
\* reads it, and it is no invariant's but TypeOK's pids \in Nat, so two
\* states that differ only in it have the same futures and the same verdicts
\* (it counts aborted and refused starts, which the control's other
\* variables forget, and without the view the control does not finish in
\* the 110 s budget)
MCSafetyView == <<producer, ready, worker, free, num, st, dest, pkt, runs, cq, pend,
                  status, nextId, eagains, demand, flushes, kernelWrite>>

=============================================================================
