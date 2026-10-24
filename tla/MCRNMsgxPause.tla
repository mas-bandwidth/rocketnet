---------------------------- MODULE MCRNMsgxPause ----------------------------
(***************************************************************************)
(* The small instance of RNMsgxPause: two workers (TX and RX on one       *)
(* socket), time bounded to four ticks, a retry of two ticks.              *)
(*   MCRNMsgxPause.cfg                       the rule; every invariant and *)
(*                                           ExpiredPauseResolved          *)
(*   MCRNMsgxPauseBrokenExpiryClearsFresh.cfg (StoreZero) must fail        *)
(*                                           FreshPauseSurvives            *)
(* README-RNMsgxPause.md gives the trace and its check against the code.  *)
(***************************************************************************)
EXTENDS RNMsgxPause
=============================================================================
