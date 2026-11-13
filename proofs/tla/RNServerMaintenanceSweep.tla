---------------------- MODULE RNServerMaintenanceSweep ----------------------
(***************************************************************************)
(* The maintenance sweep of rocketnet_server_transport_update             *)
(* (sdk/rocketnet_server_transport.cpp) at the code's layout: N client    *)
(* slots, the cursor maintenance_next_client with its modulo-N            *)
(* arithmetic, and a staging of QueueCount send queues, each with room    *)
(* for a bounded number of frames each update; slot s stages on queue     *)
(* QueueOf(s).  Every slot is connected and its keep-alive is always due, *)
(* the worst case for fairness.                                           *)
(*                                                                         *)
(* A sweep visits slots in ring order from the cursor.  Before each       *)
(* visit the cursor names the slot.  A slot whose own queue is full while *)
(* another queue has room is held (Hold): its keep-alive stays due, the   *)
(* sweep goes on, and the first slot held after the sweep's first frame   *)
(* is where the next update starts (EndSweep).  With every queue full the *)
(* sweep ends, the cursor on that slot (or the held one).  With one queue *)
(* that is the rule before queues: a full staging ends the sweep with the *)
(* cursor on the refused slot.  A reserve the transport refuses for want  *)
(* of a link-layer destination (ROCKETNET_TRANSPORT_PACKET_UNREACHABLE,   *)
(* xdp with no MAC for the address) is per-address and does not clear on *)
(* the next update: the sweep moves past that slot and serves the rest.   *)
(* So does a start the transport refuses with room left for any other     *)
(* reason (an address its socket cannot carry, rocketnet_driver.h,        *)
(* counted SEND_FAILED): the code tests the room after the refusal and    *)
(* breaks only when it is gone.                                           *)
(*                                                                         *)
(* The queues in MayJam may have no room in any update (a queue held full *)
(* by its workers or in hard send error); every other queue has room for  *)
(* at least one frame each update.  BoundedWait is owed to the reachable  *)
(* slots of those queues.                                                 *)
(*                                                                         *)
(* SkipUnreachable = FALSE is the reversed witness: an unreachable        *)
(* refusal is treated as a full staging and ends the sweep with the       *)
(* cursor on that slot.  Every later update starts there and stops at     *)
(* once, so no reachable slot is ever served again; BoundedWait fails.    *)
(* SkipRefused = FALSE is the same witness for a refusal with room left.  *)
(* QueueRule names the rule for a full queue: "perqueue" is the code;     *)
(* "minroom" is the code before queues were judged one by one, which      *)
(* ended the sweep when the fewest free frames of any queue was 0, so one *)
(* full queue starved the clients of every other; "holdfirst" holds a     *)
(* slot also before the sweep's first frame, which pins the cursor on a   *)
(* queue full for many updates; "nohold" moves the cursor past a held     *)
(* slot as past a served one, so a slot whose queue the slots before it   *)
(* fill is passed over in every update.  Each fails BoundedWait.          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,                  \* client slots
    MaxRoom,            \* most frames a queue takes in one update
    QueueCount,         \* send queues (send_queues)
    MayJam,             \* queues that may have no room in an update
    Unreachable,        \* slots whose address has no link-layer destination
    Refused,            \* slots whose start the transport refuses with room left
    SkipUnreachable,    \* the rule: an unreachable refusal advances the cursor
    SkipRefused,        \* the rule: a refusal with room left advances the cursor
    QueueRule           \* the rule for a slot whose queue is full

Slots == 0 .. (N - 1)
Queues == 0 .. (QueueCount - 1)
Reachable == Slots \ (Unreachable \cup Refused)

\* send_queue_of: the queue a slot's frames are staged on.
QueueOf(s) == s % QueueCount

\* The reachable slots whose queue always has room for a frame.
Owed == {s \in Reachable : QueueOf(s) \notin MayJam}

ASSUME N >= 2 /\ MaxRoom >= 1 /\ Unreachable \subseteq Slots
       /\ Refused \subseteq Slots /\ Unreachable \cap Refused = {}
       /\ SkipUnreachable \in BOOLEAN /\ SkipRefused \in BOOLEAN
       /\ QueueCount >= 1 /\ MayJam \subseteq Queues /\ MayJam # Queues
       /\ QueueRule \in {"perqueue", "minroom", "holdfirst", "nohold"}

VARIABLES
    cursor,     \* maintenance_next_client
    sweeping,   \* inside one update's sweep
    offset,     \* the loop's offset from the update's first slot
    first,      \* first_client, the cursor when the sweep began
    room,       \* frames each queue still takes this update
    held,       \* the first slot held after the sweep's first frame, or N for none
    served,     \* reachable slots sent a frame this update
    wait        \* updates each reachable slot has gone unserved

vars == << cursor, sweeping, offset, first, room, held, served, wait >>

RECURSIVE SumOf(_, _)
SumOf(f, Q) == IF Q = {} THEN 0 ELSE LET q == CHOOSE x \in Q : TRUE IN f[q] + SumOf(f, Q \ {q})

\* io_send_room_total: the room summed over the queues.
RoomTotal == SumOf(room, Queues)

TypeOK ==
    /\ cursor \in Slots
    /\ sweeping \in BOOLEAN
    /\ offset \in 0 .. N
    /\ first \in Slots
    /\ room \in [Queues -> 0 .. MaxRoom]
    /\ held \in 0 .. N
    /\ served \subseteq Reachable
    /\ wait \in [Reachable -> 0 .. (N + 1)]

Init ==
    /\ cursor \in Slots
    /\ sweeping = FALSE
    /\ offset = 0
    /\ first = 0
    /\ room = [q \in Queues |-> 0]
    /\ held = N
    /\ served = {}
    /\ wait = [s \in Reachable |-> 0]

\* rocketnet_server_transport_update: the sweep begins at the cursor, with
\* whatever room each queue has this update (at least one frame on a queue
\* not in MayJam: an update with none serves nobody, and fairness is not owed
\* across it)
BeginSweep ==
    /\ ~sweeping
    /\ \E r \in [Queues -> 0 .. MaxRoom] :
          /\ \A q \in Queues \ MayJam : r[q] >= 1
          /\ room' = r
    /\ sweeping' = TRUE
    /\ offset' = 0
    /\ first' = cursor
    /\ held' = N
    /\ served' = {}
    /\ UNCHANGED << cursor, wait >>

\* The sweep ends: every slot visited, or the loop left by break.  Each
\* reachable slot not served this update has waited one more update.
EndSweepVars ==
    /\ sweeping' = FALSE
    /\ wait' = [s \in Reachable |->
                   IF s \in served THEN 0
                   ELSE IF wait[s] < N + 1 THEN wait[s] + 1 ELSE wait[s]]

\* EndSweep: the cursor the next update starts at. A slot held for its queue
\* comes before any later slot the sweep stopped at; else the slot it broke
\* at, or past the last slot visited.
EndSweep(at) ==
    /\ cursor' = IF held < N THEN held ELSE at
    /\ EndSweepVars

\* The sweep breaks at slot i.
Break(i) ==
    /\ EndSweep(i)
    /\ UNCHANGED << offset, first, room, held, served >>

\* Whether slot i's queue is full as the rule reads it.
QueueFull(i) ==
    IF QueueRule = "minroom" THEN \E q \in Queues : room[q] = 0 ELSE room[QueueOf(i)] = 0

\* One visit: the cursor names slot i, then the reserve.
SweepStep ==
    /\ sweeping
    /\ offset < N
    /\ LET i == (first + offset) % N
           next == (i + 1) % N
       IN
       IF QueueFull(i)
       THEN IF QueueRule = "minroom" \/ RoomTotal = 0
            THEN \* every queue full (or the fewest-room rule): break
                 Break(i)
            ELSE \* Hold: the slot's queue is full, another has room; the
                 \* keep-alive stays due and the sweep goes on
                 /\ held' = IF held = N /\ QueueRule # "nohold" /\ (served # {} \/ QueueRule = "holdfirst")
                               THEN i ELSE held
                 /\ cursor' = next
                 /\ offset' = offset + 1
                 /\ UNCHANGED << sweeping, first, room, served, wait >>
       ELSE IF i \in Unreachable
       THEN IF SkipUnreachable
            THEN \* counted SEND_NO_LINK_ADDRESS; the keep-alive stays due
                 /\ cursor' = next
                 /\ offset' = offset + 1
                 /\ UNCHANGED << sweeping, first, room, held, served, wait >>
            ELSE \* the defect: treated as a full staging
                 Break(i)
       ELSE IF i \in Refused
       THEN IF SkipRefused
            THEN \* counted SEND_FAILED; the room is left, so the sweep goes on
                 /\ cursor' = next
                 /\ offset' = offset + 1
                 /\ UNCHANGED << sweeping, first, room, held, served, wait >>
            ELSE \* the defect: treated as a full staging
                 Break(i)
       ELSE \* reserved, the keep-alive committed, the cursor moves past i
            /\ cursor' = next
            /\ offset' = offset + 1
            /\ room' = [room EXCEPT ![QueueOf(i)] = @ - 1]
            /\ served' = served \cup {i}
            /\ UNCHANGED << sweeping, first, held, wait >>

\* The loop runs out of slots.
SweepDone ==
    /\ sweeping
    /\ offset = N
    /\ EndSweep(cursor)
    /\ UNCHANGED << offset, first, room, held, served >>

Next == BeginSweep \/ SweepStep \/ SweepDone

Spec == Init /\ [][Next]_vars

\* Every update has room for at least one frame on each queue not in MayJam,
\* so every reachable slot of those queues is served within N updates
\* however the unreachable slots lie and whatever the other queues do.
BoundedWait == \A s \in Owed : wait[s] <= N

=============================================================================
