---------------------------- MODULE MCRNRioWorker ----------------------------
(***************************************************************************)
(* The small instance of RNRioWorker: two receive slices, three banks of   *)
(* two, a process view of four, three datagrams and two staged sends.      *)
(* MCRNRioWorker.cfg is the rule.  Each reversed witness turns off one     *)
(* rule switch and must fail:                                              *)
(* MCRNRioWorkerTwoBanks.cfg is the rule with two banks and four       *)
(* datagrams, the backpressure case.                                       *)
(*   MCRNRioWorkerBrokenSwitchBeforeIdle.cfg  (SwitchOnIdle)               *)
(*                                            FillPhaseOwnedByMain         *)
(*   MCRNRioWorkerBrokenUnboundedDequeue.cfg  (BoundedDequeue)             *)
(*                                            ExactDelivery                *)
(*   MCRNRioWorkerBrokenExitBeforeDrain.cfg   (DrainBeforeExit)            *)
(*                                            FreedOnlyWhenDrained         *)
(*   MCRNRioWorkerBrokenNoWake.cfg            (WakeOnReturn)               *)
(*                                            EveryDatagramConsumed        *)
(*   MCRNRioWorkerBrokenCloseCancelsSends.cfg (DrainSendsBeforeClose)      *)
(*                                            StopSendsFinished            *)
(* MCRNRioWorkerInlineStop.cfg is the rule on create's failure path.       *)
(*   MCRNRioWorkerBrokenUnboundedRetry.cfg    (BoundedStopDrain)           *)
(*                                            StopCompletes                *)
(*   MCRNRioWorkerBrokenOneBatchPerPass.cfg   (DrainLoop and               *)
(*                                            CountStalledOnly)            *)
(*                                            LeakOnlyAfterStalls          *)
(* MCRNRioWorkerInlineStopBatchPerPass.cfg is that witness with            *)
(* CountStalledOnly on: the passes that make progress are not counted and  *)
(* the drain finishes.                                                     *)
(* MCRNRioWorkerReachRefusedPost.cfg must fail                             *)
(* NotReachedRefusedPostAtExit: the worker can exit with a send of the     *)
(* stop burst refused at its post and counted.                             *)
(*   MCRNRioWorkerBrokenUncountedPost.cfg     (CountRefusedPost)           *)
(*                                            StopSendsFinished            *)
(* MCRNRioWorkerReachLateCancelLeak.cfg must fail                          *)
(* NotReachedLateCancelLeak: the inline drain can leak after a close that  *)
(* succeeded when the cancellations arrive late, as the code can.          *)
(* MCRNRioWorkerGarbage.cfg is the rule with datagrams that fail the       *)
(* prefilter; MCRNRioWorkerBrokenBankGarbage.cfg (PrefilterInWorker) must  *)
(* fail NoGarbageBanked.                                                   *)
(* MCRNRioWorkerBrokenTakeBeforeRetire.cfg (RetireFirst) must fail          *)
(* DrainedReturnedAtPost: a pass posts a phase while one the kernel had    *)
(* finished is still held posted.                                          *)
(* README-RNRioWorker.md gives each trace and its check against the code.  *)
(***************************************************************************)
EXTENDS RNRioWorker

\* MCRNRioWorkerBrokenUncountedPost.cfg: a refused send post is lost
\* uncounted; must fail StopSendsFinished.
CountRefusedPostBroken == FALSE

\* MCRNRioWorkerGarbage.cfg: datagrams 2 and 3 fail the prefilter.
\* MCRNRioWorkerBrokenBankGarbage.cfg: and main, not the worker, refuses
\* them; must fail NoGarbageBanked.
MCGarbage == {2, 3}
MCPrefilterOnMain == FALSE

\* MCRNRioWorkerBrokenTakeBeforeRetire.cfg: the old pass order, which takes and
\* posts a phase before it retires completions; must fail
\* DrainedReturnedAtPost.
RetireFirstBroken == FALSE
=============================================================================
