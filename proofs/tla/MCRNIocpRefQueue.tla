-------------------------- MODULE MCRNIocpRefQueue --------------------------
(***************************************************************************)
(* The small instances of RNIocpRefQueue.  MCRNIocpRefQueue.cfg: two       *)
(* cells, two producers, two consumers, five pushes, counters that never  *)
(* wrap; MCRNIocpRefQueueCap4.cfg: four cells, two producers, one          *)
(* consumer, six pushes.  Reversed witnesses:                              *)
(* MCRNIocpRefQueueBrokenCapacityNotPow2.cfg (three cells, NoOverwrite),   *)
(* MCRNIocpRefQueueBrokenInitOffByOne.cfg (NoPhantom) and                  *)
(* MCRNIocpRefQueueBrokenPositionWrap.cfg (counters wrapping at 2 * Cap,   *)
(* the ABA, NoOverwrite).                                                  *)
(***************************************************************************)
EXTENDS RNIocpRefQueue
=============================================================================
