---------------------- MODULE RNServerMaintenanceSweep ----------------------
(***************************************************************************)
(* The maintenance sweep of rocketnet_server_transport_update             *)
(* (sdk/rocketnet_server_transport.cpp) at the code's layout: N client    *)
(* slots, the cursor maintenance_next_client with its modulo-N            *)
(* arithmetic, and a staging that has room for a bounded number of        *)
(* frames each update.  Every slot is connected and its keep-alive is     *)
(* always due, the worst case for fairness.                               *)
(*                                                                         *)
(* A sweep visits slots in ring order from the cursor.  Before each       *)
(* visit the cursor names the slot.  A reserve that finds the staging     *)
(* full ends the sweep with the cursor on the refused slot, so the next   *)
(* update starts there.  A reserve the transport refuses for want of a    *)
(* link-layer destination (ROCKETNET_TRANSPORT_PACKET_UNREACHABLE, xdp    *)
(* with no MAC for the address) is per-address and does not clear on the *)
(* next update: the sweep moves past that slot and serves the rest.       *)
(*                                                                         *)
(* SkipUnreachable = FALSE is the reversed witness: an unreachable        *)
(* refusal is treated as a full staging and ends the sweep with the       *)
(* cursor on that slot.  Every later update starts there and stops at     *)
(* once, so no reachable slot is ever served again; BoundedWait fails.    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,                  \* client slots
    MaxRoom,            \* most frames the staging takes in one update
    Unreachable,        \* slots whose address has no link-layer destination
    SkipUnreachable     \* the rule: an unreachable refusal advances the cursor

Slots == 0 .. (N - 1)
Reachable == Slots \ Unreachable

ASSUME N >= 2 /\ MaxRoom >= 1 /\ Unreachable \subseteq Slots
       /\ SkipUnreachable \in BOOLEAN

VARIABLES
    cursor,     \* maintenance_next_client
    sweeping,   \* inside one update's sweep
    offset,     \* the loop's offset from the update's first slot
    first,      \* first_client, the cursor when the sweep began
    room,       \* frames the staging still takes this update
    served,     \* reachable slots sent a frame this update
    wait        \* updates each reachable slot has gone unserved

vars == << cursor, sweeping, offset, first, room, served, wait >>

TypeOK ==
    /\ cursor \in Slots
    /\ sweeping \in BOOLEAN
    /\ offset \in 0 .. N
    /\ first \in Slots
    /\ room \in 0 .. MaxRoom
    /\ served \subseteq Reachable
    /\ wait \in [Reachable -> 0 .. (N + 1)]

Init ==
    /\ cursor \in Slots
    /\ sweeping = FALSE
    /\ offset = 0
    /\ first = 0
    /\ room = 0
    /\ served = {}
    /\ wait = [s \in Reachable |-> 0]

\* rocketnet_server_transport_update: the sweep begins at the cursor, with
\* whatever room the staging has this update (at least one frame: an update
\* with none serves nobody, and fairness is not owed across it)
BeginSweep ==
    /\ ~sweeping
    /\ \E r \in 1 .. MaxRoom : room' = r
    /\ sweeping' = TRUE
    /\ offset' = 0
    /\ first' = cursor
    /\ served' = {}
    /\ UNCHANGED << cursor, wait >>

\* The sweep ends: every slot visited, or the loop left by break.  Each
\* reachable slot not served this update has waited one more update.
EndSweepVars ==
    /\ sweeping' = FALSE
    /\ wait' = [s \in Reachable |->
                   IF s \in served THEN 0
                   ELSE IF wait[s] < N + 1 THEN wait[s] + 1 ELSE wait[s]]

\* One visit: the cursor names slot i, then the reserve.
SweepStep ==
    /\ sweeping
    /\ offset < N
    /\ LET i == (first + offset) % N
           next == (i + 1) % N
       IN
       IF room = 0
       THEN \* the staging is full: break, the cursor stays on i
            /\ cursor' = i
            /\ EndSweepVars
            /\ UNCHANGED << offset, first, room, served >>
       ELSE IF i \in Unreachable
       THEN IF SkipUnreachable
            THEN \* counted SEND_NO_LINK_ADDRESS; the keep-alive stays due
                 /\ cursor' = next
                 /\ offset' = offset + 1
                 /\ UNCHANGED << sweeping, first, room, served, wait >>
            ELSE \* the defect: treated as a full staging
                 /\ cursor' = i
                 /\ EndSweepVars
                 /\ UNCHANGED << offset, first, room, served >>
       ELSE \* reserved, the keep-alive committed, the cursor moves past i
            /\ cursor' = next
            /\ offset' = offset + 1
            /\ room' = room - 1
            /\ served' = served \cup {i}
            /\ UNCHANGED << sweeping, first, wait >>

\* The loop runs out of slots.
SweepDone ==
    /\ sweeping
    /\ offset = N
    /\ EndSweepVars
    /\ UNCHANGED << cursor, offset, first, room, served >>

Next == BeginSweep \/ SweepStep \/ SweepDone

Spec == Init /\ [][Next]_vars

\* Every update has room for at least one frame, so every reachable slot is
\* served within N updates however the unreachable slots lie.
BoundedWait == \A s \in Reachable : wait[s] <= N

=============================================================================
