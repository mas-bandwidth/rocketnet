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
(* Those run the armed check (OptedAtStart).  MCRNFillPhaseLatch.cfg      *)
(* starts not armed and arms at the game's first call that names a      *)
(* client; MCRNFillPhaseBrokenArmedAtCreate.cfg arms from create and must *)
(* fail NoCheckBeforeOptIn.  MCRNFillPhaseTakes.cfg adds the takes        *)
(* (update_client) beside fill calls, from not armed.  Every run lets the *)
(* main thread open and shut the window of the split update; the window's *)
(* witnesses must fail: MCRNFillPhaseBrokenNoWindow.cfg, a fill call's    *)
(* look not reading it (WindowShut), MCRNFillPhaseBrokenNoMainWindow.cfg, *)
(* main_begin not reading it (WindowShut), and                           *)
(* MCRNFillPhaseBrokenTakeUncounted.cfg, a take with no count (TakeSound). *)
(* Two fills_in lines: f2 counts on line 1, every other filler on line 0 *)
(* (MCLineChoice, for the spec's LineChoice: a thread's line is fixed),   *)
(* so at three fillers two share a line.  README-RNFillPhase.md gives the *)
(* traces TLC printed and the check against the code by hand.            *)
(***************************************************************************)
EXTENDS RNFillPhase

CONSTANTS f1, f2, f3

MCLineChoice(f) == {IF f = f2 THEN 1 ELSE 0}
=============================================================================
