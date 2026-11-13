----------------------- MODULE MCRNUringQueueWorker -----------------------
(***************************************************************************)
(* The small instance of RNUringQueueWorker: two banks published, each of  *)
(* at most two runs, two runs in the kernel at once, one segment refusal,  *)
(* two -EAGAIN answers after the stop before the worker abandons, one      *)
(* before it.  MCRNUringQueueWorker.cfg (safety) and                       *)
(* MCRNUringQueueWorkerLive.cfg (StopFinishes, Drains) are the passing     *)
(* controls; each MCRNUringQueueWorkerBroken*.cfg must violate the         *)
(* property it names (README-RNUringQueueWorker.md).                       *)
(***************************************************************************)
EXTENDS RNUringQueueWorker

MCSpec == Spec
MCLiveSpec == LiveSpec

=============================================================================
