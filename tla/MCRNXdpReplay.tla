--------------------------- MODULE MCRNXdpReplay ---------------------------
(***************************************************************************)
(* The small instance of RNXdpReplay.  MCRNXdpReplay.cfg is the design.    *)
(* The reversed witnesses override one rule switch each:                   *)
(* MCRNXdpReplayBrokenKernelAdvances.cfg sets                              *)
(* KernelAdvances <- KernelAdvancesBroken (the kernel writes the sequence  *)
(* of every packet it passes) and must fail FreshGenuineNeverDropped;      *)
(* MCRNXdpReplayBrokenNoForget.cfg sets WalkForgets <- WalkForgetsBroken   *)
(* (the walk never forgets a released slot) and must fail                  *)
(* NewSessionNeverDroppedByOldMark; MCRNXdpReplayBrokenNarrowWindow.cfg    *)
(* sets KernelWindow <- KernelWindowBroken (the kernel window one short of *)
(* the userspace window, the shape of the old 16 against 4096) and must    *)
(* fail FreshGenuineNeverDropped;                                          *)
(* MCRNXdpReplayBrokenUnmarkedNotForgotten.cfg sets                        *)
(* WalkForgetsUnmarked <- WalkForgetsUnmarkedBroken and must fail          *)
(* ReleasedSlotForgetsItsReplyMac;                                         *)
(* MCRNXdpReplayBrokenFailedForgetDropped.cfg sets                         *)
(* KeepFailedForgets <- KeepFailedForgetsBroken (a forget the kernel       *)
(* refused is dropped, not kept for the next commit) and must fail         *)
(* ForgetOwedUntilApplied;                                                 *)
(* MCRNXdpReplayBrokenStaleForgetRetried.cfg sets                          *)
(* SupersedeStaleForgets <- SupersedeStaleForgetsBroken (a refused forget  *)
(* is kept over a newer session's applied mark of the same address) and    *)
(* must fail RetryNeverDeletesNewerMark.  The control and that witness set *)
(* ForgetFailureBudget <- ForgetFailureBudgetTwo: a kept forget refused    *)
(* again in the commit that lands the newer mark.                          *)
(***************************************************************************)
EXTENDS RNXdpReplay

CONSTANTS A1

KernelAdvancesBroken == TRUE
WalkForgetsBroken    == FALSE
WalkForgetsUnmarkedBroken == FALSE
KernelWindowBroken   == W - 1
KeepFailedForgetsBroken == FALSE
SupersedeStaleForgetsBroken == FALSE
ForgetFailureBudgetTwo == 2
=============================================================================
