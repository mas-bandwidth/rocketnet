--------------------------- MODULE MCLoopbackHub ---------------------------
(***************************************************************************)
(* The small instance of LoopbackHub: a hub of two client records, two    *)
(* servers (the second is refused while the first holds record 0), three  *)
(* clients of this build (the third is refused while two hold the         *)
(* records) and one client of another build, which is always refused.     *)
(* MCLoopbackHub.cfg is the passing control with both fixes on.           *)
(* MCLoopbackHubBrokenDestroyRegistered.cfg turns DestroyChecksRegistered *)
(* off and must violate NoEndpointOutlivesHub;                            *)
(* MCLoopbackHubBrokenNoHeaderCheck.cfg turns UseChecksHeader off and     *)
(* must violate NoForeignUse.  These are the reversed witnesses.          *)
(***************************************************************************)
EXTENDS LoopbackHub

MCSpec == Spec

=============================================================================
