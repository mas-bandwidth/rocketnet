-------------------------- MODULE MCReceiveOrder --------------------------
(***************************************************************************)
(* The committed run of ReceiveOrder in the ruled order.  Every invariant  *)
(* passes while requests repeat; claims end at binding or mapping removal. *)
(* The mapping table fills and empties.  A half-open mapping can also      *)
(* leave alone on its idle deadline, as idle timeout and integrity limit   *)
(* do in the code, and that removal releases its token's claim.  The       *)
(* handshake deadline is two steps, ClaimExpire then Sweep, with the rest  *)
(* of a receive batch between them, so the one-mapping and binding rules   *)
(* are checked over unexpired mappings and the bound address's own claim;  *)
(* the DeadlineWindow cfgs keep the strict forms and must fail.  The       *)
(* witnesses are the same module under the Broken cfgs, each moving one    *)
(* cheap rung behind a crypto rung, removing the token claim, writing the  *)
(* claim at its check rung before the signature, charging the work budget  *)
(* at the cookie gate, leaving a claim behind on mapping removal, response *)
(* binding or replacement, replacing a half-open mapping before any       *)
(* check has run, letting a token bound to the requesting address replace  *)
(* its half-open mapping, or turning the repeat rung off; TLC must         *)
(* reject them.  UpdateStart empties the repeat rung's table.  Requests    *)
(* may carry bound tokens, which the used rung refuses as the code does.   *)
(***************************************************************************)
EXTENDS ReceiveOrder

VARIABLES held, stale
MCVars == <<vars, held, stale>>

\* a replacement only past every rung: the witness evicts the address's
\* half-open mapping as the request starts, before any check has run
ReplaceAtRequest == FALSE
MCReplaceAtRequest == TRUE

\* the witness that keeps the replaced mapping's claim
MCReplaceKeepsClaim == FALSE

ResponseBindReleasesClaim == TRUE
MCResponseBindLeavesClaim == FALSE

MCTokenClaimRemoved == FALSE

\* the IdleRemove action releases its mapping's claim; the Broken witness
\* turns the release off, so a claim outlives the mapping that held it
IdleRemoveReleasesClaim == TRUE
MCIdleRemoveLeavesClaim == FALSE

\* the work budget charged at the cookie gate, the code before 2026-10-31
MCWorkBudgetAtCookie == FALSE

\* the claim written as the request passes its check rung, before the
\* signature: the code before the check/write split
MCClaimWriteAtCheck == "claim"

\* no repeat rung: a token refused past the verify for a client id already
\* connected or a full server is verified again in the same update, the
\* code before 2026-11-13
MCRepeatRungOff == FALSE

\* a replacing request checked only against its own address's binding, as
\* ServerConnection's replace branch stated until 2026-11-13 (the code never
\* did): a token bound to the requesting address replaces that address's
\* half-open mapping
MCReplaceIgnoresBinding == FALSE

Done == outcome /= "pending"

Terminating == Done /\ UNCHANGED vars

\* the next request: a fresh check sequence run, keeping the claims and mappings
\* the model has built so far
NewRequest ==
    /\ Done
    /\ pos' = MinPos
    /\ fault' \in Faults
    /\ reqAddr' \in Addrs
    /\ reqToken' \in Tokens
    \* any address, any token: a connected address (fault "restart") and a
    \* request for the token its half-open mapping holds are refused at the
    \* restart rung; a request for another token from an address with a
    \* half-open mapping replaces it once accepted
    \* (sdk/rocketnet_server_protocol.cpp, replace_index).  The replaced
    \* mapping's token may be bound: a response that bound it and was then
    \* denied (no slot, or its client id already connected) leaves the
    \* mapping half-open, and the code replaces it all the same.  The
    \* request's own token may be bound too, and the used rung refuses it
    \* when it is bound to another address, or to any address on a request
    \* that would replace.  Left out: a request from a free address for a
    \* token bound to that address, the reconnect after its connection
    \* closed, which the code serves; binding expiry and eviction are outside
    \* this instance as well.
    /\ ~(bound[reqToken'] = reqAddr' /\ maps[reqAddr'] = None)
    /\ held' = maps[reqAddr']
    /\ spent' = FALSE
    /\ spentKp' = FALSE
    /\ outcome' = "pending"
    /\ refusedRung' = "none"
    /\ spentAtRefusal' = FALSE
    /\ spentKpAtRefusal' = FALSE
    /\ claimWritten' = FALSE
    /\ verified' = FALSE
    /\ wasRepeat' = (reqToken' \in repeated)
    /\ UNCHANGED repeated
    /\ IF ReplaceAtRequest /\ maps[reqAddr'] /= None
         THEN /\ maps' = [maps EXCEPT ![reqAddr'] = None]
              /\ claims' = [u \in Tokens |-> IF u = maps[reqAddr'] THEN None ELSE claims[u]]
              /\ stale' = stale \ {reqAddr'}
         ELSE UNCHANGED <<claims, maps, stale>>

