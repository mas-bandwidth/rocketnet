------------------------------ MODULE MCRNRing ------------------------------
(***************************************************************************)
(* The small instance of RNRing, section 15's: B = 64 (4 units of 16),    *)
(* single records of 1, 32 and 48 bytes (1 to 3 units once rounded; the   *)
(* 1 is there so that rounding has something to do), one block of two     *)
(* 16-byte fragments, six numbers.  MCRNRing.cfg is the committed run with *)
(* every rule on.  MCRNRingBrokenFloorAtU.cfg, MCRNRingBrokenNoPadding.cfg, *)
(* MCRNRingBrokenIdleFloor.cfg and MCRNRingBrokenNoRounding.cfg are the    *)
(* reversed witnesses, each the committed file with one rule's constant    *)
(* flipped; README-RNRing.md has the traces TLC printed, replayed by hand. *)
(***************************************************************************)
EXTENDS RNRing
=============================================================================
