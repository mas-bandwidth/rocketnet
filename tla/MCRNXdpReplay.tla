--------------------------- MODULE MCRNXdpReplay ---------------------------
(***************************************************************************)
(* The small instance of RNXdpReplay.  MCRNXdpReplay.cfg is the design.    *)
(* The reversed witnesses override one rule switch each:                   *)
(* MCRNXdpReplayBrokenKernelAdvances.cfg sets                              *)
(* KernelAdvances <- KernelAdvancesBroken (the kernel writes the sequence  *)
(* of every packet it passes) and must fail FreshGenuineNeverDropped;      *)
(* MCRNXdpReplayBrokenNoForget.cfg sets WalkForgets <- WalkForgetsBroken   *)
(* (the walk never forgets a released slot) and must fail                  *)
(* ReleasedSlotForgetsItsReplyMac (a reconnect no longer depends on the    *)
(* forget: its first mark replaces the old entry);                         *)
(* MCRNXdpReplayBrokenNarrowWindow.cfg                                     *)
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
(* MCRNXdpReplayBrokenSupersededForget.cfg sets                          *)
(* SupersedeStaleForgets <- SupersedeStaleForgetsBroken (a refused forget  *)
(* is kept over a newer session's applied mark of the same address) and    *)
(* must fail RetryNeverDeletesNewerMark;                                   *)
(* MCRNXdpReplayBrokenReconnectFreshness.cfg sets                          *)
(* MarkNewSessions <- MarkNewSessionsBroken (the walk marks only a nonzero *)
(* highest_opened, so a reconnect's first commit leaves the old entry) and *)
(* must fail NewSessionNeverDroppedByOldMark.  The controls set            *)
(* ForgetFailureBudget <- ForgetFailureBudgetTwo: a kept forget refused    *)
(* again in the commit that lands the newer mark; the superseded witness   *)
(* takes ForgetFailureBudgetThree, since the newer session's first commit  *)
(* lands a zero mark and its nonzero mark comes a commit later.            *)
(* MCRNXdpReplayReconnect.cfg is the reconnect control: two addresses,     *)
(* three sessions, one pending-forget entry.                               *)
(* MCRNXdpReplayRefusedMark.cfg is the refused-mark control: the kernel    *)
(* may refuse up to two marks (MarkFailureBudget <- MarkFailureBudgetTwo). *)
(* Its reversed witnesses: MCRNXdpReplayBrokenRefusedMarkDropped.cfg sets  *)
(* KeepRefusedMarks <- KeepRefusedMarksBroken (a refused mark is dropped)  *)
(* and must fail MarkOwedUntilApplied;                                     *)
(* MCRNXdpReplayBrokenKeptMarkAfterForget.cfg sets                         *)
(* ForgetDropsKeptMark <- ForgetDropsKeptMarkBroken (a kept mark survives  *)
(* the forget the walk queues for its address, and the commit rewrites the *)
(* released address's entry after the forget) and must fail               *)
(* ForgetOwedUntilApplied;                                                 *)
(* MCRNXdpReplayBrokenGreetBeforeMark.cfg sets                             *)
(* HoldGreetingUntilMarked <- HoldGreetingUntilMarkedBroken (the commit    *)
(* greets a new session whose zero mark the kernel refused) and must fail  *)
(* NewSessionNeverDroppedByOldMark;                                        *)
(* MCRNXdpReplayBrokenQueueRefusedAdvances.cfg sets                        *)
(* AdvanceOnQueueRefusal <- AdvanceOnQueueRefusalBroken (the walk's cache  *)
(* advances on a mark the buffer refused) and must fail                    *)
(* MarkOwedUntilApplied.                                                   *)
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
ForgetFailureBudgetThree == 3
MarkNewSessionsBroken == FALSE
KeepRefusedMarksBroken == FALSE
ForgetDropsKeptMarkBroken == FALSE
MarkFailureBudgetOne == 1
MarkFailureBudgetTwo == 2
HoldGreetingUntilMarkedBroken == FALSE
AdvanceOnQueueRefusalBroken == TRUE
=============================================================================
