--------------------------- MODULE RNRioLaneLayout ---------------------------
(***************************************************************************)
(* The RIO server transport's lanes at their layout                       *)
(* (sdk/rocketnet_rio_layout.h, sdk/rocketnet_server_rio_windows.cpp).     *)
(* L lanes, each with F send slots (its phases times its phase size).      *)
(* rocketnet_server_rio_start_packet stages a frame for destination d in   *)
(* lane LaneOf(d) = Fold(Hash(d)) % L, takes a free slot i of that lane    *)
(* and returns the packet id lane * F + i.  finish_packet and              *)
(* abort_packet decode the id back to (id \div F, id % F) after refusing   *)
(* any id at or past L * F, and act on that lane's slot.  The id is the    *)
(* only thing that carries the lane from start to finish.                  *)
(*                                                                         *)
(* The environment also hands finish ids the transport never issued: ids  *)
(* just past the last lane and beyond.  Those must be refused, never reach *)
(* a lane (the unit test pins the largest id on the code).  In the code a  *)
(* lane finds the slot from the frame pointer and refuses one that is not  *)
(* the slot the id names; the model frees the slot at finish, where the    *)
(* code holds it until its phase retires (RNRioSendSlotLayout.tla).        *)
(*                                                                         *)
(* Mode selects the code or a reversed witness:                            *)
(*   "Code"       as written.                                              *)
(*   "NoBound"    decode without the bound check: an id past the last      *)
(*                lane reads lane L, past the lanes array.                 *)
(*   "ModLane"    decode the lane as id % L rather than id \div F.         *)
(*   "LeastFull"  stage in the lane with the most free slots, not the      *)
(*                destination's: one client's frames leave on two lanes    *)
(*                and can overtake each other.                             *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   LaneInRange    every staged frame's lane and every decoded lane is    *)
(*                  0 .. L - 1.                                            *)
(*   FinishOwn      a finish or abort reaches exactly the lane and slot    *)
(*                  its start reserved, and a foreign id reaches none.     *)
(*   OneLanePerDest every frame staged and not yet sent for one            *)
(*                  destination is in one lane, so lane FIFO order is the  *)
(*                  destination's order.                                   *)
(*   NoReissue      a slot is handed to one frame at a time.               *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS L,          \* lanes: options.lanes (num_xdp_queues)
          F,          \* rocketnet_server_rio_t.lane_slots: phases times phase size
          Hashes,     \* the destinations, each named by its hash (rocketnet_rio_lane_hash
                      \* of its family, port and address, as the model reads it)
          MaxStarts,  \* start_packet calls in all
          Mode

ASSUME L >= 1 /\ F >= 1 /\ MaxStarts >= 1
ASSUME \A h \in Hashes : h \in Nat
ASSUME Mode \in {"Code", "NoBound", "ModLane", "LeastFull"}

Dests == Hashes   \* a destination is its hash: the lane reads nothing else of it

Lanes == 0..L - 1
Slots == 0..F - 1
None == -1          \* a decode that names no lane or slot
NoDest == -1        \* an empty slot, and the empty issued map's range (a hash is never negative)

\* rocketnet_rio_lane_of: the high half folded into the low, then the modulo.
\* The model's hashes are below 2^16, where the fold is the identity; the
\* unit test pins the fold on full-width hashes.
LaneOf(h) == IF L <= 1 THEN 0 ELSE h % L

Encode(lane, i) == lane * F + i

\* rocketnet_rio_lane_packet_decode: <<ok, lane, index>>
Decode(id) ==
    CASE Mode = "NoBound" -> <<TRUE, id \div F, id % F>>
      [] Mode = "ModLane" -> IF id >= L * F THEN <<FALSE, None, None>>
                             ELSE <<TRUE, id % L, id % F>>
      [] OTHER            -> IF id >= L * F THEN <<FALSE, None, None>>
                             ELSE <<TRUE, id \div F, id % F>>

\* ids the server may hand finish that start never issued
ForeignIds == {L * F, L * F + F, 2 * L * F + 1}

FreeIn(lane, sl) == {i \in Slots : sl[lane][i] = NoDest}

VARIABLES slot,      \* slot[lane][i]: the destination staged there, or NoDest
          issued,    \* id -> destination for every reserved, unfinished id
          staged,    \* sequence of <<dest, lane>> finished and not yet sent
          starts,    \* ghost: start_packet calls so far
          strayed,   \* ghost: a decode left 0 .. L - 1
          misrouted  \* ghost: a finish or abort reached a slot it did not reserve

vars == <<slot, issued, staged, starts, strayed, misrouted>>

Init ==
    /\ slot = [l \in Lanes |-> [i \in Slots |-> NoDest]]
    /\ issued = [id \in {} |-> NoDest]
    /\ staged = <<>>
    /\ starts = 0
    /\ strayed = FALSE
    /\ misrouted = FALSE

\* the lane a frame to d is staged in
StageLane(d) ==
    IF Mode = "LeastFull"
    THEN CHOOSE l \in Lanes : \A m \in Lanes : Cardinality(FreeIn(l, slot)) >= Cardinality(FreeIn(m, slot))
    ELSE LaneOf(d)

\* rocketnet_server_rio_start_packet: refused when the lane is full
Start(d) ==
    /\ starts < MaxStarts
    /\ LET lane == StageLane(d)
       IN /\ FreeIn(lane, slot) # {}
          /\ \E i \in FreeIn(lane, slot) :
                LET id == Encode(lane, i)
                IN /\ slot' = [slot EXCEPT ![lane][i] = d]
                   /\ issued' = [x \in DOMAIN issued \cup {id} |-> IF x = id THEN d ELSE issued[x]]
    /\ starts' = starts + 1
    /\ UNCHANGED <<staged, strayed, misrouted>>

Without(f, id) == [x \in DOMAIN f \ {id} |-> f[x]]

\* rocketnet_server_rio_finish_packet (keep = TRUE: the frame is sent, in
\* lane order) or abort_packet (keep = FALSE: the slot carries nothing)
Finish(id, keep) ==
    /\ id \in DOMAIN issued
    /\ LET r == Decode(id)
           d == issued[id]
       IN /\ strayed' = (strayed \/ (r[1] /\ r[2] \notin Lanes))
          /\ IF r[1] /\ r[2] \in Lanes /\ slot[r[2]][r[3]] = d /\ Encode(r[2], r[3]) = id
               THEN /\ misrouted' = misrouted
                    /\ slot' = [slot EXCEPT ![r[2]][r[3]] = NoDest]
                    /\ staged' = IF keep THEN Append(staged, <<d, r[2]>>) ELSE staged
               ELSE /\ misrouted' = TRUE
                    /\ UNCHANGED <<slot, staged>>
          /\ issued' = Without(issued, id)
    /\ UNCHANGED starts

\* the server hands an id start never issued: refused, no lane touched
Foreign(id) ==
    /\ id \in ForeignIds
    /\ ~(id \in DOMAIN issued)
    /\ LET r == Decode(id)
       IN /\ strayed' = (strayed \/ (r[1] /\ r[2] \notin Lanes))
          /\ misrouted' = (misrouted \/ r[1])
    /\ UNCHANGED <<slot, issued, staged, starts>>

\* a lane's worker posts the oldest staged frame of that lane
Send(lane) ==
    /\ \E k \in 1..Len(staged) :
          /\ staged[k][2] = lane
          /\ \A j \in 1..k - 1 : staged[j][2] # lane
          /\ staged' = [j \in 1..Len(staged) - 1 |-> IF j < k THEN staged[j] ELSE staged[j + 1]]
    /\ UNCHANGED <<slot, issued, starts, strayed, misrouted>>

Next ==
    \/ \E d \in Dests : Start(d)
    \/ \E id \in DOMAIN issued, keep \in BOOLEAN : Finish(id, keep)
    \/ \E id \in ForeignIds : Foreign(id)
    \/ \E lane \in Lanes : Send(lane)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ slot \in [Lanes -> [Slots -> Dests \cup {NoDest}]]
    /\ starts \in 0..MaxStarts
    /\ strayed \in BOOLEAN
    /\ misrouted \in BOOLEAN

LaneInRange ==
    /\ ~strayed
    /\ \A k \in 1..Len(staged) : staged[k][2] \in Lanes

FinishOwn == ~misrouted

\* every issued id names its own slot, so no slot is handed twice
NoReissue ==
    \A id \in DOMAIN issued :
        /\ id \div F \in Lanes
        /\ slot[id \div F][id % F] = issued[id]

OneLanePerDest ==
    \A j, k \in 1..Len(staged) : staged[j][1] = staged[k][1] => staged[j][2] = staged[k][2]
=============================================================================
