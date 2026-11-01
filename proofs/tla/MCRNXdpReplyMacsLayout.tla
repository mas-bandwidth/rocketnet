---------------------- MODULE MCRNXdpReplyMacsLayout ----------------------
(***************************************************************************)
(* The small instance of RNXdpReplyMacsLayout: a table of four entries     *)
(* with a probe window of two, three addresses whose home slots are every  *)
(* map of addresses to slots, two MACs.  The window is half the table, so  *)
(* full windows, wraps past the last slot and evictions are all reached.   *)
(* MCRNXdpReplyMacsLayout.cfg is the passing control; each                 *)
(* MCRNXdpReplyMacsLayoutBroken*.cfg turns one rule off and must violate   *)
(* the invariant it names (README-RNXdpReplyMacsLayout.md).                *)
(***************************************************************************)
EXTENDS RNXdpReplyMacsLayout

CONSTANTS a1, a2, a3, m1, m2

MCSpec == Spec

=============================================================================
