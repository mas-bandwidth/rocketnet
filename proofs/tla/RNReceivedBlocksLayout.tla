----------------------- MODULE RNReceivedBlocksLayout -----------------------
(***************************************************************************)
(* The receive-side statistics blocks at the code's layout                 *)
(* (sdk/rocketnet_ack.cpp: rocketnet_ack_endpoint_record_received,         *)
(* rocketnet_ack_stats_received_advance,                                   *)
(* rocketnet_ack_stats_received_clear_physical_range,                      *)
(* rocketnet_ack_stats_received_rebuild_block and                          *)
(* rocketnet_ack_stats_received_query).                                    *)
(*                                                                         *)
(* The received ring has N = B * Blocks records (2048 = 32 x 64 in the     *)
(* code); sequence s lives at slot s % N.  Each fixed block of B physical  *)
(* slots keeps a summary of its records whose sequence lies in the         *)
(* retained window [base, highest], base = max(1, highest - N + 1).  A     *)
(* receive first moves retention: the physical slots of the sequences      *)
(* [old base, new base) leave, as [start, end) with start = old_base % N,  *)
(* split at the ring end; clear_physical_range empties each block wholly   *)
(* inside the range and rebuilds the partial ones (first block from / B,   *)
(* last block (to - 1) / B); a move of N or more empties every block.      *)
(* Then the record is written and its block rebuilt.  The published        *)
(* cohort is the query over the older half [base, top], top = min(highest, *)
(* base + N/2 - 1): pieces that end at a block boundary or the range end,  *)
(* a whole aligned block read from its summary, a partial one record by    *)
(* record against its exact sequence.                                      *)
(*                                                                         *)
(* A summary is modelled as the set of sequences it counts: the code's     *)
(* bytes, count and time extrema are functions of those records, and its   *)
(* merge is their union.  RNServerStats.tla checks the aggregates the      *)
(* query feeds with the reduction taken whole; this module checks the      *)
(* blocks that compute it.                                                 *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "nowrap"     the leaving range is not split at the ring end, so the  *)
(*                slots past it keep counting old records;                 *)
(*   "firstblock" the first block of a clear is from / B rounded up, which *)
(*                misses a partial first block;                            *)
(*   "edgeblock"  the query reads a block's summary whenever the piece     *)
(*                starts on a block boundary, even a piece shorter than    *)
(*                the block.                                               *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    B,          \* slots per block: ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS, 32
    Blocks,     \* blocks: ROCKETNET_ACK_RECEIVED_STATS_BLOCKS, 64
    MaxSeq,     \* the highest sequence received
    Broken      \* "none", or the reversed witness

N == B * Blocks
Half == N \div 2

ASSUME /\ B \in Nat \ {0} /\ Blocks \in Nat \ {0} /\ N % 2 = 0
       /\ MaxSeq \in Nat \ {0}
       /\ Broken \in {"none", "nowrap", "firstblock", "edgeblock"}

Slots == 0..N-1
BlockIds == 0..Blocks-1

VARIABLES
    ring,       \* slot -> the sequence its record holds, 0 when not written since the reset
    highest,    \* endpoint->highest_received_sequence
    summary     \* block -> the set of sequences its summary counts

vars == <<ring, highest, summary>>

Base(h) == IF h >= N THEN h - N + 1 ELSE 1
Top(h) == IF h < Base(h) + Half - 1 THEN h ELSE Base(h) + Half - 1

\* rocketnet_ack_stats_received_rebuild_block
Rebuild(r, b, h) ==
    {r[s] : s \in {x \in b * B..(b * B + B - 1) : r[x] # 0 /\ r[x] >= Base(h) /\ r[x] <= h}}

\* rocketnet_ack_stats_received_clear_physical_range over [from, to)
ClearPhys(sum, r, from, to, h) ==
    IF from >= to THEN sum
    ELSE LET first == IF Broken = "firstblock" THEN (from + B - 1) \div B ELSE from \div B
             last == (to - 1) \div B
         IN [b \in BlockIds |->
               IF b < first \/ b > last THEN sum[b]
               ELSE IF from <= b * B /\ b * B + B <= to THEN {}
               ELSE Rebuild(r, b, h)]

\* rocketnet_ack_stats_received_advance to h
Advance(sum, r, old, h) ==
    IF h <= old \/ old = 0 THEN sum
    ELSE LET count == Base(h) - Base(old)
         IN IF count = 0 THEN sum
            ELSE IF count >= N THEN [b \in BlockIds |-> {}]
            ELSE LET start == Base(old) % N
                     end == start + count
                     s1 == ClearPhys(sum, r, start, IF end < N THEN end ELSE N, h)
                 IN IF end > N /\ Broken # "nowrap" THEN ClearPhys(s1, r, 0, end - N, h) ELSE s1

\* rocketnet_ack_stats_received_query over [first, last]
Query(first, last) ==
    LET RECURSIVE Go(_, _)
        Go(seq, remaining) ==
            IF remaining = 0 THEN {}
            ELSE LET slot == seq % N
                     offset == slot % B
                     take == IF B - offset > remaining THEN remaining ELSE B - offset
                     whole == IF Broken = "edgeblock" THEN offset = 0 ELSE offset = 0 /\ take = B
                     piece == IF whole THEN summary[slot \div B]
                              ELSE {ring[slot + i] : i \in {j \in 0..take-1 : ring[slot + j] = seq + j}}
                 IN piece \cup Go(seq + take, remaining - take)
    IN Go(first, last - first + 1)

Init ==
    /\ ring = [s \in Slots |-> 0]
    /\ highest = 0
    /\ summary = [b \in BlockIds |-> {}]

\* rocketnet_ack_endpoint_record_received: a sequence a ring or more behind
\* the high-water mark is refused before anything moves
Receive(s) ==
    /\ ~(highest >= N /\ s <= highest - N)
    /\ LET h == IF s > highest THEN s ELSE highest
           moved == Advance(summary, ring, highest, h)
           r == [ring EXCEPT ![s % N] = s]
           b == (s % N) \div B
       IN /\ ring' = r
          /\ highest' = h
          /\ summary' = [moved EXCEPT ![b] = Rebuild(r, b, h)]

Next == \E s \in 1..MaxSeq : Receive(s)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ ring \in [Slots -> 0..MaxSeq]
    /\ highest \in 0..MaxSeq
    /\ summary \in [BlockIds -> SUBSET (1..MaxSeq)]

\* each block's summary counts exactly its records in [base, highest]
SummariesExact == \A b \in BlockIds : summary[b] = Rebuild(ring, b, highest)

\* the older-half query is the records of [base, top] read one by one
QueryIsReference ==
    highest > 0 =>
        Query(Base(highest), Top(highest)) = {s \in Base(highest)..Top(highest) : ring[s % N] = s}

=============================================================================
