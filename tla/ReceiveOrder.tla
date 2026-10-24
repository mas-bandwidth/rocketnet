-------------------------- MODULE ReceiveOrder --------------------------
(***************************************************************************)
(* A receive path as ordered rungs, each cheap or crypto.  The request     *)
(* path runs in the ruled order: version byte, signed timestamps, address  *)
(* and key compares, cookie gate and handshake budget, used token lookup,  *)
(* the failed-signature budget, the token-to-address claim and the mapping *)
(* capacity, restart rule, signature, seal open, slots, keypair.  Keyed    *)
(* hashes (the cookie MAC,                                                 *)
(* the token identifier, key derivation) are the cheap tier and spend no   *)
(* counted cryptography.                                                   *)
(*                                                                         *)
(* CheapRefusalsFirst: a refusal at a rung above the signature spends no   *)
(* cryptography at all, and the slot refusal, which must wait for a        *)
(* verified token, spends no keypair work.  The committed instance holds   *)
(* it; the witnesses (slots after keypair, admitted after verify, version  *)
(* not first, and the token claim removed) break it or the one-mapping     *)
(* rule, each replayed against the code by hand in README-ReceiveOrder.md. *)
(*                                                                         *)
(* The claim is split in two.  ClaimCheck, the claim rung, is read-only:   *)
(* it refuses a second address while another holds the token's claim (and  *)
(* a full claim table).  ClaimWrite records the request's address for its  *)
(* token only as the request passes the signature rung, so a request that  *)
(* proves nothing writes nothing: ClaimOnlyAfterSignature.  The capacity   *)
(* rung refuses a full mapping table.  OneLiveMappingPerToken is the       *)
(* consequence of check and write together: a token holds at most one     *)
(* live half-open mapping.  The claim expires with the mapping on the      *)
(* Deadline step.  The witness ClaimBeforeSignature writes at the check    *)
(* rung, the code before the split, and breaks ClaimOnlyAfterSignature.    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS PosVersion, PosTimestamps, PosCompares, PosCookie, PosUsed,
          PosAdmitted, PosRestart, PosSignature, PosSeal, PosSlots,
          PosKeypair

Rungs == {"version", "timestamps", "compares", "cookie", "used",
          "verifybudget", "claim", "capacity", "admitted", "restart",
          "signature", "seal", "slots", "keypair"}

CryptoRungs == {"signature", "seal", "keypair"}

AboveVerify == {"version", "timestamps", "compares", "cookie", "used",
                "verifybudget", "claim", "capacity", "admitted", "restart"}

Faults == {"none", "version", "expired", "full", "mapfull", "claimed",
           "budget", "admitted", "signature", "seal"}

\* Positions are scaled by ten so the three cheap rungs below sit between
\* the used rung and the next configured rung without a new config constant.
\* The order is the code's: used, failed-signature budget, claim, mapping
\* capacity (sdk/rocketnet_server_protocol.cpp; TRANSPORT.md t12 rule 9).
Pos(r) ==
    CASE r = "version"   -> 10 * PosVersion
      [] r = "timestamps" -> 10 * PosTimestamps
      [] r = "compares"  -> 10 * PosCompares
      [] r = "cookie"    -> 10 * PosCookie
      [] r = "used"      -> 10 * PosUsed
      [] r = "verifybudget" -> 10 * PosUsed + 1
      [] r = "claim"     -> 10 * PosUsed + 2
      [] r = "capacity"  -> 10 * PosUsed + 5
      [] r = "admitted"  -> 10 * PosAdmitted
      [] r = "restart"   -> 10 * PosRestart
      [] r = "signature" -> 10 * PosSignature
      [] r = "seal"      -> 10 * PosSeal
      [] r = "slots"     -> 10 * PosSlots
      [] r = "keypair"   -> 10 * PosKeypair

RungPositions == { Pos(r) : r \in Rungs }
MinPos == CHOOSE p \in RungPositions : \A q \in RungPositions : p <= q
MaxPos == CHOOSE p \in RungPositions : \A q \in RungPositions : p >= q
RungAt(p) == CHOOSE r \in Rungs : Pos(r) = p
NextPos(p) == CHOOSE q \in RungPositions : q > p /\ \A s \in RungPositions : s > p => q <= s

\* The token claim and the mapping table are the same small instance as the
\* code: two addresses, two tokens, room for two half-open mappings.

Addrs == {"A", "B"}
Tokens == {"t1", "t2"}
None == "none"
MapCap == 2
ClaimAtRequest == TRUE

\* the rung whose passing writes the claim: the signature, so the write
\* follows a verified signature (sdk/rocketnet_server_protocol.cpp,
\* rocketnet_used_token_history_claim_record after
\* rocketnet_verify_signed_connect_data)
ClaimWriteRung == "signature"

VARIABLES pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken, claims, maps,
          claimWritten, verified

vars == <<pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken, claims, maps,
          claimWritten, verified>>

MapCount == Cardinality({ a \in Addrs : maps[a] /= None })
MapFull == MapCount >= MapCap
ClaimOther == claims[reqToken] /= None /\ claims[reqToken] /= reqAddr

Refuses(r, f) ==
    \/ (r = "version" /\ f = "version")
    \/ (r = "timestamps" /\ f = "expired")
    \/ (r = "verifybudget" /\ f = "budget")
    \/ (r = "claim" /\ (f = "claimed" \/ (ClaimAtRequest /\ ClaimOther)))
    \/ (r = "capacity" /\ (f = "mapfull" \/ MapFull))
    \/ (r = "slots" /\ f = "full")
    \/ (r = "admitted" /\ f = "admitted")
    \/ (r = "signature" /\ f = "signature")
    \/ (r = "seal" /\ f = "seal")

TypeOK ==
    /\ pos \in RungPositions \cup {MaxPos + 1}
    /\ fault \in Faults
    /\ spent \in BOOLEAN
    /\ spentKp \in BOOLEAN
    /\ outcome \in {"pending", "refused", "accepted"}
    /\ refusedRung \in Rungs \cup {"none"}
    /\ spentAtRefusal \in BOOLEAN
    /\ spentKpAtRefusal \in BOOLEAN
    /\ reqAddr \in Addrs
    /\ reqToken \in Tokens
    /\ claims \in [Tokens -> Addrs \cup {None}]
    /\ maps \in [Addrs -> Tokens \cup {None}]
    /\ claimWritten \in BOOLEAN
    /\ verified \in BOOLEAN

Init ==
    /\ pos = MinPos
    /\ fault \in Faults
    /\ spent = FALSE
    /\ spentKp = FALSE
    /\ outcome = "pending"
    /\ refusedRung = "none"
    /\ spentAtRefusal = FALSE
    /\ spentKpAtRefusal = FALSE
    /\ reqAddr \in Addrs
    /\ reqToken \in Tokens
    /\ claims = [t \in Tokens |-> None]
    /\ maps = [a \in Addrs |-> None]
    /\ claimWritten = FALSE
    /\ verified = FALSE

RefuseStep ==
    /\ outcome = "pending"
    /\ pos <= MaxPos
    /\ Refuses(RungAt(pos), fault)
    /\ outcome' = "refused"
    /\ refusedRung' = RungAt(pos)
    /\ spentAtRefusal' = spent
    /\ spentKpAtRefusal' = spentKp
    /\ pos' = MaxPos + 1
    \* a refusal after the claim write releases the request's claim, as the
    \* code does on every refusal; only a live mapping retains it
    /\ claims' = IF claimWritten /\ claims[reqToken] = reqAddr
                  THEN [u \in Tokens |-> IF u = reqToken THEN None ELSE claims[u]]
                  ELSE claims
    /\ UNCHANGED <<fault, spent, spentKp, reqAddr, reqToken, maps,
                   claimWritten, verified>>

AdvanceStep ==
    /\ outcome = "pending"
    /\ pos < MaxPos
    /\ ~Refuses(RungAt(pos), fault)
    /\ pos' = NextPos(pos)
    /\ spent' = (spent \/ (RungAt(pos) \in CryptoRungs))
    /\ spentKp' = (spentKp \/ (RungAt(pos) = "keypair"))
    \* ClaimCheck is the claim rung's refusal above; ClaimWrite is this
    \* write, on passing ClaimWriteRung
    /\ claims' = IF RungAt(pos) = ClaimWriteRung /\ ClaimAtRequest /\ claims[reqToken] = None
                  THEN [claims EXCEPT ![reqToken] = reqAddr] ELSE claims
    /\ claimWritten' = (claimWritten \/ (RungAt(pos) = ClaimWriteRung /\ ClaimAtRequest))
    /\ verified' = (verified \/ RungAt(pos) = "signature")
    /\ UNCHANGED <<fault, outcome, refusedRung, spentAtRefusal,
                   spentKpAtRefusal, reqAddr, reqToken, maps>>

AcceptStep ==
    /\ outcome = "pending"
    /\ pos = MaxPos
    /\ outcome' = "accepted"
    /\ maps' = [a \in Addrs |-> IF a = reqAddr THEN reqToken ELSE maps[a]]
    /\ UNCHANGED <<pos, fault, spent, spentKp, refusedRung,
                   spentAtRefusal, spentKpAtRefusal, reqAddr, reqToken, claims,
                   claimWritten, verified>>

Next == RefuseStep \/ AdvanceStep \/ AcceptStep

Spec == Init /\ [][Next]_vars

CheapRefusalsFirst ==
    outcome = "refused" =>
        /\ (refusedRung \in AboveVerify => ~spentAtRefusal)
        /\ (refusedRung = "slots" => ~spentKpAtRefusal)

\* one token holds at most one live half-open mapping: the claim rung
\* refuses a second address while the first mapping lives, and the mapping
\* leaves only on its handshake deadline.
OneLiveMappingPerToken == \A t \in Tokens : Cardinality({ a \in Addrs : maps[a] = t }) <= 1

\* a request writes the claim table only once its signature has verified:
\* a request refused at or above the signature rung leaves no claim behind
\* and never held one.
ClaimOnlyAfterSignature == claimWritten => verified

=============================================================================
