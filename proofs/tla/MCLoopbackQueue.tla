------------------------- MODULE MCLoopbackQueue -------------------------
(***************************************************************************)
(* The small instance of LoopbackQueue: Capacity 4, a bound of 8 sent      *)
(* datagrams and a deepest backlog of 2.  MCLoopbackQueue.cfg is the       *)
(* committed run with RestartAtEmpty = TRUE;                              *)
(* MCLoopbackQueueBrokenNoRestart.cfg flips it to FALSE and must violate   *)
(* TouchedWithinBacklog, the reversed witness.                            *)
(***************************************************************************)
EXTENDS LoopbackQueue

MCSpec == Spec

=============================================================================
