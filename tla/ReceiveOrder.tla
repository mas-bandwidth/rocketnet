-------------------------- MODULE ReceiveOrder --------------------------
(***************************************************************************)
(* A receive path as ordered rungs, each cheap or crypto.  The request     *)
(* path runs in the ruled order: version byte, signed timestamps, address  *)
(* and key compares, cookie gate and budget, used token lookup, token      *)
(* claim, map capacity, admitted lookup, restart rule, signature, seal     *)
(* open, slots, keypair.  Keyed hashes (the cookie MAC, the token          *)
(* identifier, key derivation) are the cheap tier and spend no counted     *)
(* cryptography.                                                           *)
(*                                                                         *)
(* Requests repeat: one connect token is presented from the addresses in   *)
(* Addresses, one request at a time, and an accepted request leaves a      *)
(* half-open mapping for its address (halfOpen).  Mappings of other tokens *)
(* (others) share the map's Capacity.  A half-open mapping leaves at its   *)
(* handshake deadline (Expire).  The claim rung refuses the token from an  *)
(* address while another address holds a half-open mapping for it         *)
(* (rocketnet_encryption_map_find_token_claim); the capacity rung refuses  *)
(* when the map is full (rocketnet_encryption_map_full), both in           *)
(* rocketnet_server_protocol_process_connection_request,                   *)
(* sdk/rocketnet_server_protocol.cpp.  A request from an address that      *)
(* already holds a mapping is a retransmit and never reaches the ladder.   *)
(*                                                                         *)
(* CheapRefusalsFirst: a refusal at a rung above the signature spends no   *)
(* cryptography at all, and the slot refusal, which must wait for a        *)
(* verified token, spends no keypair work.  OneHalfOpenPerToken: the       *)
(* token holds at most one live half-open mapping.  MapBounded: the map    *)
(* never holds more than Capacity mappings.  The committed instance holds  *)
(* all three; the witnesses (slots after keypair, admitted after verify,   *)
(* version not first) break CheapRefusalsFirst, and the token-many-        *)
(* addresses witness (the claim rung removed) breaks OneHalfOpenPerToken.  *)
(*                                                                         *)
(* The claim and capacity rungs sit between the used and admitted rungs:   *)
(* old rungs are placed at 3 * Pos, the two new ones at 3 * PosUsed + 1    *)
(* and + 2.  Addresses, Capacity, ClaimCheck, PosClaim and PosCapacity are *)
(* definitions a configuration may replace (CONSTANT X <- Y).              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS PosVersion, PosTimestamps, PosCompares, PosCookie, PosUsed,
          PosAdmitted, PosRestart, PosSignature, PosSeal, PosSlots,
          PosKeypair

Addresses == {"a1", "a2", "a3"}
Capacity == 2
ClaimCheck == TRUE
PosClaim == 3 * PosUsed + 1
PosCapacity == 3 * PosUsed + 2

Rungs == {"version", "timestamps", "compares", "cookie", "used", "claim",
          "capacity", "admitted", "restart", "signature", "seal", "slots",
          "keypair"}

CryptoRungs == {"signature", "seal", "keypair"}

AboveVerify == {"version", "timestamps", "compares", "cookie", "used",
                "claim", "capacity", "admitted", "restart"}

Faults == {"none", "version", "expired", "full", "admitted",
           "signature", "seal"}

Pos(r) ==
    CASE r = "version"   -> PosVersion
      [] r = "timestamps" -> PosTimestamps
      [] r = "compares"  -> PosCompares
      [] r = "cookie"    -> PosCookie
      [] r = "used"      -> PosUsed
      [] r = "admitted"  -> PosAdmitted
      [] r = "restart"   -> PosRestart
      [] r = "signature" -> PosSignature
      [] r = "seal"      -> PosSeal
      [] r = "slots"     -> PosSlots
      [] r = "keypair"   -> PosKeypair

PosOf(r) ==
    CASE r = "claim"    -> PosClaim
      [] r = "capacity" -> PosCapacity
      [] OTHER          -> 3 * Pos(r)

Places == {PosOf(r) : r \in Rungs}

Min(S) == CHOOSE x \in S : \A y \in S : x <= y

First == Min(Places)

End == 36

NextPlace(p) ==
    LET later == {q \in Places : q > p}
    IN IF later = {} THEN End ELSE Min(later)

RungAt(p) == CHOOSE r \in Rungs : PosOf(r) = p

VARIABLES pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal, from, halfOpen, others

vars == <<pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal, from, halfOpen, others>>

Refuses(r) ==
    \/ (r = "version" /\ fault = "version")
    \/ (r = "timestamps" /\ fault = "expired")
    \/ (r = "slots" /\ fault = "full")
    \/ (r = "admitted" /\ fault = "admitted")
    \/ (r = "signature" /\ fault = "signature")
    \/ (r = "seal" /\ fault = "seal")
    \/ (r = "claim" /\ ClaimCheck /\ \E b \in halfOpen : b # from)
    \/ (r = "capacity" /\ Cardinality(halfOpen) + others >= Capacity)

TypeOK ==
    /\ pos \in 0..End
    /\ fault \in Faults
    /\ spent \in BOOLEAN
    /\ spentKp \in BOOLEAN
    /\ outcome \in {"pending", "refused", "accepted"}
    /\ refusedRung \in Rungs \cup {"none"}
    /\ spentAtRefusal \in BOOLEAN
    /\ spentKpAtRefusal \in BOOLEAN
    /\ from \in Addresses
    /\ halfOpen \subseteq Addresses
    /\ others \in 0..Capacity

Init ==
    /\ pos = First
    /\ fault \in Faults
    /\ spent = FALSE
    /\ spentKp = FALSE
    /\ outcome = "pending"
    /\ refusedRung = "none"
    /\ spentAtRefusal = FALSE
    /\ spentKpAtRefusal = FALSE
    /\ from \in Addresses
    /\ halfOpen = {}
    /\ others \in 0..Capacity

RefuseStep ==
    /\ outcome = "pending"
    /\ pos < End
    /\ Refuses(RungAt(pos))
    /\ outcome' = "refused"
    /\ refusedRung' = RungAt(pos)
    /\ spentAtRefusal' = spent
    /\ spentKpAtRefusal' = spentKp
    /\ pos' = End
    /\ UNCHANGED <<fault, spent, spentKp, from, halfOpen, others>>

AdvanceStep ==
    /\ outcome = "pending"
    /\ pos < End
    /\ ~Refuses(RungAt(pos))
    /\ pos' = NextPlace(pos)
    /\ spent' = (spent \/ (RungAt(pos) \in CryptoRungs))
    /\ spentKp' = (spentKp \/ (RungAt(pos) = "keypair"))
    /\ UNCHANGED <<fault, outcome, refusedRung, spentAtRefusal,
                   spentKpAtRefusal, from, halfOpen, others>>

\* the mapping is added: rocketnet_encryption_map_add, then
\* rocketnet_encryption_map_claim_token
AcceptStep ==
    /\ outcome = "pending"
    /\ pos = End
    /\ Cardinality(halfOpen) + others < Capacity
    /\ outcome' = "accepted"
    /\ halfOpen' = halfOpen \cup {from}
    /\ UNCHANGED <<pos, fault, spent, spentKp, refusedRung,
                   spentAtRefusal, spentKpAtRefusal, from, others>>

\* the next request: any fault, from any address not holding a mapping
NewRequest ==
    /\ outcome # "pending"
    /\ from' \in Addresses \ halfOpen
    /\ fault' \in Faults
    /\ pos' = First
    /\ spent' = FALSE
    /\ spentKp' = FALSE
    /\ outcome' = "pending"
    /\ refusedRung' = "none"
    /\ spentAtRefusal' = FALSE
    /\ spentKpAtRefusal' = FALSE
    /\ UNCHANGED <<halfOpen, others>>

\* the sweep removes a half-open mapping at its handshake deadline
Expire ==
    /\ outcome # "pending"
    /\ \E a \in halfOpen : halfOpen' = halfOpen \ {a}
    /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                   spentAtRefusal, spentKpAtRefusal, from, others>>

\* mappings of other tokens come and go
OthersChange ==
    /\ outcome # "pending"
    /\ others' \in (0..(Capacity - Cardinality(halfOpen))) \ {others}
    /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                   spentAtRefusal, spentKpAtRefusal, from, halfOpen>>

Next == RefuseStep \/ AdvanceStep \/ AcceptStep \/ NewRequest \/ Expire \/
        OthersChange

Spec == Init /\ [][Next]_vars

CheapRefusalsFirst ==
    outcome = "refused" =>
        /\ (refusedRung \in AboveVerify => ~spentAtRefusal)
        /\ (refusedRung = "slots" => ~spentKpAtRefusal)

OneHalfOpenPerToken == Cardinality(halfOpen) <= 1

MapBounded == Cardinality(halfOpen) + others <= Capacity

=============================================================================
