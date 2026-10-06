---------------------------- MODULE MCSecurity ----------------------------
(***************************************************************************)
(* The small instance of Security.  Four tokens: "th" is the honest        *)
(* client's (signed, sealed to this server, client id 1, expiring at       *)
(* HonestExpire; 0 = never); "tx" is signed but sealed to another server;  *)
(* "tf" carries no valid signature; "te" is genuine and already expired at *)
(* clock 1.  Two addresses: "H" the honest client, "A" the attacker.       *)
(***************************************************************************)
EXTENDS Security

CONSTANT HonestExpire

MCCid    == [t \in Tokens |-> CASE t = "th" -> 1 [] t = "tx" -> 2 [] t = "tf" -> 3 [] t = "te" -> 4]
MCExpire == [t \in Tokens |-> CASE t = "th" -> HonestExpire [] t = "tx" -> 0 [] t = "tf" -> 0 [] t = "te" -> 1]

\* every attacker packet still in flight is bounded by the link's Capacity;
\* the ghost that the flood never grows server state is UnauthStateBounded
=============================================================================
