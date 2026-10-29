--------------------------- MODULE MCRNScheduler ---------------------------
(***************************************************************************)
(* The small instance that checks RNScheduler.  Every run is this module   *)
(* with MCRNScheduler.cfg, or that file with one constant flipped as       *)
(* README-RNScheduler.md lists.                                            *)
(*                                                                         *)
(* Two channels.  Channel 0 queues a LOW item and then an IMMEDIATE item   *)
(* (the boost case: the IMMEDIATE item must not be first-written ahead of  *)
(* the LOW one); channel 1 queues one HIGH item.  Unreliable items are     *)
(* IMMEDIATE, so the class order is checked against the most urgent        *)
(* priority.  A window of 2 numbers, a budget of 1 item per update, one    *)
(* unreliable item arriving with every update and 1 transport loss.  The   *)
(* two reversed witnesses:                                                 *)
(*   MCRNSchedulerBrokenPerMessagePriority.cfg    ChannelFIFO              *)
(*                                     FirstWriteInOrderPerChannel         *)
(*   MCRNSchedulerBrokenUnreliableStarvesOrdered.cfg  UnreliableLast       *)
(*                                     NoStarvation                        *)
(***************************************************************************)
EXTENDS RNScheduler

MCMsgs == << [ch |-> 0, lv |-> 3], [ch |-> 0, lv |-> 0], [ch |-> 1, lv |-> 1] >>

=============================================================================
