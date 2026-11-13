--------------------------- MODULE MCLoopbackHub ---------------------------
(***************************************************************************)
(* The small instance of LoopbackHub: a hub of two client records, two    *)
(* servers (the second is refused while the first holds record 0), three  *)
(* clients of this build (the third is refused while two hold the         *)
(* records) and one client of another build, which is always refused.     *)
(* MCLoopbackHub.cfg is the passing control with both fixes on and the    *)
(* caller's contract kept.                                                *)
(* MCLoopbackHubBrokenDestroyRegistered.cfg turns DestroyChecksRegistered *)
(* off and must violate NoEndpointOutlivesHub;                            *)
(* MCLoopbackHubBrokenNoHeaderCheck.cfg turns UseChecksHeader off and     *)
(* must violate NoForeignUse;                                             *)
(* MCLoopbackHubBrokenDestroyRace.cfg turns CallerSerializesDestroy off   *)
(* (a create between destroy's check and its free, the code's own         *)
(* window) and must violate NoEndpointOutlivesHub.  These are the         *)
(* reversed witnesses.                                                     *)
(***************************************************************************)
EXTENDS LoopbackHub

MCSpec == Spec

=============================================================================
