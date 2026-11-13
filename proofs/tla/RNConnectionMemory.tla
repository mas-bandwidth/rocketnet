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
(*   Connect(s, c)  a client takes a Free slot, whose region is carved     *)
(*                  first (rn_endpoint_reset, rn_region_reset)             *)
(*   Use(s)         a send, receive or hold takes room in the region       *)
(*   UseSpan(s, n)  a large send takes n records of room at once (a span)  *)
(*   Release(s)     an acked send, an abort or a released delivery gives   *)
(*                  room back; also while exhausted, since it takes none   *)
(*   Refuse(s, n)   a send of n records finds no room, or a region the     *)
(*                  receive side exhausted: the start is refused and       *)
(*                  counted, and the region and the connection are as they *)
(*                  were (standard/MESSAGES.md m1.7)                       *)
(*   Exhaust(s)     a receive placement finds no room, which only a broken *)
(*                  invariant of the receiver reaches: the region's sticky *)
(*                  exhausted state, reason MESSAGE_MEMORY_EXHAUSTED       *)
(*   Kick(s)        the owner disconnects the exhausted connection with    *)
(*                  that reason (the server kicks the slot; a client       *)
(*                  disconnects itself)                                    *)
(*   Leave(s)       an ordinary disconnect, not from exhaustion            *)
(*   Reuse(s)       the slot is freed in O(1), its region as the ended     *)
(*                  connection left it (rn_message_connection_release);    *)
(*                  the carve is the next Connect's (the lazy reset)       *)
(*                                                                         *)
(* A start runs the code's steps in the code's order: the fit              *)
(* (rn_region_send_fit_store or rn_region_send_fit_span, a const region)   *)
(* decides before any take, then the take (Use, UseSpan) or the refusal    *)
(* (Refuse: rn_region_send_refuse).  A refusal for no room moves the       *)
(* region's room_refusals, and rn_endpoint_start counts the connection's   *)
(* send_refused only when it sees room_refusals move across the start; a   *)
(* refusal of an exhausted region moves neither (the endpoint records      *)
(* send_error instead).  refused[s] is send_refused for the slot's         *)
(* connection, modulo CountMod as the code's counters wrap, zeroed with    *)
(* the connection (rn_endpoint_init, rn_endpoint_reset, so at Connect);    *)
(* CountMod = 1 drops it.  room_refusals itself is a counter the endpoint  *)
(* only compares across one start, so the model needs no variable for it.  *)
(*                                                                         *)
(* Reset = FALSE makes Connect skip the carve; CountExhausted = TRUE       *)
(* counts a refusal of an exhausted region in send_refused; CarryOn = TRUE *)
(* lets an                                                                 *)
(* exhausted connection keep taking room (Use, UseSpan); SendExhausts =    *)
(* TRUE makes a refused send exhaust the region, the send-side kick of     *)
(* v1.0; TakeBeforeFit = TRUE makes a span start take its descriptor (one  *)
(* record) before the span's room is known and return NULL without giving  *)
(* it back.  Each must fail; those are the reversed witnesses.             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Slots,        \* the server's client slots (one for a client)
          Clients,      \* the clients that may connect
          Capacity,     \* the region's room, in abstract records
          Reset,        \* Connect runs rn_region_carve on the slot's region
          CarryOn,      \* an exhausted connection keeps working (broken)
          SendExhausts, \* a send with no room exhausts the region (broken)
          TakeBeforeFit,\* a span start takes its descriptor before its room is known (broken)
          CountExhausted,\* a refusal of an exhausted region counts in send_refused (broken)
          CountMod      \* the refusal count runs 0..CountMod-1

ASSUME Slots # {} /\ Clients # {} /\ Capacity \in Nat \ {0}
ASSUME Reset \in BOOLEAN /\ CarryOn \in BOOLEAN /\ SendExhausts \in BOOLEAN
ASSUME TakeBeforeFit \in BOOLEAN /\ CountExhausted \in BOOLEAN /\ CountMod \in Nat \ {0}

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
          records,      \* records[s]: clients whose records the region still holds
          refused       \* refused[s]: the connection's refused starts, modulo CountMod

vars == <<state, owner, used, exhausted, reason, records, refused>>

\* the slot and its region; the refusal count is apart (a refused start moves it)
Slot(s) == <<state[s], owner[s], used[s], exhausted[s], reason[s], records[s]>>

OnSlot == { owner[s] : s \in { t \in Slots : state[t] # "Free" } }

TypeOK ==
    /\ state \in [Slots -> States]
    /\ owner \in [Slots -> Clients \cup {NoClient}]
    /\ used \in [Slots -> 0..Capacity]
    /\ exhausted \in [Slots -> BOOLEAN]
    /\ reason \in [Slots -> Reasons]
    /\ records \in [Slots -> SUBSET Clients]
    /\ refused \in [Slots -> 0..(CountMod - 1)]

Init ==
    /\ state = [s \in Slots |-> "Free"]
    /\ owner = [s \in Slots |-> NoClient]
    /\ used = [s \in Slots |-> 0]
    /\ exhausted = [s \in Slots |-> FALSE]
    /\ reason = [s \in Slots |-> "none"]
    /\ records = [s \in Slots |-> {}]
    /\ refused = [s \in Slots |-> 0]

\* the transport admits a client to a free slot: the binding carves the
\* slot's region before anything else touches it (rn_endpoint_reset, which
\* runs rn_region_reset and zeroes the connection's send_refused; on the
\* server the Observe that binds the slot, RNSlots), then the connection is up
Connect(s, c) ==
    /\ state[s] = "Free"
    /\ c \notin OnSlot
    /\ state' = [state EXCEPT ![s] = "Connected"]
    /\ owner' = [owner EXCEPT ![s] = c]
    /\ IF Reset
         THEN /\ used' = [used EXCEPT ![s] = 0]
              /\ exhausted' = [exhausted EXCEPT ![s] = FALSE]
              /\ reason' = [reason EXCEPT ![s] = "none"]
              /\ records' = [records EXCEPT ![s] = {}]
              /\ refused' = [refused EXCEPT ![s] = 0]
         ELSE UNCHANGED <<used, exhausted, reason, records, refused>>

\* rn_region_send_fit_store's and rn_region_send_fit_span's decision for a
\* send of n records, read off a const region before any take: not
\* exhausted, and room for all n
FitOK(s, n) == ~exhausted[s] /\ used[s] + n <= Capacity

\* rn_region_send_alloc (or a receive placement) finding room
Use(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s] \/ CarryOn
    /\ used[s] < Capacity
    /\ used' = [used EXCEPT ![s] = @ + 1]
    /\ records' = [records EXCEPT ![s] = @ \cup {owner[s]}]
    /\ UNCHANGED <<state, owner, exhausted, reason, refused>>

\* rn_region_send_alloc_span finding room: one large send takes n records
\* (a descriptor and a span), under the same guard as Use
UseSpan(s, n) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s] \/ CarryOn
    /\ used[s] + n <= Capacity
    /\ used' = [used EXCEPT ![s] = @ + n]
    /\ records' = [records EXCEPT ![s] = @ \cup {owner[s]}]
    /\ UNCHANGED <<state, owner, exhausted, reason, refused>>

\* rn_region_send_free (an ack or an abort) or a delivery released by the
\* application. Not guarded by ~exhausted: an ack that arrives between
\* Exhaust and Kick frees room (rn_endpoint_on_acked does not read the
\* exhausted state), and that room is never taken again before the next
\* Connect carves the region, because Use and UseSpan refuse while
\* exhausted.
Release(s) ==
    /\ state[s] = "Connected"
    /\ used[s] > 0
    /\ used' = [used EXCEPT ![s] = @ - 1]
    /\ UNCHANGED <<state, owner, exhausted, reason, records, refused>>

\* rn_region_send_refuse: a send of n records (one, or a span) that the fit
\* refuses (no room, or a region the receive side already exhausted) is
\* refused and counted (MESSAGE_SEND_QUEUE_FULL, MESSAGE_SEND_STORE_FULL or
\* MESSAGE_MEMORY_EXHAUSTED, and the connection's send_refused); in the code
\* the fit only reads, so nothing of the region or the connection changes
\* and a later start that fits succeeds (standard/MESSAGES.md m1.7).
\* SendExhausts (broken) is the v1.0 refusal, which exhausted the region and
\* so kicked the connection. TakeBeforeFit (broken): a span start takes its
\* descriptor first and, finding no room for the span, returns NULL with the
\* descriptor still taken.
Refuse(s, n) ==
    /\ state[s] = "Connected"
    /\ ~FitOK(s, n)
    /\ IF TakeBeforeFit /\ ~exhausted[s] /\ n >= 2 /\ used[s] < Capacity
         THEN /\ used' = [used EXCEPT ![s] = @ + 1]
              /\ records' = [records EXCEPT ![s] = @ \cup {owner[s]}]
         ELSE UNCHANGED <<used, records>>
    /\ IF SendExhausts /\ ~exhausted[s]
         THEN /\ exhausted' = [exhausted EXCEPT ![s] = TRUE]
              /\ reason' = [reason EXCEPT ![s] = Exhausted]
         ELSE UNCHANGED <<exhausted, reason>>
    \* send_refused moves when room_refusals moved across the start: a room
    \* refusal; an exhausted region's refusal moves neither
    /\ refused' = IF ~exhausted[s] \/ CountExhausted
                  THEN [refused EXCEPT ![s] = (@ + 1) % CountMod]
                  ELSE refused
    /\ UNCHANGED <<state, owner>>

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
    /\ UNCHANGED <<state, owner, used, records, refused>>

\* rn_connection_memory_decide says kick: the server disconnects the slot
\* (rocketnet_server_transport_disconnect_client); a client disconnects itself
Kick(s) ==
    /\ state[s] = "Connected"
    /\ exhausted[s]
    /\ state' = [state EXCEPT ![s] = "Disconnected"]
    /\ UNCHANGED <<owner, used, exhausted, reason, records, refused>>

\* an ordinary disconnect: the client leaves, times out or is disconnected
Leave(s) ==
    /\ state[s] = "Connected"
    /\ ~exhausted[s]
    /\ state' = [state EXCEPT ![s] = "Disconnected"]
    /\ reason' = [reason EXCEPT ![s] = Left]
    /\ UNCHANGED <<owner, used, exhausted, records, refused>>

\* rn_message_connection_release: the slot is freed in O(1), its binding
\* cleared and its region left as the ended connection left it (used room,
\* exhaustion, reason, records); the next Connect carves it. A Free slot's
\* region is touched by no action until then.
Reuse(s) ==
    /\ state[s] = "Disconnected"
    /\ state' = [state EXCEPT ![s] = "Free"]
    /\ owner' = [owner EXCEPT ![s] = NoClient]
    /\ UNCHANGED <<used, exhausted, reason, records, refused>>

Next ==
    \E s \in Slots :
        \/ \E c \in Clients : Connect(s, c)
        \/ Use(s)
        \/ \E n \in 2..Capacity : UseSpan(s, n)
        \/ Release(s)
        \/ \E n \in 1..Capacity : Refuse(s, n)
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

\* A Free slot's region may still hold the ended connection's room,
\* exhaustion, reason and records (the lazy reset); the Connect that takes
\* the slot carves it, so a connected slot holds records of its own client
\* only, and its reason is its own: none, or the receive side's exhaustion,
\* never the previous connection's end
ReusedSlotStartsClean ==
    \A s \in Slots :
        state[s] = "Connected" =>
            /\ records[s] \subseteq {owner[s]}
            /\ reason[s] \in {"none", Exhausted}

\* A send start of n records: the take with room (Use for one record,
\* UseSpan for a span), or the start the region refuses.
StartStep(s, n) ==
    \/ n = 1 /\ Use(s)
    \/ n >= 2 /\ UseSpan(s, n)
    \/ Refuse(s, n)

\* (N) No send ends its connection: no start step, refused or not, changes
\* its slot's connection state, exhausted flag or reason, read on the state
\* the step leaves (standard/MESSAGES.md m1.7). Only the receive side
\* exhausts a region (Exhaust), and an exhausted region is what the owner
\* kicks (Kick), so a slot's end never follows from a send. Reversed by
\* SendExhausts.
SendNeverEnds ==
    [][\A s \in Slots, n \in 1..Capacity :
          StartStep(s, n) => /\ state'[s] = state[s]
                             /\ exhausted'[s] = exhausted[s]
                             /\ reason'[s] = reason[s]]_vars

\* A start the fit refuses, by its decision on the state before the step
\* (exhausted, or no room for n), leaves its slot exactly as it was: state,
\* owner, room, exhaustion, reason and records (standard/MESSAGES.md m1.7,
\* rule 2), whatever the start did on the way; the slot's refusal count
\* alone moves, by one for a refusal for no room and not at all for a
\* refusal of an exhausted region (design (L)1; rn_endpoint_start counts
\* send_refused only when room_refusals moved). Reversed by TakeBeforeFit
\* (room), SendExhausts (exhaustion), CarryOn (room taken while exhausted)
\* and CountExhausted (an exhausted region's refusal counted).
RefusalLeavesStateUnchanged ==
    [][\A s \in Slots, n \in 1..Capacity :
          StartStep(s, n) /\ ~FitOK(s, n) =>
              /\ Slot(s)' = Slot(s)
              /\ refused'[s] = IF exhausted[s] THEN refused[s]
                               ELSE (refused[s] + 1) % CountMod]_vars

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
    [][\E s \in Slots : \A t \in Slots \ {s} : Slot(t)' = Slot(t) /\ refused'[t] = refused[t]]_vars

\* Liveness: an exhausted slot is eventually Free (kicked, then freed)
ExhaustedEventuallyFree ==
    \A s \in Slots : exhausted[s] ~> state[s] = "Free"

=============================================================================
