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
(*                                                                         *)
(* FloodSpec is the failed-signature budget under a forged-request flood   *)
(* (rocketnet_signature_verify_budget_allow and _record_failure,           *)
(* sdk/rocketnet_server_protocol.cpp): forging sources, each past the      *)
(* cookie gate at its own address, fail the verify until the window's     *)
(* budget is spent and the source table is full, while one honest client *)
(* with a valid token connects.  RefuseUnremembered TRUE is the code       *)
(* before 2026-10-20: a full table refused every source it could not       *)
(* remember, so the honest client was locked out for the window.  FALSE   *)
(* is the code now: only a source with a recorded failure is refused.     *)
(* The flood's clock moves forward and steps back (an NTP step, a VM       *)
(* resume), and the window rolls over when the clock passes it or steps    *)
(* back before its start.  BackwardsRollover FALSE is the code before      *)
(* 2026-10-24: the one-sided rollover, under which a clock stepped back    *)
(* keeps a spent window open for the length of the correction             *)
(* (FloodWindowBounded).                                                   *)
(* Spec leaves the flood variables at their initial values.                *)
(*                                                                         *)
(* The request-claim table (rocketnet_used_token_history_claim_can_record, *)
(* _claim_record and _claim_release, sdk/rocketnet_token_tables.cpp) at    *)
(* the slot grain: ClaimSlots slots, a free list and an order list of live *)
(* claims.  Request checks the claim and the O(1) capacity rule before the *)
(* verify and records the claim after it; RequestRefused records and then  *)
(* releases it (a refusal past the verify, or a same-address request whose *)
(* live claim it then releases, the release from the middle of the order   *)
(* list); a mapping swept by Tick at its handshake deadline releases       *)
(* nothing, its claim expires with it.  ReleaseToFree FALSE is the code   *)
(* before the free list, without its scan: a release zeroes the deadline  *)
(* and leaves the slot in place, so the O(1) rule no longer equals the    *)
(* scan                                                                    *)
(* (CanRecordMatchesScan).  ReleaseChecksLive FALSE pushes the slot on the *)
(* free list without unlinking it from its chain, so the released entry   *)
(* is still found: a second release pushes the slot twice, and a record   *)
(* of the same token puts it on the order list while it is still on the  *)
(* free list (FreeAndOrderPartitionSlots).                                 *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, Sequences

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
          gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied,
          failCount, failTable, honest, floodVerifies, fclock, windowStart,
          claimSlot, freeList, orderList, claimReplaced
claimVars == <<claimSlot, freeList, orderList, claimReplaced>>
tableVars == <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots, others,
               gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied,
               claimVars>>
floodVars == <<failCount, failTable, honest, floodVerifies, fclock, windowStart>>
vars == <<tableVars, floodVars>>

\* The failed-signature budget's small instance (sdk/rocketnet_server.h:
\* ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET 90, _TABLE_SLOTS 64).  These
\* are definitions, not constants, so the configurations of Spec need no
\* new lines; a reversed witness overrides RefuseUnremembered with <-.
\* The budget is spent before the table fills only if FailBudget > FailSlots,
\* as in the code; two forgers against one slot overflow the table.
FailBudget == 2
FailSlots == 1
Forgers == {"F1", "F2"}
HonestAddr == "H"
RefuseUnremembered == FALSE
\* The window (ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS, 10) in
\* flood clock ticks, the flood clock's range, and the backwards rollover
\* (a reversed witness overrides BackwardsRollover with <-).
FailWindow == 2
FloodMaxClock == 3
BackwardsRollover == TRUE

\* The request-claim table's small instance (the code sizes it at
\* ROCKETNET_USED_TOKEN_HISTORY_RATIO x max_clients).  Definitions, not
\* constants, as RefuseUnremembered is; a reversed witness overrides a
\* switch with <-.
ClaimSlots == 1..2
\* the tokens the claim actions take: one token fewer than Tokens keeps the
\* instance inside the checking budget; "te" (the expiring token) requests
\* without the claim rungs, as before the claim table was modelled
ClaimTokens == {"th", "tf"}
ReleaseToFree == TRUE
ReleaseChecksLive == TRUE

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

