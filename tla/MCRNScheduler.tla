--------------------------- MODULE MCRNScheduler ---------------------------
(***************************************************************************)
(* The small instance that checks RNScheduler.  Every run is this module   *)
(* with MCRNScheduler.cfg, or that file with one constant flipped as       *)
(* README-RNScheduler.md lists.                                            *)
(*                                                                         *)
(* Two channels.  Channel 0 queues a normal item and then a control item   *)
(* (the boost case: the control item must not be first-written ahead of    *)
(* the normal one); channel 1 queues one normal item.  Unreliable items    *)
(* are level 0, so the class order is checked against the boosted level.  *)
(* A window of 2 numbers, a budget of 1 item per update, one               *)
(* unreliable item arriving with every update and 1 transport loss.  The   *)
(* two reversed witnesses:                                                 *)
(*   MCRNSchedulerBrokenPerMessagePriority.cfg    ChannelFIFO              *)
(*                                     FirstWriteInOrderPerChannel         *)
(*   MCRNSchedulerBrokenUnreliableStarvesOrdered.cfg  UnreliableLast       *)
(*                                     NoStarvation                        *)
(***************************************************************************)
EXTENDS RNScheduler

MCMsgs == << [ch |-> 0, lv |-> 1], [ch |-> 0, lv |-> 0], [ch |-> 1, lv |-> 1] >>

=============================================================================
