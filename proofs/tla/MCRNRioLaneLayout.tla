-------------------------- MODULE MCRNRioLaneLayout --------------------------
(***************************************************************************)
(* The small instances of RNRioLaneLayout: three lanes of two slots, three *)
(* destinations (hashes 0, 1, 4), two of them hashing to one lane.  MCRNRioLaneLayout.cfg   *)
(* is the code; the Broken cfgs (NoBound, ModLane, LeastFull, Reverse)     *)
(* are the reversed witnesses, each of which                               *)
(* must fail the invariant it names.  README-RNRioLaneLayout.md gives each *)
(* trace and its check against the code.                                   *)
(***************************************************************************)
EXTENDS RNRioLaneLayout, TLC

=============================================================================
