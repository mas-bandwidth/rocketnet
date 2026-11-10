----------------------- MODULE MCRNLoopbackBindLayout -----------------------
(***************************************************************************)
(* The small instances of RNLoopbackBindLayout: three records (a server   *)
(* and two clients), ports 0..5 with the ephemeral range 2..5 (four ports, *)
(* one more than the records), explicit binds to port 1 (outside the       *)
(* range) and port 2 (inside it, so an explicit bind and the search        *)
(* contend); and the same records with the range 4..5 (two ports, fewer    *)
(* than the records) and explicit binds to ports 1 and 4, where a port 0   *)
(* bind can find every port held.  MCRNLoopbackBindLayout.cfg and          *)
(* MCRNLoopbackBindLayoutFullRange.cfg are the passing controls; each      *)
(* MCRNLoopbackBindLayoutBroken*.cfg turns one rule off and must violate   *)
(* the invariant it names (README-RNLoopbackBindLayout.md).                *)
(***************************************************************************)
EXTENDS RNLoopbackBindLayout

MCSpec == Spec

=============================================================================