\* ---------------------------------------------------------------------------
\* The request-claim table.  Its state is a record C: C.slot[s] holds the
\* token, the address and the deadline last written to slot s (a released
\* slot keeps its token and address, as the code's arrays do; its deadline
\* is zeroed), C.free the free list (head first), C.order the order list
\* of claims (head first), C.replaced the ghost that fires when a write
\* lands on another token's live claim.  The operations are functions of C
\* so a refusal past the verify composes the record and the release in one
\* step.  A slot is in its token's chain while it is on the order list;
\* with ReleaseChecksLive FALSE a released slot stays in its chain.
EmptyClaim == [tok |-> None, addr |-> None, dl |-> 0]
SeqSet(q) == {q[i] : i \in 1..Len(q)}
Remove(q, x) == SelectSeq(q, LAMBDA y : y # x)
ClaimState == [slot |-> claimSlot, free |-> freeList, order |-> orderList, replaced |-> claimReplaced]
SetClaimState(C) == /\ claimSlot' = C.slot /\ freeList' = C.free
                    /\ orderList' = C.order /\ claimReplaced' = C.replaced
Chained(C, s) == IF ReleaseChecksLive THEN s \in SeqSet(C.order)
                 ELSE s \in SeqSet(C.order) \/ C.slot[s].tok # None
\* rocketnet_token_table_find_any on the claim table
Found(C, tk) == {s \in ClaimSlots : Chained(C, s) /\ C.slot[s].tok = tk}
LiveAt(C, s) == C.slot[s].dl > clock
\* rocketnet_used_token_history_claim_check: no live claim on tk from
\* another address
ClaimCheckOk(tk, a) == \A s \in Found(ClaimState, tk) : LiveAt(ClaimState, s) => claimSlot[s].addr = a
\* rocketnet_used_token_history_claim_can_record, the O(1) rule: the token
\* is in the table, or the free list is not empty, or the order head's
\* deadline has passed
CanRecord(tk) == \/ Found(ClaimState, tk) # {}
                 \/ freeList # <<>>
                 \/ (orderList # <<>> /\ ~LiveAt(ClaimState, Head(orderList)))
\* the code before the free list: the same, with a scan over every slot
CanRecordScan(tk) == \/ Found(ClaimState, tk) # {}
                     \/ freeList # <<>>
                     \/ \E s \in ClaimSlots : ~LiveAt(ClaimState, s)
\* clock + HandshakeDeadline, held at MaxClock + 1: the clock never passes
\* MaxClock, so every live-or-expired answer is unchanged, and the clamp is
\* monotone, so it keeps the order list's deadline order
ClaimDeadline == IF clock + HandshakeDeadline > MaxClock THEN MaxClock + 1 ELSE clock + HandshakeDeadline

\* write slot s for tk at a and move it to the order list's tail
Write(C, s, tk, a, fl, ol) ==
    [slot |-> [C.slot EXCEPT ![s] = [tok |-> tk, addr |-> a, dl |-> ClaimDeadline]],
     free |-> fl,
     order |-> Append(Remove(ol, s), s),
     replaced |-> C.replaced \/ (C.slot[s].tok # tk /\ C.slot[s].tok # None
                                /\ LiveAt(C, s) /\ s \in SeqSet(C.order))]

\* rocketnet_used_token_history_claim_record, called only once CanRecord
\* holds: the token's own entry (live: a no-op; expired: rewritten and
\* moved to the tail), else the free list's head, else the order list's
\* head once its deadline has passed, else (never reached) a refusal.
Record(C, tk, a) ==
    IF Found(C, tk) # {}
      THEN LET s == CHOOSE x \in Found(C, tk) : TRUE
           IN IF LiveAt(C, s) THEN C ELSE Write(C, s, tk, a, C.free, C.order)
    ELSE IF C.free # <<>>
      THEN Write(C, Head(C.free), tk, a, Tail(C.free), C.order)
    ELSE IF C.order # <<>> /\ ~LiveAt(C, Head(C.order))
      THEN Write(C, Head(C.order), tk, a, C.free, Tail(C.order))
    ELSE C

\* rocketnet_used_token_history_claim_release: when the entry is found and
\* its address matches, it leaves its chain and the order list and its
\* slot is pushed on the free list (ReleaseToFree), or (the code before)
\* only its deadline is zeroed and the slot stays where it is.  In the
\* code a slot off every chain is never read until it is written again, so
\* the fixed code clears it here, which keeps the state space small; the
\* witnesses keep the stale contents their bugs read.
Release(C, tk, a) ==
    IF \E s \in Found(C, tk) : C.slot[s].addr = a
      THEN LET s == CHOOSE x \in Found(C, tk) : C.slot[x].addr = a
           IN [slot |-> [C.slot EXCEPT ![s] = IF ReleaseToFree /\ ReleaseChecksLive THEN EmptyClaim
                                                ELSE [C.slot[s] EXCEPT !.dl = 0]],
               free |-> IF ReleaseToFree THEN <<s>> \o C.free ELSE C.free,
               order |-> IF ReleaseToFree THEN Remove(C.order, s) ELSE C.order,
               replaced |-> C.replaced]
      ELSE C

Init ==
    /\ clock = 0 /\ gen = 1 /\ hasPrev = FALSE /\ prevUntil = 0 /\ verifies = 0
    /\ history = [t \in Tokens |-> None] /\ admitted = [t \in Tokens |-> 0] /\ insertOrder = 0
    /\ map = [a \in Addrs |-> None] /\ mapGen = [a \in Addrs |-> 0] /\ mapBorn = [a \in Addrs |-> 0]
    /\ slots = {} /\ others = 0
    /\ deniedPulse = FALSE /\ admits = [t \in Tokens |-> 0]
    /\ lateAdmit = FALSE /\ evictedLive = FALSE /\ churnDenied = FALSE
    /\ failCount = 0 /\ failTable = {} /\ honest = "start" /\ floodVerifies = 0
    /\ fclock = 0 /\ windowStart = 0
    /\ claimSlot = [s \in ClaimSlots |-> EmptyClaim]
    /\ freeList = [i \in 1..Cardinality(ClaimSlots) |-> i]
    /\ orderList = <<>>
    /\ claimReplaced = FALSE

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
\* The rungs a request passes before its verify: the cookie generation,
\* the expiry, the history from another address, the admitted set (with
\* HoldUntilExpire), the per-update budget, then the claim from another
\* address (claim_check) and the claim table's capacity (claim_can_record,
\* read-only).  Past the verify the claim is recorded (claim_record).
RequestRungs(a, tok, g) ==
    /\ ~Live(a)
    \* a connected address provokes nothing from the request path
    \* (CONNECTION_REQUEST_ALREADY_CONNECTED)
    /\ ~(\E s \in slots : s.addr = a)
    /\ IF g \notin LiveGens \/ TokExpired(tok) THEN FALSE
       ELSE IF HistLive(tok) /\ HistAddr(tok) # a THEN FALSE
       ELSE IF Used(tok) THEN FALSE
       ELSE IF CapVerifyBudget /\ verifies >= VerifyBudget THEN FALSE
       ELSE IF tok \in ClaimTokens /\ ~ClaimCheckOk(tok, a) THEN FALSE
       ELSE IF tok \in ClaimTokens /\ ~CanRecord(tok) THEN FALSE
       ELSE TRUE
    /\ verifies' = verifies + 1

