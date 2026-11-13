------------------------- MODULE MCRNIocpEndpoints --------------------------
(***************************************************************************)
(* The small instances of RNIocpEndpoints.  MCRNIocpEndpoints.cfg: three   *)
(* endpoints of two receives, every rule on, the invariants and            *)
(* EveryWorkerExits.  Reversed witnesses:                                  *)
(* MCRNIocpEndpointsBrokenRepostOnPoster.cfg (a repost on the posting      *)
(* worker's socket, OnOwnerSocket) and                                     *)
(* MCRNIocpEndpointsBrokenCancelSocketZero.cfg (every worker cancels on    *)
(* the first socket, so endpoint 1's receives are never cancelled and its  *)
(* worker never exits, EveryWorkerExits).                                  *)
(***************************************************************************)
EXTENDS RNIocpEndpoints
=============================================================================
