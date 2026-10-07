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

MCCid    == [t \in Tokens |-> CASE t = "th" -> 1 [] t = "tx" -> 2 [] t = "tf" -> 3 [] t = "te" -> 4 [] OTHER -> 5]
MCExpire == [t \in Tokens |-> CASE t = "th" -> HonestExpire [] t = "tx" -> 0 [] t = "tf" -> 0 [] t = "te" -> 1 [] OTHER -> 0]

\* every attacker packet still in flight is bounded by the link's Capacity;
\* the ghost that the flood never grows server state is UnauthStateBounded
\* A bounded witness search for the concurrent-mapping admission boundary.
\* Every conjunct below restricts an ordinary Security action; it adds no
\* synthetic admission or authentication step. H and A possess th; B presents
\* a separately issued tb only to evict th from the reduced history.
MCSharingHolders == {<<"A", "th">>}

MCSharingRequest(t, a) ==
    /\ InjectRequest
    /\ C2S!Send([type |-> "request", from |-> a, tok |-> t,
                  cookie |-> IF seenCookies[a] = None THEN Zero ELSE seenCookies[a]])
MCSharingNext ==
    \/ Connect
    \/ (nonceCtr = 0 /\ ClientSend)
    \/ (nonceCtr = 1 /\ others = 0 /\ OtherJoins)
    \/ (nonceCtr \in 1..HistoryCap /\ others = 1 /\
        MCSharingRequest(CHOOSE t \in Tokens \ {HonestToken} : ~HistLive(t), "B"))
    \/ (nonceCtr = HistoryCap + 1 /\ others = 1 /\ OtherLeaves)
    \/ (nonceCtr = HistoryCap + 1 /\ others = 0 /\ MCSharingRequest("th", "A"))
    \/ (nonceCtr = HistoryCap + 2 /\ cstate = 3 /\ ClientSend)
    \/ (nonceCtr = HistoryCap + 2 /\ admits["th"] = 1 /\ ~Est(HonestAddr) /\ HolderResponse)
    \/ ClientRecvCookie \/ ClientRecvChallenge \/ ClientRecvDenied \/ ClientRecvKeepAlive \/ OtherRecv
    \/ ServerRecvRequest \/ ServerRecvResponse
    \/ (clock < 3 /\ Tick)
MCSharingSpec == Init /\ [][MCSharingNext]_vars

=============================================================================
