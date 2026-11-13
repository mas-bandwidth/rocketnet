----------------------- MODULE MCRNUringQueueWorker -----------------------
(***************************************************************************)
(* The small instance of RNUringQueueWorker: two banks published, each of  *)
(* at most two runs, two runs in the kernel at once, one segment refusal,  *)
(* two waits after shutdown with no run done before the worker abandons,   *)
(* one full answer before the server stops.  MCRNUringQueueWorker.cfg      *)
(* (safety) and MCRNUringQueueWorkerLive.cfg (StopFinishes, Drains,        *)
(* ShutdownDrains) are the passing                                         *)
(* controls; each MCRNUringQueueWorkerBroken*.cfg must violate the         *)
(* property it names (README-RNUringQueueWorker.md).                       *)
(***************************************************************************)
EXTENDS RNUringQueueWorker

MCSpec == Spec
MCLiveSpec == LiveSpec

=============================================================================
