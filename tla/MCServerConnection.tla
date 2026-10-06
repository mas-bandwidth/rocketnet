------------------------ MODULE MCServerConnection ------------------------
(***************************************************************************)
(* The small instance that checks ServerConnection.  Every run is this     *)
(* module with MCServerConnection.cfg, or that file with a few lines       *)
(* changed as README-ServerConnection.md lists; tla/RUNS.tsv records each. *)
(*                                                                         *)
(* The committed cfg is the hang check the request named, at two slots    *)
(* and two clients: the code's re-sending (RepeatKeepAlive and             *)
(* ResendChallenge TRUE) under KindLoss = "firstkind", every timeout and   *)
(* deadline off, nobody leaving: Connects and ServerNotStranded must hold, *)
(* with every safety property.  Flipping either witness to FALSE must make *)
(* Connects fail.                                                          *)
(***************************************************************************)
EXTENDS ServerConnection

\* the safety properties every run checks
SafetyInvariants ==
    /\ TypeOK /\ ModelBoundsOK /\ FreeListExact /\ AddressIdentity
    /\ SlotMappingBijection /\ NoTwoClientsInOneSlot /\ NoCrossDelivery /\ Ledger

=============================================================================
