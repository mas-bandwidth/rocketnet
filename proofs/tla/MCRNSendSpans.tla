---------------------------- MODULE MCRNSendSpans ----------------------------
(***************************************************************************)
(* The small instance of RNSendSpans: SR = 96 (six 16-byte units),        *)
(* message lengths 1, 32, 48 and 96 bytes (1 so that rounding has          *)
(* something to do, 96 the maximal message, the whole ring, as SR = B at   *)
(* the defaults), and four send descriptors, so four spans can be live    *)
(* and freed in any order.                                                 *)
(* MCRNSendSpans.cfg is the committed run with every rule on.             *)
(* MCRNSendSpansBrokenFloorAtNewest.cfg and                               *)
(* MCRNSendSpansBrokenNoPadding.cfg are the reversed witnesses, each the   *)
(* committed file with one rule's constant flipped.                        *)
(***************************************************************************)
EXTENDS RNSendSpans
=============================================================================
