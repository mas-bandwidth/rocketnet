----------------------------- MODULE MCRNRioDriver -----------------------------
(***************************************************************************)
(* Small instances of the historical caller-driven RNRioDriver design.   *)
(* The control uses one slot per phase and two receive slices. Four      *)
(* reversed witnesses are declared with their exact bounds in CASES.tsv  *)
(* and the matching cfgs, including BrokenUnboundedReceive.               *)
(* README-RNRioDriver.md gives the traces TLC printed and the             *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNRioDriver
=============================================================================
