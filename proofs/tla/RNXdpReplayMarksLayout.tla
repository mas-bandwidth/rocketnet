----------------------- MODULE RNXdpReplayMarksLayout -----------------------
(***************************************************************************)
(* The userspace pending buffer for the kernel replay map, as laid out in  *)
(* sdk/rocketnet_xdp_replay_marks.h, with the code's own arithmetic.       *)
(* RNXdpReplay.tla models the buffer's contents as sets; this model        *)
(* carries the arrays those sets cannot see.                               *)
(*                                                                         *)
(* The buffer: the array forgets with forget_count, the parallel arrays    *)
(* mark_keys and mark_values with mark_count, and kept_mark_count, the     *)
(* length of the prefix of the marks the last commit refused.  An update   *)
(* is the walk's forget pass (rocketnet_xdp_replay_marks_forget: coalesce    *)
(* a pending key or append a new key,                                     *)
(* then drop a kept mark of the same key by shifting the rest down,        *)
(* rocketnet_xdp_replay_marks_drop_kept), then its mark pass               *)
(* (rocketnet_xdp_replay_marks_mark: replace a kept mark of the key in     *)
(* place, else append), then the commit, whose end is                      *)
(* rocketnet_xdp_replay_marks_reset_keeping: two in-place compactions,     *)
(* each loop iteration one step here, so a write that lands on an entry    *)
(* the loop has not read yet is visible.                                   *)
(*                                                                         *)
(* Memory past a count is not cleared: an entry the compaction or a drop   *)
(* left behind keeps its old key and value, and only a search that runs    *)
(* past its bound can read it.                                             *)
(*                                                                         *)
(* The ghosts absF, absM and absK are the contract the arrays must hold:   *)
(* the pending forgets in order, the pending marks in order and the kept   *)
(* prefix's length, updated with sequence operators, never with the        *)
(* arrays' arithmetic.  The kernel's answer to a commit (which forgets it  *)
(* refused, which marks it applied) is chosen freely.                      *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   DeduplicateForgets pending forgets append a duplicate key            *)
(*   FullTestExact   the forget full test one late (forget_count > MAX)    *)
(*   KeptSearchBound the forget's kept search runs one past the prefix     *)
(*   CompactFromZero the mark compaction writes from the old kept count,   *)
(*                   over entries it has not read                          *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    M,                  \* configured marks->capacity
    Keys,               \* source addresses (keys of the kernel map)
    Vals,               \* high-water values
    DeduplicateForgets, \* a pending forget of the key consumes no new cell
    FullTestExact,      \* rocketnet_xdp_replay_marks_forget refuses at forget_count >= MAX
    KeptSearchBound,    \* the forget's kept search runs i < kept_mark_count
    CompactFromZero,    \* reset_keeping's mark compaction writes from index 0
    NONE                \* a cell nothing has written (a model value)

ASSUME M \in Nat \ {0}
ASSUME \A f \in {DeduplicateForgets, FullTestExact, KeptSearchBound, CompactFromZero} : f \in BOOLEAN

Cells == 0..M                   \* cell M is the one past the end: a write there is out of bounds
Idx == 0..(M - 1)

VARIABLES
    forgets,        \* forgets[i]
    fc,             \* forget_count
    mkeys,          \* mark_keys[i]
    mvals,          \* mark_values[i].high_water
    mc,             \* mark_count
    kc,             \* kept_mark_count
    refused,        \* refused
    phase,          \* "forget", "mark" (the walk's passes), "cf", "cm" (reset_keeping's two loops)
    li,             \* the compaction loop's i (or j)
    lk,             \* the compaction loop's kept (or kept_marks)
    failed,         \* forget_failed: the forget indices the kernel refused
    applied,        \* mark_applied: the mark indices the kernel took
    doneF,          \* the keys the walk forgot this update
    doneM,          \* the keys the walk marked this update
    absF,           \* ghost: the pending forgets, in order
    absM,           \* ghost: the pending marks, in order, each [k, v]
    absK,           \* ghost: how many of absM's first entries are kept
    overflow        \* ghost: a write at or past MAX

vars == <<forgets, fc, mkeys, mvals, mc, kc, refused, phase, li, lk, failed, applied,
          doneF, doneM, absF, absM, absK, overflow>>

\* A write at index x: in bounds it lands, past the end it is recorded and lands in cell M
Put(arr, x, val) == [ arr EXCEPT ![IF x > M THEN M ELSE x] = val ]
Past(x) == x >= M

\* rocketnet_xdp_replay_marks_drop_kept(i): every entry above i moves down one,
\* in a loop that reads j before it writes j - 1; the top entry stays behind
Shift(arr, i, count) == [ x \in Cells |-> IF x >= i /\ x < count - 1 THEN arr[x + 1] ELSE arr[x] ]

\* the forget's search of the kept prefix: the first index whose key is k
SearchEnd == IF KeptSearchBound THEN kc ELSE kc + 1
KeptHit(k) == { i \in 0..(SearchEnd - 1) : i <= M /\ mkeys[i] = k }
FirstKept(k) == CHOOSE i \in KeptHit(k) : \A j \in KeptHit(k) : i <= j

\* the mark's search: the kept prefix only
MarkHit(k) == { i \in 0..(kc - 1) : mkeys[i] = k }

\* sequence helpers for the contract
Sub(s, keep) == [ n \in 1..Cardinality({ p \in 1..Len(s) : keep[p] }) |->
                    LET ps == { p \in 1..Len(s) : keep[p] }
                        Nth(nn) == CHOOSE p \in ps : Cardinality({ q \in ps : q <= p }) = nn
                    IN s[Nth(n)] ]
SeqWithout(s, p) == [ n \in 1..(Len(s) - 1) |-> IF n < p THEN s[n] ELSE s[n + 1] ]
AbsKeptHit(k) == { p \in 1..absK : absM[p].k = k }
ForgetHit(k) == { i \in 0..(fc - 1) : forgets[i] = k }
PendingForget(k) == DeduplicateForgets /\ ForgetHit(k) # {}
AbsForgetHit(k) == { p \in 1..Len(absF) : absF[p] = k }

Init ==
    /\ forgets = [ x \in Cells |-> NONE ]
    /\ fc = 0
    /\ mkeys = [ x \in Cells |-> NONE ]
    /\ mvals = [ x \in Cells |-> NONE ]
    /\ mc = 0
    /\ kc = 0
    /\ refused = 0
    /\ phase = "forget"
    /\ li = 0 /\ lk = 0
    /\ failed = {} /\ applied = {}
    /\ doneF = {} /\ doneM = {}
    /\ absF = << >>
    /\ absM = << >>
    /\ absK = 0
    /\ overflow = FALSE

\* rocketnet_xdp_replay_marks_forget, from the walk's forget pass
Forget(k) ==
    /\ phase = "forget"
    /\ k \notin doneF
    /\ doneF' = doneF \cup {k}
    /\ IF ~PendingForget(k) /\ (IF FullTestExact THEN fc >= M ELSE fc > M)
       THEN /\ refused' = refused + 1
            /\ UNCHANGED <<forgets, fc, mkeys, mvals, mc, kc, overflow>>
       ELSE /\ forgets' = IF PendingForget(k) THEN forgets ELSE Put(forgets, fc, k)
            /\ fc' = IF PendingForget(k) THEN fc ELSE fc + 1
            /\ overflow' = (overflow \/ (~PendingForget(k) /\ Past(fc)))
            /\ IF KeptHit(k) # {}
               THEN /\ mkeys' = Shift(mkeys, FirstKept(k), mc)
                    /\ mvals' = Shift(mvals, FirstKept(k), mc)
                    /\ mc' = mc - 1
                    /\ kc' = kc - 1
               ELSE UNCHANGED <<mkeys, mvals, mc, kc>>
            /\ UNCHANGED refused
    \* the contract: the forget is owed, and a kept mark of its key is dropped
    /\ IF AbsForgetHit(k) = {} /\ Len(absF) >= M
       THEN UNCHANGED <<absF, absM, absK>>
       ELSE /\ absF' = IF AbsForgetHit(k) # {} THEN absF ELSE Append(absF, k)
            /\ IF AbsKeptHit(k) # {}
               THEN LET p == CHOOSE p \in AbsKeptHit(k) : \A q \in AbsKeptHit(k) : p <= q
                    IN /\ absM' = SeqWithout(absM, p)
                       /\ absK' = absK - 1
               ELSE UNCHANGED <<absM, absK>>
    /\ UNCHANGED <<phase, li, lk, failed, applied, doneM>>

\* rocketnet_xdp_replay_marks_mark, from the walk's mark pass (which follows
\* the forget pass and marks each key at most once)
Mark(k, v) ==
    /\ phase \in {"forget", "mark"}
    /\ k \notin doneM
    /\ phase' = "mark"
    /\ doneM' = doneM \cup {k}
    /\ IF MarkHit(k) # {}
       THEN LET i == CHOOSE i \in MarkHit(k) : \A j \in MarkHit(k) : i <= j
            IN /\ mvals' = [ mvals EXCEPT ![i] = v ]
               /\ UNCHANGED <<mkeys, mc, refused, overflow>>
       ELSE IF mc >= M
            THEN /\ refused' = refused + 1
                 /\ UNCHANGED <<mkeys, mvals, mc, overflow>>
            ELSE /\ mkeys' = Put(mkeys, mc, k)
                 /\ mvals' = Put(mvals, mc, v)
                 /\ mc' = mc + 1
                 /\ overflow' = (overflow \/ Past(mc))
                 /\ UNCHANGED refused
    /\ IF AbsKeptHit(k) # {}
       THEN LET p == CHOOSE p \in AbsKeptHit(k) : \A q \in AbsKeptHit(k) : p <= q
            IN /\ absM' = [ absM EXCEPT ![p] = [ k |-> k, v |-> v ] ]
               /\ UNCHANGED absK
       ELSE IF Len(absM) >= M
            THEN UNCHANGED <<absM, absK>>
            ELSE /\ absM' = Append(absM, [ k |-> k, v |-> v ])
                 /\ UNCHANGED absK
    /\ UNCHANGED <<forgets, fc, kc, li, lk, failed, applied, doneF, absF>>

\* the transport's commit: the kernel refuses some forgets and applies some
\* marks; rocketnet_xdp_replay_marks_reset_keeping starts.  The contract's
\* result is taken here: the refused forgets no applied mark of their key
\* superseded, in order, and the refused marks, in order, all kept.
Commit ==
    /\ phase \in {"forget", "mark"}
    /\ \E fs \in SUBSET (0..(fc - 1)), as \in SUBSET (0..(mc - 1)) :
          /\ failed' = fs
          /\ applied' = as
          /\ LET superseded(p) == \E j \in as : j + 1 <= Len(absM) /\ absM[j + 1].k = absF[p]
                 keepF == [ p \in 1..Len(absF) |-> (p - 1) \in fs /\ ~superseded(p) ]
                 keepM == [ p \in 1..Len(absM) |-> (p - 1) \notin as ]
             IN /\ absF' = Sub(absF, keepF)
                /\ absM' = Sub(absM, keepM)
                /\ absK' = Len(Sub(absM, keepM))
    /\ phase' = "cf"
    /\ li' = 0
    /\ lk' = 0
    /\ UNCHANGED <<forgets, fc, mkeys, mvals, mc, kc, refused, doneF, doneM, overflow>>

\* one iteration of reset_keeping's forget loop; the end sets forget_count
\* and starts the mark loop
CompactForgetStep ==
    /\ phase = "cf"
    /\ IF li < fc
       THEN LET superseded == \E j \in applied : mkeys[j] = forgets[li]
                keep == li \in failed /\ ~superseded
            IN /\ forgets' = IF keep /\ lk # li THEN Put(forgets, lk, forgets[li]) ELSE forgets
               /\ overflow' = (overflow \/ (keep /\ lk # li /\ Past(lk)))
               /\ lk' = IF keep THEN lk + 1 ELSE lk
               /\ li' = li + 1
               /\ UNCHANGED <<fc, phase>>
       ELSE /\ fc' = lk
            /\ phase' = "cm"
            /\ li' = 0
            /\ lk' = IF CompactFromZero THEN 0 ELSE kc
            /\ UNCHANGED <<forgets, overflow>>
    /\ UNCHANGED <<mkeys, mvals, mc, kc, refused, failed, applied, doneF, doneM, absF, absM, absK>>

\* one iteration of reset_keeping's mark loop; the end sets mark_count and
\* kept_mark_count, zeroes refused and opens the next update
CompactMarkStep ==
    /\ phase = "cm"
    /\ IF li < mc
       THEN LET keep == li \notin applied
            IN /\ mkeys' = IF keep /\ lk # li THEN Put(mkeys, lk, mkeys[li]) ELSE mkeys
               /\ mvals' = IF keep /\ lk # li THEN Put(mvals, lk, mvals[li]) ELSE mvals
               /\ overflow' = (overflow \/ (keep /\ lk # li /\ Past(lk)))
               /\ lk' = IF keep THEN lk + 1 ELSE lk
               /\ li' = li + 1
               /\ UNCHANGED <<mc, kc, refused, phase, failed, applied, doneF, doneM>>
       ELSE /\ mc' = lk
            /\ kc' = lk
            /\ refused' = 0
            /\ phase' = "forget"
            /\ li' = 0 /\ lk' = 0
            /\ failed' = {} /\ applied' = {}
            /\ doneF' = {} /\ doneM' = {}
            /\ UNCHANGED <<mkeys, mvals, overflow>>
    /\ UNCHANGED <<forgets, fc, absF, absM, absK>>

Next ==
    \/ \E k \in Keys : Forget(k)
    \/ \E k \in Keys, v \in Vals : Mark(k, v)
    \/ Commit
    \/ CompactForgetStep
    \/ CompactMarkStep

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

Quiet == phase \in {"forget", "mark"}

TypeOK ==
    /\ forgets \in [ Cells -> Keys \cup {NONE} ]
    /\ mkeys \in [ Cells -> Keys \cup {NONE} ]
    /\ mvals \in [ Cells -> Vals \cup {NONE} ]
    /\ fc \in 0..(M + 1) /\ mc \in 0..(M + 1) /\ kc \in 0..(M + 1)
    /\ phase \in {"forget", "mark", "cf", "cm"}
    /\ overflow \in BOOLEAN

\* no write lands at or past MAX
InBounds == ~overflow

\* between commits, the arrays' live prefixes are the contract: the pending
\* forgets in order, the pending marks in order, the kept prefix its length
LayoutIsQueue ==
    Quiet =>
        /\ fc = Len(absF)
        /\ \A i \in 0..(fc - 1) : i < M /\ forgets[i] = absF[i + 1]
        /\ mc = Len(absM)
        /\ \A i \in 0..(mc - 1) : i < M /\ mkeys[i] = absM[i + 1].k /\ mvals[i] = absM[i + 1].v
        /\ kc = absK

\* the kept prefix names each key at most once, so the forget's and the
\* mark's first-match searches find the one entry of the key
KeptKeysUnique ==
    Quiet => \A i, j \in 0..(kc - 1) : i # j => mkeys[i] # mkeys[j]

=============================================================================
