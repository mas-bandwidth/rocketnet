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
(*                                                                         *)
(* The send threads.  Threads send threads (the code's                     *)
(* rocketnet_server_iocp_send_threads) post the lanes: lane l belongs to   *)
(* thread l % Threads (rocketnet_server_iocp_send_thread_of_lane), and     *)
(* thread t posts lanes t, t + Threads, ... (ThreadLanes), each bank whole *)
(* in staged order.  postedBy[l] is the set of threads that ever posted    *)
(* lane l, a ghost.  OnlyOwnerPosts: a lane is only ever posted by thread  *)
(* l % Threads.  OneOwner: every lane is in exactly one thread's lanes, so *)
(* none is starved (it has an owner) and none is double-posted (it has     *)
(* only one).  The mapping is the layout arithmetic, kept in the model     *)
(* (l % Threads), not abstracted.  Reversed witnesses: OwnerDefect = 1     *)
(* has thread t post the lanes l with (l + 1) % Threads = t, a thread one  *)
(* lane past its own; OwnerDefect = 2 has thread t post every lane that is *)
(* not its own (l % Threads # t), the ownership test inverted.  Both fall  *)
(* OnlyOwnerPosts at the code's two threads, which checks postedBy against *)
(* the true l % Threads, never against the guard the defect changes.       *)
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
    ReleaseMod,     \* defect: release to lane i % Lanes
    Threads,        \* rocketnet_server_iocp_send_threads: the send threads, at most the lanes
    OwnerDefect     \* defect: 0 none, 1 a thread posts the lane past its own, 2 a thread posts every lane but its own

ASSUME Lanes >= 1 /\ Size >= 1 /\ MaxStarts >= 1 /\ IndexBits >= 1
ASSUME Dests \subseteq Nat
ASSUME AnyLane \in BOOLEAN /\ ReleaseMod \in BOOLEAN
ASSUME Threads \in Nat /\ Threads >= 1 /\ Threads <= Lanes
ASSUME OwnerDefect \in 0 .. 2

Ops == 0 .. Lanes * Size - 1
LaneIds == 0 .. Lanes - 1
ThreadIds == 0 .. Threads - 1

\* the thread that owns lane l: rocketnet_server_iocp_send_thread_of_lane
ThreadOf( l ) == l % Threads

\* the lanes thread t posts: rocketnet_iocp_send_thread_lanes walks
\* t, t + Threads, ... below Lanes, which is the lanes l with l % Threads = t
ThreadLanes( t ) ==
    IF OwnerDefect = 1 THEN { l \in LaneIds : ( l + 1 ) % Threads = t }
    ELSE IF OwnerDefect = 2 THEN { l \in LaneIds : l % Threads # t }
    ELSE { l \in LaneIds : l % Threads = t }

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
    posted,     \* [Dests -> Seq(Nat)]: the sequence numbers posted to each destination, in post order
    postedBy    \* [LaneIds -> SUBSET ThreadIds]: the threads that ever posted each lane

vars == << free, bank, inflight, started, posted, postedBy >>

Init ==
    /\ free = [ l \in LaneIds |-> [ k \in 1 .. Size |-> l * Size + k - 1 ] ]
    /\ bank = [ l \in LaneIds |-> << >> ]
    /\ inflight = {}
    /\ started = [ d \in Dests |-> 0 ]
    /\ posted = [ d \in Dests |-> << >> ]
    /\ postedBy = [ l \in LaneIds |-> {} ]

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
    /\ UNCHANGED << inflight, posted, postedBy >>

\* rocketnet_server_iocp_start_packet refused: the destination's lane has no
\* free operation, and nothing changes
StartRefused( d, l ) ==
    /\ StageLane( d, l )
    /\ free[l] = << >>
    /\ UNCHANGED vars

\* rocketnet_iocp_send_worker of thread t over rocketnet_iocp_send_bank: a
\* lane the thread takes, its bank posted whole in staged order
Post( t, l ) ==
    /\ l \in ThreadLanes( t )
    /\ Len( bank[l] ) > 0
    /\ inflight' = inflight \cup { bank[l][k][1] : k \in 1 .. Len( bank[l] ) }
    /\ posted' = [ d \in Dests |-> posted[d] \o
                    [ k \in 1 .. Len( SelectSeq( bank[l], LAMBDA e : e[2] = d ) ) |->
                        SelectSeq( bank[l], LAMBDA e : e[2] = d )[k][3] ] ]
    /\ bank' = [ bank EXCEPT ![l] = << >> ]
    /\ postedBy' = [ postedBy EXCEPT ![l] = @ \cup { t } ]
    /\ UNCHANGED << free, started >>

\* a send completion: rocketnet_iocp_retire_send, rocketnet_iocp_release_send
Complete( i ) ==
    /\ i \in inflight
    /\ inflight' = inflight \ { i }
    /\ free' = [ free EXCEPT ![ReleaseLane( i )] = Append( @, i ) ]
    /\ UNCHANGED << bank, started, posted, postedBy >>

Next ==
    \/ \E d \in Dests, l \in LaneIds, i \in Ops : Start( d, l, i )
    \/ \E d \in Dests, l \in LaneIds : StartRefused( d, l )
    \/ \E t \in ThreadIds, l \in LaneIds : Post( t, l )
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
    /\ postedBy \in [ LaneIds -> SUBSET ThreadIds ]

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

\* a lane is only ever posted by its owner, thread l % Threads
OnlyOwnerPosts == \A l \in LaneIds : postedBy[l] \subseteq { ThreadOf( l ) }

\* every lane is in exactly one thread's lanes: none is starved (no thread
\* takes it) and none is double-posted (two threads take it)
\* (a predicate over the constants: it holds or fails at the initial state,
\* and TLC reports a failure as an error, not a trace. OnlyOwnerPosts is the
\* invariant with force over the behaviours: it checks what posted against
\* the true arithmetic l % Threads, not against the guard of Post)
OneOwner == \A l \in LaneIds : Cardinality( { t \in ThreadIds : l \in ThreadLanes( t ) } ) = 1

\* no two operations a handle may name at once share its index bits
HandleUnique ==
    \A i, j \in StagedOps \cup inflight : i # j => HandleIndex( i ) # HandleIndex( j )

=============================================================================
