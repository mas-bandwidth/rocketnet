--------------------------- MODULE MCRNUpdateClock ---------------------------
(***************************************************************************)
(* The small instance that checks RNUpdateClock: the caller's time in     *)
(* 0..6 from a seed of 3, steps of up to 2 either way, five updates.      *)
(*                                                                         *)
(* MCRNUpdateClock.cfg is the rule and passes every invariant.            *)
(* MCRNUpdateClockBrokenStall.cfg sets BrokenStall = TRUE (the clamp that *)
(* holds the last good time until the caller's clock passes it again) and *)
(* must fail on ResumesExactly.  README-RNUpdateClock.md gives the trace, *)
(* replayed against sdk/rocketnet_client_transport.cpp by hand.           *)
(***************************************************************************)
EXTENDS RNUpdateClock

MCSpec == Spec
=============================================================================
