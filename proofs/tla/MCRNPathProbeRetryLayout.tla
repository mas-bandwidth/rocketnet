------------------------ MODULE MCRNPathProbeRetryLayout ------------------------
(***************************************************************************)
(* The instances of RNPathProbeRetryLayout.  MCRNPathProbeRetryLayout.cfg  *)
(* is the small scale: gaps of 1 for the first three re-tries, doubling to *)
(* a cap of 5 (1, 2, 3, then 5, 9, 14, then 5 apart), the count saturating *)
(* at 6, re-tries that run up to 7 past their slot, to the time 60.        *)
(* MCRNPathProbeRetryLayoutCode.cfg is the code's own scale, 10, 3 and     *)
(* 300 s, saturating at 8, to 3000 s.  Each                                *)
(* MCRNPathProbeRetryLayoutBroken*.cfg must violate the invariant it names. *)
(***************************************************************************)
EXTENDS RNPathProbeRetryLayout

=============================================================================
