------------------------- MODULE MCServerConnection -------------------------
(***************************************************************************)
(* The small instance that checks ServerConnection.  Every run is this     *)
(* module with MCServerConnection.cfg, or that file with a few lines       *)
(* changed as README-ServerConnection.md lists; tla/RUNS.tsv records each. *)
(*                                                                         *)
(* The committed cfg is the slot-and-mapping safety run: two clients with  *)
(* their own tokens, one slot, a two-entry map, both applications'         *)
(* disconnects and both deadlines on, a FIFO link with no loss: the        *)
(* server's tables are checked against every interleaving of two clients  *)
(* contending for one slot and reusing it.  The address change, the       *)
(* shared token, the liveness runs (the hang check) and the reversed       *)
(* witnesses are edits to it.                                              *)
(***************************************************************************)
EXTENDS ServerConnection

\* the safety properties every run checks
SafetyInvariants ==
    /\ TypeOK
    /\ OneClientPerSlot /\ OneSlotPerClientId
    /\ SlotMappingBijection /\ NoSlotBeforeResponse /\ MappingsBounded
    /\ OneConnectionPerToken /\ NonceFresh
    /\ StoredChallengeNeverReplaced
    /\ TimeoutHonoured /\ DeadlineHonoured

=============================================================================
