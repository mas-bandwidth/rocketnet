------------------------- MODULE MCLinkReconfigure -------------------------
(***************************************************************************)
(* The live link simulator setting: the loss setting changes while packets *)
(* are in flight.  A developer turns loss on in the middle of a session    *)
(* and the next packet the client sends is already held to the new setting *)
(* (rocketnet_client_network_simulator and rocketnet_server_network_        *)
(* simulator, sdk/rocketnet_client_transport.cpp and                       *)
(* sdk/rocketnet_server_transport.cpp; the setting reaches the thread that *)
(* owns the simulator as a config record under a tiny mutex and is applied  *)
(* there, outside the lock).                                                *)
(*                                                                         *)
(* Link models the wire; this module adds the setting in force, the        *)
(* Reconfigure action and the invariant the change must satisfy:            *)
(*                                                                         *)
(*   - a packet sent after the change is held to the NEW setting: it is    *)
(*     let out only when the setting in force at its send was "open", and  *)
(*     it is lost only when that setting was "lossy";                       *)
(*                                                                         *)
(*   - no packet sent before the change is dropped BY it: the copies in    *)
(*     flight when the setting changed keep their accounting (they are    *)
(*     still on the wire, or they left through a delivery), because the    *)
(*     change itself removes nothing.                                      *)
(*                                                                         *)
(* The setting is modelled as two values, "open" and "lossy": that is the  *)
(* shape of the change the API offers (a loss percentage that is zero or   *)
(* not), and the simulator decides the loss of a copy at Send              *)
(* (rocketnet_network_simulator_send), which is why a copy already in      *)
(* flight keeps the fate it was given.                                     *)
(*                                                                         *)
(* The counts are per version: copiesOut and copiesRemoved balance against *)
(* the copies on the wire, and outSince and lostSince count what the       *)
(* setting in force has done since the last change.                        *)
(*                                                                         *)
(* Fault is the control the module carries: "none" is the real behaviour, *)
(* "holds_old_setting" is a link that keeps the setting it started with   *)
(* (a change that never reaches the sends) and "drops_in_flight" is a     *)
(* link that drops what was already in flight when the setting changes.    *)
(* Both faults break LiveSettingHolds, and the committed configs that     *)
(* check them say which invariant they break.                              *)
(***************************************************************************)
EXTENDS Link, FiniteSets

CONSTANTS
    Reconfigurable,  \* BOOLEAN: may the setting change while packets are in flight
    MaxVersions,     \* bound on the number of setting changes, >= 1
    Fault            \* "none", "holds_old_setting" or "drops_in_flight"

ASSUME /\ Reconfigurable \in BOOLEAN
       /\ MaxVersions \in Nat \ {0}
       /\ Fault \in {"none", "holds_old_setting", "drops_in_flight"}

VARIABLES
    setting,        \* "open" or "lossy": the loss setting in force
    ver,            \* the version of the setting in force
    settings,       \* the setting of every version: settings[v+1] is version v
    copiesOut,      \* the copies that were let out, in all versions
    copiesRemoved,  \* the copies that left the wire
    outSince,       \* the copies the setting in force let out since the last change
    lostSince       \* the sends the setting in force lost since the last change

mcVars == <<wire, sent, pulse, setting, ver, settings, copiesOut, copiesRemoved, outSince, lostSince>>

Settings == {"open", "lossy"}

(***************************************************************************)
(* The setting a send is held to: the one in force, unless the link is    *)
(* the control that never learned the change, which keeps the setting of  *)
(* its first version.                                                      *)
(***************************************************************************)
HeldSetting == IF Fault = "holds_old_setting" THEN settings[1] ELSE setting

(***************************************************************************)
(* The send.  The loss decision happens here, under the setting in force  *)
(* then: a copy sent while the setting is "lossy" is lost, and a copy     *)
(* sent while it is "open" goes on the wire.                               *)
(***************************************************************************)
MCSend(p) ==
    /\ p \in Packets
    /\ InFlight < Capacity
    /\ sent' = NextCount
    /\ IF HeldSetting = "lossy" \/ LostAtSend(NextCount)
         THEN /\ UNCHANGED wire
              /\ lostSince' = lostSince + 1
              /\ UNCHANGED copiesOut
              /\ UNCHANGED outSince
         ELSE /\ \E d \in 0..MaxDelay : wire' = Append(wire, [pkt |-> p, due |-> d])
              /\ copiesOut' = copiesOut + 1
              /\ outSince' = outSince + 1
              /\ UNCHANGED lostSince
    /\ UNCHANGED <<pulse, setting, ver, settings, copiesRemoved>>

