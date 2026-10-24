-------------------------- MODULE MCReceiveOrder --------------------------
(***************************************************************************)
(* The committed run of ReceiveOrder in the ruled order.  Every invariant  *)
(* passes while requests repeat, each claim lives until its mapping dies,  *)
(* and the mapping table fills and empties.  A half-open mapping can also  *)
(* leave alone on its idle deadline, as idle timeout and integrity limit   *)
(* do in the code, and that removal releases its token's claim.  The       *)
(* witnesses are the same module under the Broken cfgs, each moving one    *)
(* cheap rung behind a crypto rung, removing the token claim, writing the  *)
(* claim at its check rung before the signature, or leaving a claim behind *)
(* when its mapping is removed; TLC must reject them.                     *)
(***************************************************************************)
EXTENDS ReceiveOrder

MCTokenClaimRemoved == FALSE

\* the IdleRemove action releases its mapping's claim; the Broken witness
\* turns the release off, so a claim outlives the mapping that held it
IdleRemoveReleasesClaim == TRUE
MCIdleRemoveLeavesClaim == FALSE

\* the claim written as the request passes its check rung, before the
\* signature: the code before the check/write split
MCClaimWriteAtCheck == "claim"

Done == outcome /= "pending"

Terminating == Done /\ UNCHANGED vars

\* the next request: a fresh ladder run, keeping the claims and mappings
\* the model has built so far
NewRequest ==
    /\ Done
    /\ pos' = MinPos
    /\ fault' \in Faults
    /\ reqAddr' \in Addrs
    /\ reqToken' \in Tokens
    \* the code answers a request from an address that already has a
    \* mapping with its stored challenge and never reaches the claim or
    \* accept rungs; a new ladder run starts only for a free address
    /\ maps[reqAddr'] = None
    /\ spent' = FALSE
    /\ spentKp' = FALSE
    /\ outcome' = "pending"
    /\ refusedRung' = "none"
    /\ spentAtRefusal' = FALSE
    /\ spentKpAtRefusal' = FALSE
    /\ claimWritten' = FALSE
    /\ verified' = FALSE
    /\ UNCHANGED <<claims, maps>>

\* the handshake deadline: one token's claim and its mapping leave together
Deadline ==
    /\ Done
    /\ \E t \in Tokens : claims[t] /= None
    /\ LET t == CHOOSE t \in Tokens : claims[t] /= None IN
       /\ claims' = [u \in Tokens |-> IF u = t THEN None ELSE claims[u]]
       /\ maps' = [a \in Addrs |-> IF maps[a] = t THEN None ELSE maps[a]]
       /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                      spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken,
                      claimWritten, verified>>

\* a half-open mapping leaves alone before its handshake deadline -- the
\* idle timeout or the integrity limit in the code -- and its token's claim
\* leaves with it unless the witness has turned the release off
IdleRemove ==
    /\ Done
    /\ \E a \in Addrs : maps[a] /= None
    /\ LET a == CHOOSE a \in Addrs : maps[a] /= None IN
       LET t == maps[a] IN
       /\ maps' = [b \in Addrs |-> IF b = a THEN None ELSE maps[b]]
       /\ claims' = IF IdleRemoveReleasesClaim
                      THEN [u \in Tokens |-> IF u = t THEN None ELSE claims[u]]
                      ELSE claims
       /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                      spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken,
                      claimWritten, verified>>

MCNext == Next \/ NewRequest \/ Deadline \/ IdleRemove \/ Terminating

MCSpec == Init /\ [][MCNext]_vars

\* a claim that is live once a request is done names a token some address's
\* mapping holds: it cannot outlive the mapping it stands for
ClaimHasMapping ==
    Done => \A t \in Tokens : claims[t] /= None => \E a \in Addrs : maps[a] = t

=============================================================================
