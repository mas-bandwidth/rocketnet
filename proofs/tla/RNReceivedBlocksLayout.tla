----------------------- MODULE RNReceivedBlocksLayout -----------------------
(***************************************************************************)
(* The receive-side statistics at the code's layout (sdk/rocketnet_ack.cpp: *)
(* rocketnet_ack_endpoint_record_received,                                 *)
(* rocketnet_ack_stats_received_summarize,                                 *)
(* rocketnet_ack_stats_received_generation and                            *)
(* rocketnet_ack_stats_received_refresh).                                  *)
(*                                                                         *)
(* The received ring has N = B * Blocks records (2048 = 32 x 64 in the     *)
(* code); sequence s lives at slot s % N.  A generation is the B           *)
(* sequences [g * B, g * B + B - 1]; all of them live in physical block    *)
(* g % Blocks.  Each block keeps one generation's summary and its tag      *)
(* g + 1 (0 = none).  A receive refuses a sequence a ring or more behind,  *)
(* writes the record, then the summary step: a block tagged with a later   *)
(* generation is left alone, an earlier tag is replaced by this            *)
(* generation holding only this record, a duplicate rebuilds the           *)
(* generation from its records, and any other record joins the summary.    *)
(*                                                                         *)
(* The published cohort is the older half [base, top], top = min(highest,  *)
(* base + N/2 - 1).  Inside one generation it is read record by record and *)
(* nothing is cached.  Otherwise it is three cached pieces: the left piece *)
(* read from suffixes built over base's generation, the middle (the whole  *)
(* generations between, read from their summaries) and the right piece     *)
(* (top's generation from its start to top, appended as top moves).  The  *)
(* pieces are reused only while the write lands above the cached top; top *)
(* leaving its generation with base in place moves the finished right      *)
(* piece and any passed generations into the middle; base leaving its      *)
(* generation rebuilds the suffixes and the middle.                        *)
(*                                                                         *)
(* A summary or piece is modelled as the bag (multiset) of records it      *)
(* counts, each a <<sequence, bytes>> pair: the code's merge               *)
(* (rocketnet_ack_stats_received_merge) adds counts and bytes, so it is    *)
(* bag addition (+), and a record merged twice is counted twice, in the    *)
(* model as in the code; the time extrema are functions of the bag's       *)
(* records.  Two payloads make a duplicate that is merged rather than      *)
(* rebuilt visible as two pairs of one sequence.  The order the code       *)
(* merges in (the first record's NaN) is checked by the unit tests against *)
(* the scan oracle.                                                        *)
(*                                                                         *)
(* Init is rocketnet_ack_endpoint_reset: the tags, the cached base and top *)
(* and the middle and right aggregates are zero (the empty aggregate); the *)
(* block summaries and the left suffixes are left as they were, here the   *)
(* bag Unset of a record no receive writes, so a read of one before a      *)
(* write sets it reaches the published cohort and fails QueryIsReference.  *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "inside"     the pieces are reused when the write lands inside the    *)
(*                cached [base, top];                                      *)
(*   "later"      a write into a block tagged with a later generation      *)
(*                retags it to its own;                                    *)
(*   "duplicate"  a duplicate joins its summary instead of rebuilding it;  *)
(*   "basemove"   the left piece and middle are reused after base leaves   *)
(*                its generation;                                          *)
(*   "overlap"    the right piece is appended from the cached top itself,  *)
(*                not the sequence after it, so that record is counted     *)
(*                twice (a set union would hide it).                       *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, Bags

CONSTANTS
    B,          \* slots per block: ROCKETNET_ACK_RECEIVED_STATS_BLOCK_SLOTS, 32
    Blocks,     \* blocks: ROCKETNET_ACK_RECEIVED_STATS_BLOCKS, 64
    MaxSeq,     \* the highest sequence received
    Payloads,   \* the wire bytes a record may carry
    Broken      \* "none", or the reversed witness

N == B * Blocks
Half == N \div 2
None == <<0, 0>>

ASSUME /\ B \in Nat \ {0} /\ Blocks \in Nat \ {0} /\ N % 2 = 0 /\ Half % B = 0
       /\ MaxSeq \in Nat \ {0}
       /\ Payloads \subseteq Nat /\ Payloads # {}
       /\ Broken \in {"none", "inside", "later", "duplicate", "basemove", "overlap"}

Slots == 0..N-1
BlockIds == 0..Blocks-1
Offsets == 0..B-1
Records == {None} \cup ((1..MaxSeq) \X Payloads)

\* the words a reset leaves as they were: a record no receive writes
Unset == SetToBag({None})

VARIABLES
    ring,       \* slot -> the <<sequence, bytes>> its record holds, None when not written since the reset
    highest,    \* endpoint->highest_received_sequence
    tag,        \* block -> generation + 1 its summary holds, 0 = none
    summary,    \* block -> the bag of records its summary counts
    suffix,     \* offset -> the bag of records of the left generation from that offset on
    middle,     \* the cached middle (stats words at ROCKETNET_ACK_RECEIVED_STATS_MIDDLE_WORD)
    right,      \* the cached right piece (ROCKETNET_ACK_RECEIVED_STATS_RIGHT_WORD)
    cacheBase,  \* the cached base (ROCKETNET_ACK_RECEIVED_STATS_CACHE_BASE_WORD), 0 = no pieces
    cacheTop,   \* the cached top (ROCKETNET_ACK_RECEIVED_STATS_CACHE_TOP_WORD)
    published   \* the bag of records the published count, bytes and extrema reduce

vars == <<ring, highest, tag, summary, suffix, middle, right, cacheBase, cacheTop, published>>

Base(h) == IF h >= N THEN h - N + 1 ELSE 1
Top(h) == IF h < Base(h) + Half - 1 THEN h ELSE Base(h) + Half - 1

\* rocketnet_ack_stats_received_present over [first, last], merged: each
\* sequence's slot holds at most one record, so the bag holds each once
Present(r, first, last) == SetToBag({r[x % N] : x \in {y \in first..last : r[y % N][1] = y}})

\* the records of generation g
Gen(r, g) == Present(r, g * B, g * B + B - 1)

\* rocketnet_ack_stats_received_generation
GenSummary(t, sum, g) == IF t[g % Blocks] = g + 1 THEN sum[g % Blocks] ELSE EmptyBag

\* generations [from, to) merged in order
RECURSIVE Gens(_, _, _, _)
Gens(t, sum, from, to) == IF from >= to THEN EmptyBag ELSE GenSummary(t, sum, from) (+) Gens(t, sum, from + 1, to)

Init ==
    /\ ring = [x \in Slots |-> None]
    /\ highest = 0
    /\ tag = [b \in BlockIds |-> 0]
    /\ summary = [b \in BlockIds |-> Unset]
    /\ suffix = [o \in Offsets |-> Unset]
    /\ middle = EmptyBag
    /\ right = EmptyBag
    /\ cacheBase = 0
    /\ cacheTop = 0
    /\ published = EmptyBag

\* rocketnet_ack_endpoint_record_received, with the summary step
\* (rocketnet_ack_stats_received_summarize) and the pieces
\* (rocketnet_ack_stats_received_refresh)
Receive(s, w) ==
    /\ ~(highest >= N /\ s <= highest - N)
    /\ LET dup == ring[s % N][1] = s
           r == [ring EXCEPT ![s % N] = <<s, w>>]
           h == IF s > highest THEN s ELSE highest
           blk == (s % N) \div B
           t == s \div B + 1
           held == tag[blk]
           retag == held < t \/ (Broken = "later" /\ held > t)
           tg2 == IF retag THEN [tag EXCEPT ![blk] = t] ELSE tag
           sum2 == IF held > t /\ ~retag THEN summary
                   ELSE IF retag THEN [summary EXCEPT ![blk] = SetToBag({<<s, w>>})]
                   ELSE IF dup /\ Broken # "duplicate" THEN [summary EXCEPT ![blk] = Gen(r, t - 1)]
                   ELSE [summary EXCEPT ![blk] = summary[blk] (+) SetToBag({<<s, w>>})]
           base == Base(h)
           top == Top(h)
           bg == base \div B
           tgen == top \div B
           valid == cacheBase # 0 /\ (s > cacheTop \/ Broken = "inside")
           cbg == cacheBase \div B
           ctg == cacheTop \div B
           sameBase == valid /\ (cbg = bg \/ Broken = "basemove")
           sameTop == valid /\ ctg = tgen
           \* the first sequence the right piece appends: cache_top + 1
           next == IF Broken = "overlap" THEN cacheTop ELSE cacheTop + 1
           \* the right piece, and the middle when top leaves its generation with base in place
           right2 == IF sameTop THEN right (+) Present(r, next, top)
                     ELSE Present(r, tgen * B, top)
           moved == middle (+) (right (+) Present(r, next, ctg * B + B - 1))
                           (+) Gens(tg2, sum2, ctg + 1, tgen)
           middle1 == IF ~sameTop /\ sameBase THEN moved ELSE middle
           \* the suffixes and the middle, rebuilt when base leaves its generation
           suffix2 == IF sameBase THEN suffix
                      ELSE [o \in Offsets |-> IF o >= base % B THEN Present(r, bg * B + o, bg * B + B - 1)
                                              ELSE suffix[o]]
           middle2 == IF sameBase THEN middle1 ELSE Gens(tg2, sum2, bg + 1, tgen)
       IN /\ ring' = r
          /\ highest' = h
          /\ tag' = tg2
          /\ summary' = sum2
          /\ IF bg = tgen
             THEN /\ published' = Present(r, base, top)
                  /\ cacheBase' = 0
                  /\ UNCHANGED <<suffix, middle, right, cacheTop>>
             ELSE /\ suffix' = suffix2
                  /\ middle' = middle2
                  /\ right' = right2
                  /\ cacheBase' = base
                  /\ cacheTop' = top
                  /\ published' = suffix2[base % B] (+) middle2 (+) right2

Next == \E s \in 1..MaxSeq, w \in Payloads : Receive(s, w)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ ring \in [Slots -> Records]
    /\ highest \in 0..MaxSeq
    /\ tag \in [BlockIds -> 0..(MaxSeq \div B + 1)]
    /\ \A b \in BlockIds : IsABag(summary[b]) /\ BagToSet(summary[b]) \subseteq Records
    /\ cacheBase \in 0..MaxSeq
    /\ cacheTop \in 0..MaxSeq

\* a block's summary counts exactly its generation's records while none of
\* that generation has left the retained window
SummariesExact ==
    \A b \in BlockIds :
        (tag[b] > 0 /\ (tag[b] - 1) * B >= Base(highest)) => summary[b] = Gen(ring, tag[b] - 1)

\* every generation wholly inside the older half reads as its records
GenerationsExact ==
    highest > 0 =>
        \A g \in (Base(highest) \div B + 1)..(Top(highest) \div B - 1) :
            GenSummary(tag, summary, g) = Gen(ring, g)

\* the cached pieces are their ranges' records
PiecesExact ==
    cacheBase # 0 =>
        /\ right = Present(ring, (cacheTop \div B) * B, cacheTop)
        /\ middle = Present(ring, (cacheBase \div B + 1) * B, (cacheTop \div B) * B - 1)
        /\ \A o \in (cacheBase % B)..(B - 1) :
               suffix[o] = Present(ring, (cacheBase \div B) * B + o, (cacheBase \div B) * B + B - 1)

\* the published cohort is the records of [base, top] read one by one
QueryIsReference ==
    highest > 0 => published = Present(ring, Base(highest), Top(highest))

=============================================================================
