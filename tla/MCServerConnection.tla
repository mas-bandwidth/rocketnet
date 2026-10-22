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
    /\ TimeoutHonoured /\ DeadlineHonoured /\ HalfOpenIdleHonoured

\* the dual-family runs (MCServerConnectionDualFamily.cfg and its reversed
\* witness MCServerConnectionBrokenIpv6OnlyMatch.cfg): a token naming both
\* families, a client host with IPv4 only, a server serving IPv4
MCTokenBothFamilies == {"ipv4", "ipv6"}
MCHostIpv4Only      == {"ipv4"}
MCServedIpv4        == "ipv4"
MCMatchEither       == "either"
MCMatchIpv6Only     == "ipv6only"


\* both families (MCServerConnectionServesBothFamilies.cfg and the reversed
\* witness MCServerConnectionBrokenOneFamilyServed.cfg): client 1 is on an
\* IPv4-only host, client 2 on an IPv6-only host, the config names both
MCHostSplit(c) == IF c = 1 THEN {"ipv4"} ELSE {"ipv6"}
MCConfiguredBoth == {"ipv4", "ipv6"}
MCServeFirstOnly == TRUE

MCRefreshConnectedOnResponse == TRUE

=============================================================================
