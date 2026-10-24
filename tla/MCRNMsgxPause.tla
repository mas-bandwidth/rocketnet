--------------------------- MODULE MCRNMsgxPause ----------------------------
(***************************************************************************)
(* The small instance of RNMsgxPause. Two workers, a clock of a few ticks, *)
(* a retry of two. MCRNMsgxPause.cfg is the compare-and-swap.              *)
(* MCRNMsgxPauseBrokenExpiryClearsFresh.cfg stores 0 with no compare and   *)
(* must fail FreshPauseSurvives. README-RNMsgxPause.md is the reading      *)
(* against the code.                                                       *)
(***************************************************************************)
EXTENDS RNMsgxPause
=============================================================================
