----------------------------- MODULE MCRNBlocks -----------------------------
(***************************************************************************)
(* The small instance of RNBlocks: W = 4, F = 2 units (the stride), B = 8  *)
(* units, two messages: one single record of 2 units and one block of 5    *)
(* units, three fragments of 2, 2 and 1 units at consecutive numbers 1..3. *)
(* The link loses any copy ("arbitrary"), duplicates and reorders,         *)
(* capacity 2.                                                             *)
(*                                                                         *)
(* MCRNBlocks.cfg starts the ring at 0: offsets 0, then 2, 4, 6.           *)
(* MCRNBlocksWrap.cfg starts it at 5 (RingStart <- MCRingStartLate): the   *)
(* single takes 5..6, the block's span of 5 would cross 8 from 7, so it is *)
(* padded to 8 and its offsets wrap to 0, 2, 4.  MCRNBlocksForged.cfg lets *)
(* the outside world forge one chunk's count; the receiver refuses it.     *)
(*                                                                         *)
(* Reversed witnesses: MCRNBlocksBrokenPlaceByArrival.cfg                  *)
(* (DeliveredEqualsSent), MCRNBlocksBrokenNoPad.cfg (SpanInsideRing),      *)
(* MCRNBlocksBrokenNoAnchorCheck.cfg (NoPartialDelivery), and              *)
(* MCRNBlocksForgedRefused.cfg (NeverRefused: the refusal is reached).     *)
(* README-RNBlocks.md has each trace, replayed by hand.                    *)
(***************************************************************************)
EXTENDS RNBlocks

MCUnits == <<2, 5>>

MCRingStartLate == 5
=============================================================================
