---------------------------- MODULE MCRNIndexRing ----------------------------
(***************************************************************************)
(* The small instance of RNIndexRing.  MCRNIndexRing.cfg is the committed *)
(* run; MCRNIndexRingAtCapacity.cfg checks one entry per live slot.      *)
(* MCRNIndexRingBrokenStaleBySlot.cfg and                                *)
(* MCRNIndexRingBrokenSmallRing.cfg are the reversed witnesses, each the  *)
(* committed file with one constant changed.  Tickets are bounded, so a   *)
(* run ends in terminal states once they are spent.                        *)
(***************************************************************************)
EXTENDS RNIndexRing
=============================================================================
