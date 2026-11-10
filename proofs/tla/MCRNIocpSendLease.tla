-------------------------- MODULE MCRNIocpSendLease --------------------------
(***************************************************************************)
(* The small instances of RNIocpSendLease.  MCRNIocpSendLease.cfg: two     *)
(* send operations and two cells, two callers, one completion worker,      *)
(* generations up to 2 (so operations reach GenMax and retire) and eight   *)
(* acquisitions; MCRNIocpSendLeaseCap4.cfg: four operations and four cells;*)
(* MCRNIocpSendLeaseStalledPop.cfg: the two-cell instance under SpecFair,  *)
(* where a release push refused by a stalled pop retries and completes.    *)
(* Reversed witnesses: MCRNIocpSendLeaseBrokenGenWrap.cfg lets the         *)
(* generation wrap (StaleNeverNamesLease);                                 *)
(* MCRNIocpSendLeaseBrokenRetireRefused.cfg retires the operation of a     *)
(* refused release push (RetiredOnlyAtGenMax).                             *)
(***************************************************************************)
EXTENDS RNIocpSendLease
=============================================================================
