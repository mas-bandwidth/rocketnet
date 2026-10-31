---------------------------- MODULE RNClientLadder ----------------------------
\* The client ladder at create with the family fallback
\* (rocketnet_client_transport_create_internal in
\* sdk/rocketnet_client_transport.cpp). Candidates are tried left to right;
\* a one-element list is a pin. On each candidate the client opens a socket
\* for the server the token selected and connects it. A token naming both
\* families on a host with IPv6 selects its IPv6 server; when that socket is
\* refused, at open or at connect, the same candidate is opened again for
\* the token's IPv4 server before the ladder falls to the next candidate. The
\* first socket opened and connected is granted. A refusal for want of memory
\* ends the create on either family. A list whose every socket is refused is
\* exhausted.
\*
\* Each socket's outcome is the environment's, chosen once at Init for every
\* candidate and family: "ok", "refused" or "oom". The host is one with IPv6,
\* so a token naming IPv6 selects it first.
\*
\* Broken names the reversed witnesses: "nofallback" falls to the next
\* candidate on an IPv6 refusal, "oomfalls" tries the IPv4 server after an
\* IPv6 refusal for want of memory, and "ignorestoken" falls back to IPv4 for
\* a token that names IPv6 alone.
EXTENDS Naturals, Sequences

CONSTANTS NumCandidates, Broken

Families == {"v4", "v6"}
Outcomes == {"ok", "refused", "oom"}
Candidates == 1..NumCandidates
Tokens == {{"v4"}, {"v6"}, {"v4", "v6"}}

VARIABLES token, outcome, rung, family, result, tried

vars == <<token, outcome, rung, family, result, tried>>

\* the family the token selects on a host with IPv6
Primary(t) == IF "v6" \in t THEN "v6" ELSE "v4"

\* an IPv6 refusal falls back to IPv4 on the same candidate
Falls(t) == IF "ignorestoken" \in Broken THEN "v6" \in t ELSE t = Families

\* the order the code tries sockets in: each candidate's selected family,
\* then its IPv4 fallback when the token names both
RungOrder(t) == IF t = Families THEN <<"v6", "v4">> ELSE <<Primary(t)>>

Order(t) ==
    LET k == Len(RungOrder(t))
    IN [i \in 1..(NumCandidates * k) |-> <<((i - 1) \div k) + 1, RungOrder(t)[((i - 1) % k) + 1]>>]

Init ==
    /\ token \in Tokens
    /\ outcome \in [Candidates \X Families -> Outcomes]
    /\ rung = 1
    /\ family = Primary(token)
    /\ result = "trying"
    /\ tried = <<>>

\* the next candidate, or the exhausted list
NextRung ==
    IF rung = NumCandidates
    THEN /\ result' = "exhausted"
         /\ UNCHANGED <<rung, family>>
    ELSE /\ rung' = rung + 1
         /\ family' = Primary(token)
         /\ result' = "trying"

\* the socket for the server the token selected, on this candidate
TryPrimary ==
    /\ result = "trying"
    /\ family = Primary(token)
    /\ tried' = Append(tried, <<rung, family>>)
    /\ LET o == outcome[<<rung, family>>]
       IN CASE o = "ok" ->
                 /\ result' = "granted"
                 /\ UNCHANGED <<rung, family>>
            [] o = "oom" /\ ~("oomfalls" \in Broken /\ family = "v6" /\ Falls(token)) ->
                 /\ result' = "oom"
                 /\ UNCHANGED <<rung, family>>
            [] OTHER ->
                 IF family = "v6" /\ Falls(token) /\ ~("nofallback" \in Broken)
                 THEN /\ family' = "v4"
                      /\ UNCHANGED <<rung, result>>
                 ELSE NextRung
    /\ UNCHANGED <<token, outcome>>

\* the token's IPv4 server on the same candidate, after its IPv6 socket was
\* refused
TryFallback ==
    /\ result = "trying"
    /\ family # Primary(token)
    /\ tried' = Append(tried, <<rung, family>>)
    /\ LET o == outcome[<<rung, family>>]
       IN CASE o = "ok" ->
                 /\ result' = "granted"
                 /\ UNCHANGED <<rung, family>>
            [] o = "oom" ->
                 /\ result' = "oom"
                 /\ UNCHANGED <<rung, family>>
            [] OTHER -> NextRung
    /\ UNCHANGED <<token, outcome>>

\* the create has ended: granted, exhausted or out of memory
Done ==
    /\ result # "trying"
    /\ UNCHANGED vars

Next == TryPrimary \/ TryFallback \/ Done

Spec == Init /\ [][Next]_vars /\ WF_vars(TryPrimary \/ TryFallback)

TypeOK ==
    /\ token \in Tokens
    /\ outcome \in [Candidates \X Families -> Outcomes]
    /\ rung \in Candidates
    /\ family \in Families
    /\ result \in {"trying", "granted", "exhausted", "oom"}
    /\ Len(tried) <= 2 * NumCandidates

Last == tried[Len(tried)]

\* every socket dials a server the token names
TriedInToken == \A i \in 1..Len(tried) : tried[i][2] \in token

\* sockets are tried in the code's order: candidate by candidate, the
\* selected family first and its IPv4 fallback next, no candidate skipped
LadderOrder ==
    /\ Len(tried) <= Len(Order(token))
    /\ \A i \in 1..Len(tried) : tried[i] = Order(token)[i]

\* the ladder exhausts only when no socket the token names on any candidate
\* would have been granted
NoExhaustWhileReachable ==
    result = "exhausted" =>
        \A r \in Candidates, f \in token : outcome[<<r, f>>] # "ok"

\* a refusal for want of memory is the last socket tried
NoTryAfterOom == \A i \in 1..(Len(tried) - 1) : outcome[tried[i]] # "oom"

\* the create ends out of memory only on a socket refused for want of memory
OomOnlyFromOom == result = "oom" => (Len(tried) > 0 /\ outcome[Last] = "oom")

\* a grant is the last socket tried, and it opened and connected
GrantIsLast == result = "granted" => (Len(tried) > 0 /\ outcome[Last] = "ok" /\ Last = <<rung, family>>)

\* the IPv4 server is granted only after the IPv6 socket on the same
\* candidate was refused
FallbackOnlyAfterRefusal ==
    (result = "granted" /\ token = Families /\ family = "v4") => outcome[<<rung, "v6">>] = "refused"

\* every create ends
Terminates == <>(result # "trying")
=============================================================================
