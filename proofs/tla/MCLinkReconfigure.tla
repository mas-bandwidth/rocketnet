------------------------- MODULE MCLinkReconfigure -------------------------
(***************************************************************************)
(* Link's live setting: a sender hands packets to one link while the       *)
(* caller changes the setting (rocketnet_client_network_simulator and      *)
(* rocketnet_server_network_simulator, sdk/rocketnet_api.cpp, through      *)
(* rocketnet_network_simulator_reconfigure and _take_setting in            *)
(* sdk/rocketnet_network_simulator.cpp).                                   *)
(*                                                                         *)
(* The setting is "pass" (the packet is let out, with Link's latency and   *)
(* jitter) or "lose" (the packet is dropped at the send).  A change is a  *)
(* hand-over in two steps, as the code does it:                           *)
(*   MCReconfigure  the caller writes a pending record under the           *)
(*                  simulator's mutex and returns (reconfigure);           *)
(*   Take           the thread that owns the simulator, before a send,    *)
(*                  try-acquires the mutex and, when it gets it, copies   *)
(*                  the pending record into the setting in force          *)
(*                  (take_setting).  A busy mutex is no Take: the old     *)
(*                  setting stays in force and the record waits for the   *)
(*                  next uncontended pass, so a send may act on the old   *)
(*                  setting after the change (the unit test               *)
(*                  test_network_simulator_take_setting_does_not_wait).    *)
(* A record written over a waiting one replaces it.  Neither step touches *)
(* the packets already on the wire.                                        *)
(*                                                                         *)
(* setting is the one the caller last asked for, pendingSetting the        *)
(* record waiting, pending whether one waits, and applied the setting in  *)
(* force, the one a send acts on.  This module is Link's reconfiguration,  *)
(* kept beside Link so Link's own measured configurations keep the model   *)
(* they were measured on.                                                  *)
(*                                                                         *)
(* LiveSettingHolds:                                                       *)
(*   SendUsesForce   every send acts on the setting the caller last asked  *)
(*                   for, unless a change was still waiting to be taken    *)
(*                   at that send: once the owner has taken a change,      *)
(*                   every later send is held to it;                       *)
(*   NoInFlightDrop  a change removes nothing already in flight, so none   *)
(*                   sent before it is dropped by it.                      *)
(*                                                                         *)
(* Defect = "inflight" changes the setting and clears the wire (the        *)
(* reversed witness for NoInFlightDrop).  Defect = "stale" makes Take      *)
(* clear the pending flag without copying the record, so later sends use  *)
(* the old setting with nothing waiting (the reversed witness for         *)
(* SendUsesForce).  Defect = "none" is the control.                       *)
(***************************************************************************)
EXTENDS Link, Naturals, Sequences

CONSTANTS
    MaxSends,   \* bound on the sends a run explores, >= 1
    Defect      \* "none", "inflight" or "stale"

Settings == {"pass", "lose"}

ASSUME /\ MaxSends \in Nat \ {0}
       /\ Defect \in {"none", "inflight", "stale"}

(***************************************************************************)
(* Reconfigure is the relation a live change keeps: the requested setting  *)
(* moves to newSetting, and a packet already in flight keeps the setting   *)
(* it was sent under.                                                      *)
(***************************************************************************)
Reconfigure( oldSetting, newSetting ) ==
    /\ oldSetting \in Settings
    /\ newSetting \in Settings
    /\ newSetting # oldSetting

VARIABLES
    setting,          \* the setting the caller last asked for
    pendingSetting,   \* the record handed over and not yet taken
    pending,          \* a record waits (config_pending, the PENDING bit)
    applied,          \* the setting in force: the one a send acts on
    sendLog,          \* one record per send: what was asked, what applied, whether a change waited
    reconfigureDrops  \* copies a change removed from the wire (must be 0)

SendRecord == [pkt : Packets, inForce : Settings, appliedAtSend : Settings,
               pendingAtSend : BOOLEAN]

rcVars == <<wire, sent, pulse, setting, pendingSetting, pending, applied,
            sendLog, reconfigureDrops>>

RCInit ==
    /\ Init
    /\ setting = "pass"
    /\ pendingSetting = "pass"
    /\ pending = FALSE
    /\ applied = "pass"
    /\ sendLog = <<>>
    /\ reconfigureDrops = 0

(***************************************************************************)
(* The sender.  A send is counted whether or not the setting drops it, as  *)
(* Link!Send does; "lose" leaves the wire as it was.  It acts on applied.  *)
(***************************************************************************)
RCSend ==
    \E p \in Packets :
        /\ InFlight < Capacity
        /\ Len(sendLog) < MaxSends
        /\ sent' = NextCount
        /\ IF applied = "lose"
             THEN UNCHANGED wire
             ELSE \E d \in 0..MaxDelay : wire' = Append(wire, [pkt |-> p, due |-> d])
        /\ sendLog' = Append(sendLog, [pkt |-> p, inForce |-> setting,
                                       appliedAtSend |-> applied,
                                       pendingAtSend |-> pending])
        /\ UNCHANGED <<pulse, setting, pendingSetting, pending, applied, reconfigureDrops>>

RCDeliver ==
    \E p \in Packets :
        /\ Deliver(p)
        /\ UNCHANGED <<setting, pendingSetting, pending, applied, sendLog, reconfigureDrops>>

RCEnvironment ==
    /\ Environment
    /\ UNCHANGED <<setting, pendingSetting, pending, applied, sendLog, reconfigureDrops>>

(***************************************************************************)
(* The caller's change: the pending record and the flag, under the mutex.  *)
(* It leaves the wire alone and the setting in force as it was.            *)
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
        /\ pendingSetting' = next
        /\ pending' = TRUE
        /\ UNCHANGED <<sent, pulse, applied, sendLog>>

(***************************************************************************)
(* The owner's uncontended take: the waiting record becomes the setting in *)
(* force and the flag clears.                                              *)
(***************************************************************************)
Take ==
    /\ pending
    /\ applied' = IF Defect = "stale" THEN applied ELSE pendingSetting
    /\ pending' = FALSE
    /\ UNCHANGED <<wire, sent, pulse, setting, pendingSetting, sendLog, reconfigureDrops>>

RCNext == RCSend \/ RCDeliver \/ RCEnvironment \/ MCReconfigure \/ Take

Spec == RCInit /\ [][RCNext]_rcVars

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
RCTypeOK ==
    /\ TypeOK
    /\ setting \in Settings
    /\ pendingSetting \in Settings
    /\ pending \in BOOLEAN
    /\ applied \in Settings
    /\ sendLog \in Seq(SendRecord)
    /\ reconfigureDrops \in Nat

\* A send acts on the setting last asked for, unless a change still waited.
SendUsesForce ==
    \A i \in DOMAIN sendLog :
        sendLog[i].pendingAtSend \/ sendLog[i].appliedAtSend = sendLog[i].inForce

\* None sent before the change is dropped by it.
NoInFlightDrop == reconfigureDrops = 0

LiveSettingHolds == SendUsesForce /\ NoInFlightDrop

=============================================================================
