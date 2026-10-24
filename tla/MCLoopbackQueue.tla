--------------------------- MODULE MCLoopbackQueue ---------------------------
(***************************************************************************)
(* The small instance that checks LoopbackQueue: a queue of 4 slots and 8  *)
(* send attempts, so the full-queue drop, the wrap of the indices and runs *)
(* of every length from 1 to 4 are all reached.                            *)
(*                                                                         *)
(* MCLoopbackQueue.cfg is the rule (RestartAtEmpty = TRUE) and passes every *)
(* invariant.  MCLoopbackQueueBrokenNoRestart.cfg sets RestartAtEmpty =    *)
(* FALSE (free-running indices, the queue before the restart) and must     *)
(* fail on TouchedWithinBacklog: Send, Receive, Send writes slot 1 with a  *)
(* deepest backlog of 1.                                                   *)
(***************************************************************************)
EXTENDS LoopbackQueue

MCSpec == Spec
=============================================================================
