------------------------- MODULE MCXdpHardwareBench -------------------------
(***************************************************************************)
(* The small instance of XdpHardwareBench: two runs contending for one     *)
(* machine, every crash point open.  MCXdpHardwareBench.cfg is the         *)
(* committed run with both rules on.  The reversed witnesses are the       *)
(* committed file with one constant flipped:                               *)
(*   MCXdpHardwareBenchBrokenNoRestore.cfg  MarkCrash = FALSE, must fail   *)
(*                                          FreeMeansRestored              *)
(*   MCXdpHardwareBenchBrokenTwoRuns.cfg    UseLock = FALSE, must fail     *)
(*                                          OneRunAtATime                  *)
(* README-XdpHardwareBench.md has the traces TLC printed, replayed by hand *)
(* against hold.sh and restore.sh.                                         *)
(***************************************************************************)
EXTENDS XdpHardwareBench
=============================================================================