\* The handshake deadline comes in two steps, as in the code.  The claim and
\* its mapping carry the same deadline, current_time plus
\* handshake_deadline_seconds, but the claim expires the moment the clock
\* reaches it (rocketnet_used_token_history_claim_check and _claim_record in
\* sdk/rocketnet_token_tables.cpp compare claim_deadline <= current_time),
\* while the mapping leaves only at the sweep in
\* rocketnet_server_protocol_update, which the server transport runs after
\* the receive batch of the same update.  ClaimExpire is the clock passing
\* the deadline: the claim is gone and its address's mapping is stale, past
\* its deadline but still in the table.  Requests in the window between
\* the two steps -- the rest of that receive batch -- see no claim, so a
\* second address can claim the token and gain a mapping beside the stale
\* one.
ClaimExpire ==
    /\ Done
    /\ \E t \in Tokens :
        /\ claims[t] /= None
        /\ claims' = [claims EXCEPT ![t] = None]
        /\ stale' = stale \cup {a \in Addrs : a = claims[t] /\ maps[a] = t}
    /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                   spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken,
                   maps, claimWritten, verified, repeated, wasRepeat>>

\* the update's sweep removes every stale mapping, each releasing its
\* token's claim only when its own address holds it
\* (rocketnet_server_release_mapping_claim); a claim a second address took
\* in the window stays with that address's mapping
Sweep ==
    /\ Done
    /\ stale /= {}
    /\ maps' = [a \in Addrs |-> IF a \in stale THEN None ELSE maps[a]]
    /\ claims' = [u \in Tokens |-> IF \E a \in stale : maps[a] = u /\ claims[u] = a
                                    THEN None ELSE claims[u]]
    /\ stale' = {}
    /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                   spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken,
                   claimWritten, verified, repeated, wasRepeat>>

\* a half-open mapping leaves alone before its handshake deadline -- the
\* idle timeout or the integrity limit in the code -- and its token's claim
\* leaves with it, when its own address holds it, unless the witness has
\* turned the release off.  Any mapping may leave, each on its own deadline
\* (the update's idle sweep and rocketnet_server_integrity_failure each
\* remove the one mapping that reached its limit).
IdleRemove ==
    /\ Done
    /\ \E a \in Addrs :
       LET t == maps[a] IN
       /\ maps[a] /= None
       /\ maps' =[b \in Addrs |-> IF b = a THEN None ELSE maps[b]]
       /\ claims' = IF IdleRemoveReleasesClaim /\ claims[t] = a
                      THEN [u \in Tokens |-> IF u = t THEN None ELSE claims[u]]
                      ELSE claims
       /\ stale' = stale \ {a}
       /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                      spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken,
                      claimWritten, verified, repeated, wasRepeat>>

