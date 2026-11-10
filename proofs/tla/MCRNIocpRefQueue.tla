-------------------------- MODULE MCRNIocpRefQueue --------------------------
(***************************************************************************)
(* The small instances of RNIocpRefQueue.  MCRNIocpRefQueue.cfg: two       *)
(* cells, two producers, two consumers, five pushes, counters that never  *)
(* wrap; MCRNIocpRefQueueCap4.cfg: four cells, two producers, one          *)
(* consumer, six pushes; MCRNIocpRefQueueStalledPop.cfg: two cells and   *)
(* at most two operations live, the code's sizing, under SpecFair.        *)
(* Reversed witnesses:                                                    *)
(* MCRNIocpRefQueueBrokenCapacityNotPow2.cfg (three cells, NoOverwrite),   *)
(* MCRNIocpRefQueueBrokenInitOffByOne.cfg (NoPhantom),                     *)
(* MCRNIocpRefQueueBrokenPositionWrap.cfg (counters wrapping at 2 * Cap,   *)
(* the ABA, NoOverwrite) and MCRNIocpRefQueueBrokenDropRefused.cfg (a     *)
(* push refused by a stalled pop drops its operation, RefusedNeverLost).  *)
(***************************************************************************)
EXTENDS RNIocpRefQueue
=============================================================================
