--------------------------- MODULE MCRNClientLadder ---------------------------
(***************************************************************************)
(* The small instances of RNClientLadder: three candidates (the default    *)
(* ladder) and one (a pin), every token shape and every outcome of every   *)
(* socket.  MCRNClientLadder.cfg and MCRNClientLadderPin.cfg are the       *)
(* passing controls.  MCRNClientLadderBrokenNoFallback.cfg,                *)
(* MCRNClientLadderBrokenOomFalls.cfg and                                   *)
(* MCRNClientLadderBrokenIgnoresToken.cfg are the reversed witnesses.      *)
(* README-RNClientLadder.md has the traces TLC printed.                     *)
(***************************************************************************)
EXTENDS RNClientLadder
=============================================================================
