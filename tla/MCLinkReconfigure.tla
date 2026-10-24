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
(* Link models the wire: the copies in flight, their due counters, the     *)
(* loss patterns and the delivery.  This module adds the setting in force, *)
(* the Reconfigure action and the invariant the change must satisfy:       *)
(*                                                                         *)
(*   - a packet sent after the change is held to the NEW setting: it is    *)
(*     let out only when the setting in force at its send was "open", and  *)
(*     it is lost only when that setting was "lossy";                       *)
(*                                                                         *)
(*   - no packet sent before the change is dropped BY it: the copies in    *)
(*     flight when the setting changed keep their place, because the change *)
(*     itself removes nothing.                                              *)
(*                                                                         *)
(* The setting is modelled as two values, "open" and "lossy": that is the  *)
(* shape of the change the API offers (a loss percentage that is zero or   *)
(* not), and the simulator decides the loss of a copy at Send              *)
(* (rocketnet_network_simulator_send), which is why a copy already in      *)
(* flight keeps the fate it was given.                                     *)
(*                                                                         *)
(* The model lives beside Link rather than inside it, and the reason is    *)
(* mechanical: Link's variables are substituted by eight other models      *)
(* (FragmentsAndAcks, RNEndpoint, RNWindow, ClientConnection, RNBlocks,    *)
(* ServerConnection, RNSlots and RNChannel), so a new variable in Link    *)
(* would change every one of their fingerprints and their measured        *)
(* records.  The Reconfigure action below is the one Link needs for a     *)
(* live setting, and it is stated over the same wire.                      *)
(*                                                                         *)
(* Fault is the control the module carries: "none" is the real behaviour,  *)
(* "holds_old_setting" is a link that keeps the setting it started with   *)
(* (a change that never reaches the sends) and "drops_in_flight" is a      *)
(* link that drops what was already in flight when the setting changes.    *)
(* Both faults break LiveSettingHolds, and the committed configs that     *)
(* check them say which invariant they break.                              *)
(***************************************************************************)
EXTENDS Link

CONSTANTS
    Reconfigurable,  \* BOOLEAN: may the setting change while packets are in flight
    MaxVersions,     \* bound on the number of setting changes, >= 1
    Fault            \* "none", "holds_old_setting" or "drops_in_flight"

ASSUME /\ Reconfigurable \in BOOLEAN
       /\ MaxVersions \in Nat \ {0}
       /\ Fault \in {"none", "holds_old_setting", "drops_in_flight"}

VARIABLES
    setting,         \* "open" or "lossy": the loss setting in force
    ver,             \* how many changes the setting has taken
    outSince,        \* 1 when a copy was let out since the last change
    lostSince,       \* 1 when a send was lost since the last change
    droppedByChange  \* 1 when a change removed a copy that was in flight

mcVars == <<wire, sent, pulse, setting, ver, outSince, lostSince, droppedByChange>>

Settings == {"open", "lossy"}

(***************************************************************************)
(* The setting a send is held to: the one in force, unless the link is    *)
(* the control that never learned the change, which keeps the setting it   *)
(* started with.                                                           *)
(***************************************************************************)
HeldSetting == IF Fault = "holds_old_setting" THEN "open" ELSE setting

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
              /\ lostSince' = 1
              /\ UNCHANGED outSince
         ELSE /\ \E d \in 0..MaxDelay : wire' = Append(wire, [pkt |-> p, due |-> d])
              /\ outSince' = 1
              /\ UNCHANGED lostSince
    /\ UNCHANGED <<pulse, setting, ver, droppedByChange>>

(***************************************************************************)
(* The delivery.  It is the only way a copy leaves the wire in this model  *)
(* (the cfg turns the arbitrary-loss pattern and the duplication off), so  *)
(* the wire holds exactly the copies that were let out and not delivered.  *)
(***************************************************************************)
MCDeliver(p) ==
    /\ Deliver(p)
    /\ UNCHANGED <<setting, ver, outSince, lostSince, droppedByChange>>

(***************************************************************************)
(* The live change.  It replaces the setting and counts the change.  It    *)
(* touches nothing else: the copies in flight when it happens keep their   *)
(* place and their fate.                                                   *)
(***************************************************************************)
MCReconfigure(s) ==
    /\ Reconfigurable
    /\ s \in Settings
    /\ s # setting
    /\ ver < MaxVersions
    /\ setting' = s
    /\ ver' = ver + 1
    /\ outSince' = 0
    /\ lostSince' = 0
    /\ IF Fault = "drops_in_flight"
         THEN /\ wire' = <<>>
              /\ droppedByChange' = IF Len(wire) > 0 THEN 1 ELSE droppedByChange
         ELSE /\ UNCHANGED wire
              /\ UNCHANGED droppedByChange
    /\ UNCHANGED <<sent, pulse>>

(***************************************************************************)
(* The rest of the wire: the reordering, the jitter and the passage of    *)
(* time.  None of them changes how many copies the wire holds.             *)
(***************************************************************************)
MCEnvironment ==
    /\ ( (\E i \in DOMAIN wire : Reorder(i) \/ Delay(i))
         \/ (MaxDelay > 0 /\ Age) )
    /\ UNCHANGED <<setting, ver, outSince, lostSince, droppedByChange>>

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
    /\ outSince = 0
    /\ lostSince = 0
    /\ droppedByChange = 0

MCNext ==
    \/ (\E p \in Packets : MCSend(p))
    \/ (\E p \in Packets : MCDeliver(p))
    \/ MCEnvironment
    \/ (\E s \in Settings : MCReconfigure(s))

Spec == MCInit /\ [][MCNext]_mcVars

(***************************************************************************)
(* The invariant.                                                          *)
(*                                                                         *)
(* LiveSettingHolds says both halves of the change:                         *)
(*                                                                         *)
(*   - the change removes nothing: no change ever dropped a copy that was  *)
(*     already in flight (droppedByChange stays 0);                        *)
(*                                                                         *)
(*   - the setting in force held every send since the last change: a copy *)
(*     sent after the change went out only when the new setting was        *)
(*     "open", and was lost only when the new setting was "lossy".         *)
(***************************************************************************)
LiveSettingHolds ==
    /\ droppedByChange = 0
    /\ outSince = 1 => setting = "open"
    /\ lostSince = 1 => setting = "lossy"

MCTypeOK ==
    /\ wire \in Seq(Copy)
    /\ sent \in 0..Horizon
    /\ pulse \in {0, 1}
    /\ setting \in Settings
    /\ ver \in 0..MaxVersions
    /\ outSince \in {0, 1}
    /\ lostSince \in {0, 1}
    /\ droppedByChange \in {0, 1}

(***************************************************************************)
(* The change itself removes nothing: the first half of the change, said   *)
(* as the flag above (a change that dropped the copies in flight would     *)
(* break it).                                                              *)
(***************************************************************************)

=============================================================================
