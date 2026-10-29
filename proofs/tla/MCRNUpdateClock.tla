--------------------------- MODULE MCRNUpdateClock ---------------------------
(***************************************************************************)
(* The small instance of RNUpdateClock: the caller's clock bounded to      *)
(* MaxTime, the run bounded to MaxSteps samples, and BASE's stalling rule   *)
(* off.  MCRNUpdateClock.cfg is the committed run with every rule on.      *)
(* MCRNUpdateClockBrokenStall.cfg is the reversed witness: BASE's rule,    *)
(* the clock that stands still until the caller's clock passes the last    *)
(* good time again.  README-RNUpdateClock.md has the traces TLC printed.    *)
(***************************************************************************)
EXTENDS RNUpdateClock
=============================================================================
