------------------------- MODULE RNIocpReceiveAdmit -------------------------
(***************************************************************************)
(* The IOCP server transport's admission of a received datagram to a      *)
(* completion worker's receive bank, as the code lays it out             *)
(* (sdk/rocketnet_server_iocp_windows.cpp, rocketnet_iocp_receive_admit   *)
(* and rocketnet_iocp_bank_datagram).  A bank is Capacity slots filled    *)
(* in order at num_packets (rocketnet_server_iocp_receive_queue_size).    *)
(* Each completed datagram is one Arrive step of the worker: the worker   *)
(* prefilter first (rocketnet_receive_prefilter, the stateless rungs main *)
(* would refuse it on), then the capacity check num_packets < capacity.   *)
(* A datagram the prefilter refuses is counted by its reason and never    *)
(* takes a slot; one it accepts is banked if there is room and otherwise  *)
(* counted as BANK_FULL (ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL).  Before *)
(* the filter is published (FilterOn = FALSE) every datagram meets only   *)
(* the capacity check, and main's ladder filters it, as before.  Drain is *)
(* the main thread taking the bank (rocketnet_server_iocp_receive_packets *)
(* over the RNBankFifoLayout hand-off, which this model abstracts to one  *)
(* step: the bank's whole content leaves and its slots are free).         *)
(*                                                                         *)
(* Datagrams are of three kinds: a player's, garbage (fails the           *)
(* stateless prefilter: wrong filter value, a size its type forbids and   *)
(* the rest of rocketnet_receive_drop_t), and hostile (well formed, so it *)
(* passes the prefilter, but not a player's: a forged request, a replay;  *)
(* only main's keyed rungs refuse it).  The property the blast row        *)
(* measured broken: garbage never evicts a player.  A player's datagram   *)
(* may still be refused as BANK_FULL when the bank is full of well-formed *)
(* datagrams (players' or hostile): that is option 2's case, not this     *)
(* layer's.                                                                *)
(*                                                                         *)
(* Ghost state: evicted, set when a player's datagram is refused BANK_FULL *)
(* while the bank holds garbage.                                          *)
(*                                                                         *)
(* Reversed witnesses: BankFirst = TRUE is the order the blast row         *)
(* measured (the capacity check first, the prefilter on main after        *)
(* banking): garbage takes the slots and a player is refused             *)
(* (NoEvictionByGarbage).  UncountedFull = TRUE drops the BANK_FULL count, *)
(* the drop the server's counters never saw (EveryDropCounted).           *)
(* CheckPastEnd = TRUE is the capacity check num_packets <= capacity, one *)
(* slot past the bank (InBounds).                                         *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Capacity,       \* a worker's bank: rocketnet_server_iocp_receive_queue_size( max_clients, workers )
    MaxArrivals,    \* exploration bound: datagrams completed
    FilterOn,       \* the filter is published (rocketnet_server_iocp_set_packet_filter)
    BankFirst,      \* defect: capacity before the prefilter, the order the blast row measured
    UncountedFull,  \* defect: a bank-full drop is not counted
    CheckPastEnd    \* defect: the capacity check num_packets <= capacity

ASSUME Capacity >= 1 /\ MaxArrivals >= 1
ASSUME FilterOn \in BOOLEAN /\ BankFirst \in BOOLEAN /\ UncountedFull \in BOOLEAN /\ CheckPastEnd \in BOOLEAN

Kinds == { "player", "garbage", "hostile" }

\* what rocketnet_receive_prefilter accepts: only garbage fails its stateless rungs
PassesPrefilter(k) == k # "garbage"

VARIABLES
    bank,        \* the bank's slots in fill order: buffers[fill_index], data[0 .. num_packets - 1]
    arrived,     \* datagrams completed, by kind
    refused,     \* refused by the worker prefilter, by kind (the reason's tally)
    full,        \* refused for a full bank, by kind (ROCKETNET_RECEIVE_DROP_BANK_FULL)
    counted,     \* the drops the worker's tally holds, by kind
    delivered,   \* datagrams main took from the bank, by kind
    evicted      \* ghost: a player refused BANK_FULL while garbage held a slot

vars == << bank, arrived, refused, full, counted, delivered, evicted >>

Zero == [ k \in Kinds |-> 0 ]

Init ==
    /\ bank = << >>
    /\ arrived = Zero
    /\ refused = Zero
    /\ full = Zero
    /\ counted = Zero
    /\ delivered = Zero
    /\ evicted = FALSE

Arrivals == arrived[ "player" ] + arrived[ "garbage" ] + arrived[ "hostile" ]

HasRoom == IF CheckPastEnd THEN Len( bank ) <= Capacity ELSE Len( bank ) < Capacity

HoldsGarbage == \E i \in 1 .. Len( bank ) : bank[i] = "garbage"

Refuse( k ) ==
    /\ refused' = [ refused EXCEPT ![k] = @ + 1 ]
    /\ counted' = [ counted EXCEPT ![k] = @ + 1 ]
    /\ UNCHANGED << bank, full, evicted >>

Bank( k ) ==
    /\ bank' = Append( bank, k )
    /\ UNCHANGED << refused, full, counted, evicted >>

Full( k ) ==
    /\ full' = [ full EXCEPT ![k] = @ + 1 ]
    /\ counted' = IF UncountedFull THEN counted ELSE [ counted EXCEPT ![k] = @ + 1 ]
    /\ evicted' = ( evicted \/ ( k = "player" /\ HoldsGarbage ) )
    /\ UNCHANGED << bank, refused >>

\* One completed datagram: rocketnet_iocp_receive_admit, then
\* rocketnet_iocp_bank_datagram's bank or count.
Arrive( k ) ==
    /\ Arrivals < MaxArrivals
    /\ arrived' = [ arrived EXCEPT ![k] = @ + 1 ]
    /\ IF FilterOn /\ ~BankFirst /\ ~PassesPrefilter( k )
       THEN Refuse( k )
       ELSE IF HasRoom THEN Bank( k ) ELSE Full( k )
    /\ UNCHANGED delivered

\* The main thread takes the bank: every datagram in it is delivered to the
\* protocol (which, under BankFirst, is where garbage is refused).
Drain ==
    /\ Len( bank ) > 0
    /\ delivered' = [ k \in Kinds |-> delivered[k] + Len( SelectSeq( bank, LAMBDA x : x = k ) ) ]
    /\ bank' = << >>
    /\ UNCHANGED << arrived, refused, full, counted, evicted >>

Next == ( \E k \in Kinds : Arrive( k ) ) \/ Drain

Spec == Init /\ [][ Next ]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ bank \in Seq( Kinds )
    /\ \A k \in Kinds : arrived[k] \in Nat /\ refused[k] \in Nat /\ full[k] \in Nat /\ counted[k] \in Nat /\ delivered[k] \in Nat
    /\ evicted \in BOOLEAN

\* num_packets never passes capacity: no write past the bank's arrays
InBounds == Len( bank ) <= Capacity

InBank( k ) == Len( SelectSeq( bank, LAMBDA x : x = k ) )

\* every completed datagram is in exactly one place: the bank, main, or a drop
Accounted == \A k \in Kinds : arrived[k] = InBank( k ) + delivered[k] + refused[k] + full[k]

\* every drop is counted by a named reason (no datagram goes uncounted)
EveryDropCounted == \A k \in Kinds : counted[k] = refused[k] + full[k]

\* with the filter on, garbage never takes a slot
NoGarbageBanked == FilterOn => ( ~HoldsGarbage /\ delivered[ "garbage" ] = 0 )

\* the property the blast row measured broken
NoEvictionByGarbage == ~evicted

=============================================================================
