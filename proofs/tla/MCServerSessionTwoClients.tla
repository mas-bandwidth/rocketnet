----------------------- MODULE MCServerSessionTwoClients -----------------------
(***************************************************************************)
(* The two-client instance of ServerSession.  MCServerSession.cfg keeps    *)
(* one token per run so a slot is reused across a stop and a start; with   *)
(* one token no two slots are ever CONNECTED at once, so the multi-client  *)
(* invariants (the distinct half of ConnIdLiveAndDistinct,                 *)
(* DisconnectAllIsAtomic and StopDisconnectsAll across several clients)    *)
(* say nothing there.  MCServerSessionTwoClients.cfg is the passing        *)
(* control with two tokens and one run (MaxStarts = 1), every invariant    *)
(* of MCServerSession.cfg; MCServerSessionTwoClientsLive.cfg and           *)
(* MCServerSessionTwoClientsBursts.cfg check HandshakeResolves and         *)
(* BurstsDrain on the same instance, one property a run.                   *)
(* MCServerSessionTwoClientsReachable.cfg checks NotTwoConnected and must  *)
(* fail: two slots CONNECTED at once is reachable in that instance, so the *)
(* multi-client invariants are not vacuous there.                          *)
(***************************************************************************)
EXTENDS ServerSession

NotTwoConnected == ~(\A i \in Slots : slot[i].st = "CONNECTED")
=============================================================================
