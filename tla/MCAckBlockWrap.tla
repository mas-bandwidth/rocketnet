--------------------------- MODULE MCAckBlockWrap ---------------------------
(***************************************************************************)
(* The small instance that checks AckBlockWrap: Modulus 16, the session    *)
(* starting at 13 (above Modulus/2, the start the DEV seam gives near      *)
(* 2^32) and running to 22, across the wrap at 16, where a genuine largest *)
(* of 16 has wire value 0 and must still read nonempty.                    *)
(*                                                                         *)
(* MCAckBlockWrap.cfg is the rule and passes every invariant.              *)
(* MCAckBlockWrapBrokenHalfRange.cfg sets BrokenHalfRange = TRUE (the      *)
(* empty test on the reconstructed largest, the reader before 2026-10-24)  *)
(* and must fail on EmptyRecognised.  README-AckBlockWrap.md gives the     *)
(* trace, replayed against sdk/rocketnet_ack.cpp by hand.                  *)
(***************************************************************************)
EXTENDS AckBlockWrap

MCSpec == Spec
=============================================================================
