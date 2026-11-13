-------------------------- MODULE RNIocpSendLanes --------------------------
(***************************************************************************)
(* The IOCP server's send lanes as the code lays them out                 *)
(* (sdk/rocketnet_server_iocp_windows.cpp).  Lanes lanes of Size send      *)
(* operations each: operation i belongs to lane i \div Size                *)
(* (rocketnet_iocp_lane_of_index), the operations are carved lane by lane, *)
(* and each lane's free queue starts with its own operations.  A packet to *)
(* destination d stages in lane DestLane(d) (rocketnet_iocp_lane_index,    *)
(* the address hash, given as the set DestLaneAt of d * Lanes + l, which a *)
(* cfg can carry where a function cannot), taking that lane's free         *)
(* operation, or is refused when it has none (StartRefused); the lane's TX *)
(* worker posts its bank in staged order; a completion releases the        *)
(* operation to the free queue of the lane its index names                 *)
(* (rocketnet_iocp_release_send).  A packet handle keeps IndexBits bits    *)
(* for the operation index (rocketnet_iocp_packet_id).                     *)
(*                                                                         *)
(* The three-bank hand-off inside a lane is RNBankFifoLayout's, the free   *)
(* queue's cells RNIocpRefQueue's, the lease generations RNIocpSendLease's *)
(* and the run ring RNIocpRunRing's; here a lane's bank is one sequence    *)
(* the worker posts whole, and the queue is a sequence.                    *)
(*                                                                         *)
(* Reversed witnesses: AnyLane = TRUE stages a packet in any lane (no      *)
(* destination hash): a destination's packets reach two lanes and one lane *)
(* posts a later packet first (InOrder).  ReleaseMod = TRUE releases an    *)
(* operation to lane i % Lanes rather than i \div Size, the carve read the *)
(* wrong way: a lane's queue holds another lane's operation (OwnLane).     *)
(* IndexBits too small for Lanes * Size: two live operations share a       *)
(* packet handle's index (HandleUnique).                                   *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Lanes,          \* rocketnet_server_iocp_send_lanes
    Size,           \* rocketnet_server_iocp_send_queue_size, one lane's operations
    Dests,          \* destinations
    DestLaneAt,     \* rocketnet_iocp_lane_index: d * Lanes + l for destination d in lane l
    IndexBits,      \* the packet handle's index bits (16 in the code)
    MaxStarts,      \* exploration bound: packets started
    AnyLane,        \* defect: a packet stages in any lane
    ReleaseMod      \* defect: release to lane i % Lanes

ASSUME Lanes >= 1 /\ Size >= 1 /\ MaxStarts >= 1 /\ IndexBits >= 1
ASSUME Dests \subseteq Nat
ASSUME AnyLane \in BOOLEAN /\ ReleaseMod \in BOOLEAN

Ops == 0 .. Lanes * Size - 1
LaneIds == 0 .. Lanes - 1

\* every destination is in exactly one lane
ASSUME \A d \in Dests : Cardinality( { l \in LaneIds : d * Lanes + l \in DestLaneAt } ) = 1

\* the destination's lane: rocketnet_iocp_lane_index
DestLane( d ) == CHOOSE l \in LaneIds : d * Lanes + l \in DestLaneAt

\* the code's carve: operation i is lane i \div Size's
OpLane( i ) == i \div Size

\* the lane a completion releases to
ReleaseLane( i ) == IF ReleaseMod THEN i % Lanes ELSE OpLane( i )

\* rocketnet_iocp_packet_id: the low IndexBits bits name the operation
RECURSIVE Pow2Rec( _ )
Pow2Rec( n ) == IF n = 0 THEN 1 ELSE 2 * Pow2Rec( n - 1 )
HandleIndex( i ) == i % Pow2Rec( IndexBits )

VARIABLES
    free,       \* [LaneIds -> Seq(Ops)]: each lane's free queue
    bank,       \* [LaneIds -> Seq(<<op, dest, seq>>)]: each lane's staged packets, in staged order
    inflight,   \* the posted operations awaiting their completion
    started,    \* [Dests -> Nat]: packets started to each destination
    posted      \* [Dests -> Seq(Nat)]: the sequence numbers posted to each destination, in post order

vars == << free, bank, inflight, started, posted >>

Init ==
    /\ free = [ l \in LaneIds |-> [ k \in 1 .. Size |-> l * Size + k - 1 ] ]
    /\ bank = [ l \in LaneIds |-> << >> ]
    /\ inflight = {}
    /\ started = [ d \in Dests |-> 0 ]
    /\ posted = [ d \in Dests |-> << >> ]

Starts == LET Sum[ S \in SUBSET Dests ] == IF S = {} THEN 0 ELSE LET d == CHOOSE x \in S : TRUE IN started[d] + Sum[ S \ { d } ]
          IN Sum[ Dests ]

\* the lane a packet to d stages in
StageLane( d, l ) == IF AnyLane THEN TRUE ELSE l = DestLane( d )

\* rocketnet_server_iocp_start_packet: the destination's lane l, that lane's
\* free operation i (the one the pop returns), the next slot of that lane's
\* producer bank
Start( d, l, i ) ==
    /\ Starts < MaxStarts
    /\ StageLane( d, l )
    /\ Len( free[l] ) > 0
    /\ i = Head( free[l] )
    /\ free' = [ free EXCEPT ![l] = Tail( @ ) ]
    /\ bank' = [ bank EXCEPT ![l] = Append( @, << i, d, started[d] >> ) ]
    /\ started' = [ started EXCEPT ![d] = @ + 1 ]
    /\ UNCHANGED << inflight, posted >>

\* rocketnet_server_iocp_start_packet refused: the destination's lane has no
\* free operation, and nothing changes
StartRefused( d, l ) ==
    /\ StageLane( d, l )
    /\ free[l] = << >>
    /\ UNCHANGED vars

\* rocketnet_iocp_send_worker over rocketnet_iocp_send_bank: the lane's bank
\* posted in staged order
Post( l ) ==
    /\ Len( bank[l] ) > 0
    /\ inflight' = inflight \cup { bank[l][k][1] : k \in 1 .. Len( bank[l] ) }
    /\ posted' = [ d \in Dests |-> posted[d] \o
                    [ k \in 1 .. Len( SelectSeq( bank[l], LAMBDA e : e[2] = d ) ) |->
                        SelectSeq( bank[l], LAMBDA e : e[2] = d )[k][3] ] ]
    /\ bank' = [ bank EXCEPT ![l] = << >> ]
    /\ UNCHANGED << free, started >>

\* a send completion: rocketnet_iocp_retire_send, rocketnet_iocp_release_send
Complete( i ) ==
    /\ i \in inflight
    /\ inflight' = inflight \ { i }
    /\ free' = [ free EXCEPT ![ReleaseLane( i )] = Append( @, i ) ]
    /\ UNCHANGED << bank, started, posted >>

Next ==
    \/ \E d \in Dests, l \in LaneIds, i \in Ops : Start( d, l, i )
    \/ \E d \in Dests, l \in LaneIds : StartRefused( d, l )
    \/ \E l \in LaneIds : Post( l )
    \/ \E i \in Ops : Complete( i )

Spec == Init /\ [][ Next ]_vars

-----------------------------------------------------------------------------

FreeOps == UNION { { free[l][k] : k \in 1 .. Len( free[l] ) } : l \in LaneIds }
StagedOps == UNION { { bank[l][k][1] : k \in 1 .. Len( bank[l] ) } : l \in LaneIds }
FreeCount == LET Sum[ L \in SUBSET LaneIds ] == IF L = {} THEN 0 ELSE LET l == CHOOSE x \in L : TRUE IN Len( free[l] ) + Sum[ L \ { l } ]
             IN Sum[ LaneIds ]
StagedCount == LET Sum[ L \in SUBSET LaneIds ] == IF L = {} THEN 0 ELSE LET l == CHOOSE x \in L : TRUE IN Len( bank[l] ) + Sum[ L \ { l } ]
               IN Sum[ LaneIds ]

TypeOK ==
    /\ \A l \in LaneIds : free[l] \in Seq( Ops ) /\ Len( free[l] ) <= Lanes * Size
    /\ inflight \subseteq Ops
    /\ started \in [ Dests -> Nat ]

\* every operation is in exactly one place: a free queue, a bank, or posted
Conserved ==
    /\ FreeCount + StagedCount + Cardinality( inflight ) = Lanes * Size
    /\ FreeOps \cup StagedOps \cup inflight = Ops

\* a lane's queue and bank hold only its own operations
OwnLane ==
    \A l \in LaneIds :
        /\ \A k \in 1 .. Len( free[l] ) : OpLane( free[l][k] ) = l
        /\ \A k \in 1 .. Len( bank[l] ) : OpLane( bank[l][k][1] ) = l

\* a destination's packets are posted in the order they were started
InOrder == \A d \in Dests : \A k \in 1 .. Len( posted[d] ) : posted[d][k] = k - 1

\* no two operations a handle may name at once share its index bits
HandleUnique ==
    \A i, j \in StagedOps \cup inflight : i # j => HandleIndex( i ) # HandleIndex( j )

=============================================================================
