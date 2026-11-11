------------------------- MODULE RNConnectionMemory -------------------------
(***************************************************************************)
(* Per-connection message memory: a send refused for no room, and the     *)
(* kick on the receive side's exhaustion                                   *)
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
(*   UseSpan(s, n)  a large send takes n records of room at once (a span)  *)
(*   Release(s)     an acked send, an abort or a released delivery gives   *)
(*                  room back; also while exhausted, since it takes none   *)
(*   Refuse(s)      a send finds no room for what it asks: the start is    *)
(*                  refused and counted, and the region and the connection *)
(*                  are as they were (standard/MESSAGES.md m1.7)           *)
(*   Exhaust(s)     a receive placement finds no room, which only a broken *)
(*                  invariant of the receiver reaches: the region's sticky *)
(*                  exhausted state, reason MESSAGE_MEMORY_EXHAUSTED       *)
(*   Kick(s)        the owner disconnects the exhausted connection with    *)
(*                  that reason (the server kicks the slot; a client       *)
(*                  disconnects itself)                                    *)
(*   Leave(s)       an ordinary disconnect, not from exhaustion            *)
(*   Reuse(s)       the slot is freed and its region reset                 *)
(*                                                                         *)
(* Reset = FALSE makes Reuse skip the reset; CarryOn = TRUE lets an        *)
(* exhausted connection keep taking room (Use, UseSpan); SendExhausts =    *)
(* TRUE makes a refused send exhaust the region, the send-side kick of     *)
(* v1.0.  Each must fail; those are the reversed witnesses.  The ghost     *)
(* cause records which side exhausted a region.                            *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Slots,        \* the server's client slots (one for a client)
          Clients,      \* the clients that may connect
          Capacity,     \* the region's room, in abstract records
          Reset,        \* Reuse runs rn_region_carve
          CarryOn,      \* an exhausted connection keeps working (broken)
          SendExhausts  \* a send with no room exhausts the region (broken)

ASSUME Slots # {} /\ Clients # {} /\ Capacity \in Nat \ {0}
ASSUME Reset \in BOOLEAN /\ CarryOn \in BOOLEAN /\ SendExhausts \in BOOLEAN

NoClient == "none"
Exhausted == "MESSAGE_MEMORY_EXHAUSTED"
Left == "LEFT"
Reasons == {"none", Exhausted, Left}
States == {"Free", "Connected", "Disconnected"}
Causes == {"none", "send", "receive"}

VARIABLES state,        \* state[s]: the slot's connection state
          owner,        \* owner[s]: the client on the slot, or NoClient
          used,         \* used[s]: region room in use
          exhausted,    \* exhausted[s]: the region's sticky exhausted state
          reason,       \* reason[s]: why the slot's connection ended
          records,      \* records[s]: clients whose records the region still holds
          cause         \* ghost: cause[s], the side that exhausted the region

vars == <<state, owner, used, exhausted, reason, records, cause>>

Slot(s) == <<state[s], owner[s], used[s], exhausted[s], reason[s], records[s], cause[s]>>

OnSlot == { owner[s] : s \in { t \in Slots : state[t] # "Free" } }

TypeOK ==
    /\ state \in [Slots -> States]
    /\ owner \in [Slots -> Clients \cup {NoClient}]
    /\ used \in [Slots -> 0..Capacity]
    /\ exhausted \in [Slots -> BOOLEAN]
    /\ reason \in [Slots -> Reasons]
    /\ records \in [Slots -> SUBSET Clients]
    /\ cause \in [Slots -> Causes]

Init ==
    /\ state = [s \in Slots |-> "Free"]
    /\ owner = [s \in Slots |-> NoClient]
    /\ used = [s \in Slots |-> 0]
    /\ exhausted = [s \in Slots |-> FALSE]
    /\ reason = [s \in Slots |-> "none"]
    /\ records = [s \in Slots |-> {}]
    /\ cause = [s \in Slots |-> "none"]

\* the transport admits a client to a free slot; the region is as Reuse left it
Connect(s, c) ==
    /\ state[s] = "Free"
    /\ c \notin OnSlot
    /\ state' = [state EXCEPT ![s] = "Connected"]
    /\ owner' = [owner EXCEPT ![s] = c]
    /\ UNCHANGED <<used, exhausted, reason, records, cause>>

\* rn_region_send_alloc (or a receive placement) finding room
Use(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s] \/ CarryOn
    /\ used[s] < Capacity
    /\ used' = [used EXCEPT ![s] = @ + 1]
    /\ records' = [records EXCEPT ![s] = @ \cup {owner[s]}]
    /\ UNCHANGED <<state, owner, exhausted, reason, cause>>

\* rn_region_send_alloc_span finding room: one large send takes n records
\* (a descriptor and a span), under the same guard as Use
UseSpan(s, n) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s] \/ CarryOn
    /\ used[s] + n <= Capacity
    /\ used' = [used EXCEPT ![s] = @ + n]
    /\ records' = [records EXCEPT ![s] = @ \cup {owner[s]}]
    /\ UNCHANGED <<state, owner, exhausted, reason, cause>>

\* rn_region_send_free (an ack or an abort) or a delivery released by the
\* application. Not guarded by ~exhausted: an ack that arrives between
\* Exhaust and Kick frees room (rn_endpoint_on_acked does not read the
\* exhausted state), and that room is never taken again before Reuse,
\* because Use and UseSpan refuse while exhausted.
Release(s) ==
    /\ state[s] = "Connected"
    /\ used[s] > 0
    /\ used' = [used EXCEPT ![s] = @ - 1]
    /\ UNCHANGED <<state, owner, exhausted, reason, records, cause>>

\* rn_region_send_refuse: a send (one record, or a span of n) that finds no
\* room is refused and counted (MESSAGE_SEND_QUEUE_FULL or
\* MESSAGE_SEND_STORE_FULL, and the connection's send_refused); nothing of the
\* region or the connection changes, so a later start that fits succeeds
\* (standard/MESSAGES.md m1.7). A start on a region the receive side already
\* exhausted is refused the same way. SendExhausts (broken) is the v1.0
\* refusal, which exhausted the region and so kicked the connection.
Refuse(s) ==
    /\ state[s] = "Connected"
    /\ \E n \in 1..Capacity : used[s] + n > Capacity
    /\ IF SendExhausts /\ ~exhausted[s]
         THEN /\ exhausted' = [exhausted EXCEPT ![s] = TRUE]
              /\ reason' = [reason EXCEPT ![s] = Exhausted]
              /\ cause' = [cause EXCEPT ![s] = "send"]
         ELSE UNCHANGED <<exhausted, reason, cause>>
    /\ UNCHANGED <<state, owner, used, records>>

\* rn_region_exhaust from the receive side: a receive placement that cannot
\* get the room it asks for (one record, or a span of n) sets the sticky
\* state. Only a broken invariant of the receiver reaches it: the receive
\* window and ring bound what a remote endpoint can make it hold
\* (standard/MESSAGES.md m1.9, rule 1).
Exhaust(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s]
    /\ \E n \in 1..Capacity : used[s] + n > Capacity
    /\ exhausted' = [exhausted EXCEPT ![s] = TRUE]
    /\ reason' = [reason EXCEPT ![s] = Exhausted]
    /\ cause' = [cause EXCEPT ![s] = "receive"]
    /\ UNCHANGED <<state, owner, used, records>>

\* rn_connection_memory_decide says kick: the server disconnects the slot
\* (rocketnet_server_transport_disconnect_client); a client disconnects itself
Kick(s) ==
    /\ state[s] = "Connected"
    /\ exhausted[s]
    /\ state' = [state EXCEPT ![s] = "Disconnected"]
    /\ UNCHANGED <<owner, used, exhausted, reason, records, cause>>

\* an ordinary disconnect: the client leaves, times out or is disconnected
Leave(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s]
    /\ state' = [state EXCEPT ![s] = "Disconnected"]
    /\ reason' = [reason EXCEPT ![s] = Left]
    /\ UNCHANGED <<owner, used, exhausted, records, cause>>

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
              /\ cause' = [cause EXCEPT ![s] = "none"]
         ELSE UNCHANGED <<used, exhausted, reason, records, cause>>

Next ==
    \E s \in Slots :
        \/ \E c \in Clients : Connect(s, c)
        \/ Use(s)
        \/ \E n \in 2..Capacity : UseSpan(s, n)
        \/ Release(s)
        \/ Refuse(s)
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

\* (N) no send ends its connection: a slot never ends with a region the send
\* side exhausted, because a send with no room is a refusal, never an
\* exhaustion (standard/MESSAGES.md m1.7). Reversed by SendExhausts.
SendNeverEnds ==
    \A s \in Slots : state[s] = "Disconnected" => cause[s] # "send"

\* a refused send leaves its slot exactly as it was: state, owner, room,
\* exhaustion, reason and records (standard/MESSAGES.md m1.7, rule 2)
RefusalLeavesStateUnchanged ==
    [][\A s \in Slots : Refuse(s) => Slot(s)' = Slot(s)]_vars

\* the receive side's exhaustion (the only one there is): from exhausted, the
\* slot takes no further room: the steps that change it
\* are Kick to Disconnected with the reason kept, and a Release that gives
\* one record back and changes nothing else
ExhaustionOnlyLeadsToDisconnect ==
    [][\A s \in Slots :
          (exhausted[s] /\ state[s] = "Connected") =>
              \/ Slot(s)' = Slot(s)
              \/ /\ state'[s] = "Disconnected"
                 /\ reason'[s] = Exhausted
                 /\ used'[s] = used[s]
                 /\ records'[s] = records[s]
              \/ /\ used'[s] = used[s] - 1
                 /\ state'[s] = state[s]
                 /\ owner'[s] = owner[s]
                 /\ exhausted'[s] = exhausted[s]
                 /\ reason'[s] = reason[s]
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
