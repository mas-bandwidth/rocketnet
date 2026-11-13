------------------------- MODULE MCRNIocpEndpoints --------------------------
(***************************************************************************)
(* The small instances of RNIocpEndpoints.  MCRNIocpEndpoints.cfg: three   *)
(* endpoints of two receives, every rule on, the invariants and            *)
(* EveryWorkerExits.  Reversed witnesses:                                  *)
(* MCRNIocpEndpointsBrokenRepostOnPoster.cfg (a repost on the posting      *)
(* worker's socket, OnOwnerSocket),                                        *)
(* MCRNIocpEndpointsBrokenCancelSocketZero.cfg (every worker cancels on    *)
(* the first socket, so endpoint 1's receives are never cancelled and its  *)
(* worker never exits, EveryWorkerExits) and                              *)
(* MCRNIocpEndpointsBrokenCheckBeforeCount.cfg (a post tests stopping     *)
(* before it counts itself pending, so an owner exits before another      *)
(* worker's retry of its receive lands on its port, ExitedOnlyDrained).    *)
(***************************************************************************)
EXTENDS RNIocpEndpoints
=============================================================================
