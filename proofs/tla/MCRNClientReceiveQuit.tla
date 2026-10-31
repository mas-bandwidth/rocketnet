------------------------ MODULE MCRNClientReceiveQuit ------------------------
(***************************************************************************)
(* The instances of RNClientReceiveQuit.  The model is finite as written: *)
(* one thread, one stop, one wake.  MCRNClientReceiveQuit.cfg (linux),     *)
(* MCRNClientReceiveQuitMac.cfg and MCRNClientReceiveQuitWindows.cfg are  *)
(* the committed runs with every rule on, one per platform variant;       *)
(* MCRNClientReceiveQuitBrokenWakeBeforeQuit.cfg and                      *)
(* MCRNClientReceiveQuitBrokenNoWakeNoTimeout.cfg each restore one defect *)
(* and must fail.  README-RNClientReceiveQuit.md gives the traces TLC     *)
(* printed and the check against the code by hand.                        *)
(***************************************************************************)
EXTENDS RNClientReceiveQuit
=============================================================================
