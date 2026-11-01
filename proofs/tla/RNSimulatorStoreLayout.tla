----------------------- MODULE RNSimulatorStoreLayout -----------------------
(***************************************************************************)
(* The development link simulator's slot store and its index, as laid out  *)
(* in sdk/rocketnet_network_simulator.cpp, with the code's own arithmetic. *)
(* Link.tla is the wire's contract (what is lost, duplicated, reordered,   *)
(* delayed); this model is the store that holds the packets in flight and  *)
(* the index that picks the next one.                                      *)
(*                                                                         *)
(* The store: Cap slots, each with used, a delivery time, a destination    *)
(* and a generation that changes on reuse, and free_hint (every slot below *)
(* it is occupied).  rocketnet_simulator_insert scans from free_hint for   *)
(* the first free slot, bumps its generation, fills it, adds it to the     *)
(* index and moves the hint past it; with no free slot it sets the hint to *)
(* Cap and drops the packet.  rocketnet_simulator_retire removes a handle  *)
(* (slot and generation) from the index, frees the slot and lowers the     *)
(* hint to it.                                                             *)
(*                                                                         *)
(* The index: per destination shard (the shard is the address hash masked  *)
(* by the shard count), occupancy words of W bits over the physical slots  *)
(* (slot s is bit s % W of word s \div W) and summary words of SB bits     *)
(* over the occupancy words (word w is bit w % SB of summary word          *)
(* w \div SB); slot_index_shard records each slot's membership; and        *)
(* shard_min_delivery_time, a lower bound on the earliest delivery time in *)
(* the shard.  rocketnet_simulator_index_add sets the slot's bit, the      *)
(* summary bit when the word was empty, and lowers the bound;              *)
(* rocketnet_simulator_index_remove refuses a stale handle (slot free or   *)
(* generation changed), clears the bit and, when the word became empty,    *)
(* its summary bit, and leaves the bound as it is.                         *)
(* rocketnet_simulator_select_fast returns at once when the bound is past  *)
(* now; otherwise it walks summary words, their set bits low to high, each *)
(* named occupancy word and its set bits low to high, and takes the first  *)
(* slot due and addressed to the destination; a walk that finds none sets  *)
(* the bound to the exact minimum it saw.  The selection rule is           *)
(* rocketnet_simulator_select_reference: the lowest used slot that is due  *)
(* and addressed to the destination.  The ghost mismatch records a fast    *)
(* selection that differs from it (the audit's comparison).                *)
(*                                                                         *)
(* Words are sets of bit positions: a word is zero when its set is empty,  *)
(* ctz is the set's minimum.  Times are 0..T and DBL_MAX is T + 1.         *)
(* Destinations are 0..ND-1 and the hash is the identity, so the shard is  *)
(* d % NShards and one shard holds more than one destination.              *)
(*                                                                         *)
(* Generations run modulo GenMod.  The code's are 64-bit and never wrap;   *)
(* the model keeps one stale handle per slot (the last one retired), whose *)
(* generation differs from a reused slot's after one bump at any modulus   *)
(* of 2 or more, so the comparison is exact.  StaleRetire is the outside   *)
(* event of a caller holding that old handle                               *)
(* (rocketnet_network_simulator_test_remove_handle).                       *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   LowerHint       retire does not lower free_hint                       *)
(*   SummaryOnEmpty  remove clears the summary bit with the word nonempty  *)
(*   KeepBoundLow    remove raises the bound when the removed packet was   *)
(*                   at it                                                 *)
(*   BumpGeneration  insert reuses a slot without bumping its generation   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Cap,            \* slots
    W,              \* bits per occupancy word (64 in the code)
    SB,             \* bits per summary word (64 in the code)
    NShards,        \* shards (ROCKETNET_SIMULATOR_SHARDS, 64 in the code)
    ND,             \* destinations
    T,              \* last time
    GenMod,         \* generations run 0..GenMod-1
    LowerHint,      \* rocketnet_simulator_retire lowers free_hint to the slot
    SummaryOnEmpty, \* rocketnet_simulator_index_remove clears the summary bit only when the word is empty
    KeepBoundLow,   \* rocketnet_simulator_index_remove leaves the bound as it is
    BumpGeneration  \* rocketnet_simulator_insert bumps the generation

ASSUME Cap \in Nat \ {0} /\ W \in Nat \ {0} /\ SB \in Nat \ {0}
ASSUME NShards \in Nat \ {0} /\ ND \in Nat \ {0} /\ T \in Nat /\ GenMod \in Nat /\ GenMod >= 2
ASSUME \A f \in {LowerHint, SummaryOnEmpty, KeepBoundLow, BumpGeneration} : f \in BOOLEAN

Slots == 0..(Cap - 1)
OccWords == (Cap + W - 1) \div W            \* ceil(capacity / 64)
SumWords == (OccWords + SB - 1) \div SB     \* ceil(occupancy_words / 64)
Shards == 0..(NShards - 1)
Dests == 0..(ND - 1)
Times == 0..T
INF == T + 1                                \* DBL_MAX
NOSLOT == -1                                \* ROCKETNET_SIMULATOR_NO_SLOT
Gens == 0..(GenMod - 1)

Shard(d) == d % NShards                     \* rocketnet_simulator_shard: hash & shard_mask
WordOf(s) == s \div W                       \* slot >> 6
BitOf(s) == s % W                           \* slot & 63
SumWordOf(w) == w \div SB                   \* word >> 6
SumBitOf(w) == w % SB                       \* word & 63

Min(S) == CHOOSE x \in S : \A y \in S : x <= y

VARIABLES
    used,           \* used[s]
    dt,             \* delivery_time[s]
    to,             \* to[s]
    gen,            \* generation[s]
    sis,            \* slot_index_shard[s], NOSLOT when not a member
    occ,            \* occ[sh][w]: shard sh's occupancy word w, a set of bit positions
    sum,            \* sum[sh][v]: shard sh's summary word v, a set of bit positions
    minDT,          \* shard_min_delivery_time[sh]
    hint,           \* free_hint
    now,            \* the caller's current time
    oldGen,         \* the generation of the handle last retired from slot s
    oldValid,       \* a handle was retired from slot s
    mismatch,       \* ghost: the fast selection differed from the reference scan
    badInsert,      \* ghost: an insert took a slot that is not the lowest free one
    badDrop,        \* ghost: an insert dropped its packet with a slot free
    staleHit        \* ghost: a stale handle retired a live packet

vars == <<used, dt, to, gen, sis, occ, sum, minDT, hint, now, oldGen, oldValid,
          mismatch, badInsert, badDrop, staleHit>>

Init ==
    /\ used = [s \in Slots |-> FALSE]
    /\ dt = [s \in Slots |-> 0]
    /\ to = [s \in Slots |-> 0]
    /\ gen = [s \in Slots |-> 0]
    /\ sis = [s \in Slots |-> NOSLOT]
    /\ occ = [sh \in Shards |-> [w \in 0..(OccWords - 1) |-> {}]]
    /\ sum = [sh \in Shards |-> [v \in 0..(SumWords - 1) |-> {}]]
    /\ minDT = [sh \in Shards |-> INF]
    /\ hint = 0
    /\ now = 0
    /\ oldGen = [s \in Slots |-> 0]
    /\ oldValid = [s \in Slots |-> FALSE]
    /\ mismatch = FALSE
    /\ badInsert = FALSE
    /\ badDrop = FALSE
    /\ staleHit = FALSE

----------------------------------------------------------------------------
(* rocketnet_simulator_select_reference: the lowest used slot due now and  *)
(* addressed to d.                                                         *)

RefCandidates(d) == { s \in Slots : used[s] /\ dt[s] <= now /\ to[s] = d }
Reference(d) == IF RefCandidates(d) = {} THEN NOSLOT ELSE Min(RefCandidates(d))

(* rocketnet_simulator_select_fast's walk over shard sh: every visited     *)
(* (summary word, summary bit, occupancy bit) in its enumeration order,    *)
(* lexicographic on the triple; the slot is                                *)
(* (summary_index * SB + summary bit) * W + occupancy bit.                 *)

OccIndex(v, b) == v * SB + b
Visited(sh) ==
    { <<v, b, c>> \in (0..(SumWords - 1)) \X (0..(SB - 1)) \X (0..(W - 1)) :
        /\ b \in sum[sh][v]
        /\ OccIndex(v, b) < OccWords
        /\ c \in occ[sh][OccIndex(v, b)] }
SlotAt(t) == OccIndex(t[1], t[2]) * W + t[3]
LexLess(x, y) ==
    \/ x[1] < y[1]
    \/ x[1] = y[1] /\ x[2] < y[2]
    \/ x[1] = y[1] /\ x[2] = y[2] /\ x[3] < y[3]
LexMin(S) == CHOOSE x \in S : \A y \in S : y = x \/ LexLess(x, y)

FastMatches(d) == { t \in Visited(Shard(d)) : SlotAt(t) < Cap /\ dt[SlotAt(t)] <= now /\ to[SlotAt(t)] = d }
BoundFuture(d) == minDT[Shard(d)] > now
Fast(d) == IF BoundFuture(d) \/ FastMatches(d) = {} THEN NOSLOT ELSE SlotAt(LexMin(FastMatches(d)))
\* the minimum a walk that found nothing due saw: every visited slot's delivery time
WalkMinimum(sh) ==
    LET ts == { dt[SlotAt(t)] : t \in { u \in Visited(sh) : SlotAt(u) < Cap } }
    IN IF ts = {} THEN INF ELSE Min(ts)

----------------------------------------------------------------------------
(* rocketnet_simulator_index_remove and rocketnet_simulator_retire for     *)
(* handle (i, g): whether it is accepted, and the state after it.          *)

RetireOK(i, g) ==
    /\ used[i]
    /\ gen[i] = g
    /\ sis[i] = Shard(to[i])
    /\ BitOf(i) \in occ[Shard(to[i])][WordOf(i)]

RetireSteps(i) ==
    LET sh == Shard(to[i])
        w == WordOf(i)
        left == occ[sh][w] \ {BitOf(i)}
        clearSummary == IF SummaryOnEmpty THEN left = {} ELSE TRUE
    IN /\ occ' = [occ EXCEPT ![sh][w] = left]
       /\ sum' = IF clearSummary
                 THEN [sum EXCEPT ![sh][SumWordOf(w)] = @ \ {SumBitOf(w)}]
                 ELSE sum
       /\ sis' = [sis EXCEPT ![i] = NOSLOT]
       /\ minDT' = IF ~KeepBoundLow /\ dt[i] = minDT[sh]
                   THEN [minDT EXCEPT ![sh] = INF]
                   ELSE minDT
       /\ used' = [used EXCEPT ![i] = FALSE]
       /\ hint' = IF LowerHint /\ i < hint THEN i ELSE hint
       /\ oldGen' = [oldGen EXCEPT ![i] = gen[i]]
       /\ oldValid' = [oldValid EXCEPT ![i] = TRUE]

----------------------------------------------------------------------------

FreeFromHint == { i \in hint..(Cap - 1) : ~used[i] }
FreeSlots == { i \in Slots : ~used[i] }

\* rocketnet_simulator_insert of a packet for d due at t, with
\* rocketnet_simulator_index_add: rocketnet_network_simulator_send (through
\* rocketnet_simulator_emit and rocketnet_network_simulator_flush_window) and
\* rocketnet_network_simulator_deliver_packets
Send(d, t) ==
    /\ t \in now..T
    /\ IF FreeFromHint # {}
       THEN LET i == Min(FreeFromHint)
                sh == Shard(d)
                w == WordOf(i)
                wasEmpty == occ[sh][w] = {}
            IN /\ gen' = [gen EXCEPT ![i] = IF BumpGeneration THEN (@ + 1) % GenMod ELSE @]
               /\ used' = [used EXCEPT ![i] = TRUE]
               /\ dt' = [dt EXCEPT ![i] = t]
               /\ to' = [to EXCEPT ![i] = d]
               /\ occ' = [occ EXCEPT ![sh][w] = @ \cup {BitOf(i)}]
               /\ sum' = IF wasEmpty
                         THEN [sum EXCEPT ![sh][SumWordOf(w)] = @ \cup {SumBitOf(w)}]
                         ELSE sum
               /\ sis' = [sis EXCEPT ![i] = sh]
               /\ minDT' = IF t < minDT[sh] THEN [minDT EXCEPT ![sh] = t] ELSE minDT
               /\ hint' = i + 1
               /\ badInsert' = (badInsert \/ i # Min(FreeSlots))
               /\ UNCHANGED <<badDrop>>
       ELSE /\ hint' = Cap
            /\ badDrop' = (badDrop \/ FreeSlots # {})
            /\ UNCHANGED <<used, dt, to, gen, sis, occ, sum, minDT, badInsert>>
    /\ UNCHANGED <<now, oldGen, oldValid, mismatch, staleHit>>

\* rocketnet_network_simulator_receive: rocketnet_simulator_select_fast (the
\* audit compares it with rocketnet_simulator_select_reference), then
\* rocketnet_simulator_retire of the chosen slot's handle; a walk that finds
\* nothing due refreshes the shard's bound to the exact minimum
Receive(d) ==
    LET f == Fast(d)
        sh == Shard(d)
    IN /\ mismatch' = (mismatch \/ f # Reference(d))
       /\ IF f = NOSLOT
          THEN /\ minDT' = IF BoundFuture(d) THEN minDT ELSE [minDT EXCEPT ![sh] = WalkMinimum(sh)]
               /\ UNCHANGED <<used, sis, occ, sum, hint, oldGen, oldValid>>
          ELSE IF RetireOK(f, gen[f])
          THEN RetireSteps(f)
          \* the code asserts the retire; a refusal here leaves the store as it is
          ELSE UNCHANGED <<used, sis, occ, sum, minDT, hint, oldGen, oldValid>>
       /\ UNCHANGED <<dt, to, gen, now, badInsert, badDrop, staleHit>>

\* rocketnet_network_simulator_receive_any: the lowest used slot due now,
\* any destination, retired through its handle
ReceiveAny ==
    LET due == { s \in Slots : used[s] /\ dt[s] <= now }
    IN /\ due # {}
       /\ LET i == Min(due) IN RetireOK(i, gen[i]) /\ RetireSteps(i)
       /\ UNCHANGED <<dt, to, gen, now, mismatch, badInsert, badDrop, staleHit>>

\* a caller presents the handle last retired from slot s
\* (rocketnet_network_simulator_test_remove_handle): index_remove refuses it
\* unless the slot holds that generation again
StaleRetire(s) ==
    /\ oldValid[s]
    /\ IF RetireOK(s, oldGen[s])
       THEN /\ RetireSteps(s)
            /\ staleHit' = TRUE
       ELSE UNCHANGED <<used, sis, occ, sum, minDT, hint, oldGen, oldValid, staleHit>>
    /\ UNCHANGED <<dt, to, gen, now, mismatch, badInsert, badDrop>>

\* the caller's clock
Tick ==
    /\ now < T
    /\ now' = now + 1
    /\ UNCHANGED <<used, dt, to, gen, sis, occ, sum, minDT, hint, oldGen, oldValid,
                   mismatch, badInsert, badDrop, staleHit>>

Next ==
    \/ \E d \in Dests, t \in Times : Send(d, t)
    \/ \E d \in Dests : Receive(d)
    \/ ReceiveAny
    \/ \E s \in Slots : StaleRetire(s)
    \/ Tick

Spec == Init /\ [][Next]_vars

\* The fingerprint view: a free slot's delivery time and destination are dead
\* (insert writes both before anything reads them), so states that differ only
\* in them are one state.
View ==
    << used,
       [s \in Slots |-> IF used[s] THEN dt[s] ELSE 0],
       [s \in Slots |-> IF used[s] THEN to[s] ELSE 0],
       gen, sis, occ, sum, minDT, hint, now, oldGen, oldValid,
       mismatch, badInsert, badDrop, staleHit >>

----------------------------------------------------------------------------

TypeOK ==
    /\ used \in [Slots -> BOOLEAN]
    /\ dt \in [Slots -> Times]
    /\ to \in [Slots -> Dests]
    /\ gen \in [Slots -> Gens]
    /\ sis \in [Slots -> Shards \cup {NOSLOT}]
    /\ occ \in [Shards -> [0..(OccWords - 1) -> SUBSET (0..(W - 1))]]
    /\ sum \in [Shards -> [0..(SumWords - 1) -> SUBSET (0..(SB - 1))]]
    /\ minDT \in [Shards -> Times \cup {INF}]
    /\ hint \in 0..Cap
    /\ now \in Times
    /\ oldGen \in [Slots -> Gens] /\ oldValid \in [Slots -> BOOLEAN]
    /\ {mismatch, badInsert, badDrop, staleHit} \subseteq BOOLEAN

\* receive chooses the slot the selection rule names (the audit's comparison)
SelectMatchesReference == ~mismatch

\* send places a packet in the lowest free slot, and drops it only when none is free
SendTakesLowest == ~badInsert /\ ~badDrop

\* a stale handle never retires a live packet
NoStaleRemoval == ~staleHit

\* every slot below free_hint is occupied (rocketnet_simulator_audit_index)
HintBelowFree == \A i \in Slots : i < hint => used[i]

\* a slot's bit is set in exactly its destination's shard, and only while used
OccupancyExact ==
    \A sh \in Shards, s \in Slots :
        (BitOf(s) \in occ[sh][WordOf(s)]) <=> (used[s] /\ Shard(to[s]) = sh)

\* slot_index_shard names the shard of every used slot and is NOSLOT otherwise
MembershipExact == \A s \in Slots : sis[s] = IF used[s] THEN Shard(to[s]) ELSE NOSLOT

\* a summary bit is set exactly when its occupancy word is nonempty
SummaryExact ==
    \A sh \in Shards, v \in 0..(SumWords - 1), b \in 0..(SB - 1) :
        (b \in sum[sh][v]) <=> (OccIndex(v, b) < OccWords /\ occ[sh][OccIndex(v, b)] # {})

\* the shard bound is at or below every delivery time in the shard
BoundBelowShard == \A s \in Slots : used[s] => minDT[Shard(to[s])] <= dt[s]

=============================================================================