Request(a, tok, g) ==
    /\ RequestRungs(a, tok, g)
    /\ IF tok \in ClaimTokens THEN SetClaimState(Record(ClaimState, tok, a)) ELSE UNCHANGED claimVars
    /\ IF BindAtRequest THEN Bind(tok, a, FALSE)
       ELSE UNCHANGED <<history, insertOrder, evictedLive>>
    /\ map' = [map EXCEPT ![a] = tok] /\ mapGen' = [mapGen EXCEPT ![a] = g] /\ mapBorn' = [mapBorn EXCEPT ![a] = clock]
    /\ UNCHANGED <<clock, admitted, slots, others, gen, hasPrev, prevUntil, deniedPulse, admits, lateAdmit, churnDenied>>

\* A request refused past the verify (the sealed block does not open, the
\* client id is connected, the server is full, a small-order key, no
\* mapping): the claim is recorded and released in the same step, and no
\* mapping is made.
RequestRefused(a, tok, g) ==
    /\ tok \in ClaimTokens
    /\ RequestRungs(a, tok, g)
    /\ SetClaimState(Release(Record(ClaimState, tok, a), tok, a))
    /\ UNCHANGED <<clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots, others,
                   gen, hasPrev, prevUntil, deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>


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
    /\ UNCHANGED claimVars
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
              /\ UNCHANGED <<claimVars, clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots,
                             gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>
OtherLeaves == /\ others > 0 /\ others' = others - 1
               /\ UNCHANGED <<claimVars, clock, history, admitted, insertOrder, map, mapGen, mapBorn, slots,
                              gen, hasPrev, prevUntil, verifies, deniedPulse, admits, lateAdmit, evictedLive, churnDenied>>
Disconnect(a) == /\ \E s \in slots : s.addr = a
                 /\ slots' = {s \in slots : s.addr # a}
                 /\ UNCHANGED <<claimVars, clock, history, admitted, insertOrder, map, mapGen, mapBorn, others,
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
                   deniedPulse, admits, lateAdmit, evictedLive, churnDenied, claimVars>>

