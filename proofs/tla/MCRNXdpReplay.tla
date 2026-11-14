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
(* MarkNewSessions <- MarkNewSessionsBroken (the walk marks a reconnect    *)
(* only at a nonzero highest_opened, so its first commit leaves the old    *)
(* entry; a slot never seen keeps its zero mark, without which the kernel  *)
(* drops every packet of the first session as no-session) and must fail   *)
(* NewSessionNeverDroppedByOldMark.  The controls set                      *)
(* ForgetFailureBudget = 2: a kept forget refused again in the commit     *)
(* that lands the newer mark; the superseded witness                       *)
(* takes ForgetFailureBudget = 3, since the newer session's first commit   *)
(* lands a zero mark and its nonzero mark comes a commit later.            *)
(* MCRNXdpReplayReconnect.cfg is the reconnect control: two addresses,     *)
(* three sessions, one pending-forget entry.                               *)
(* MCRNXdpReplayRefusedMark.cfg is the refused-mark control: the kernel    *)
(* may refuse up to two marks (MarkFailureBudget = 2).                     *)
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
(* MCRNXdpReplayBrokenGreetBeforeEntry.cfg sets the same switch for a      *)
(* first session (no entry at all once its zero mark is refused) and must  *)
(* fail LiveSessionNeverDroppedForNoEntry: the kernel drops a session      *)
(* packet with no entry;                                                   *)
(* MCRNXdpReplayBrokenQueueRefusedAdvances.cfg sets                      *)
(* AdvanceOnQueueRefusal <- AdvanceOnQueueRefusalBroken (the walk's cache  *)
(* advances on a mark the buffer refused) and must fail                    *)
(* MarkOwedUntilApplied.                                                   *)
(* MCRNXdpReplayHeldReply.cfg is the held-reply control: the link holds up *)
(* to two replies (HeldReplyBudget = 2), a handshake                       *)
(* reply, a session reply or one to an address no frame came from, and     *)
(* sends each later with no frame in hand.  Its reversed witnesses:        *)
(* MCRNXdpReplayBrokenAnsweredNotKept.cfg sets KeepAnswered <-             *)
(* KeepAnsweredBroken (the table before the answered entry: a held         *)
(* handshake reply finds no MAC) and must fail HeldReplyFindsItsMac;       *)
(* MCRNXdpReplayBrokenForgetDropsAnswered.cfg sets ForgetLeavesAnswered <- *)
(* ForgetLeavesAnsweredBroken (a forget drops the MAC outright: a reply    *)
(* held for the released client finds none) and must fail                  *)
(* HeldReplyFindsItsMac; MCRNXdpReplayBrokenAnswerAnyAddress.cfg sets      *)
(* AnswerKeyedByAddress <- AnswerKeyedByAddressBroken (a reply with no     *)
(* frame in hand takes any address's answered MAC) and must fail           *)
(* NeverSeenNeverSentTo.                                                   *)
(***************************************************************************)
EXTENDS RNXdpReplay

CONSTANTS A1

KernelAdvancesBroken == TRUE
WalkForgetsBroken    == FALSE
WalkForgetsUnmarkedBroken == FALSE
KernelWindowBroken   == W - 1
KeepFailedForgetsBroken == FALSE
SupersedeStaleForgetsBroken == FALSE
MarkNewSessionsBroken == FALSE
KeepRefusedMarksBroken == FALSE
ForgetDropsKeptMarkBroken == FALSE
HoldGreetingUntilMarkedBroken == FALSE
AdvanceOnQueueRefusalBroken == TRUE
KeepAnsweredBroken == FALSE
ForgetLeavesAnsweredBroken == FALSE
AnswerKeyedByAddressBroken == FALSE
=============================================================================
