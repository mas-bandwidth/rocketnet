--------------------------- MODULE RNSecurityTables ---------------------------
(***************************************************************************)
(* rocketnet: the security tables, with an attacker in the model.          *)
(*                                                                         *)
(* The used-token history (sdk/rocketnet_token_tables.cpp), the          *)
(* one-live-connection rung, the cookie gate and its key                   *)
(* rotation (sdk/rocketnet_server_protocol.cpp, the cookie gate,           *)
(* rocketnet_server_protocol_rotate_cookie_key_if_due,                     *)
(* rocketnet_server_process_opened_request and                             *)
(* rocketnet_server_process_connection_response), and the failed-verify    *)
(* budget (sdk/rocketnet_server.h, 64 table slots), as TRANSPORT.md        *)
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
(* their fixed position the invariants hold.  HoldUntilExpire TRUE is    *)
(* the code before 2026-10-18: an admitted-token set of AdmittedCap       *)
(* entries, each held until its token expires, which denies ordinary      *)
(* churn (ChurnNeverDenied); FALSE is the code now: no set, and a token   *)
(* is refused only while a connection holds it: by the history from      *)
(* another address and by the client id it carries at the response       *)
(* (OneTokenOneLiveConnection).  CheckCidAtResponse FALSE drops the       *)
(* client-id rung, the witness for that invariant.  Rotation, AcceptPrevious *)
(* and PreviousOutlivesWindow state the cookie-key rotation window and     *)
(* its stall bound.  BindAtRequest TRUE is the code before 2026-10-17:    *)
(* the request records the history entry, so a replay from an address     *)
(* that holds no keys binds the token.  The same change made the code's    *)
(* RefuseAdmittedAtResponse TRUE.  Holds[t] is the set of addresses        *)
(* holding token t's keys; only they can open a response, and every        *)
(* other address is the replaying attacker.                                *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    None, Addrs, Tokens, Expire, Cid, MaxClock, CookieLife, RotatePeriod,
    MaxClients, HistoryCap, AdmittedCap, VerifyBudget, HandshakeDeadline,
    Rotation, AcceptPrevious,
    RefuseAdmittedAtResponse, AdmitExpiredAtResponse,
    EvictLiveHistory, DeniedConsumesToken, CapVerifyBudget,
    PreviousOutlivesWindow, BindAtRequest, Holds,
    HoldUntilExpire, CheckCidAtResponse

ASSUME /\ MaxClock \in Nat \ {0} /\ CookieLife \in Nat \ {0} /\ RotatePeriod \in Nat \ {0}
       /\ MaxClients \in Nat \ {0} /\ HistoryCap \in Nat \ {0} /\ AdmittedCap \in Nat \ {0}
       /\ VerifyBudget \in Nat \ {0} /\ HandshakeDeadline \in Nat \ {0}
       /\ Rotation \in BOOLEAN /\ AcceptPrevious \in BOOLEAN
       /\ RefuseAdmittedAtResponse \in BOOLEAN /\ AdmitExpiredAtResponse \in BOOLEAN
       /\ EvictLiveHistory \in BOOLEAN /\ DeniedConsumesToken \in BOOLEAN
       /\ CapVerifyBudget \in BOOLEAN /\ PreviousOutlivesWindow \in BOOLEAN
       /\ BindAtRequest \in BOOLEAN /\ Holds \in [Tokens -> SUBSET Addrs]
       /\ HoldUntilExpire \in BOOLEAN /\ CheckCidAtResponse \in BOOLEAN
       /\ Expire \in [Tokens -> Nat] /\ Cid \in [Tokens -> Nat]

Gens == 1..3
ExpTime(tk) == IF Expire[tk] = 0 THEN MaxClock + 1 ELSE Expire[tk]

VARIABLES clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots, others,
          gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied
vars == <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots, others,
          gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>

PrevGen == IF gen = 1 THEN 3 ELSE gen - 1
NextRotate == gen * RotatePeriod
PrevStoppedAt == (gen - 1) * RotatePeriod
LiveGens == {gen} \cup (IF AcceptPrevious /\ hasPrev /\ clock < prevUntil THEN {PrevGen} ELSE {})

TokExpired(tk) == ExpTime(tk) <= clock
HistLive(tk) == IF history[tk] = None THEN FALSE ELSE history[tk].exp > clock
HistAddr(tk) == IF history[tk] = None THEN tk ELSE history[tk].addr
HistOrder(u) == IF history[u] = None THEN insertOrder + MaxClock + 1 ELSE history[u].order
AdmLive(tk) == admitted[tk] # 0 /\ admitted[tk] > clock
HistCount == Cardinality({tk \in Tokens : history[tk] # None})
AdmCount == Cardinality({tk \in Tokens : admitted[tk] # 0})
NumFree == MaxClients - Cardinality(slots) - others
\* The admitted-token set's rule, with HoldUntilExpire only: a token in
\* the set until it expires is refused.  The code now has no such rung:
\* one live connection per token rests on the history (another address),
\* the connected-address check and the client-id check.
Used(tk) == HoldUntilExpire /\ AdmLive(tk)
\* A fresh, unexpired, never-admitted token whose client id holds no slot,
\* at a server with a free slot: ordinary churn, never to be denied.
Fresh(tk) == ~TokExpired(tk) /\ NumFree > 0 /\ admits[tk] = 0 /\ ~(\E s \in slots : s.cid = Cid[tk])
Live(a) == map[a] # None

Init ==
    /\ clock = 0 /\ gen = 1 /\ hasPrev = FALSE /\ prevUntil = 0 /\ verifies = 0
    /\ history = [t \in Tokens |-> None] /\ admitted = [t \in Tokens |-> 0] /\ insertOrder = 0
    /\ map = [a \in Addrs |-> None] /\ mapGen = [a \in Addrs |-> 0] /\ mapBorn = [a \in Addrs |-> 0]
    /\ slots = {} /\ others = 0
    /\ deniedPulse = FALSE /\ admits = [t \in Tokens |-> 0]
    /\ lateAdmit = FALSE /\ evictedLive = FALSE /\ churnDenied = FALSE

IsExpired(u) == IF history[u] = None THEN FALSE ELSE history[u].exp <= clock

\* rocketnet_used_token_history_record: no-op if live; else a free slot,
\* then an expired slot, then (code) the oldest entry, live or not.  The
\* ghost field opened says whether the record was made by a response from
\* a that opened under tk's handshake key (TRUE) or by a request (FALSE).
HistoryRecord(tk, a, op) ==
    IF HistLive(tk) THEN history
    ELSE IF HistCount < HistoryCap \/ history[tk] # None
      THEN [history EXCEPT ![tk] = [addr |-> a, exp |-> ExpTime(tk), order |-> insertOrder, opened |-> op]]
      ELSE LET expired == {u \in Tokens : IsExpired(u)}
           IN IF expired # {}
                THEN LET v == CHOOSE victim \in expired : TRUE
                     IN [history EXCEPT ![v] = None, ![tk] = [addr |-> a, exp |-> ExpTime(tk), order |-> insertOrder, opened |-> op]]
                ELSE IF EvictLiveHistory
                  THEN LET present == {x \in Tokens : history[x] # None}
                           v == CHOOSE victim \in present : \A w \in present : HistOrder(victim) <= HistOrder(w)
                       IN [history EXCEPT ![v] = None, ![tk] = [addr |-> a, exp |-> ExpTime(tk), order |-> insertOrder, opened |-> op]]
                  ELSE history

EvictsLive(tk) ==
    /\ ~HistLive(tk) /\ HistCount >= HistoryCap /\ history[tk] = None
    /\ ~(\E u \in Tokens : IsExpired(u))
    /\ EvictLiveHistory

\* The history record and its ghosts: rocketnet_used_token_history_record
\* for token tk at address a.  An expired token is not recorded (the
\* request path refuses it at its top; the response path checks before
\* the record), and the insert order moves only when an entry is written:
\* a response can repeat, so a counter that moved on a no-op record would
\* be unbounded.
Bind(tk, a, op) ==
    IF TokExpired(tk) THEN UNCHANGED <<history, insertOrder, evictedLive>>
    ELSE /\ evictedLive' = (evictedLive \/ (\E u \in Tokens : u # tk /\ HistLive(u) /\ (HistoryRecord(tk, a, op))[u] = None))
         /\ history' = HistoryRecord(tk, a, op)
         /\ insertOrder' = IF HistoryRecord(tk, a, op) = history THEN insertOrder ELSE insertOrder + 1

\* A request past the cookie gate, from any address, holding any token,
\* under a cookie of generation g.  The attacker is this quantification:
\* it presents a captured or shared token from any source, and the gate
\* is the only thing that has run before it.
Request(a, tok, g) ==
    /\ ~Live(a)
    \* a connected address provokes nothing from the request path
    \* (CONNECTION_REQUEST_ALREADY_CONNECTED)
    /\ ~(\E s \in slots : s.addr = a)
    /\ IF g \notin LiveGens \/ TokExpired(tok) THEN FALSE
       ELSE IF HistLive(tok) /\ HistAddr(tok) # a THEN FALSE
       ELSE IF Used(tok) THEN FALSE
       ELSE IF CapVerifyBudget /\ verifies >= VerifyBudget THEN FALSE
       ELSE TRUE
    /\ verifies' = verifies + 1
    /\ IF BindAtRequest THEN Bind(tok, a, FALSE)
       ELSE UNCHANGED <<history, insertOrder, evictedLive>>
    /\ map' = [map EXCEPT ![a] = tok] /\ mapGen' = [mapGen EXCEPT ![a] = g] /\ mapBorn' = [mapBorn EXCEPT ![a] = clock]
    /\ UNCHANGED <<clock, admitted, slots, others, gen, hasPrev, prevUntil, deniedPulse, admits, lateAdmit, churnDenied>>

\* The response rung: the packet opened under the mapping, so only an
\* address in Holds[tok] takes this step.  The record runs first (unless
\* BindAtRequest, where the request made it).  The code then checks the
\* admitted set (RefuseAdmittedAtResponse, with HoldUntilExpire only) and
\* the client id (CheckCidAtResponse), both silent refusals, then the
\* expiry, then the slots, then, with HoldUntilExpire, the add, which
\* succeeds when the token is present.
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
       /\ a \in Holds[tok]
       /\ IF BindAtRequest THEN UNCHANGED <<history, insertOrder, evictedLive>>
          ELSE Bind(tok, a, TRUE)
       /\ IF (Used(tok) /\ RefuseAdmittedAtResponse) \/ ((\E s \in slots : s.cid = Cid[tok]) /\ CheckCidAtResponse) THEN
               /\ churnDenied' = (churnDenied \/ Fresh(tok))
               /\ UNCHANGED <<clock, admitted, map, mapGen, mapBorn, slots, others, gen, hasPrev,
                              prevUntil, verifies, deniedPulse, admits, lateAdmit>>
          ELSE IF TokExpired(tok) /\ ~AdmitExpiredAtResponse THEN
               /\ churnDenied' = (churnDenied \/ Fresh(tok))
               /\ UNCHANGED <<clock, admitted, map, mapGen, mapBorn, slots, others, gen, hasPrev,
                              prevUntil, verifies, deniedPulse, admits, lateAdmit>>
          ELSE IF NumFree <= 0 \/ (HoldUntilExpire /\ ~AdmittedAddOk(tok)) THEN
               /\ deniedPulse' = ~deniedPulse
               /\ churnDenied' = (churnDenied \/ Fresh(tok))
               /\ admitted' = IF DeniedConsumesToken /\ HoldUntilExpire THEN AdmittedAfterAdd(tok) ELSE admitted
               /\ UNCHANGED <<clock, map, mapGen, mapBorn, slots, others,
                              gen, hasPrev, prevUntil, verifies, admits, lateAdmit>>
          ELSE
               /\ admitted' = IF HoldUntilExpire THEN AdmittedAfterAdd(tok) ELSE admitted
               /\ slots' = slots \cup {[cid |-> Cid[tok], tok |-> tok, addr |-> a]}
               /\ map' = [map EXCEPT ![a] = None]
               \* the ghost saturates at 1 (ever admitted): a token may
               \* connect again after its connection closes, and the state
               \* stays finite
               /\ admits' = [admits EXCEPT ![tok] = 1]
               /\ lateAdmit' = (lateAdmit \/ TokExpired(tok))
               /\ UNCHANGED <<clock, mapGen, mapBorn, others, gen, hasPrev,
                              prevUntil, verifies, deniedPulse, churnDenied>>

OtherJoins == /\ others < MaxClients /\ NumFree > 0 /\ others' = others + 1
              /\ UNCHANGED <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots,
                             gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>
OtherLeaves == /\ others > 0 /\ others' = others - 1
               /\ UNCHANGED <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots,
                              gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>
Disconnect(a) == /\ \E s \in slots : s.addr = a
                 /\ slots' = {s \in slots : s.addr # a}
                 /\ UNCHANGED <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, others,
                                gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>

Tick ==
    /\ \/ clock' = IF clock < MaxClock THEN clock + 1 ELSE clock
       \/ (clock < NextRotate /\ NextRotate + RotatePeriod <= MaxClock
           /\ clock' = NextRotate + RotatePeriod)
    /\ verifies' = 0
    /\ IF Rotation /\ clock' # clock /\ clock' >= NextRotate
         THEN /\ gen' = (gen % 3) + 1
              /\ IF clock' < NextRotate + RotatePeriod
                   THEN hasPrev' = TRUE /\ prevUntil' = NextRotate + RotatePeriod
                   ELSE IF PreviousOutlivesWindow
                          THEN hasPrev' = TRUE /\ prevUntil' = clock' + RotatePeriod
                          ELSE hasPrev' = FALSE /\ prevUntil' = 0
         ELSE UNCHANGED <<gen, hasPrev, prevUntil>>
    /\ map' = [a \in Addrs |-> IF Live(a) /\ clock' >= mapBorn[a] + HandshakeDeadline THEN None ELSE map[a]]
    /\ UNCHANGED <<history, admitted, insertOrder, mapGen, mapBorn, slots, others,
                   deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>

Next == (\E a \in Addrs, tok \in Tokens, g \in Gens : Request(a, tok, g))
        \/ (\E a \in Addrs : Response(a) \/ Disconnect(a))
        \/ OtherJoins \/ OtherLeaves \/ Tick
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)
SpecFair == Spec

TypeOK ==
    /\ clock \in 0..MaxClock /\ gen \in Gens /\ verifies \in 0..VerifyBudget + 1
    /\ hasPrev \in BOOLEAN /\ prevUntil \in 0..(MaxClock + RotatePeriod)
    /\ \A t \in Tokens : admitted[t] \in Nat /\ admits[t] \in 0..1
    /\ \A a \in Addrs : map[a] = None \/ map[a] \in Tokens

\* TokenUsedOnce, the headline rule, as the server enforces it: one token,
\* one live connection.  No two client slots hold one token at once.  A
\* token whose connection has closed may connect again inside its short
\* expiry: the accepted residual risk.
OneTokenOneLiveConnection == \A t \in Tokens : Cardinality({s \in slots : s.tok = t}) <= 1
\* Ordinary churn is never denied: a fresh token at a server with a free
\* slot is never refused or denied at the response.
ChurnNeverDenied == ~churnDenied
\* A token is bound in the history only by a response that opened under
\* it, from the address it binds, so a replay from an address that holds
\* no keys binds nothing (TRANSPORT.md t12,
\* rule 11).  NoBindToNonHolder is its consequence for the attacker: no
\* address outside Holds[t] is ever bound to t.
BoundOnlyToOpener == \A t \in Tokens : history[t] # None => history[t].opened
NoBindToNonHolder == \A t \in Tokens : history[t] # None => history[t].addr \in Holds[t]
NoLiveHistoryEviction == ~evictedLive
ExpiredNeverAdmitted == ~lateAdmit
DeniedTokenStaysUsable == [][deniedPulse' # deniedPulse => admitted' = admitted]_vars
RotationWindow == [][\A a \in Addrs : (~Live(a) /\ Live(a)') => mapGen'[a] \in LiveGens']_vars
PreviousBoundedToOnePeriod ==
    [][/\ \A a \in Addrs : (~Live(a) /\ Live(a)' /\ mapGen'[a] # gen') => clock' < prevUntil'
       /\ prevUntil' <= (IF hasPrev' THEN PrevStoppedAt' + RotatePeriod ELSE 0)]_vars
InFlightCookieSurvivesRotation ==
    [][(CookieLife < RotatePeriod /\ gen' # gen /\ clock' - clock <= CookieLife)
        => gen \in LiveGens']_vars
VerifyBudgetRespected == CapVerifyBudget => verifies <= VerifyBudget
TablesBounded == HistCount <= HistoryCap /\ AdmCount <= AdmittedCap
                 /\ Cardinality(slots) + others <= MaxClients
CookieRaceResolves == \A a \in Addrs, t \in Tokens :
    (Live(a) /\ map[a] = t) ~> (admits[t] >= 1 \/ ~Live(a))

SafetyInvariants == TypeOK /\ OneTokenOneLiveConnection /\ ChurnNeverDenied /\ BoundOnlyToOpener /\ NoBindToNonHolder
                    /\ NoLiveHistoryEviction
                    /\ ExpiredNeverAdmitted /\ VerifyBudgetRespected /\ TablesBounded
=============================================================================