Next == /\ \/ (\E a \in Addrs, tok \in Tokens, g \in Gens : Request(a, tok, g) \/ RequestRefused(a, tok, g))
           \/ (\E a \in Addrs : Response(a) \/ Disconnect(a))
           \/ OtherJoins \/ OtherLeaves \/ Tick
        /\ UNCHANGED floodVars
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)
SpecFair == Spec

\* ---------------------------------------------------------------------------
\* The flood.  rocketnet_signature_verify_budget_allow: while the window's
\* failure count is under the budget everyone verifies; once it is spent a
\* source with a recorded failure is refused.  With RefuseUnremembered (the
\* code before 2026-10-20, `if ( empty_slot == -1 ) return 0;`) a source
\* the full table does not hold is refused too.  Every request here has
\* passed the cookie gate and spends one unit of the per-update handshake
\* work budget (VerifyBudget, ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE),
\* checked before this gate.  The flood keeps the window spent for longer
\* than one honest attempt runs (the handshake deadline, 5 s, is under the
\* window, 10 s).
\*
\* rocketnet_signature_verify_budget_decay runs at the top of both calls:
\* the window rolls over, clearing the count and the table, when the clock
\* has passed it or, with BackwardsRollover, stepped back before its start.
\* CountNow and TableNow are the budget as the call sees it after the decay.
Rolled == \/ fclock >= windowStart + FailWindow
          \/ (BackwardsRollover /\ fclock < windowStart)
CountNow == IF Rolled THEN 0 ELSE failCount
TableNow == IF Rolled THEN {} ELSE failTable
WindowNow == IF Rolled THEN fclock ELSE windowStart

FailAllow(a) ==
    \/ CountNow < FailBudget
    \/ /\ a \notin TableNow
       /\ ~(RefuseUnremembered /\ Cardinality(TableNow) >= FailSlots)

\* a forged request from f: past the work budget and the failure gate, the
\* verify fails and rocketnet_signature_verify_budget_record_failure runs:
\* the count rises (held at FailBudget here; the code only compares it
\* against the budget), and the source is remembered while a slot is free.
Forge(f) ==
    /\ floodVerifies < VerifyBudget
    /\ FailAllow(f)
    /\ floodVerifies' = floodVerifies + 1
    /\ windowStart' = WindowNow
    /\ failCount' = IF CountNow < FailBudget THEN CountNow + 1 ELSE CountNow
    /\ failTable' = IF f \notin TableNow /\ Cardinality(TableNow) < FailSlots
                     THEN TableNow \cup {f} ELSE TableNow
    /\ UNCHANGED <<honest, fclock, tableVars>>

\* the honest client's request: a valid token, so the verify passes and
\* nothing is recorded; it gets the challenge.  It retransmits at the send
\* rate, so a request refused by the work budget arrives again later.
HonestRequest ==
    /\ honest = "start"
    /\ floodVerifies < VerifyBudget
    /\ FailAllow(HonestAddr)
    /\ floodVerifies' = floodVerifies + 1
    /\ honest' = "challenged"
    \* the allow call ran the decay
    /\ windowStart' = WindowNow /\ failCount' = CountNow /\ failTable' = TableNow
    /\ UNCHANGED <<fclock, tableVars>>

\* the response opens under the handshake key: no failure budget applies
HonestResponse ==
    /\ honest = "challenged"
    /\ honest' = "connected"
    /\ UNCHANGED <<failCount, failTable, floodVerifies, fclock, windowStart, tableVars>>

\* the top of an update resets the work budget
FloodUpdate ==
    /\ floodVerifies > 0
    /\ floodVerifies' = 0
    /\ UNCHANGED <<failCount, failTable, honest, fclock, windowStart, tableVars>>

\* the host clock: it moves forward, or is stepped back to any earlier
\* time (an NTP step, a VM resume).  The timestamp is the unix wall clock
\* whenever no game clock epoch is configured
\* (sdk/rocketnet_server_transport.cpp, rocketnet_server_transport_update).
FloodClockForward ==
    /\ fclock < FloodMaxClock
    /\ fclock' = fclock + 1
    /\ UNCHANGED <<failCount, failTable, honest, floodVerifies, windowStart, tableVars>>
FloodClockBack ==
    /\ \E t \in 0..(fclock - 1) : fclock' = t
    /\ UNCHANGED <<failCount, failTable, honest, floodVerifies, windowStart, tableVars>>

FloodNext == (\E f \in Forgers : Forge(f)) \/ HonestRequest \/ HonestResponse \/ FloodUpdate
             \/ FloodClockForward \/ FloodClockBack
