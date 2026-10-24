------------------------- MODULE MCLinkReconfigure -------------------------
(***************************************************************************)
(* Link's live setting: a sender hands packets to one link while the       *)
(* setting in force changes (rocketnet_client_network_simulator and        *)
(* rocketnet_server_network_simulator, sdk/rocketnet_api.cpp).             *)
(*                                                                         *)
(* The setting is "pass" (the packet is let out, with Link's latency and   *)
(* jitter) or "lose" (the packet is dropped at the send).  The setting     *)
(* applies when a packet is sent: applied is the one a send acts on, and   *)
(* setting is the one the caller last asked for.  A correct change sets    *)
(* both, and it never touches the packets already on the wire, so their    *)
(* setting is the one they were sent under (Reconfigure below).  This      *)
(* module is Link's reconfiguration, kept beside Link so Link's own        *)
(* measured configurations keep the model they were measured on.           *)
(*                                                                         *)
(* LiveSettingHolds is the invariant the code cites:                      *)
(*   SendUsesForce      every send used the setting in force at that send, *)
(*                      so a packet sent after the change is held to the   *)
(*                      new setting;                                       *)
(*   NoInFlightDrop     a change removes nothing already in flight, so     *)
(*                      none sent before it is dropped by it.              *)
(*                                                                         *)
(* Defect = "inflight" changes the setting and clears the wire (the        *)
(* reversed witness for NoInFlightDrop).  Defect = "stale" changes setting *)
(* but leaves applied as it was, so later sends use the old setting (the   *)
(* reversed witness for SendUsesForce).  Defect = "none" is the control.   *)
(***************************************************************************)
EXTENDS Link, Naturals, Sequences

CONSTANTS
    MaxSends,   \* bound on the sends a run explores, >= 1
    Defect      \* "none", "inflight" or "stale"

Settings == {"pass", "lose"}

ASSUME /\ MaxSends \in Nat \ {0}
       /\ Defect \in {"none", "inflight", "stale"}

(***************************************************************************)
(* Reconfigure is the relation a live change keeps: the setting in force   *)
(* moves to newSetting, and a packet already in flight keeps the setting   *)
(* it was sent under.  A packet sent after the change is held to the new   *)
(* setting; none sent before it is dropped by it.                          *)
(***************************************************************************)
Reconfigure( oldSetting, newSetting ) ==
    /\ oldSetting \in Settings
    /\ newSetting \in Settings
    /\ newSetting # oldSetting

VARIABLES
    setting,          \* the setting the caller last asked for
    applied,          \* the setting a send acts on
    sendLog,          \* one record per send: the force and what it applied
    reconfigureDrops  \* copies a change removed from the wire (must be 0)

SendRecord == [pkt : Packets, inForce : Settings, appliedAtSend : Settings]

rcVars == <<wire, sent, pulse, setting, applied, sendLog, reconfigureDrops>>

RCInit ==
    /\ Init
    /\ setting = "pass"
    /\ applied = "pass"
    /\ sendLog = <<>>
    /\ reconfigureDrops = 0

(***************************************************************************)
(* The sender.  A send is counted whether or not the setting drops it, as  *)
(* Link!Send does; "lose" leaves the wire as it was.                       *)
(***************************************************************************)
RCSend ==
    \E p \in Packets :
        /\ InFlight < Capacity
        /\ Len(sendLog) < MaxSends
        /\ sent' = NextCount
        /\ IF applied = "lose"
             THEN UNCHANGED wire
             ELSE \E d \in 0..MaxDelay : wire' = Append(wire, [pkt |-> p, due |-> d])
        /\ sendLog' = Append(sendLog, [pkt |-> p, inForce |-> setting, appliedAtSend |-> applied])
        /\ UNCHANGED <<pulse, setting, applied, reconfigureDrops>>

RCDeliver ==
    \E p \in Packets :
        /\ Deliver(p)
        /\ UNCHANGED <<setting, applied, sendLog, reconfigureDrops>>

RCEnvironment ==
    /\ Environment
    /\ UNCHANGED <<setting, applied, sendLog, reconfigureDrops>>

(***************************************************************************)
(* The change.  A correct change sets the setting in force and the one a   *)
(* send acts on, and leaves the wire alone.  The two defects above are the *)
(* reversed witnesses.                                                     *)
(***************************************************************************)
MCReconfigure ==
    \E next \in Settings :
        /\ Reconfigure(setting, next)
        /\ IF Defect = "inflight" /\ next = "lose"
             THEN /\ reconfigureDrops' = reconfigureDrops + Len(wire)
                  /\ wire' = <<>>
             ELSE /\ reconfigureDrops' = reconfigureDrops
                  /\ UNCHANGED wire
        /\ setting' = next
        /\ IF Defect = "stale"
             THEN UNCHANGED applied
             ELSE applied' = next
        /\ UNCHANGED <<sent, pulse, sendLog>>

RCNext == RCSend \/ RCDeliver \/ RCEnvironment \/ MCReconfigure

Spec == RCInit /\ [][RCNext]_rcVars

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
RCTypeOK ==
    /\ TypeOK
    /\ setting \in Settings
    /\ applied \in Settings
    /\ sendLog \in Seq(SendRecord)
    /\ reconfigureDrops \in Nat

\* A packet sent after the change is held to the new setting.
SendUsesForce ==
    \A i \in DOMAIN sendLog : sendLog[i].appliedAtSend = sendLog[i].inForce

\* None sent before the change is dropped by it.
NoInFlightDrop == reconfigureDrops = 0

LiveSettingHolds == SendUsesForce /\ NoInFlightDrop

=============================================================================
