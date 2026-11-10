-------------------------- MODULE MCRNIocpRetryStop --------------------------
(***************************************************************************)
(* The small instances of RNIocpRetryStop.  MCRNIocpRetryStop.cfg: two    *)
(* receive operations, two workers, two cells, three refused posts and    *)
(* two completions, counters that never wrap, every invariant.            *)
(* MCRNIocpRetryStopLive.cfg: one operation and two workers under the     *)
(* fairness, both liveness properties.  MCRNIocpRetryStopLiveTwoOps.cfg:  *)
(* the same at two operations, the smallest instance where a refused      *)
(* repost pushes onto a cell another worker's pass still holds and waits. *)
(* Reversed witnesses:                                                    *)
(* MCRNIocpRetryStopBrokenStopOffByOne.cfg (PassBounded),                  *)
(* MCRNIocpRetryStopBrokenClaimBeforeStop.cfg (the lost retry, RetryOwned),*)
(* MCRNIocpRetryStopBrokenStopWrap.cfg (the stop ticket wraps,             *)
(* PassBounded) and MCRNIocpRetryStopBrokenRefuseRetires.cfg (a push a     *)
(* stalled pass refuses retires the receive, RetiredOnlyByStop).           *)
(***************************************************************************)
EXTENDS RNIocpRetryStop
=============================================================================
