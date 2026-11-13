---------------------------- MODULE RNDestroyWipe ----------------------------
(***************************************************************************)
(* What destroy writes (sdk/rocketnet_api.cpp rocketnet_server_destroy,   *)
(* sdk/rocketnet_server_transport.cpp rocketnet_server_transport_destroy, *)
(* sdk/rocketnet_encryption_map.cpp rocketnet_encryption_map_remove and    *)
(* rocketnet_encryption_map_destroy, sdk/rocketnet_client_slots.cpp       *)
(* rocketnet_client_slots_release and rocketnet_client_slots_destroy).     *)
(* The server side only: the client's wipes                                *)
(* (rocketnet_client_protocol_wipe_secrets at reset and at the client      *)
(* transport's destroy) and the client I/O's copy of the open key are not  *)
(* modelled.                                                               *)
(*                                                                         *)
(* The server is one pre-allocated block of pages.  Nothing in a run gives *)
(* a page back, so the pages resident are the pages written since create   *)
(* (touched), and the run's resident peak is the pages the run wrote       *)
(* (runTouched).  Each secret lives on a page: the server keys on KeyPage, *)
(* the disconnect ring's key copies on RingPages (both written at create:  *)
(* the keys set, the ring's key array zeroed), each mapping's entry on     *)
(* EntryPage[m] and the user data in its client's slot on SlotPage[m].     *)
(* Every write the model makes, Destroy's wipes included, touches the page *)
(* it writes, so a wipe whose extent reached a page the run never wrote    *)
(* would show in touched.                                                  *)
(*                                                                         *)
(* The secrets: the server keys (the server secret key and both cookie     *)
(* keys); each mapping's session keys, nonce and connect-token user data,  *)
(* written when the mapping is added; the client slot's copy of that user  *)
(* data, written when the slot is assigned; and the server-to-client       *)
(* session key the transport copies into its disconnect ring when it       *)
(* defers a disconnect burst (rocketnet_server_transport_defer_disconnect_ *)
(* burst), before the connection reset removes the mapping.  That copy     *)
(* outlives the mapping while the burst waits; it is wiped when the burst  *)
(* leaves the ring head, and the whole key array at destroy.  Destroy      *)
(* wipes exactly the secrets and frees the block as it is: the OS          *)
(* zero-fills a page before another process gets it.                       *)
(*                                                                         *)
(* Add folds rocketnet_encryption_map_add and rocketnet_client_slots_assign *)
(* into one step; the code runs them in its own order, and nothing here    *)
(* reads between them.                                                     *)
(*                                                                         *)
(* SecretsZeroAtFree: when the block is freed, no secret is set.           *)
(* ShutdownPeakWithinRun: destroy touches no page the run did not, so the  *)
(* shutdown's resident peak is never above the run's.                      *)
(* NoSecretOutsideLive: while running, a mapping and its slot hold         *)
(* secrets only while it is live.  RingCopyOnlyWhileWaiting: a ring key    *)
(* copy is set only while its burst waits.                                 *)
(*                                                                         *)
(* Reversed witnesses, each must fail: WipeWholeBlock = TRUE is the old    *)
(* destroy (sodium_memzero over the whole block once a start used it);     *)
(* WipeOnRemove = FALSE is a remove that leaves the mapping's secrets;     *)
(* WipeLiveAtDestroy = FALSE is a destroy that skips the live mappings;    *)
(* WipeSlotOnRelease = FALSE is a slot release that leaves the user data;  *)
(* WipeSlotsAtDestroy = FALSE is a slots destroy that leaves it;           *)
(* WipeEverySlot = TRUE is a slots destroy that wipes every slot, not only *)
(* the connected ones, and so writes slot pages the run never touched;     *)
(* WipeRingOnLeave = FALSE leaves a burst's key copy after it leaves the   *)
(* ring; WipeRingAtDestroy = FALSE frees the ring's key array unwiped.     *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Pages,              \* the block's pages
          Maps,               \* the encryption map's entries
          KeyPage,            \* the page of the server keys
          RingPages,          \* the pages of the disconnect ring's key array
          EntryPage,          \* the page of each mapping's entry
          SlotPage,           \* the page of each mapping's client slot
          WipeWholeBlock,     \* destroy wipes every page of the block
          WipeOnRemove,       \* remove wipes the mapping's secrets
          WipeLiveAtDestroy,  \* destroy wipes the live mappings' secrets
          WipeSlotOnRelease,  \* a slot's release wipes its user data
          WipeSlotsAtDestroy, \* the slots' destroy wipes the connected slots' user data
          WipeEverySlot,      \* the slots' destroy wipes every slot, connected or not
          WipeRingOnLeave,    \* a burst leaving the ring head wipes its key copy
          WipeRingAtDestroy   \* the transport's destroy wipes the ring's key array

ASSUME WipeWholeBlock \in BOOLEAN /\ WipeOnRemove \in BOOLEAN /\ WipeLiveAtDestroy \in BOOLEAN
ASSUME WipeSlotOnRelease \in BOOLEAN /\ WipeSlotsAtDestroy \in BOOLEAN /\ WipeEverySlot \in BOOLEAN
ASSUME WipeRingOnLeave \in BOOLEAN /\ WipeRingAtDestroy \in BOOLEAN
ASSUME KeyPage \in Pages /\ RingPages \subseteq Pages
ASSUME EntryPage \in [ Maps -> Pages ] /\ SlotPage \in [ Maps -> Pages ]

VARIABLES phase,      \* "running" or "freed"
          touched,    \* the pages resident: written since create
          runTouched, \* the pages written before destroy
          serverKeys, \* "set" or "zero": the server secret key and cookie keys
          live,       \* the live mappings
          keys,       \* each mapping's session keys, nonce and user data, "set" or "zero"
          slotData,   \* the user data in the slot of each mapping's client, "set" or "zero"
          waiting,    \* the mappings whose disconnect burst waits in the ring
          ringKey     \* each mapping's key copy in the ring, "set" or "zero"

vars == <<phase, touched, runTouched, serverKeys, live, keys, slotData, waiting, ringKey>>

\* create: the server keys written and the ring's key array zeroed
CreatePages == {KeyPage} \cup RingPages

Init ==
    /\ phase = "running"
    /\ touched = CreatePages
    /\ runTouched = CreatePages
    /\ serverKeys = "set"
    /\ live = {}
    /\ keys = [ m \in Maps |-> "zero" ]
    /\ slotData = [ m \in Maps |-> "zero" ]
    /\ waiting = {}
    /\ ringKey = [ m \in Maps |-> "zero" ]

\* a run's write to the block: touched and runTouched both grow
RunWrites(P) ==
    /\ touched' = touched \cup P
    /\ runTouched' = runTouched \cup P

\* a connection writes a page of the block (a message, a record, a buffer)
Use(p) ==
    /\ phase = "running"
    /\ RunWrites({p})
    /\ UNCHANGED <<phase, serverKeys, live, keys, slotData, waiting, ringKey>>

\* rocketnet_encryption_map_add: the entry written whole, then its keys and
\* user data; rocketnet_client_slots_assign: the slot's copy (one step: the
\* slot is assigned to the mapping's client)
Add(m) ==
    /\ phase = "running"
    /\ m \notin live
    /\ m \notin waiting
    /\ live' = live \cup {m}
    /\ keys' = [ keys EXCEPT ![m] = "set" ]
    /\ slotData' = [ slotData EXCEPT ![m] = "set" ]
    /\ RunWrites({EntryPage[m], SlotPage[m]})
    /\ UNCHANGED <<phase, serverKeys, waiting, ringKey>>

\* rocketnet_server_connection_reset: rocketnet_encryption_map_remove (the
\* secrets wiped, the entry not live), then rocketnet_client_slots_release.
\* The reset follows either a plain end, or a disconnect_client that
\* deferred the burst (rocketnet_server_transport_defer_disconnect_burst),
\* which first copies the mapping's server-to-client key into the ring.
Remove(m) ==
    /\ phase = "running"
    /\ m \in live
    /\ live' = live \ {m}
    /\ keys' = IF WipeOnRemove THEN [ keys EXCEPT ![m] = "zero" ] ELSE keys
    /\ slotData' = IF WipeSlotOnRelease THEN [ slotData EXCEPT ![m] = "zero" ] ELSE slotData
    /\ \/ /\ UNCHANGED <<waiting, ringKey>>
          /\ RunWrites({EntryPage[m], SlotPage[m]})
       \/ /\ waiting' = waiting \cup {m}
          /\ ringKey' = [ ringKey EXCEPT ![m] = "set" ]
          /\ RunWrites({EntryPage[m], SlotPage[m]} \cup RingPages)
    /\ UNCHANGED <<phase, serverKeys>>

\* rocketnet_server_transport_drain_disconnect_bursts and the stop step: a
\* burst complete or abandoned leaves the ring head, its key copy wiped
BurstLeaves(m) ==
    /\ phase = "running"
    /\ m \in waiting
    /\ waiting' = waiting \ {m}
    /\ ringKey' = IF WipeRingOnLeave THEN [ ringKey EXCEPT ![m] = "zero" ] ELSE ringKey
    /\ RunWrites(RingPages)
    /\ UNCHANGED <<phase, serverKeys, live, keys, slotData>>

\* the slots destroy wipes the connected slots (rocketnet_client_slots_destroy),
\* or, in the witness, every slot
SlotsWiped == IF WipeEverySlot THEN Maps ELSE live

\* the pages destroy's wipes write
DestroyPages ==
    {KeyPage}
    \cup { EntryPage[m] : m \in live }
    \cup { SlotPage[m] : m \in SlotsWiped }
    \cup RingPages

\* stop, then destroy: the server keys, the live mappings' secrets, the
\* connected slots' user data and the ring's key array wiped, the block
\* freed as it is
Destroy ==
    /\ phase = "running"
    /\ phase' = "freed"
    /\ serverKeys' = "zero"
    /\ keys' = [ m \in Maps |-> IF m \in live /\ WipeLiveAtDestroy THEN "zero" ELSE keys[m] ]
    /\ slotData' = [ m \in Maps |-> IF m \in SlotsWiped /\ WipeSlotsAtDestroy THEN "zero" ELSE slotData[m] ]
    /\ ringKey' = IF WipeRingAtDestroy THEN [ m \in Maps |-> "zero" ] ELSE ringKey
    /\ touched' = IF WipeWholeBlock THEN Pages ELSE touched \cup DestroyPages
    /\ UNCHANGED <<runTouched, live, waiting>>

Freed ==
    /\ phase = "freed"
    /\ UNCHANGED vars

Next ==
    \/ Destroy
    \/ Freed
    \/ \E p \in Pages : Use(p)
    \/ \E m \in Maps : Add(m) \/ Remove(m) \/ BurstLeaves(m)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in { "running", "freed" }
    /\ touched \subseteq Pages
    /\ runTouched \subseteq Pages
    /\ serverKeys \in { "set", "zero" }
    /\ live \subseteq Maps
    /\ keys \in [ Maps -> { "set", "zero" } ]
    /\ slotData \in [ Maps -> { "set", "zero" } ]
    /\ waiting \subseteq Maps
    /\ ringKey \in [ Maps -> { "set", "zero" } ]

SecretsZeroAtFree ==
    phase = "freed" =>
        /\ serverKeys = "zero"
        /\ \A m \in Maps : keys[m] = "zero" /\ slotData[m] = "zero" /\ ringKey[m] = "zero"

ShutdownPeakWithinRun == touched \subseteq runTouched

\* while running, a mapping and its slot hold secrets only while it is live
NoSecretOutsideLive == phase = "running" => \A m \in Maps : ( keys[m] = "set" \/ slotData[m] = "set" ) => m \in live

\* while running, a ring key copy is set only while its burst waits (the
\* bounded retention past the mapping's removal)
RingCopyOnlyWhileWaiting == phase = "running" => \A m \in Maps : ringKey[m] = "set" => m \in waiting
=============================================================================
