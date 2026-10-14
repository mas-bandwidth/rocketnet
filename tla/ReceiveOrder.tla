-------------------------- MODULE ReceiveOrder --------------------------
(***************************************************************************)
(* A receive path as ordered rungs, each cheap or crypto.  The request     *)
(* path runs in the ruled order: version byte, signed timestamps, address  *)
(* and key compares, cookie gate and budget, used and admitted token       *)
(* lookups, restart rule, signature, seal open, slots, keypair.  Keyed     *)
(* hashes (the cookie MAC, the token identifier, key derivation) are the   *)
(* cheap tier and spend no counted cryptography.                           *)
(*                                                                         *)
(* CheapRefusalsFirst: a refusal at a rung above the signature spends no   *)
(* cryptography at all, and the slot refusal, which must wait for a        *)
(* verified token, spends no keypair work.  The committed instance holds   *)
(* it; the three witnesses (slots after keypair, admitted after verify,    *)
(* version not first) break it, each replayed against the code by hand     *)
(* in README-ReceiveOrder.md.                                              *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS PosVersion, PosTimestamps, PosCompares, PosCookie, PosUsed,
          PosAdmitted, PosRestart, PosSignature, PosSeal, PosSlots,
          PosKeypair

Rungs == {"version", "timestamps", "compares", "cookie", "used",
          "admitted", "restart", "signature", "seal", "slots", "keypair"}

CryptoRungs == {"signature", "seal", "keypair"}

AboveVerify == {"version", "timestamps", "compares", "cookie", "used",
                "admitted", "restart"}

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

RungAt(p) == CHOOSE r \in Rungs : Pos(r) = p

Refuses(r, f) ==
    \/ (r = "version" /\ f = "version")
    \/ (r = "timestamps" /\ f = "expired")
    \/ (r = "slots" /\ f = "full")
    \/ (r = "admitted" /\ f = "admitted")
    \/ (r = "signature" /\ f = "signature")
    \/ (r = "seal" /\ f = "seal")

VARIABLES pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal

vars == <<pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal>>

TypeOK ==
    /\ pos \in 1..12
    /\ fault \in Faults
    /\ spent \in BOOLEAN
    /\ spentKp \in BOOLEAN
    /\ outcome \in {"pending", "refused", "accepted"}
    /\ refusedRung \in Rungs \cup {"none"}
    /\ spentAtRefusal \in BOOLEAN
    /\ spentKpAtRefusal \in BOOLEAN

Init ==
    /\ pos = 1
    /\ fault \in Faults
    /\ spent = FALSE
    /\ spentKp = FALSE
    /\ outcome = "pending"
    /\ refusedRung = "none"
    /\ spentAtRefusal = FALSE
    /\ spentKpAtRefusal = FALSE

RefuseStep ==
    /\ outcome = "pending"
    /\ pos <= 11
    /\ Refuses(RungAt(pos), fault)
    /\ outcome' = "refused"
    /\ refusedRung' = RungAt(pos)
    /\ spentAtRefusal' = spent
    /\ spentKpAtRefusal' = spentKp
    /\ pos' = 12
    /\ UNCHANGED <<fault, spent, spentKp>>

AdvanceStep ==
    /\ outcome = "pending"
    /\ pos <= 11
    /\ ~Refuses(RungAt(pos), fault)
    /\ pos' = pos + 1
    /\ spent' = (spent \/ (RungAt(pos) \in CryptoRungs))
    /\ spentKp' = (spentKp \/ (RungAt(pos) = "keypair"))
    /\ UNCHANGED <<fault, outcome, refusedRung, spentAtRefusal,
                   spentKpAtRefusal>>

AcceptStep ==
    /\ outcome = "pending"
    /\ pos = 12
    /\ outcome' = "accepted"
    /\ UNCHANGED <<pos, fault, spent, spentKp, refusedRung,
                   spentAtRefusal, spentKpAtRefusal>>

Next == RefuseStep \/ AdvanceStep \/ AcceptStep

Spec == Init /\ [][Next]_vars

CheapRefusalsFirst ==
    outcome = "refused" =>
        /\ (refusedRung \in AboveVerify => ~spentAtRefusal)
        /\ (refusedRung = "slots" => ~spentKpAtRefusal)

=============================================================================
