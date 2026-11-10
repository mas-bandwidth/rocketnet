------------------------ MODULE MCRNSequencingWrap ------------------------
(***************************************************************************)
(* The small instance of RNSequencingWrap: the 24-bit wire indices scaled  *)
(* to modulo 8 (half range 4), uint32 arithmetic to modulo 32, three       *)
(* records in flight, five sequenced and four ordered sends from a         *)
(* quiescent start anywhere on the ring, so both the ordering and the      *)
(* sequencing pass the wire wrap.  MCRNSequencingWrap.cfg is the passing   *)
(* control; each MCRNSequencingWrapBroken*.cfg writes one compare of       *)
(* sdk/rocketnet_channel_sequenced.cpp wrong and must violate its declared *)
(* invariant (README-RNSequencingWrap.md).                                 *)
(***************************************************************************)
EXTENDS RNSequencingWrap

MCSpec == Spec

=============================================================================
