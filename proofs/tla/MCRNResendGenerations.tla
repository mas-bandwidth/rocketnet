------------------------ MODULE MCRNResendGenerations ------------------------
(***************************************************************************)
(* The small instance of RNResendGenerations: a window of four slots, six *)
(* numbers (so numbers 4 and 5 reuse the slots of 0 and 1), two           *)
(* descriptors taken again and again, and a packet count of two per      *)
(* number.  GenMax = 6 is above the six takes, so no generation wraps.    *)
(* MCRNResendGenerations.cfg is the passing control; each                 *)
(* MCRNResendGenerationsBroken*.cfg turns one line of the code off and    *)
(* must violate the invariant it names (README-RNResendGenerations.md).   *)
(***************************************************************************)
EXTENDS RNResendGenerations

MCSpec == Spec

=============================================================================
