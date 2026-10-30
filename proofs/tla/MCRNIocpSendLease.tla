-------------------------- MODULE MCRNIocpSendLease --------------------------
(***************************************************************************)
(* The small instance of RNIocpSendLease: two send operations, two        *)
(* callers, generations up to 2 (so operations reach GenMax and retire)   *)
(* and eight acquisitions.  MCRNIocpSendLease.cfg is the passing control; *)
(* MCRNIocpSendLeaseBrokenGenWrap.cfg lets the generation wrap and must    *)
(* violate StaleNeverNamesLease.                                           *)
(***************************************************************************)
EXTENDS RNIocpSendLease
=============================================================================
