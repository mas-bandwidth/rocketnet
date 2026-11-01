---------------------------- MODULE MCRNUringDriver ----------------------------
(***************************************************************************)
(* The small instance of RNUringDriver: two send slots, a ring of two     *)
(* provided buffers, two datagrams, two bank swaps and a budget of one   *)
(* CQE per call.                                                         *)
(* MCRNUringDriver.cfg is the rule, checked with the eight invariants    *)
(* and the action property NoCallEndsBehindRoom;                         *)
(* MCRNUringDriverLiveSends.cfg checks SubmittedEventuallyFree and       *)
(* MCRNUringDriverLiveReceive.cfg DestroyEventuallyFrees and             *)
(* ReceiveLiveness, both under SpecFair.  The eight reversed witnesses   *)
(* turn off one rule switch each and must fail:                          *)
(* MCRNUringDriverBrokenResetBeforeReap.cfg (ResetIndex right after      *)
(* Submit) fails NoReuseWhileKernelOwned,                                *)
(* MCRNUringDriverBrokenOneBank.cfg (Reap writes receive CQEs into the   *)
(* exposed bank) fails ExposedStable,                                    *)
(* MCRNUringDriverBrokenUnboundedReap.cfg (a call reaps until the cq is  *)
(* empty) fails ReapWithinBudget,                                        *)
(* MCRNUringDriverBrokenNoRearmRetry.cfg (receive_packets never re-arms  *)
(* a disarmed multishot) fails ReceiveLiveness,                          *)
(* MCRNUringDriverBrokenFreeRunAtSubmit.cfg (a segmented run's slots     *)
(* after its head handed back at Submit) fails NoReuseWhileKernelOwned,  *)
(* MCRNUringDriverBrokenReturnBeforeSplit.cfg (ReturnBuffer once the    *)
(* first segment of a coalesced buffer is copied, ReturnAfterLastCopy    *)
(* replaced with MCReturnAfterFirstCopy) fails BufferReturnedAfterCopy,  *)
(* MCRNUringDriverBrokenUnboundedFill.cfg (a receive reaped into a full  *)
(* bank) fails FillWithinRoom, and                                       *)
(* MCRNUringDriverBrokenStopAtRoom.cfg (a receive CQE without room left  *)
(* in cq, the call ended at it, HoldWithoutRoom replaced with            *)
(* MCStopAtRoom) fails the action property NoCallEndsBehindRoom.         *)
(* Submit may send both slots as one run, and one receive buffer carries  *)
(* up to Segments = 2 datagrams (UDP_GRO).  README-RNUringDriver.md gives *)
(* the traces and the check of each against the code by hand.          *)
(***************************************************************************)
EXTENDS RNUringDriver

\* the reversed witness's replacement for the rule switch
\* ReturnAfterLastCopy (MCRNUringDriverBrokenReturnBeforeSplit.cfg)
MCReturnAfterFirstCopy == FALSE

\* the reversed witness's replacement for the rule switch HoldWithoutRoom
\* (MCRNUringDriverBrokenStopAtRoom.cfg)
MCStopAtRoom == FALSE
=============================================================================
