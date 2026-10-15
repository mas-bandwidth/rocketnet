------------------------- MODULE RNConnectionMemory -------------------------
(***************************************************************************)
(* Per-connection message memory, and the kick on exhaustion               *)
(* (docs/CHANNELS.md 4; sdk/rocketnet_allocator.cpp, the region's          *)
(* exhausted state, rn_region_reset and rn_connection_memory_decide).      *)
(*                                                                         *)
(* A server owns one region per client slot, all carved from one block at  *)
(* create; a client owns one region (a server of one slot is the same      *)
(* machine).  A slot is Free, Connected or Disconnected.  Its region has a *)
(* used count against a fixed capacity, a sticky exhausted flag with its   *)
(* reason, and the set of clients whose records it still holds.            *)
(*                                                                         *)
(* Actions:                                                                *)
(*   Connect(s, c)  a client takes a Free slot                             *)
(*   Use(s)         a send, receive or hold takes room in the region       *)
(*   Release(s)     an acked send or a released delivery gives room back   *)
(*   Exhaust(s)     an operation finds no room: the region's sticky        *)
(*                  exhausted state, reason MESSAGE_MEMORY_EXHAUSTED       *)
(*   Kick(s)        the owner disconnects the exhausted connection with    *)
(*                  that reason (the server kicks the slot; a client       *)
(*                  disconnects itself)                                    *)
(*   Leave(s)       an ordinary disconnect, not from exhaustion            *)
(*   Reuse(s)       the slot is freed and its region reset                 *)
(*                                                                         *)
(* Reset = FALSE makes Reuse skip the reset; CarryOn = TRUE lets an        *)
(* exhausted connection keep using and releasing.  Each must fail; those   *)
(* are the reversed witnesses.                                             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Slots,        \* the server's client slots (one for a client)
          Clients,      \* the clients that may connect
          Capacity,     \* the region's room, in abstract records
          Reset,        \* Reuse runs rn_region_carve
          CarryOn       \* an exhausted connection keeps working (broken)

ASSUME Slots # {} /\ Clients # {} /\ Capacity \in Nat \ {0}
ASSUME Reset \in BOOLEAN /\ CarryOn \in BOOLEAN

NoClient == "none"
Exhausted == "MESSAGE_MEMORY_EXHAUSTED"
Left == "LEFT"
Reasons == {"none", Exhausted, Left}
States == {"Free", "Connected", "Disconnected"}

VARIABLES state,        \* state[s]: the slot's connection state
          owner,        \* owner[s]: the client on the slot, or NoClient
          used,         \* used[s]: region room in use
          exhausted,    \* exhausted[s]: the region's sticky exhausted state
          reason,       \* reason[s]: why the slot's connection ended
          records       \* records[s]: clients whose records the region still holds

vars == <<state, owner, used, exhausted, reason, records>>

Slot(s) == <<state[s], owner[s], used[s], exhausted[s], reason[s], records[s]>>

OnSlot == { owner[s] : s \in { t \in Slots : state[t] # "Free" } }

TypeOK ==
    /\ state \in [Slots -> States]
    /\ owner \in [Slots -> Clients \cup {NoClient}]
    /\ used \in [Slots -> 0..Capacity]
    /\ exhausted \in [Slots -> BOOLEAN]
    /\ reason \in [Slots -> Reasons]
    /\ records \in [Slots -> SUBSET Clients]

Init ==
    /\ state = [s \in Slots |-> "Free"]
    /\ owner = [s \in Slots |-> NoClient]
    /\ used = [s \in Slots |-> 0]
    /\ exhausted = [s \in Slots |-> FALSE]
    /\ reason = [s \in Slots |-> "none"]
    /\ records = [s \in Slots |-> {}]

\* the transport admits a client to a free slot; the region is as Reuse left it
Connect(s, c) ==
    /\ state[s] = "Free"
    /\ c \notin OnSlot
    /\ state' = [state EXCEPT ![s] = "Connected"]
    /\ owner' = [owner EXCEPT ![s] = c]
    /\ UNCHANGED <<used, exhausted, reason, records>>

\* rn_region_send_alloc (or a receive placement) finding room
Use(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s] \/ CarryOn
    /\ used[s] < Capacity
    /\ used' = [used EXCEPT ![s] = @ + 1]
    /\ records' = [records EXCEPT ![s] = @ \cup {owner[s]}]
    /\ UNCHANGED <<state, owner, exhausted, reason>>

\* rn_region_send_free (an ack) or a delivery released by the application
Release(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s] \/ CarryOn
    /\ used[s] > 0
    /\ used' = [used EXCEPT ![s] = @ - 1]
    /\ UNCHANGED <<state, owner, exhausted, reason, records>>

\* rn_region_exhaust: an operation that cannot get room sets the sticky state
Exhaust(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s]
    /\ used[s] = Capacity
    /\ exhausted' = [exhausted EXCEPT ![s] = TRUE]
    /\ reason' = [reason EXCEPT ![s] = Exhausted]
    /\ UNCHANGED <<state, owner, used, records>>

\* rn_connection_memory_decide says kick: the server disconnects the slot
\* (rocketnet_server_transport_disconnect_client); a client disconnects itself
Kick(s) ==
    /\ state[s] = "Connected"
    /\ exhausted[s]
    /\ state' = [state EXCEPT ![s] = "Disconnected"]
    /\ UNCHANGED <<owner, used, exhausted, reason, records>>

\* an ordinary disconnect: the client leaves, times out or is disconnected
Leave(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s]
    /\ state' = [state EXCEPT ![s] = "Disconnected"]
    /\ reason' = [reason EXCEPT ![s] = Left]
    /\ UNCHANGED <<owner, used, exhausted, records>>

\* rn_region_reset: the slot is freed and its region carved before reuse
Reuse(s) ==
    /\ state[s] = "Disconnected"
    /\ state' = [state EXCEPT ![s] = "Free"]
    /\ owner' = [owner EXCEPT ![s] = NoClient]
    /\ IF Reset
         THEN /\ used' = [used EXCEPT ![s] = 0]
              /\ exhausted' = [exhausted EXCEPT ![s] = FALSE]
              /\ reason' = [reason EXCEPT ![s] = "none"]
              /\ records' = [records EXCEPT ![s] = {}]
         ELSE UNCHANGED <<used, exhausted, reason, records>>

Next ==
    \E s \in Slots :
        \/ \E c \in Clients : Connect(s, c)
        \/ Use(s)
        \/ Release(s)
        \/ Exhaust(s)
        \/ Kick(s)
        \/ Leave(s)
        \/ Reuse(s)

Fairness == \A s \in Slots : WF_vars(Kick(s)) /\ WF_vars(Reuse(s))

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
\* Safety

UsedBounded == \A s \in Slots : used[s] <= Capacity

\* exhaustion has one name, and only exhaustion carries it
ExhaustedReasonNamed ==
    \A s \in Slots : exhausted[s] <=> reason[s] = Exhausted

\* a Free slot is a freshly carved region, so the Connect that takes it sees
\* used = 0, no exhaustion and no record of the previous client; a connected
\* slot holds records of its own client only
ReusedSlotStartsClean ==
    \A s \in Slots :
        /\ state[s] = "Free" => /\ used[s] = 0
                                /\ ~exhausted[s]
                                /\ reason[s] = "none"
                                /\ records[s] = {}
        /\ state[s] = "Connected" => records[s] \subseteq {owner[s]}

\* from exhausted, the only step that changes the slot is Kick to Disconnected
\* with the reason kept: no further use, release or delivery
ExhaustionOnlyLeadsToDisconnect ==
    [][\A s \in Slots :
          (exhausted[s] /\ state[s] = "Connected") =>
              \/ Slot(s)' = Slot(s)
              \/ /\ state'[s] = "Disconnected"
                 /\ reason'[s] = Exhausted
                 /\ used'[s] = used[s]
                 /\ records'[s] = records[s]
      ]_vars

\* every step changes at most one slot: one slot's use or exhaustion never
\* changes another slot's state or region
NoCrossSlotEffect ==
    [][\E s \in Slots : \A t \in Slots \ {s} : Slot(t)' = Slot(t)]_vars

\* Liveness: an exhausted slot is eventually Free (kicked, then reset)
ExhaustedEventuallyFree ==
    \A s \in Slots : exhausted[s] ~> state[s] = "Free"

=============================================================================
