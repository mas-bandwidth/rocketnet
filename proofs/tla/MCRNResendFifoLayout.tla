------------------------ MODULE MCRNResendFifoLayout ------------------------
(***************************************************************************)
(* The small instance of RNResendFifoLayout: a window of four slots, both *)
(* classes, every slot free to change class at any time.                  *)
(* MCRNResendFifoLayout.cfg is the passing control; each                  *)
(* MCRNResendFifoLayoutBroken*.cfg turns a rule off and must violate the  *)
(* invariant it names (README-RNResendFifoLayout.md).                     *)
(***************************************************************************)
EXTENDS RNResendFifoLayout

MCSpec == Spec

=============================================================================
