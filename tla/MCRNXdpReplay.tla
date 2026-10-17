--------------------------- MODULE MCRNXdpReplay ---------------------------
(***************************************************************************)
(* The small instance of RNXdpReplay.  MCRNXdpReplay.cfg is the design.    *)
(* The reversed witnesses override one rule switch each:                   *)
(* MCRNXdpReplayBrokenKernelAdvances.cfg sets                              *)
(* KernelAdvances <- KernelAdvancesBroken (the kernel writes the sequence  *)
(* of every packet it passes) and must fail FreshGenuineNeverDropped;      *)
(* MCRNXdpReplayBrokenNoForget.cfg sets WalkForgets <- WalkForgetsBroken   *)
(* (the walk never forgets a released slot) and must fail                  *)
(* NewSessionNeverDroppedByOldMark.                                        *)
(***************************************************************************)
EXTENDS RNXdpReplay

CONSTANTS A1

KernelAdvancesBroken == TRUE
WalkForgetsBroken    == FALSE
=============================================================================
