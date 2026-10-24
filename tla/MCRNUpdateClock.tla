--------------------------- MODULE MCRNUpdateClock --------------------------
(***************************************************************************)
(* The instances of RNUpdateClock.  MCRNUpdateClock.cfg is the control     *)
(* with the resume rule; MCRNUpdateClockBrokenStall.cfg restores the rule  *)
(* this fix removes and must fail ResumesExactly.  README-RNUpdateClock.md *)
(* gives the traces and the check against the code by hand.                *)
(***************************************************************************)
EXTENDS RNUpdateClock
=============================================================================
