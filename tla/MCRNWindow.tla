---------------------------- MODULE MCRNWindow ----------------------------
(***************************************************************************)
(* The small instance that checks RNWindow.  Every run is this module with *)
(* MCRNWindow.cfg, or that file with a few lines changed as                *)
(* README-RNWindow.md lists.                                               *)
(*                                                                         *)
(* The committed cfg is the design's rule set (every guarding constant     *)
(* TRUE) with W = 2 and the wire modulus NumMod = 4, so numbers 0..4 wrap  *)
(* (4 is sent as 0), on a FIFO link of capacity 2 losing every other send *)
(* both ways ("alternate"): every safety invariant, and                    *)
(* EveryReliableAckedAndDelivered under Spec.  MCRNWindowSafety.cfg is the *)
(* safety case under "arbitrary" loss with numbers 0..5.                   *)
(* The five reversed witnesses are the same file with one constant         *)
(* flipped, each its own cfg:                                              *)
(*   MCRNWindowBrokenBlockUPrime.cfg      BlockUPrime    NoSlotCollision   *)
(*   MCRNWindowBrokenSameUpdateReply.cfg  KeepP6         NoSlotCollision   *)
(*   MCRNWindowBrokenHalfRange.cfg        HalfRange      WireNumberReconstructs *)
(*   MCRNWindowBrokenHoldOrdered.cfg      HoldOrdered    OrderedDeliveredInOrder *)
(*   MCRNWindowBrokenDropDuplicates.cfg   DropDuplicates DeliveredOnce     *)
(* The cfgs list each invariant by name, so TLC names the one that fails.  *)
(***************************************************************************)
EXTENDS RNWindow

\* every safety invariant at once, for a run that wants one name
SafetyInvariants ==
    /\ TypeOK /\ TablesBounded
    /\ ReceiverNeverOutsideWindow /\ NoSlotCollision
    /\ AckedImpliesReceived /\ DeliveredOnce /\ OrderedDeliveredInOrder
    /\ WireNumberReconstructs /\ HeldIsUnreleasable

=============================================================================
