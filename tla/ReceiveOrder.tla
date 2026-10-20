-------------------------- MODULE ReceiveOrder --------------------------
(***************************************************************************)
(* A receive path as ordered rungs, each cheap or crypto.  The request     *)
(* path runs in the ruled order: version byte, signed timestamps, address  *)
(* and key compares, cookie gate and budget, used and claimed token        *)
(* lookups, the encryption-map capacity check, restart rule, signature,    *)
(* seal open, slots, keypair.  Keyed hashes (the cookie MAC, the token     *)
(* identifier, key derivation) are the cheap tier and spend no counted     *)
(* cryptography.                                                           *)
(*                                                                         *)
(* CheapRefusalsFirst: a refusal at a rung above the signature spends no   *)
(* cryptography at all, and the slot refusal, which must wait for a        *)
(* verified token, spends no keypair work.  That includes the request-time *)
(* token claim and the full-map capacity rung, both decided before the     *)
(* signature (STANDARD.md t12 step 7).  The committed instance holds it;   *)
(* the witnesses (slots after keypair, admitted after verify, version not  *)
(* first) break it, each replayed against the code by hand in             *)
(* README-ReceiveOrder.md.                                                 *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS PosVersion, PosTimestamps, PosCompares, PosCookie, PosUsed,
          PosAdmitted, PosRestart, PosSignature, PosSeal, PosSlots,
          PosKeypair

Rungs == {"version", "timestamps", "compares", "cookie", "used",
          "admitted", "restart", "signature", "seal", "slots", "keypair",
          "capacity", "claim"}

CryptoRungs == {"signature", "seal", "keypair"}

AboveVerify == {"version", "timestamps", "compares", "cookie", "used",
                "admitted", "restart", "capacity", "claim"}

Faults == {"none", "version", "expired", "full", "mapfull", "claimed",
           "admitted", "signature", "seal"}

RawPos(r) ==
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

(* The two rungs this card adds sit immediately after the used-token lookup,
   and every old rung after it moves down two places, so the witness cfgs
   keep reordering only the rungs they name.  No new configuration constant
   is needed: the reference is the existing PosUsed. *)

Pos(r) ==
    IF r = "capacity" THEN PosUsed + 1
    ELSE IF r = "claim" THEN PosUsed + 2
    ELSE IF RawPos(r) > PosUsed THEN RawPos(r) + 2
    ELSE RawPos(r)

Top == 13

RungAt(p) == CHOOSE r \in Rungs : Pos(r) = p

Refuses(r, f) ==
    \/ (r = "version" /\ f = "version")
    \/ (r = "timestamps" /\ f = "expired")
    \/ (r = "capacity" /\ f = "mapfull")
    \/ (r = "claim" /\ f = "claimed")
    \/ (r = "slots" /\ f = "full")
    \/ (r = "admitted" /\ f = "admitted")
    \/ (r = "signature" /\ f = "signature")
    \/ (r = "seal" /\ f = "seal")

VARIABLES pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal

vars == <<pos, fault, spent, spentKp, outcome, refusedRung,
          spentAtRefusal, spentKpAtRefusal>>

TypeOK ==
    /\ pos \in 1..(Top + 1)
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
    /\ pos <= Top
    /\ Refuses(RungAt(pos), fault)
    /\ outcome' = "refused"
    /\ refusedRung' = RungAt(pos)
    /\ spentAtRefusal' = spent
    /\ spentKpAtRefusal' = spentKp
    /\ pos' = Top + 1
    /\ UNCHANGED <<fault, spent, spentKp>>

AdvanceStep ==
    /\ outcome = "pending"
    /\ pos <= Top
    /\ ~Refuses(RungAt(pos), fault)
    /\ pos' = pos + 1
    /\ spent' = (spent \/ (RungAt(pos) \in CryptoRungs))
    /\ spentKp' = (spentKp \/ (RungAt(pos) = "keypair"))
    /\ UNCHANGED <<fault, outcome, refusedRung, spentAtRefusal,
                   spentKpAtRefusal>>

AcceptStep ==
    /\ outcome = "pending"
    /\ pos = Top + 1
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
