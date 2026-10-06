---------------------------- MODULE MCSecurity ----------------------------
(***************************************************************************)
(* The small instance of Security.  Four tokens: "th" is the honest        *)
(* client's (signed, sealed to this server, client id 1, expiring at       *)
(* HonestExpire; 0 = never); "tx" is signed but sealed to another server;  *)
(* "tf" carries no valid signature; "te" is genuine and already expired at *)
(* clock 1.  Two addresses: "H" the honest client, "A" the attacker.       *)
(* MCSecurity.cfg holds all four, so the attacker presents every one.      *)
(***************************************************************************)
EXTENDS Security

CONSTANT HonestExpire

MCCid    == [t \in Tokens |-> CASE t = "th" -> 1 [] t = "tx" -> 2 [] t = "tf" -> 3 [] t = "te" -> 4]
MCExpire == [t \in Tokens |-> CASE t = "th" -> HonestExpire [] t = "tx" -> 0 [] t = "tf" -> 0 [] t = "te" -> 1]

\* every attacker packet still in flight is bounded by the link's Capacity;
\* the ghost that the flood never grows server state is UnauthStateBounded

\* Reversed witnesses, one per hostile token.  NeverPresented(t): the server
\* is never about to read a request carrying t.  NeverPastGate(t): nor one
\* carrying t with a cookie the attacker saw, the request that reaches the
\* expire, server-key and signature rungs.  Each is expected to FAIL; a pass
\* would mean the run never presents that token.  Not in MCSecurity.cfg; add
\* one under INVARIANTS to run it (README-Security.md, "How to run it").
NextRequest(t)    == c2sWire # <<>> /\ Head(c2sWire).due = 0
                     /\ Head(c2sWire).pkt.type = "request" /\ Head(c2sWire).pkt.tok = t
NeverPresented(t) == ~NextRequest(t)
NeverPastGate(t)  == ~(NextRequest(t) /\ Head(c2sWire).pkt.cookie \notin {Zero, Bad})
WitnessForeign         == NeverPresented("tx")
WitnessForged          == NeverPresented("tf")
WitnessExpired         == NeverPresented("te")
WitnessForeignPastGate == NeverPastGate("tx")
WitnessForgedPastGate  == NeverPastGate("tf")
WitnessExpiredPastGate == NeverPastGate("te")
=============================================================================
