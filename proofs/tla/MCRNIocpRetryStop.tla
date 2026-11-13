-------------------------- MODULE MCRNIocpRetryStop --------------------------
(***************************************************************************)
(* The small instances of RNIocpRetryStop.  MCRNIocpRetryStop.cfg: two    *)
(* receive operations, two workers, two cells, three refused posts and    *)
(* two completions, counters that never wrap, every invariant.            *)
(* MCRNIocpRetryStopLive.cfg: one operation and two workers under the     *)
(* fairness, both liveness properties.  Reversed witnesses:               *)
(* MCRNIocpRetryStopBrokenStopOffByOne.cfg (PassBounded),                  *)
(* MCRNIocpRetryStopBrokenClaimBeforeStop.cfg (the lost retry, RetryOwned),*)
(* MCRNIocpRetryStopBrokenStopWrap.cfg (the stop ticket wraps,             *)
(* PassBounded), MCRNIocpRetryStopBrokenRefuseRetires.cfg (a push a       *)
(* stalled pass refuses retires the receive, RetiredOnlyByStop) and       *)
(* MCRNIocpRetryStopBrokenCheckBeforeCount.cfg (a post tests stopping     *)
(* before it counts itself pending, AdmittedPostHoldsExit).               *)
(***************************************************************************)
EXTENDS RNIocpRetryStop
=============================================================================
