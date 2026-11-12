----------------------- MODULE MCRNUringQueueWorker -----------------------
(***************************************************************************)
(* The small instance of RNUringQueueWorker: two pieces of work handed     *)
(* over, two runs in the kernel at once, two -EAGAIN answers after the     *)
(* stop before the worker abandons, one before it.                         *)
(* MCRNUringQueueWorker.cfg (safety) and MCRNUringQueueWorkerLive.cfg      *)
(* (StopFinishes, Drains) are the passing controls; each                   *)
(* MCRNUringQueueWorkerBroken*.cfg must violate the property it names      *)
(* (README-RNUringQueueWorker.md).                                          *)
(***************************************************************************)
EXTENDS RNUringQueueWorker

MCSpec == Spec
MCLiveSpec == LiveSpec

=============================================================================
