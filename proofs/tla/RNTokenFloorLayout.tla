------------------------- MODULE RNTokenFloorLayout -------------------------
(***************************************************************************)
(* The used-token history's bindings table with its expiry floor, as       *)
(* sdk/rocketnet_token_tables.cpp lays it out after PR #1017: Cap slots,   *)
(* each with a token and an expiry timestamp, a dense free stack filled at *)
(* create, the insertion order (its ring and tombstones are               *)
(* RNIndexRingArray.tla's; here only its head, the oldest live slot, is   *)
(* read), and expire_floor, a lower bound on every live binding's expiry. *)
(*                                                                         *)
(*   Record(t, e)  rocketnet_used_token_history_record: a live binding of  *)
(*                 t that has not expired is kept (no step); one that has  *)
(*                 expired is retired and its slot reused; otherwise       *)
(*                 rn_token_binding_select_slot picks the slot, then       *)
(*                 rn_token_slot_insert writes it and lowers the floor to  *)
(*                 e when e is below it, and rn_token_order_push puts the  *)
(*                 slot at the tail of the order.                          *)
(*   Select        rn_token_binding_select_slot: a free slot if any; else  *)
(*                 the oldest when it has expired; else, when the floor is *)
(*                 above the clock, the oldest with no scan (no binding    *)
(*                 can have expired); else a scan of slots 0 .. Cap-1 that *)
(*                 takes the first expired slot, and when none is found    *)
(*                 takes the oldest and sets the floor to the minimum      *)
(*                 expiry it read over all Cap slots.  The scan is one     *)
(*                 call with no other table operation inside it, so it is  *)
(*                 one step here, with the loop's arithmetic: break at the *)
(*                 first expired index, the minimum only over the slots    *)
(*                 read.                                                   *)
(*   Tick          the clock moves forward.                                *)
(*                                                                         *)
(* Infinity is UINT64_MAX, the floor at create.                            *)
(*                                                                         *)
(* Broken selects a reversed witness:                                      *)
(*   "prefixfloor"  a scan that breaks at an expired slot still sets the   *)
(*                  floor to the minimum of the slots it read before it,   *)
(*                  so the floor is raised without a full scan: an expired *)
(*                  slot after the break sits below the floor              *)
(*                  (FloorBelowLive), the table then believes no binding   *)
(*                  has expired and evicts the live oldest while it stays  *)
(*                  (NoLiveEvictionWhileExpired);                          *)
(*   "noinsertlower" rn_token_slot_insert does not lower the floor, so a   *)
(*                  new binding expires below it (FloorBelowLive).         *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Cap,        \* the bindings table's capacity
    Tokens,     \* the token identifiers a client may record
    MaxTime,    \* the clock's last value
    Life,       \* the furthest a recorded expiry lies past the clock
    Broken      \* "none", or the reversed witness

ASSUME /\ Cap \in Nat \ {0} /\ MaxTime \in Nat /\ Life \in Nat \ {0}
       /\ Broken \in {"none", "prefixfloor", "noinsertlower"}

Idx == 0..Cap-1
Infinity == MaxTime + Life + 1     \* UINT64_MAX: above every expiry

VARIABLES
    now,        \* current_timestamp
    live,       \* live[s]: slot s holds a binding (its ticket is current)
    tok,        \* tok[s]: the binding's token identifier
    exp,        \* exp[s]: expire_timestamp[s]
    freeCount,  \* free_count of the dense free stack
    order,      \* the live slots in insertion order, oldest first
    floor,      \* expire_floor
    lastEvict   \* ghost: the last live-slot eviction, as [live, expiredLeft]

vars == << now, live, tok, exp, freeCount, order, floor, lastEvict >>

Min(S) == IF S = {} THEN Infinity ELSE CHOOSE m \in S : \A x \in S : m <= x

Without(seq, s) == SelectSeq(seq, LAMBDA x : x # s)

Expired(s) == exp[s] <= now

\* the first expired index of the scan, Cap when the loop runs to its end
FirstExpired == Min({ s \in Idx : Expired(s) } \cup {Cap})

\* the minimum expiry over the slots the scan read before index i
ReadMin(i) == Min({ exp[s] : s \in 0..i-1 })

TypeOK ==
    /\ now \in 0..MaxTime
    /\ live \in [Idx -> BOOLEAN]
    /\ tok \in [Idx -> Tokens \cup {0}]
    /\ exp \in [Idx -> 0..Infinity]
    /\ freeCount \in 0..Cap
    /\ order \in Seq(Idx)
    /\ floor \in 0..Infinity
    /\ lastEvict \in [live : BOOLEAN, expiredLeft : BOOLEAN]

Init ==
    /\ now = 0
    /\ live = [s \in Idx |-> FALSE]
    /\ tok = [s \in Idx |-> 0]
    /\ exp = [s \in Idx |-> 0]
    /\ freeCount = Cap
    /\ order = << >>
    /\ floor = Infinity
    /\ lastEvict = [live |-> FALSE, expiredLeft |-> FALSE]

Tick ==
    /\ now < MaxTime
    /\ now' = now + 1
    /\ UNCHANGED << live, tok, exp, freeCount, order, floor, lastEvict >>

\* rn_token_slot_insert's floor, from the floor the selection left
Lowered(f, e) == IF Broken # "noinsertlower" /\ e < f THEN e ELSE f

\* write binding (t, e) into slot v and push v to the order's tail
Insert(v, t, e, f) ==
    /\ live' = [live EXCEPT ![v] = TRUE]
    /\ tok' = [tok EXCEPT ![v] = t]
    /\ exp' = [exp EXCEPT ![v] = e]
    /\ order' = Append(Without(order, v), v)
    /\ floor' = Lowered(f, e)

Found(t) == { s \in Idx : live[s] /\ tok[s] = t }

\* a binding of t that has expired: retired, its slot written again
RecordExpired(t, e) ==
    \E s \in Found(t) :
        /\ Expired(s)
        /\ Insert(s, t, e, floor)
        /\ UNCHANGED << now, freeCount, lastEvict >>

\* no binding of t: a free slot (the first pops return 0, 1, 2, ...)
RecordFree(t, e) ==
    /\ Found(t) = {}
    /\ freeCount > 0
    /\ Insert(Cap - freeCount, t, e, floor)
    /\ freeCount' = freeCount - 1
    /\ UNCHANGED << now, lastEvict >>

\* no binding of t and no free slot: rn_token_binding_select_slot's eviction
RecordEvict(t, e) ==
    /\ Found(t) = {}
    /\ freeCount = 0
    /\ order # << >>
    /\ LET oldest == Head(order)
           scans  == ~Expired(oldest) /\ floor <= now
           first  == FirstExpired
           victim == IF scans /\ first < Cap THEN first ELSE oldest
           f      == IF ~scans THEN floor
                     ELSE IF first = Cap THEN ReadMin(Cap)
                     ELSE IF Broken = "prefixfloor" THEN ReadMin(first)
                     ELSE floor
       IN /\ lastEvict' = [live |-> ~Expired(victim),
                           expiredLeft |-> \E s \in Idx \ {victim} : live[s] /\ Expired(s)]
          /\ Insert(victim, t, e, f)
    /\ UNCHANGED << now, freeCount >>

Record(t, e) == RecordExpired(t, e) \/ RecordFree(t, e) \/ RecordEvict(t, e)

Next ==
    \/ Tick
    \/ \E t \in Tokens, e \in now+1 .. now+Life : Record(t, e)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants                                                              *)
(***************************************************************************)

\* expire_floor is a lower bound on every live binding's expiry
FloorBelowLive == \A s \in Idx : live[s] => floor <= exp[s]

\* a live binding is never evicted while an expired one remains
NoLiveEvictionWhileExpired == ~(lastEvict.live /\ lastEvict.expiredLeft)

\* the order holds each live slot once, and only live slots
OrderIsLive ==
    /\ Len(order) = Cardinality({ s \in Idx : live[s] })
    /\ \A i \in 1..Len(order) : live[order[i]]
    /\ freeCount = Cap - Len(order)

\* a token is bound in at most one live slot
OneSlotPerToken == \A s1, s2 \in Idx : live[s1] /\ live[s2] /\ tok[s1] = tok[s2] => s1 = s2

=============================================================================
