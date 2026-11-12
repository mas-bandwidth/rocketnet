------------------------- MODULE MCRNSegmentRefusal -------------------------
(***************************************************************************)
(* The small instance of RNSegmentRefusal: one destination that accepts   *)
(* segmentation and one whose route refuses it, a threshold of two, six   *)
(* runs. MCRNSegmentRefusal.cfg is the code; MCRNSegmentRefusalKernel.cfg *)
(* is a kernel that refuses every destination. The two Broken configs are  *)
(* the code before the per-destination rule and must fail                  *)
(* OtherDestinationUnaffected (README-RNSegmentRefusal.md).                *)
(***************************************************************************)
EXTENDS RNSegmentRefusal
=============================================================================
