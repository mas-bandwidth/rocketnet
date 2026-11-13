--------------------------- MODULE RNRioLaneLayout ---------------------------
(***************************************************************************)
(* The RIO server transport's lanes at their layout                       *)
(* (sdk/rocketnet_rio_layout.h, sdk/rocketnet_server_rio_windows.cpp).     *)
(* L lanes, each with F send slots (its phases times its phase size).      *)
(* rocketnet_server_rio_start_packet stages a frame for destination d in   *)
(* lane LaneOf(d) = Fold(Hash(d)) % L, takes that lane's next slot i (the  *)
(* lane's reservation counter, next[fill].fetch_add in                     *)
(* rocketnet_rio_lane_start_packet, refused once the slots are used) and   *)
(* returns the packet id lane * F + i.  finish_packet and abort_packet     *)
(* decode the id back to (id \div F, id % F) after refusing any id at or   *)
(* past L * F, and act on that lane's slot: finish writes its length,      *)
(* abort leaves it zero, and either way the slot stays reserved.  The id   *)
(* is the only thing that carries the lane from start to finish.  A lane's *)
(* worker posts the lane's reserved slots in slot order, which is their    *)
(* reservation order (rocketnet_rio_post_phase walks slot 0 up to the      *)
(* count), skips the zero ones, and the slots are free again.              *)
(*                                                                         *)
(* Abstraction.  The code reserves within the open fill phase (index =     *)
(* phase * Q + slot); here one counter runs over the lane's F ids, which   *)
(* is the code's order within one phase, and Send posts and frees them     *)
(* all at once.  How the three phases take turns, and the slot's hold      *)
(* until its completion, are RNRioSendSlotLayout.tla's.  Send waits until  *)
(* no slot of the lane is reserved and unfinished: the code's main thread  *)
(* hands a phase with the fill threads joined, each start finished or      *)
(* aborted.                                                                *)
(*                                                                         *)
(* The environment also hands finish ids the transport never issued: ids  *)
(* just past the last lane and beyond.  Those must be refused, never reach *)
(* a lane (the unit test pins the largest id on the code).  In the code a  *)
(* lane finds the slot from the frame pointer and refuses one that is not  *)
(* the slot the id names.  An id below L * F that is not outstanding (one  *)
(* already finished, or never issued) decodes to its own slot, and with    *)
(* that slot's frame pointer the code acts on it: no generation is         *)
(* carried, so finishing or aborting each start once is the caller's       *)
(* contract.  The model hands finish only outstanding ids and foreign ids  *)
(* past the lanes, so FinishOwn says nothing about that case.              *)
(*                                                                         *)
(* Mode selects the code or a reversed witness:                            *)
(*   "Code"       as written.                                              *)
(*   "NoBound"    decode without the bound check: an id past the last      *)
(*                lane reads lane L, past the lanes array.                 *)
(*   "ModLane"    decode the lane as id % L rather than id \div F.         *)
(*   "LeastFull"  stage in the lane with the most free slots, not the      *)
(*                destination's: one client's frames leave on two lanes    *)
(*                and can overtake each other.                             *)
(*   "Reverse"    the worker posts a lane's slots from the last down,      *)
(*                not in slot order: a client's later frame leaves first.  *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   LaneInRange    every decoded lane is 0 .. L - 1.                      *)
(*   FinishOwn      a finish or abort of an outstanding id reaches exactly *)
(*                  the lane and slot its start reserved, and a foreign id *)
(*                  past the lanes reaches none.                           *)
(*   OneLanePerDest every frame reserved and not yet sent for one          *)
(*                  destination is in one lane.                            *)
(*   NoReissue      every outstanding id names its own reserved slot: a    *)
(*                  slot is handed to one frame at a time.                 *)
(*   InOrder        each destination's frames are posted in the order      *)
(*                  they were started.                                     *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS L,          \* lanes: rocketnet_server_rio_t.num_lanes (the server's num_rio_lanes)
          F,          \* rocketnet_server_rio_t.lane_slots: phases times phase size
          Hashes,     \* the destinations, each named by its hash (rocketnet_rio_lane_hash
                      \* of its family, port and address, as the model reads it)
          MaxStarts,  \* start_packet calls in all
          Mode

ASSUME L >= 1 /\ F >= 1 /\ MaxStarts >= 1
ASSUME \A h \in Hashes : h \in Nat
ASSUME Mode \in {"Code", "NoBound", "ModLane", "LeastFull", "Reverse"}

Dests == Hashes   \* a destination is its hash: the lane reads nothing else of it

Lanes == 0..L - 1
Slots == 0..F - 1
None == -1          \* a decode that names no lane or slot
NoDest == -1        \* the empty issued map's range (a hash is never negative)
Empty == <<>>       \* a free slot

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

VARIABLES slot,      \* slot[lane][i]: Empty, or <<dest, start, state>> with state
                     \* "res" (reserved), "live" (finished) or "zero" (aborted)
          next,      \* next[lane]: the lane's reservation counter
          issued,    \* id -> destination for every reserved, unfinished id
          started,   \* ghost: frames started to each destination
          posted,    \* ghost: each destination's start numbers, in post order
          starts,    \* ghost: start_packet calls so far
          strayed,   \* ghost: a decode left 0 .. L - 1
          misrouted  \* ghost: a finish or abort reached a slot it did not reserve

vars == <<slot, next, issued, started, posted, starts, strayed, misrouted>>

Init ==
    /\ slot = [l \in Lanes |-> [i \in Slots |-> Empty]]
    /\ next = [l \in Lanes |-> 0]
    /\ issued = [id \in {} |-> NoDest]
    /\ started = [d \in Dests |-> 0]
    /\ posted = [d \in Dests |-> <<>>]
    /\ starts = 0
    /\ strayed = FALSE
    /\ misrouted = FALSE

\* the lane a frame to d is staged in
StageLane(d) ==
    IF Mode = "LeastFull"
    THEN CHOOSE l \in Lanes : \A m \in Lanes : next[l] <= next[m]
    ELSE LaneOf(d)

\* rocketnet_server_rio_start_packet: the lane's next slot; refused (no step)
\* once the lane's slots are used
Start(d) ==
    /\ starts < MaxStarts
    /\ LET lane == StageLane(d)
           i == next[lane]
           id == Encode(lane, i)
       IN /\ i < F
          /\ slot' = [slot EXCEPT ![lane][i] = <<d, started[d], "res">>]
          /\ next' = [next EXCEPT ![lane] = i + 1]
          /\ issued' = [x \in DOMAIN issued \cup {id} |-> IF x = id THEN d ELSE issued[x]]
    /\ started' = [started EXCEPT ![d] = @ + 1]
    /\ starts' = starts + 1
    /\ UNCHANGED <<posted, strayed, misrouted>>

Without(f, id) == [x \in DOMAIN f \ {id} |-> f[x]]

\* rocketnet_server_rio_finish_packet (keep = TRUE: the slot gets its length)
\* or abort_packet (keep = FALSE: the slot stays zero); the slot stays
\* reserved until its lane is sent
Finish(id, keep) ==
    /\ id \in DOMAIN issued
    /\ LET r == Decode(id)
           d == issued[id]
       IN /\ strayed' = (strayed \/ (r[1] /\ r[2] \notin Lanes))
          /\ IF /\ r[1] /\ r[2] \in Lanes
                /\ slot[r[2]][r[3]] # Empty
                /\ slot[r[2]][r[3]][1] = d /\ slot[r[2]][r[3]][3] = "res"
                /\ Encode(r[2], r[3]) = id
               THEN /\ misrouted' = misrouted
                    /\ slot' = [slot EXCEPT ![r[2]][r[3]] = <<@[1], @[2], IF keep THEN "live" ELSE "zero">>]
               ELSE /\ misrouted' = TRUE
                    /\ UNCHANGED slot
          /\ issued' = Without(issued, id)
    /\ UNCHANGED <<next, started, posted, starts>>

\* the server hands an id start never issued: refused, no lane touched
Foreign(id) ==
    /\ id \in ForeignIds
    /\ ~(id \in DOMAIN issued)
    /\ LET r == Decode(id)
       IN /\ strayed' = (strayed \/ (r[1] /\ r[2] \notin Lanes))
          /\ misrouted' = (misrouted \/ r[1])
    /\ UNCHANGED <<slot, next, issued, started, posted, starts>>

\* the order a lane's worker walks its reserved slots
PostOrder(n) == IF Mode = "Reverse" THEN [k \in 1..n |-> n - k] ELSE [k \in 1..n |-> k - 1]

\* a lane's worker posts the lane's reserved slots in slot order, the
\* finished ones (rocketnet_rio_post_phase skips a zero slot), and the slots
\* are free again; only once every reserved slot of the lane is finished or
\* aborted
Send(lane) ==
    /\ next[lane] > 0
    /\ ~\E i \in Slots : slot[lane][i] # Empty /\ slot[lane][i][3] = "res"
    /\ LET walk == [k \in 1..next[lane] |-> slot[lane][PostOrder(next[lane])[k]]]
           live == SelectSeq(walk, LAMBDA e : e # Empty /\ e[3] = "live")
       IN posted' = [d \in Dests |-> posted[d] \o
                        [k \in 1..Len(SelectSeq(live, LAMBDA e : e[1] = d)) |->
                            SelectSeq(live, LAMBDA e : e[1] = d)[k][2]]]
    /\ slot' = [slot EXCEPT ![lane] = [i \in Slots |-> Empty]]
    /\ next' = [next EXCEPT ![lane] = 0]
    /\ UNCHANGED <<issued, started, starts, strayed, misrouted>>

Next ==
    \/ \E d \in Dests : Start(d)
    \/ \E id \in DOMAIN issued, keep \in BOOLEAN : Finish(id, keep)
    \/ \E id \in ForeignIds : Foreign(id)
    \/ \E lane \in Lanes : Send(lane)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ slot \in [Lanes -> [Slots -> {Empty} \cup (Dests \X (0..MaxStarts - 1) \X {"res", "live", "zero"})]]
    /\ next \in [Lanes -> 0..F]
    /\ starts \in 0..MaxStarts
    /\ strayed \in BOOLEAN
    /\ misrouted \in BOOLEAN

LaneInRange == ~strayed

FinishOwn == ~misrouted

\* every issued id names its own reserved slot, so no slot is handed twice
NoReissue ==
    \A id \in DOMAIN issued :
        /\ id \div F \in Lanes
        /\ slot[id \div F][id % F] # Empty
        /\ slot[id \div F][id % F][1] = issued[id]
        /\ slot[id \div F][id % F][3] = "res"

OneLanePerDest ==
    \A l, m \in Lanes, i, j \in Slots :
        (slot[l][i] # Empty /\ slot[m][j] # Empty /\ slot[l][i][1] = slot[m][j][1]) => l = m

\* each destination's frames leave in the order they were started
InOrder ==
    \A d \in Dests : \A k \in 1..Len(posted[d]) - 1 : posted[d][k] < posted[d][k + 1]
=============================================================================
