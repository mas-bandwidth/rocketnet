---------------------------- MODULE RNDestroyWipe ----------------------------
(***************************************************************************)
(* What destroy writes (sdk/rocketnet_api.cpp rocketnet_server_destroy,   *)
(* sdk/rocketnet_server_transport.cpp rocketnet_server_transport_destroy, *)
(* sdk/rocketnet_encryption_map.cpp rocketnet_encryption_map_remove and    *)
(* rocketnet_encryption_map_destroy; the client side is the same shape:    *)
(* rocketnet_client_protocol_wipe_secrets at reset and at transport        *)
(* destroy).                                                               *)
(*                                                                         *)
(* The server is one pre-allocated block of pages.  A run touches the      *)
(* pages its connections use, and only those.  Its secrets are the server  *)
(* keys (the server secret key and both cookie keys), each mapping's       *)
(* session keys, nonce and connect-token user data, written when the       *)
(* mapping is added, and the client slot's copy of that user data         *)
(* (sdk/rocketnet_client_slots.cpp), written when the slot is assigned.    *)
(* Destroy wipes exactly the secrets and frees the block as it is: the OS  *)
(* zero-fills a page before another process gets it.                       *)
(*                                                                         *)
(* SecretsZeroAtFree: when the block is freed, no secret is set.           *)
(* ShutdownPeakWithinRun: destroy touches no page the run did not, so the  *)
(* shutdown's resident peak is never above the run's.                      *)
(*                                                                         *)
(* WipeWholeBlock = TRUE is the old destroy (sodium_memzero over the whole *)
(* block once a start used it); WipeOnRemove = FALSE is a remove that      *)
(* leaves the mapping's secrets; WipeLiveAtDestroy = FALSE is a destroy    *)
(* that skips the live mappings; WipeSlotOnRelease = FALSE is a slot       *)
(* release that leaves the user data; WipeSlotsAtDestroy = FALSE is a      *)
(* slots destroy that leaves it.  Each must fail: the reversed witnesses.  *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Pages,              \* the block's pages
          Maps,               \* the encryption map's entries
          WipeWholeBlock,     \* destroy wipes every page of the block
          WipeOnRemove,       \* remove wipes the mapping's secrets
          WipeLiveAtDestroy,  \* destroy wipes the live mappings' secrets
          WipeSlotOnRelease,  \* a slot's release wipes its user data
          WipeSlotsAtDestroy  \* the slots' destroy wipes every slot's user data

ASSUME WipeWholeBlock \in BOOLEAN /\ WipeOnRemove \in BOOLEAN /\ WipeLiveAtDestroy \in BOOLEAN
ASSUME WipeSlotOnRelease \in BOOLEAN /\ WipeSlotsAtDestroy \in BOOLEAN

VARIABLES phase,      \* "running" or "freed"
          touched,    \* the pages resident: touched by a write since create
          runTouched, \* the pages the run touched
          serverKeys, \* "set" or "zero": the server secret key and cookie keys
          live,       \* the live mappings
          keys,       \* each mapping's session keys, nonce and user data, "set" or "zero"
          slotData    \* the user data in the slot of each mapping's client, "set" or "zero"

vars == <<phase, touched, runTouched, serverKeys, live, keys, slotData>>

Init ==
    /\ phase = "running"
    /\ touched = {}
    /\ runTouched = {}
    /\ serverKeys = "set"
    /\ live = {}
    /\ keys = [ m \in Maps |-> "zero" ]
    /\ slotData = [ m \in Maps |-> "zero" ]

\* a connection writes a page of the block (a message, a record, a buffer)
Use(p) ==
    /\ phase = "running"
    /\ touched' = touched \cup {p}
    /\ runTouched' = runTouched \cup {p}
    /\ UNCHANGED <<phase, serverKeys, live, keys, slotData>>

\* rocketnet_encryption_map_add: the entry written whole, then its keys and
\* user data; rocketnet_client_slots_assign: the slot's copy (one step: the
\* slot is assigned to the mapping's client)
Add(m) ==
    /\ phase = "running"
    /\ m \notin live
    /\ live' = live \cup {m}
    /\ keys' = [ keys EXCEPT ![m] = "set" ]
    /\ slotData' = [ slotData EXCEPT ![m] = "set" ]
    /\ UNCHANGED <<phase, touched, runTouched, serverKeys>>

\* rocketnet_server_connection_reset: rocketnet_encryption_map_remove (the
\* secrets wiped, the entry not live), then rocketnet_client_slots_release
Remove(m) ==
    /\ phase = "running"
    /\ m \in live
    /\ live' = live \ {m}
    /\ keys' = IF WipeOnRemove THEN [ keys EXCEPT ![m] = "zero" ] ELSE keys
    /\ slotData' = IF WipeSlotOnRelease THEN [ slotData EXCEPT ![m] = "zero" ] ELSE slotData
    /\ UNCHANGED <<phase, touched, runTouched, serverKeys>>

\* stop, then destroy: the server keys and the live mappings' secrets wiped,
\* the block freed as it is
Destroy ==
    /\ phase = "running"
    /\ phase' = "freed"
    /\ serverKeys' = "zero"
    /\ keys' = [ m \in Maps |-> IF m \in live /\ WipeLiveAtDestroy THEN "zero" ELSE keys[m] ]
    /\ slotData' = IF WipeSlotsAtDestroy THEN [ m \in Maps |-> "zero" ] ELSE slotData
    /\ touched' = IF WipeWholeBlock THEN Pages ELSE touched
    /\ UNCHANGED <<runTouched, live>>

Freed ==
    /\ phase = "freed"
    /\ UNCHANGED vars

Next == Destroy \/ Freed \/ \E p \in Pages : Use(p) \/ \E m \in Maps : Add(m) \/ Remove(m)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in { "running", "freed" }
    /\ touched \subseteq Pages
    /\ runTouched \subseteq Pages
    /\ serverKeys \in { "set", "zero" }
    /\ live \subseteq Maps
    /\ keys \in [ Maps -> { "set", "zero" } ]
    /\ slotData \in [ Maps -> { "set", "zero" } ]

SecretsZeroAtFree ==
    phase = "freed" => ( serverKeys = "zero" /\ \A m \in Maps : keys[m] = "zero" /\ slotData[m] = "zero" )

ShutdownPeakWithinRun == touched \subseteq runTouched

\* while running, a mapping and its slot hold secrets only while it is live
NoSecretOutsideLive == phase = "running" => \A m \in Maps : ( keys[m] = "set" \/ slotData[m] = "set" ) => m \in live
=============================================================================
