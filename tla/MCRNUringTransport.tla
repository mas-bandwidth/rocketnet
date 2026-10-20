---------------------------- MODULE MCRNUringTransport ----------------------------
(***************************************************************************)
(* The small instance of RNUringTransport: two send slots, a ring of two     *)
(* provided buffers, two datagrams, two bank swaps and a budget of one   *)
(* CQE per call.                                                         *)
(* MCRNUringTransport.cfg is the rule, checked with the invariants and the  *)
(* two liveness properties under SpecFair.  The reversed witnesses turn  *)
(* off one rule switch each and must fail:                               *)
(* MCRNUringTransportBrokenResetBeforeReap.cfg (ResetIndex right after      *)
(* Submit) fails NoReuseWhileKernelOwned, and                            *)
(* MCRNUringTransportBrokenOneBank.cfg (Reap writes receive CQEs into the   *)
(* exposed bank) fails ExposedStable, and                                *)
(* MCRNUringTransportBrokenUnboundedReap.cfg (a call reaps until the cq is  *)
(* empty) fails ReapWithinBudget.  README-RNUringTransport.md gives the     *)
(* traces and the check of each against THE DESIGN by hand.              *)
(***************************************************************************)
EXTENDS RNUringTransport
=============================================================================
