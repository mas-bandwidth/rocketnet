---------------------------- MODULE MCRNUringDriver ----------------------------
(***************************************************************************)
(* The small instance of RNUringDriver: two send slots, a ring of two     *)
(* provided buffers, two datagrams and two bank swaps.                 *)
(* MCRNUringDriver.cfg is the rule, checked with the invariants and the  *)
(* two liveness properties under SpecFair.  The reversed witnesses turn  *)
(* off one rule switch each and must fail:                               *)
(* MCRNUringDriverBrokenResetBeforeReap.cfg (ResetIndex right after      *)
(* Submit) fails NoReuseWhileKernelOwned, and                            *)
(* MCRNUringDriverBrokenOneBank.cfg (Reap writes receive CQEs into the   *)
(* exposed bank) fails ExposedStable.  README-RNUringDriver.md gives the *)
(* traces and the check of each against THE DESIGN by hand.              *)
(***************************************************************************)
EXTENDS RNUringDriver
=============================================================================