\* An opened response binds an unexpired token and ends its request claim.
ResponseBind ==
    /\ Done
    /\ \E a \in Addrs :
        /\ maps[a] /= None
        /\ bound[maps[a]] = None
        /\ LET t == maps[a] IN
           /\ bound' = [bound EXCEPT ![t] = a]
           /\ claims' = IF ResponseBindReleasesClaim /\ claims[t] = a
                         THEN [claims EXCEPT ![t] = None] ELSE claims
        /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                       spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken,
                       maps, claimWritten, verified, repeated, wasRepeat>>

\* the start of an update (rocketnet_server_protocol_update): the repeat
\* rung's table empties with the work budget, so a token refused in one
\* update is verified again in the next
UpdateStart ==
    /\ Done
    /\ repeated' = {}
    /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                   spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken,
                   claims, maps, claimWritten, verified, wasRepeat>>

\* held is the half-open mapping the current request's address held as it
\* began; a step that moves the table on for other reasons ends that view
\* stale is the set of addresses whose half-open mapping is past its
\* handshake deadline and not yet swept; an accepted request installs a
\* fresh mapping, with a fresh deadline, at its address
MCNext == ((Next \/ Terminating) /\ UNCHANGED <<bound, held>>
           /\ stale' = IF outcome' = "accepted" /\ outcome = "pending"
                         THEN stale \ {reqAddr} ELSE stale)
          \/ (NewRequest /\ UNCHANGED bound)
          \/ (ClaimExpire /\ UNCHANGED <<bound, held>>)
          \/ ((Sweep \/ IdleRemove) /\ UNCHANGED bound /\ held' = None)
          \/ (ResponseBind /\ held' = None /\ UNCHANGED stale)
          \/ (UpdateStart /\ UNCHANGED <<bound, held, stale>>)

MCInit == Init /\ held = None /\ stale = {}
MCSpec == MCInit /\ [][MCNext]_MCVars

BoundTypeOK == /\ bound \in [Tokens -> Addrs \cup {None}]
               /\ held \in Tokens \cup {None}
               /\ stale \subseteq { a \in Addrs : maps[a] /= None }

\* OneLiveMappingPerToken with the window: a token holds at most one
\* half-open mapping that is not yet past its handshake deadline.  A stale
\* mapping beside a second address's fresh one lasts only until the sweep
\* at the end of that update's receive batch.
OneUnexpiredMappingPerToken ==
    \A t \in Tokens : Cardinality({ a \in Addrs : maps[a] = t /\ a \notin stale }) <= 1

\* a refused request leaves its address's half-open mapping as it was: only
\* a request that passed every check, its signature included, replaces one
RefusalKeepsHalfOpen == (outcome = "refused" /\ held /= None) => maps[reqAddr] = held

\* a request that replaced its address's half-open mapping carried a token
\* bound to no address, its own included: a replay of a token that has
\* already connected never displaces a handshake in flight (the used rung's
\* check against the zero address)
ReplaceOnlyUnboundToken == (outcome = "accepted" /\ held /= None) => bound[reqToken] = None

BoundHasNoClaim == Done => \A t \in Tokens : bound[t] /= None => claims[t] = None

\* BoundHasNoClaim with the window: a response binds its token and releases
\* its own address's claim, so a bound token never keeps a claim of the
\* address it is bound to.  It can keep another address's: B claims the
\* token in the window, then A's stale mapping, which the response path
\* does not check against its deadline, opens a response and binds the
\* token to A; rocketnet_used_token_history_claim_release(t, A) leaves B's
\* claim, which lasts until B's response or B's own deadline.
\* MCReceiveOrderDeadlineWindowBound.cfg shows it.
BoundHasNoOwnClaim == Done => \A t \in Tokens : bound[t] /= None => claims[t] /= bound[t]

\* a claim that is live once a request is done names a token some address's
\* mapping holds: it cannot outlive the mapping it stands for
ClaimHasMapping ==
    Done => \A t \in Tokens : claims[t] /= None => \E a \in Addrs : maps[a] = t

=============================================================================
