---------------------------- MODULE MCRNFillPhase ----------------------------
(***************************************************************************)
(* The small instance of RNFillPhase: two fillers, one client, two main   *)
(* operations and two fill calls per filler, with an environment free to *)
(* break the contract.  MCRNFillPhase.cfg is the committed run, with the  *)
(* check as the code makes it.  The reversed witnesses each restore one   *)
(* defect and must fail: MCRNFillPhaseBrokenLookFirst.cfg looks before it *)
(* announces (Sound), MCRNFillPhaseBrokenLoadStore.cfg claims by a load   *)
(* then a store (OneFiller), MCRNFillPhaseBrokenNoReset.cfg leaves the    *)
(* owners set at main_end (NoFalseAlarm).  README-RNFillPhase.md gives    *)
(* the traces TLC printed and the check against the code by hand.        *)
(***************************************************************************)
EXTENDS RNFillPhase
=============================================================================
