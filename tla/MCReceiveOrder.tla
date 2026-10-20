-------------------------- MODULE MCReceiveOrder --------------------------
(***************************************************************************)
(* The committed run of ReceiveOrder in the ruled order, plus the token    *)
(* claim state machine.  Every invariant passes.  The witnesses are the    *)
(* same module under the Broken cfgs; TLC must reject each.                *)
(*                                                                         *)
(* TokenRequestFixed is the request-time claim: a token with no live       *)
(* half-open mapping is claimed by its first address, and a second address *)
(* cannot add a mapping until that claim expires with the handshake        *)
(* deadline.  TokenRequestBroken is the same request without the claim     *)
(* rung, the BASE defect: one token buys one half-open mapping per address *)
(* until the map is full.  MCSpecBrokenTokenManyAddresses selects it for   *)
(* MCReceiveOrderBrokenTokenManyAddresses.cfg; OneHalfOpenPerToken fails   *)
(* in the second state, matching the code probe in the card's unit case.   *)
(***************************************************************************)
EXTENDS ReceiveOrder, FiniteSets

Addresses == {"a1", "a2"}

NoAddress == "none"

TokenCapacity == 2

VARIABLES claimOwner, halfOpen, mapCount

TokenVars == <<claimOwner, halfOpen, mapCount>>

MCVars == <<pos, fault, spent, spentKp, outcome, refusedRung,
            spentAtRefusal, spentKpAtRefusal, claimOwner, halfOpen,
            mapCount>>

Done == outcome # "pending"

MCTerminating == Done /\ UNCHANGED MCVars

TokenInit ==
    /\ claimOwner = NoAddress
    /\ halfOpen = {}
    /\ mapCount = 0

MCInit == Init /\ TokenInit

TokenTypeOK ==
    /\ claimOwner \in Addresses \cup {NoAddress}
    /\ halfOpen \subseteq Addresses
    /\ mapCount \in 0..TokenCapacity

OneHalfOpenPerToken == Cardinality(halfOpen) <= 1

MapBounded == mapCount <= TokenCapacity

TokenRequestFixed(addr) ==
    /\ addr \in Addresses
    /\ mapCount < TokenCapacity
    /\ addr \notin halfOpen
    /\ claimOwner \in {NoAddress, addr}
    /\ claimOwner' = addr
    /\ halfOpen' = halfOpen \cup {addr}
    /\ mapCount' = mapCount + 1
    /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                   spentAtRefusal, spentKpAtRefusal>>

TokenRequestBroken(addr) ==
    /\ addr \in Addresses
    /\ mapCount < TokenCapacity
    /\ addr \notin halfOpen
    /\ claimOwner' = IF claimOwner = NoAddress THEN addr ELSE claimOwner
    /\ halfOpen' = halfOpen \cup {addr}
    /\ mapCount' = mapCount + 1
    /\ UNCHANGED <<pos, fault, spent, spentKp, outcome, refusedRung,
                   spentAtRefusal, spentKpAtRefusal>>

MCNext == (Next /\ UNCHANGED TokenVars) \/ MCTerminating \/
          \E addr \in Addresses : TokenRequestFixed(addr)

MCSpec == MCInit /\ [][MCNext]_MCVars

MCNextBrokenTokenManyAddresses == (Next /\ UNCHANGED TokenVars) \/ MCTerminating \/
          \E addr \in Addresses : TokenRequestBroken(addr)

MCSpecBrokenTokenManyAddresses ==
    MCInit /\ [][MCNextBrokenTokenManyAddresses]_MCVars

=============================================================================
