--------------------------- MODULE MCAckBlockWrap ---------------------------
(***************************************************************************)
(* The small instance that checks AckBlockWrap: Modulus 8, the session     *)
(* starting at 5 (above Modulus/2, as the DEV seam's start near 2^32 is)   *)
(* and running to 12, across the wrap at 8, where a genuine largest of 8   *)
(* has wire value 0 and must still read nonempty.                          *)
(*                                                                         *)
(* MCAckBlockWrap.cfg is the rule and passes every invariant.              *)
(* MCAckBlockWrapFromOne.cfg is the rule with the session from 1, where a  *)
(* wire zero below Modulus/2 reconstructs to 0, and passes too.            *)
(* MCAckBlockWrapBrokenHalfRange.cfg sets BrokenHalfRange = TRUE (the      *)
(* empty test on the reconstructed largest, the reader before 2026-10-24)  *)
(* and must fail on EmptyRecognised.                                       *)
(* MCAckBlockWrapBrokenRunBound.cfg sets RunBoundChecked = FALSE (no        *)
(* refusal of a first range length above the reconstructed largest) and    *)
(* must fail on MalformedRefused.  README-AckBlockWrap.md gives the        *)
(* traces, replayed against sdk/rocketnet_ack.cpp by hand.                 *)
(***************************************************************************)
EXTENDS AckBlockWrap

MCSpec == Spec
=============================================================================
