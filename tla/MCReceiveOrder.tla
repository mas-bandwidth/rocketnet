-------------------------- MODULE MCReceiveOrder --------------------------
(***************************************************************************)
(* The committed run of ReceiveOrder in the ruled order.  Every invariant  *)
(* passes while requests repeat, each claim lives until its mapping dies,  *)
(* and the mapping table fills and empties.  The witnesses are the same    *)
(* module under the Broken cfgs, each moving one cheap rung behind a crypto *)
(* rung or removing the token claim; TLC must reject them.                 *)
(***************************************************************************)
EXTENDS ReceiveOrder

MCTokenClaimRemoved == FALSE

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
    /\ spent' = FALSE
    /\ spentKp' = FALSE
    /\ outcome' = "pending"
    /\ refusedRung' = "none"
    /\ spentAtRefusal' = FALSE
    /\ spentKpAtRefusal' = FALSE
    /\ UNCHANGED <<claims, maps>>

\* the handshake deadline: one token's claim and its mapping leave together
Deadline ==
    /\ Done
    /\ \E t \in Tokens : claims[t] /= None
    /\ LET t == CHOOSE t \in Tokens : claims[t] /= None IN
       /\ claims' = [u \in Tokens |-> IF u = t THEN None ELSE claims[u]]
       /\ maps' = [a \in Addrs |-> IF maps[a] = t THEN None ELSE maps[a]]
       /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                      spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken>>

MCNext == Next \/ NewRequest \/ Deadline \/ Terminating

MCSpec == Init /\ [][MCNext]_vars

=============================================================================