(***************************************************************************)
(* The delivery.  It is the only way a copy leaves the wire in this model  *)
(* (the cfg turns the arbitrary-loss pattern and the duplication off), so  *)
(* the count of removals is the count of deliveries.                       *)
(***************************************************************************)
MCDeliver(p) ==
    /\ Deliver(p)
    /\ copiesRemoved' = copiesRemoved + 1
    /\ UNCHANGED <<setting, ver, settings, copiesOut, outSince, lostSince>>

(***************************************************************************)
(* The live change.  It replaces the setting and opens a new version.  It  *)
(* touches nothing else: the copies in flight when it happens keep their  *)
(* place and their fate.                                                   *)
(***************************************************************************)
MCReconfigure(s) ==
    /\ Reconfigurable
    /\ s \in Settings
    /\ s # setting
    /\ ver < MaxVersions
    /\ setting' = s
    /\ ver' = ver + 1
    /\ settings' = Append(settings, s)
    /\ outSince' = 0
    /\ lostSince' = 0
    /\ IF Fault = "drops_in_flight"
         THEN /\ wire' = <<>>
              /\ copiesRemoved' = copiesRemoved + Len(wire)
         ELSE /\ UNCHANGED wire
              /\ UNCHANGED copiesRemoved
    /\ UNCHANGED <<sent, pulse, copiesOut>>

(***************************************************************************)
(* The rest of the wire: the reordering, the jitter and the passage of    *)
(* time.  None of them changes how many copies the wire holds.             *)
(***************************************************************************)
MCEnvironment ==
    /\ \E i \in DOMAIN wire : Reorder(i) \/ Delay(i)
    /\ (MaxDelay > 0 /\ Age)

(***************************************************************************)
(* The model.  The setting starts "open": a connection that starts with    *)
(* the simulator off is the "open" setting, and the first change is the   *)
(* developer's first call.                                                 *)
(***************************************************************************)
MCInit ==
    /\ wire = <<>>
    /\ sent = 0
    /\ pulse = 0
    /\ setting = "open"
    /\ ver = 0
    /\ settings = <<"open">>
    /\ copiesOut = 0
    /\ copiesRemoved = 0
    /\ outSince = 0
    /\ lostSince = 0

MCNext ==
    \/ (\E p \in Packets : MCSend(p))
    \/ (\E p \in Packets : MCDeliver(p))
    \/ MCEnvironment
    \/ (\E s \in Settings : MCReconfigure(s))

MCWireFair ==
    /\ \A p \in Packets : SF_mcVars(MCDeliver(p))
    /\ WF_mcVars(Age)

Spec == MCInit /\ [][MCNext]_mcVars /\ MCWireFair

(***************************************************************************)
(* The invariant.                                                          *)
(*                                                                         *)
(* LiveSettingHolds says both halves of the change:                         *)
(*                                                                         *)
(*   - the accounting balances: every copy that was let out is either      *)
(*     still on the wire or was delivered.  A change that dropped the      *)
(*     copies in flight breaks the balance, because the change removes     *)
(*     nothing it does not account for.                                    *)
(*                                                                         *)
(*   - the setting in force held every send since the last change: a copy *)
(*     sent after the change went out only when the new setting was        *)
(*     "open", and was lost only when the new setting was "lossy".         *)
(***************************************************************************)
LiveSettingHolds ==
    /\ copiesOut = copiesRemoved + Len(wire)
    /\ outSince > 0 => setting = "open"
    /\ lostSince > 0 => setting = "lossy"

MCTypeOK ==
    /\ wire \in Seq(Copy)
    /\ sent \in 0..Horizon
    /\ pulse \in {0, 1}
    /\ setting \in Settings
    /\ ver \in 0..MaxVersions
    /\ settings \in Seq(Settings)
    /\ Len(settings) = ver + 1
    /\ copiesOut \in Nat
    /\ copiesRemoved \in Nat
    /\ outSince \in Nat
    /\ lostSince \in Nat

(***************************************************************************)
(* The change itself removes nothing: the second half of the change, said *)
(* as the balance of the counts above (a change that dropped the copies   *)
(* in flight would break it).                                              *)
(***************************************************************************)

=============================================================================
