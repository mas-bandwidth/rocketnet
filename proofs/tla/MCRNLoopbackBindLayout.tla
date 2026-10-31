----------------------- MODULE MCRNLoopbackBindLayout -----------------------
(***************************************************************************)
(* The small instance of RNLoopbackBindLayout: three records (a server and *)
(* two clients), ports 0..5 with the ephemeral range 2..5 (four ports, one *)
(* more than the records, the code's static assert), explicit binds to     *)
(* port 1 (outside the range) and port 2 (inside it, so an explicit bind   *)
(* and the search contend).  MCRNLoopbackBindLayout.cfg is the passing     *)
(* control; each MCRNLoopbackBindLayoutBroken*.cfg turns one rule off and  *)
(* must violate the invariant it names (README-RNLoopbackBindLayout.md).   *)
(***************************************************************************)
EXTENDS RNLoopbackBindLayout

MCSpec == Spec

=============================================================================
