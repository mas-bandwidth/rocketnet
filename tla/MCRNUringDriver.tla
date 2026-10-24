---------------------------- MODULE MCRNUringDriver ----------------------------
(***************************************************************************)
(* The small instance of RNUringDriver: two send slots, a ring of two     *)
(* provided buffers, two datagrams, two bank swaps and a budget of one   *)
(* CQE per call.                                                         *)
(* MCRNUringDriver.cfg is the rule, checked with the invariants and the  *)
(* two liveness properties under SpecFair.  The reversed witnesses turn  *)
(* off one rule switch each and must fail:                               *)
(* MCRNUringDriverBrokenResetBeforeReap.cfg (ResetIndex right after      *)
(* Submit) fails NoReuseWhileKernelOwned, and                            *)
(* MCRNUringDriverBrokenOneBank.cfg (Reap writes receive CQEs into the   *)
(* exposed bank) fails ExposedStable, and                                *)
(* MCRNUringDriverBrokenUnboundedReap.cfg (a call reaps until the cq is  *)
(* empty) fails ReapWithinBudget, and                                    *)
(* MCRNUringDriverBrokenFreeRunAtSubmit.cfg (a segmented run's slots     *)
(* after its head handed back at Submit) fails NoReuseWhileKernelOwned.  *)
(* MCRNUringDriverBrokenReturnBeforeSplit.cfg (ReturnBuffer once the    *)
(* first segment of a coalesced buffer is copied, ReturnAfterLastCopy    *)
(* replaced with MCReturnAfterFirstCopy) fails BufferReturnedAfterCopy.  *)
(* Submit may send both slots as one run, and one receive buffer carries  *)
(* up to Segments = 2 datagrams (UDP_GRO).  README-RNUringDriver.md gives *)
(* the traces and the check of each against the code by hand.          *)
(***************************************************************************)
EXTENDS RNUringDriver

\* the reversed witness's replacement for the rule switch
\* ReturnAfterLastCopy (MCRNUringDriverBrokenReturnBeforeSplit.cfg)
MCReturnAfterFirstCopy == FALSE
=============================================================================