\* the honest request is enabled again after every update while the flood
\* takes the budget, hence strong fairness for it
FloodSpec == Init /\ [][FloodNext]_vars
             /\ SF_vars(HonestRequest) /\ WF_vars(HonestResponse) /\ WF_vars(FloodUpdate)

\* No global lockout: an honest client with a valid token connects while
\* forging sources keep the failed-signature budget spent and the source
\* table full.
HonestConnectsUnderFlood == <>(honest = "connected")
\* A recorded failure refuses its source for at most the window length of
\* clock movement in either direction: whenever a source would be refused
\* now, the window it was recorded in began no later than the clock and
\* less than one window before it.  BASE's one-sided rollover breaks it on
\* a backwards step (MCRNSecurityTablesBrokenBackwardsWindow.cfg).
FloodWindowBounded ==
    \A a \in Forgers \cup {HonestAddr} :
        ~FailAllow(a) => (windowStart <= fclock /\ fclock < windowStart + FailWindow)
\* the work budget bounds the verifies of one update, and the table never
\* passes its slots
FloodInvariants == /\ floodVerifies \in 0..VerifyBudget
                   /\ failCount \in 0..FailBudget
                   /\ failTable \subseteq Forgers /\ Cardinality(failTable) <= FailSlots
                   /\ honest \in {"start", "challenged", "connected"}
                   /\ fclock \in 0..FloodMaxClock /\ windowStart \in 0..FloodMaxClock
                   /\ FloodWindowBounded

TypeOK ==
    /\ clock \in 0..MaxClock /\ gen \in Gens /\ verifies \in 0..VerifyBudget + 1
    /\ hasPrev \in BOOLEAN /\ prevUntil \in 0..(MaxClock + RotatePeriod)
    /\ \A t \in Tokens : admitted[t] \in Nat /\ admits[t] \in 0..1
    /\ \A a \in Addrs : map[a] = None \/ map[a] \in Tokens

\* TokenUsedOnce, the headline rule, as the server enforces it: one token,
\* one live connection.  No two client slots hold one token at once.  A
\* token whose connection has closed may connect again inside its short
\* expiry: the accepted residual risk (docs/SESSION.md, Token reuse).
OneTokenOneLiveConnection == \A t \in Tokens : Cardinality({s \in slots : s.tok = t}) <= 1
\* Ordinary churn is never denied: a fresh token at a server with a free
\* slot is never refused or denied at the response.
ChurnNeverDenied == ~churnDenied
\* A token is bound in the history only by a response that opened under
\* it, from the address it binds, so a replay from an address that holds
\* no keys binds nothing (STANDARD.md, Processing Connection Response
\* Packets).  NoBindToNonHolder is its consequence for the attacker: no
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

\* The request-claim table (sdk/rocketnet_token_tables.cpp).  Every slot is
\* on exactly one of the free list or the order list, and neither list
\* holds a slot twice.
FreeAndOrderPartitionSlots ==
    /\ SeqSet(freeList) \cap SeqSet(orderList) = {}
    /\ SeqSet(freeList) \cup SeqSet(orderList) = ClaimSlots
    /\ Len(freeList) = Cardinality(SeqSet(freeList))
    /\ Len(orderList) = Cardinality(SeqSet(orderList))
\* the order list is deadline order, so its head has the earliest deadline
OrderIsDeadlineOrder ==
    \A i, j \in 1..Len(orderList) : i < j => claimSlot[orderList[i]].dl <= claimSlot[orderList[j]].dl
\* a record never lands on another token's live claim
LiveClaimNeverReplaced == ~claimReplaced
\* the O(1) capacity answer equals the scan's, for every token
CanRecordMatchesScan == \A tk \in ClaimTokens : CanRecord(tk) = CanRecordScan(tk)
TablesBounded == HistCount <= HistoryCap /\ AdmCount <= AdmittedCap
                 /\ Cardinality(slots) + others <= MaxClients
CookieRaceResolves == \A a \in Addrs, t \in Tokens :
    (Live(a) /\ map[a] = t) ~> (admits[t] >= 1 \/ ~Live(a))

SafetyInvariants == TypeOK /\ OneTokenOneLiveConnection /\ ChurnNeverDenied /\ BoundOnlyToOpener /\ NoBindToNonHolder
                    /\ NoLiveHistoryEviction
                    /\ ExpiredNeverAdmitted /\ VerifyBudgetRespected /\ TablesBounded
                    /\ FreeAndOrderPartitionSlots /\ OrderIsDeadlineOrder /\ LiveClaimNeverReplaced
                    /\ CanRecordMatchesScan
=============================================================================
