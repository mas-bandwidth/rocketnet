---------------------------- MODULE MCRNFillPhase ----------------------------
(***************************************************************************)
(* The small instance of RNFillPhase: two fillers, two clients, two main  *)
(* operations and two fill calls per filler, with an environment free to *)
(* break the contract.  MCRNFillPhase.cfg is the committed run, with the  *)
(* check as the code makes it.  The reversed witnesses each restore one   *)
(* defect and must fail: MCRNFillPhaseBrokenLookFirst.cfg looks before it *)
(* announces (Sound), MCRNFillPhaseBrokenLoadStore.cfg locks by a load    *)
(* then a store (OneFiller), MCRNFillPhaseBrokenNoReset.cfg leaves the    *)
(* epoch at main_end (NoFalseAlarm), MCRNFillPhaseBrokenNoEpochLook.cfg   *)
(* looks at main_calls only (OneFiller), MCRNFillPhaseBrokenNoRestore.cfg *)
(* leaves a refused look's claim (NoFalseAlarm),                        *)
(* MCRNFillPhaseBrokenScanOne.cfg looks at one fills_in line (Sound).     *)
(* Two fills_in lines: f2 counts on l2, every other filler on l1, so at   *)
(* three fillers two share a line.  README-RNFillPhase.md gives the       *)
(* traces TLC printed and the check against the code by hand.            *)
(***************************************************************************)
EXTENDS RNFillPhase

CONSTANTS f1, f2, f3, l1, l2

MCLineOf == [f \in Fillers |-> IF f = f2 THEN l2 ELSE l1]
=============================================================================
