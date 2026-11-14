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
(* Zero-byte slots.  start_packet reserves the slot with packet_bytes 0;   *)
(* finish_packet writes the length, and abort_packet (or a finish that     *)
(* refuses its length, counted SEND_FAILED) leaves it 0 (Abort).  The      *)
(* send bank releases every zero-byte slot's operation to the free queue   *)
(* of the lane its index names, unposted, and posts the rest               *)
(* (rocketnet_iocp_send_bank).  zeroed holds the <<destination, start>>    *)
(* of every packet left zero: in a bank it is the slot's zero length, and  *)
(* once the bank is posted it is history, so InOrder can say which starts  *)
(* must never be posted.  Here a started packet is finished until Abort    *)
(* makes it zero; the code's caller finishes or aborts every start before  *)
(* send_packets publishes the bank.                                        *)
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
(* packet handle's index (HandleUnique).  AbortDefect = 1 posts a          *)
(* zero-byte slot as if finished: an aborted packet is posted (InOrder).   *)
(* AbortDefect = 2 neither posts nor releases a zero-byte slot: its        *)
(* operation is in no free queue, no bank and not in flight (Conserved).   *)
(*                                                                         *)
(* Not modelled: start_packet's refusal after its pop (an address the      *)
(* socket cannot carry), which releases the operation to its lane's tail   *)
(* and starts no packet; its refusal of a slot at or past the send queue   *)
(* size, unreachable while each lane holds send_queue_size operations; and *)
(* a refused post, which releases the operation as a completion does       *)
(* (Complete).                                                             *)
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
    OwnerDefect,    \* defect: 0 none, 1 a thread posts the lane past its own, 2 a thread posts every lane but its own
    AbortDefect     \* defect: 0 none, 1 a zero-byte slot is posted, 2 a zero-byte slot is neither posted nor released

ASSUME Lanes >= 1 /\ Size >= 1 /\ MaxStarts >= 1 /\ IndexBits >= 1
ASSUME Dests \subseteq Nat
ASSUME AnyLane \in BOOLEAN /\ ReleaseMod \in BOOLEAN
ASSUME Threads \in Nat /\ Threads >= 1 /\ Threads <= Lanes
ASSUME OwnerDefect \in 0 .. 2
ASSUME AbortDefect \in 0 .. 2

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
    postedBy,   \* [LaneIds -> SUBSET ThreadIds]: the threads that ever posted each lane
    zeroed      \* the <<dest, seq>> of every packet left zero bytes (abort_packet, or a refused finish)

vars == << free, bank, inflight, started, posted, postedBy, zeroed >>

Init ==
    /\ free = [ l \in LaneIds |-> [ k \in 1 .. Size |-> l * Size + k - 1 ] ]
    /\ bank = [ l \in LaneIds |-> << >> ]
    /\ inflight = {}
    /\ started = [ d \in Dests |-> 0 ]
    /\ posted = [ d \in Dests |-> << >> ]
    /\ postedBy = [ l \in LaneIds |-> {} ]
    /\ zeroed = {}

\* a staged entry <<op, dest, seq>> whose slot carries zero bytes
Zero( e ) == << e[2], e[3] >> \in zeroed

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
    /\ UNCHANGED << inflight, posted, postedBy, zeroed >>

\* rocketnet_server_iocp_start_packet refused: the destination's lane has no
\* free operation, and nothing changes
StartRefused( d, l ) ==
    /\ StageLane( d, l )
    /\ free[l] = << >>
    /\ UNCHANGED vars

\* rocketnet_server_iocp_abort_packet on staged operation i (or a
\* finish_packet that refuses its length): the slot keeps zero bytes
Abort( i ) ==
    /\ \E l \in LaneIds : \E k \in 1 .. Len( bank[l] ) :
          /\ bank[l][k][1] = i
          /\ ~Zero( bank[l][k] )
          /\ zeroed' = zeroed \cup { << bank[l][k][2], bank[l][k][3] >> }
    /\ UNCHANGED << free, bank, inflight, started, posted, postedBy >>

\* the entries of a bank the send posts: every finished one, and under
\* AbortDefect = 1 the zero-byte ones too
Sent( b ) == IF AbortDefect = 1 THEN b ELSE SelectSeq( b, LAMBDA e : ~Zero( e ) )

\* the entries of a bank the send releases unposted: every zero-byte one
\* (none under the two defects)
Unsent( b ) == IF AbortDefect = 0 THEN SelectSeq( b, LAMBDA e : Zero( e ) ) ELSE << >>

\* rocketnet_iocp_send_worker of thread t over rocketnet_iocp_send_bank: a
\* lane the thread takes; each zero-byte slot's operation goes back to the
\* free queue of the lane its index names, unposted, and the rest of the
\* bank is posted in staged order
Post( t, l ) ==
    /\ l \in ThreadLanes( t )
    /\ Len( bank[l] ) > 0
    /\ LET sent == Sent( bank[l] )
           unsent == Unsent( bank[l] )
       IN /\ inflight' = inflight \cup { sent[k][1] : k \in 1 .. Len( sent ) }
          /\ posted' = [ d \in Dests |-> posted[d] \o
                          [ k \in 1 .. Len( SelectSeq( sent, LAMBDA e : e[2] = d ) ) |->
                              SelectSeq( sent, LAMBDA e : e[2] = d )[k][3] ] ]
          /\ free' = [ m \in LaneIds |-> free[m] \o
                        [ k \in 1 .. Len( SelectSeq( unsent, LAMBDA e : ReleaseLane( e[1] ) = m ) ) |->
                            SelectSeq( unsent, LAMBDA e : ReleaseLane( e[1] ) = m )[k][1] ] ]
    /\ bank' = [ bank EXCEPT ![l] = << >> ]
    /\ postedBy' = [ postedBy EXCEPT ![l] = @ \cup { t } ]
    /\ UNCHANGED << started, zeroed >>

\* a send completion: rocketnet_iocp_retire_send, rocketnet_iocp_release_send
Complete( i ) ==
    /\ i \in inflight
    /\ inflight' = inflight \ { i }
    /\ free' = [ free EXCEPT ![ReleaseLane( i )] = Append( @, i ) ]
    /\ UNCHANGED << bank, started, posted, postedBy, zeroed >>

Next ==
    \/ \E d \in Dests, l \in LaneIds, i \in Ops : Start( d, l, i )
    \/ \E d \in Dests, l \in LaneIds : StartRefused( d, l )
    \/ \E i \in Ops : Abort( i )
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
    /\ zeroed \subseteq Dests \X ( 0 .. MaxStarts - 1 )

\* every operation is in exactly one place: a free queue, a bank, or posted
Conserved ==
    /\ FreeCount + StagedCount + Cardinality( inflight ) = Lanes * Size
    /\ FreeOps \cup StagedOps \cup inflight = Ops

\* a lane's queue and bank hold only its own operations
OwnLane ==
    \A l \in LaneIds :
        /\ \A k \in 1 .. Len( free[l] ) : OpLane( free[l][k] ) = l
        /\ \A k \in 1 .. Len( bank[l] ) : OpLane( bank[l][k][1] ) = l

\* the packets to d still in a bank
InBank( d, n ) == \E l \in LaneIds : \E k \in 1 .. Len( bank[l] ) : bank[l][k][2] = d /\ bank[l][k][3] = n

\* a finite set of naturals in increasing order
RECURSIVE Ascending( _ )
Ascending( S ) == IF S = {} THEN << >>
                  ELSE LET m == CHOOSE x \in S : \A y \in S : x <= y
                       IN << m >> \o Ascending( S \ { m } )

\* a destination's packets are posted in the order they were started: what
\* was posted to d is exactly its started packets that have left their bank
\* and were not left zero, in start order (an aborted packet is never posted,
\* a finished one is posted once, and none overtakes an earlier one)
InOrder ==
    \A d \in Dests :
        posted[d] = Ascending( { n \in 0 .. started[d] - 1 : ~InBank( d, n ) /\ << d, n >> \notin zeroed } )

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
