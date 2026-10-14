--------------------------- MODULE RNSecurityTables ---------------------------
(***************************************************************************)
(* rocketnet: the security tables, with an attacker in the model.          *)
(*                                                                         *)
(* The used-token history and the admitted-token set                       *)
(* (sdk/rocketnet_token_tables.cpp), the cookie gate and its key           *)
(* generation (sdk/rocketnet_server_protocol.cpp, the cookie gate and      *)
(* rocketnet_server_process_opened_request and                             *)
(* rocketnet_server_process_connection_response), and the failed-verify    *)
(* budget (sdk/rocketnet_server.h, 64 table slots), as STANDARD.md         *)
(* specifies them (Connect token sharing, Processing Connection            *)
(* Requests, Processing Connection Response Packets).  This module         *)
(* lands the held draft of tla/Security.tla at the table grain: the        *)
(* full handshake, the link and the replay window stay in that draft;      *)
(* here a request that passes the cookie gate, a response that opens       *)
(* under its mapping, and a tick are the only events.                      *)
(*                                                                         *)
(* Switches name the code and the fixed code.  RefuseAdmittedAtResponse,   *)
(* AdmitExpiredAtResponse, EvictLiveHistory, DeniedConsumesToken and       *)
(* CapVerifyBudget in their code position reproduce a finding each; in     *)
(* their fixed position the invariants hold.  AcceptPrevious and Rotation  *)
(* state the cookie-key window the rotation code is written against.       *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    None, Addrs, Tokens, Expire, Cid, MaxClock, CookieLife, RotatePeriod,
    MaxClients, HistoryCap, AdmittedCap, VerifyBudget, HandshakeDeadline,
    Rotation, AcceptPrevious,
    RefuseAdmittedAtResponse, AdmitExpiredAtResponse,
    EvictLiveHistory, DeniedConsumesToken, CapVerifyBudget

ASSUME /\ MaxClock \in Nat \ {0} /\ CookieLife \in Nat \ {0} /\ RotatePeriod \in Nat \ {0}
       /\ MaxClients \in Nat \ {0} /\ HistoryCap \in Nat \ {0} /\ AdmittedCap \in Nat \ {0}
       /\ VerifyBudget \in Nat \ {0} /\ HandshakeDeadline \in Nat \ {0}
       /\ Rotation \in BOOLEAN /\ AcceptPrevious \in BOOLEAN
       /\ RefuseAdmittedAtResponse \in BOOLEAN /\ AdmitExpiredAtResponse \in BOOLEAN
       /\ EvictLiveHistory \in BOOLEAN /\ DeniedConsumesToken \in BOOLEAN
       /\ CapVerifyBudget \in BOOLEAN
       /\ Expire \in [Tokens -> Nat] /\ Cid \in [Tokens -> Nat]

Gens == 1..3
ExpTime(tk) == IF Expire[tk] = 0 THEN MaxClock + 1 ELSE Expire[tk]

VARIABLES clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots, others,
          gen, hasPrev, verifies, deniedPulse, admits, lateAdmit, evictedLive
vars == <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots, others,
          gen, hasPrev, verifies, deniedPulse, admits, lateAdmit, evictedLive>>

LiveGens == {gen} \cup (IF AcceptPrevious /\ hasPrev THEN {IF gen = 1 THEN 3 ELSE gen - 1} ELSE {})

TokExpired(tk) == ExpTime(tk) <= clock
HistLive(tk) == IF history[tk] = None THEN FALSE ELSE history[tk].exp > clock
HistAddr(tk) == IF history[tk] = None THEN tk ELSE history[tk].addr
HistOrder(u) == IF history[u] = None THEN insertOrder + MaxClock + 1 ELSE history[u].order
AdmLive(tk) == admitted[tk] # 0 /\ admitted[tk] > clock
HistCount == Cardinality({tk \in Tokens : history[tk] # None})
AdmCount == Cardinality({tk \in Tokens : admitted[tk] # 0})
NumFree == MaxClients - Cardinality(slots) - others
Live(a) == map[a] # None

Init ==
    /\ clock = 0 /\ gen = 1 /\ hasPrev = FALSE /\ verifies = 0
    /\ history = [t \in Tokens |-> None] /\ admitted = [t \in Tokens |-> 0] /\ insertOrder = 0
    /\ map = [a \in Addrs |-> None] /\ mapGen = [a \in Addrs |-> 0] /\ mapBorn = [a \in Addrs |-> 0]
    /\ slots = {} /\ others = 0
    /\ deniedPulse = FALSE /\ admits = [t \in Tokens |-> 0]
    /\ lateAdmit = FALSE /\ evictedLive = FALSE

IsExpired(u) == IF history[u] = None THEN FALSE ELSE history[u].exp <= clock

\* rocketnet_used_token_history_record: no-op if live; else a free slot,
\* then an expired slot, then (code) the oldest entry, live or not.
HistoryRecord(tk, a) ==
    IF HistLive(tk) THEN history
    ELSE IF HistCount < HistoryCap \/ history[tk] # None
      THEN [history EXCEPT ![tk] = [addr |-> a, exp |-> ExpTime(tk), order |-> insertOrder]]
      ELSE LET expired == {u \in Tokens : IsExpired(u)}
           IN IF expired # {}
                THEN LET v == CHOOSE victim \in expired : TRUE
                     IN [history EXCEPT ![v] = None, ![tk] = [addr |-> a, exp |-> ExpTime(tk), order |-> insertOrder]]
                ELSE IF EvictLiveHistory
                  THEN LET present == {x \in Tokens : history[x] # None}
                           v == CHOOSE victim \in present : \A w \in present : HistOrder(victim) <= HistOrder(w)
                       IN [history EXCEPT ![v] = None, ![tk] = [addr |-> a, exp |-> ExpTime(tk), order |-> insertOrder]]
                  ELSE history

EvictsLive(tk) ==
    /\ ~HistLive(tk) /\ HistCount >= HistoryCap /\ history[tk] = None
    /\ ~(\E u \in Tokens : IsExpired(u))
    /\ EvictLiveHistory

\* A request past the cookie gate, from any address, holding any token,
\* under a cookie of generation g.  The attacker is this quantification:
\* it presents a captured or shared token from any source, and the gate
\* is the only thing that has run before it.
Request(a, tok, g) ==
    /\ ~Live(a)
    /\ IF g \notin LiveGens \/ TokExpired(tok) THEN FALSE
       ELSE IF HistLive(tok) /\ HistAddr(tok) # a THEN FALSE
       ELSE IF AdmLive(tok) THEN FALSE
       ELSE IF CapVerifyBudget /\ verifies >= VerifyBudget THEN FALSE
       ELSE TRUE
    /\ verifies' = verifies + 1
    /\ evictedLive' = (evictedLive \/ (\E u \in Tokens : u # tok /\ HistLive(u) /\ (HistoryRecord(tok, a))[u] = None))
    /\ history' = HistoryRecord(tok, a)
    /\ insertOrder' = IF HistLive(tok) THEN insertOrder ELSE insertOrder + 1
    /\ map' = [map EXCEPT ![a] = tok] /\ mapGen' = [mapGen EXCEPT ![a] = g] /\ mapBorn' = [mapBorn EXCEPT ![a] = clock]
    /\ UNCHANGED <<clock, admitted, slots, others, gen, hasPrev, deniedPulse, admits, lateAdmit>>

\* The response rung: the packet opened under the mapping.  The code
\* checks the client id, then the slots, then the admitted set, whose
\* add succeeds when the token is already present.
AdmittedAddOk(tk) == AdmLive(tk) \/ AdmCount < AdmittedCap \/ admitted[tk] # 0
                    \/ \E u \in Tokens : admitted[u] # 0 /\ admitted[u] <= clock
AdmittedAfterAdd(tk) ==
    IF AdmLive(tk) THEN admitted
    ELSE IF AdmCount < AdmittedCap \/ admitted[tk] # 0 THEN [admitted EXCEPT ![tk] = ExpTime(tk)]
    ELSE LET v == CHOOSE victim \in Tokens : admitted[victim] # 0 /\ admitted[victim] <= clock
         IN [admitted EXCEPT ![v] = 0, ![tk] = ExpTime(tk)]

Response(a) ==
    /\ Live(a)
    /\ LET tok == map[a] IN
       IF \E s \in slots : s.cid = Cid[tok] THEN UNCHANGED vars
       ELSE IF TokExpired(tok) /\ ~AdmitExpiredAtResponse THEN UNCHANGED vars
       ELSE IF AdmLive(tok) /\ RefuseAdmittedAtResponse THEN UNCHANGED vars
       ELSE IF NumFree <= 0 \/ ~AdmittedAddOk(tok) THEN
            /\ deniedPulse' = ~deniedPulse
            /\ admitted' = IF DeniedConsumesToken THEN AdmittedAfterAdd(tok) ELSE admitted
            /\ UNCHANGED <<clock, history, insertOrder, map, mapGen, mapBorn, slots, others,
                           gen, hasPrev, verifies, admits, lateAdmit, evictedLive>>
       ELSE
            /\ admitted' = AdmittedAfterAdd(tok)
            /\ slots' = slots \cup {[cid |-> Cid[tok], tok |-> tok, addr |-> a]}
            /\ map' = [map EXCEPT ![a] = None]
            /\ admits' = [admits EXCEPT ![tok] = @ + 1]
            /\ lateAdmit' = (lateAdmit \/ TokExpired(tok))
            /\ UNCHANGED <<clock, history, insertOrder, mapGen, mapBorn, others, gen, hasPrev,
                           verifies, deniedPulse, evictedLive>>

OtherJoins == /\ others < MaxClients /\ NumFree > 0 /\ others' = others + 1
              /\ UNCHANGED <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots,
                             gen, hasPrev, verifies, deniedPulse, admits, lateAdmit, evictedLive>>
OtherLeaves == /\ others > 0 /\ others' = others - 1
              /\ UNCHANGED <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots,
                             gen, hasPrev, verifies, deniedPulse, admits, lateAdmit, evictedLive>>
Disconnect(a) == /\ \E s \in slots : s.addr = a
                 /\ slots' = {s \in slots : s.addr # a}
                 /\ UNCHANGED <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, others,
                                gen, hasPrev, verifies, deniedPulse, admits, lateAdmit, evictedLive>>

Tick ==
    /\ clock' = IF clock < MaxClock THEN clock + 1 ELSE clock
    /\ verifies' = 0
    /\ IF Rotation /\ clock' % RotatePeriod = 0 /\ clock' # clock
         THEN gen' = (gen % 3) + 1 /\ hasPrev' = TRUE
         ELSE UNCHANGED <<gen, hasPrev>>
    /\ map' = [a \in Addrs |-> IF Live(a) /\ clock' >= mapBorn[a] + HandshakeDeadline THEN None ELSE map[a]]
    /\ UNCHANGED <<history, admitted, insertOrder, mapGen, mapBorn, slots, others,
                   deniedPulse, admits, lateAdmit, evictedLive>>

Next == (\E a \in Addrs, tok \in Tokens, g \in Gens : Request(a, tok, g))
        \/ (\E a \in Addrs : Response(a) \/ Disconnect(a))
        \/ OtherJoins \/ OtherLeaves \/ Tick
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)
SpecFair == Spec

TypeOK ==
    /\ clock \in 0..MaxClock /\ gen \in Gens /\ verifies \in 0..VerifyBudget + 1
    /\ \A t \in Tokens : admitted[t] \in Nat /\ admits[t] \in Nat
    /\ \A a \in Addrs : map[a] = None \/ map[a] \in Tokens

OneConnectionPerToken == \A t \in Tokens : admits[t] <= 1
NoLiveHistoryEviction == ~evictedLive
ExpiredNeverAdmitted == ~lateAdmit
DeniedTokenStaysUsable == [][deniedPulse' # deniedPulse => admitted' = admitted]_vars
RotationWindow == [][\A a \in Addrs : (~Live(a) /\ Live(a)') => mapGen'[a] \in LiveGens']_vars
VerifyBudgetRespected == CapVerifyBudget => verifies <= VerifyBudget
TablesBounded == HistCount <= HistoryCap /\ AdmCount <= AdmittedCap
                 /\ Cardinality(slots) + others <= MaxClients
CookieRaceResolves == \A a \in Addrs, t \in Tokens :
    (Live(a) /\ map[a] = t) ~> (admits[t] = 1 \/ ~Live(a))

SafetyInvariants == TypeOK /\ OneConnectionPerToken /\ NoLiveHistoryEviction
                    /\ ExpiredNeverAdmitted /\ VerifyBudgetRespected /\ TablesBounded
=============================================================================
