------------------------- MODULE RNSendGatherLayout -------------------------
(***************************************************************************)
(* The transport-neutral send gather (X2, sdk/rocketnet_send_gather.cpp)   *)
(* at the code's layout.  A send worker hands it one bank of S staged      *)
(* slots, each with a destination and a size (0: aborted).  The gather     *)
(* groups the slots by destination with a stable counting placement, no   *)
(* links (docs/SEQUENCE-BUFFERS.md):                                       *)
(*                                                                         *)
(*   scan    slot by slot, a staged slot finds its destination's group in  *)
(*           an open-addressed table of Buckets entries, by linear probing *)
(*           from Hash(dest) with the index taken modulo Buckets; a        *)
(*           destination not yet seen takes a new group, numbered in order *)
(*           of its first slot.  The slot's group is recorded and the      *)
(*           group's count goes up.  An aborted slot has no group.         *)
(*   prefix  group by group, each count becomes the group's start in the   *)
(*           output: the sum of the counts before it.                      *)
(*   place   slot by slot again, each staged slot goes to its group's next *)
(*           position, which moves on; after it, each group's start is the *)
(*           end of its range.                                             *)
(*   emit    group by group, the group's range is written to order and cut *)
(*           into runs: a run takes the next packet while it holds fewer   *)
(*           than MaxSeg, the total stays within MaxBytes and the packet  *)
(*           is no larger than the run's first; a smaller packet ends the  *)
(*           run.  A packet larger than MaxBytes is a run of its own.     *)
(*                                                                         *)
(* Every input is checked: Init chooses each slot's destination and size.  *)
(* The checked claims, at the end of the gather:                           *)
(*   InBounds      no probe reads a bucket outside 0..Buckets-1.           *)
(*   Permutation   order holds every staged slot once and nothing else.    *)
(*   Grouped       one destination's slots are consecutive in order, in   *)
(*                 staged order; destinations in the order of their first *)
(*                 slot.                                                   *)
(*   RunsTile      the runs cover order exactly, in order.                 *)
(*   RunOneDest    a run's packets go to one destination.                  *)
(*   RunCaps       a run holds at most MaxSeg packets, and at most         *)
(*                 MaxBytes unless it is one packet.                       *)
(*   RunShape      every packet of a run is the size of its first but the *)
(*                 last, which may be smaller.                             *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   ProbeRule   "mod" wraps the probe (the code); "nomod" walks off the   *)
(*               table's end (InBounds).                                   *)
(*   MatchRule   "dest" takes a bucket only for its own destination (the   *)
(*               code); "any" joins the first occupied bucket, so a run    *)
(*               crosses destinations (RunOneDest).                        *)
(*   PlaceRule   "forward" places the slots first to last (the code);      *)
(*               "reverse" places them last to first, reversing each       *)
(*               destination's slots (Grouped).                            *)
(*   CapRule     "bytes" keeps MaxBytes (the code); "none" ignores it      *)
(*               (RunCaps).                                                *)
(*   ShortRule   "close" ends a run at a smaller packet (the code);        *)
(*               "open" lets the run go on past it (RunShape).             *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS S,          \* slots in the bank
          Buckets,    \* entries of the open-addressed table
          Dests,      \* destinations
          Sizes,      \* packet sizes, 0 = aborted
          Hash(_),    \* a destination's home bucket
          MaxSeg,     \* most packets in a run
          MaxBytes,   \* most bytes in a run of more than one packet
          ProbeRule, MatchRule, PlaceRule, CapRule, ShortRule

ASSUME S >= 1 /\ Buckets >= S + 1 /\ MaxSeg >= 1 /\ MaxBytes >= 1
ASSUME 0 \in Sizes /\ \A d \in Dests : Hash(d) \in 0..(Buckets - 1)
ASSUME ProbeRule \in {"mod", "nomod"} /\ MatchRule \in {"dest", "any"}
ASSUME PlaceRule \in {"forward", "reverse"} /\ CapRule \in {"bytes", "none"}
ASSUME ShortRule \in {"close", "open"}

Slots == 0..(S - 1)
NoRun == [count |-> 0]

VARIABLES dest, size,       \* the bank: each slot's destination and size
          bucket,           \* the table: a group or -1
          head,             \* each group's first slot
          group,            \* each slot's group, -1 for an aborted slot
          start,            \* each group's count, then start, then end
          ng,               \* groups so far
          pos,              \* the placed output: a slot per position, or -1
          pc, i, k,         \* the pass, its slot or group, the probe's bucket
          acc,              \* the prefix's running sum
          j,                \* the position emit is at in a group, or -1
          order, runs,      \* the output
          run,              \* the run being built: first, count, seg, total, last, closed
          oob               \* a probe read outside the table

vars == <<dest, size, bucket, head, group, start, ng, pos, pc, i, k, acc, j, order, runs, run, oob>>

Init ==
    /\ dest \in [Slots -> Dests]
    /\ size \in [Slots -> Sizes]
    /\ bucket = [b \in 0..(Buckets - 1) |-> -1]
    /\ head = [g \in Slots |-> -1]
    /\ group = [s \in Slots |-> -1]
    /\ start = [g \in Slots |-> 0]
    /\ ng = 0
    /\ pos = [p \in Slots |-> -1]
    /\ pc = "scan" /\ i = 0 /\ k = 0 /\ acc = 0 /\ j = -1
    /\ order = <<>> /\ runs = <<>> /\ run = NoRun
    /\ oob = FALSE

Fixed == UNCHANGED <<dest, size>>

\* scan: skip an aborted slot, start probing for a staged one
Scan ==
    /\ pc = "scan"
    /\ IF i = S
         THEN /\ pc' = "prefix" /\ i' = 0
              /\ UNCHANGED <<k>>
         ELSE IF size[i] = 0
                THEN /\ i' = i + 1 /\ UNCHANGED <<pc, k>>
                ELSE /\ pc' = "probe" /\ k' = Hash(dest[i]) /\ UNCHANGED <<i>>
    /\ Fixed
    /\ UNCHANGED <<bucket, head, group, start, ng, pos, acc, j, order, runs, run, oob>>

\* probe bucket k for slot i's destination
Probe ==
    /\ pc = "probe"
    /\ IF k \notin DOMAIN bucket
         THEN /\ oob' = TRUE /\ pc' = "done"
              /\ UNCHANGED <<bucket, head, group, start, ng, i, k>>
       ELSE IF bucket[k] = -1
         THEN \* a new destination: a new group, slot i its first
              /\ bucket' = [bucket EXCEPT ![k] = ng]
              /\ head' = [head EXCEPT ![ng] = i]
              /\ group' = [group EXCEPT ![i] = ng]
              /\ start' = [start EXCEPT ![ng] = 1]
              /\ ng' = ng + 1
              /\ i' = i + 1 /\ pc' = "scan"
              /\ UNCHANGED <<k, oob>>
       ELSE IF MatchRule = "any" \/ dest[head[bucket[k]]] = dest[i]
         THEN \* slot i joins the bucket's group
              /\ group' = [group EXCEPT ![i] = bucket[k]]
              /\ start' = [start EXCEPT ![bucket[k]] = @ + 1]
              /\ i' = i + 1 /\ pc' = "scan"
              /\ UNCHANGED <<bucket, head, ng, k, oob>>
         ELSE \* another destination's bucket: probe on
              /\ k' = IF ProbeRule = "mod" THEN (k + 1) % Buckets ELSE k + 1
              /\ UNCHANGED <<bucket, head, group, start, ng, i, pc, oob>>
    /\ Fixed
    /\ UNCHANGED <<pos, acc, j, order, runs, run>>

\* prefix: group i's count becomes its start
Prefix ==
    /\ pc = "prefix"
    /\ IF i = ng
         THEN /\ pc' = "place"
              /\ i' = IF PlaceRule = "forward" THEN 0 ELSE S - 1
              /\ UNCHANGED <<start, acc>>
         ELSE /\ start' = [start EXCEPT ![i] = acc]
              /\ acc' = acc + start[i]
              /\ i' = i + 1 /\ UNCHANGED pc
    /\ Fixed
    /\ UNCHANGED <<bucket, head, group, ng, pos, k, j, order, runs, run, oob>>

\* place: slot i goes to its group's next position
Place ==
    /\ pc = "place"
    /\ IF i < 0 \/ i = S
         THEN /\ pc' = "emit" /\ i' = 0
              /\ UNCHANGED <<pos, start>>
         ELSE /\ IF group[i] = -1
                   THEN UNCHANGED <<pos, start>>
                   ELSE /\ pos' = [pos EXCEPT ![start[group[i]]] = i]
                        /\ start' = [start EXCEPT ![group[i]] = @ + 1]
              /\ i' = IF PlaceRule = "forward" THEN i + 1 ELSE i - 1
              /\ UNCHANGED pc
    /\ Fixed
    /\ UNCHANGED <<bucket, head, group, ng, k, acc, j, order, runs, run, oob>>

\* the first position of group g: the end of the group before it
From(g) == IF g = 0 THEN 0 ELSE start[g - 1]

\* emit: start group i's range with a fresh run; past the last group, done
Emit ==
    /\ pc = "emit"
    /\ IF i = ng
         THEN /\ pc' = "done" /\ UNCHANGED <<i, j>>
         ELSE /\ pc' = "walk" /\ j' = From(i) /\ UNCHANGED i
    /\ Fixed
    /\ UNCHANGED <<bucket, head, group, start, ng, pos, k, acc, order, runs, run, oob>>

Fits(b) ==
    /\ run.count > 0
    /\ ~run.closed
    /\ run.count < MaxSeg
    /\ (CapRule = "none" \/ run.total + b <= MaxBytes)
    /\ b <= run.seg

Close(r, rs) == IF r.count > 0 THEN Append(rs, [first |-> r.first, count |-> r.count,
                                                 seg |-> r.seg, last |-> r.last]) ELSE rs

\* walk: the slot at position j goes to order and into the run, or ends the
\* run and starts one
Walk ==
    /\ pc = "walk"
    /\ LET c == pos[j]
           b == size[c]
           at == Len(order) + 1
           r2 == IF Fits(b)
                   THEN [run EXCEPT !.count = @ + 1, !.total = @ + b, !.last = b,
                                    !.closed = (ShortRule = "close" /\ b < run.seg)]
                   ELSE [first |-> at, count |-> 1, seg |-> b, total |-> b, last |-> b, closed |-> FALSE]
           rs2 == IF Fits(b) THEN runs ELSE Close(run, runs)
       IN /\ order' = Append(order, c)
          /\ IF j + 1 = start[i]
               THEN \* the group ends: so does its run
                    /\ runs' = Close(r2, rs2) /\ run' = NoRun
                    /\ j' = -1 /\ i' = i + 1 /\ pc' = "emit"
               ELSE /\ runs' = rs2 /\ run' = r2
                    /\ j' = j + 1 /\ UNCHANGED <<i, pc>>
    /\ Fixed
    /\ UNCHANGED <<bucket, head, group, start, ng, pos, k, acc, oob>>

Done ==
    /\ pc = "done"
    /\ UNCHANGED vars

Next == Scan \/ Probe \/ Prefix \/ Place \/ Emit \/ Walk \/ Done

Spec == Init /\ [][Next]_vars

LiveSpec == Spec /\ WF_vars(Next)

-----------------------------------------------------------------------------

Staged == {s \in Slots : size[s] > 0}
Pos == 1..Len(order)
Members(r) == [x \in 1..r.count |-> order[r.first + x - 1]]
FirstSlot(d) == CHOOSE s \in Staged : dest[s] = d /\ \A t \in Staged : dest[t] = d => s <= t

AtEnd(P) == pc = "done" /\ ~oob => P

InBounds == ~oob

Permutation == AtEnd(
    /\ Len(order) = Cardinality(Staged)
    /\ {order[x] : x \in Pos} = Staged)

Grouped == AtEnd(
    /\ \A a, b, e \in Pos : a < b /\ b < e /\ dest[order[a]] = dest[order[e]] => dest[order[b]] = dest[order[a]]
    /\ \A a, b \in Pos : a < b /\ dest[order[a]] = dest[order[b]] => order[a] < order[b]
    /\ \A a, b \in Pos : a < b /\ dest[order[a]] # dest[order[b]] => FirstSlot(dest[order[a]]) < FirstSlot(dest[order[b]]))

RunsTile == AtEnd(
    /\ \A x \in 1..Len(runs) : runs[x].count >= 1
    /\ (Len(runs) = 0 => Len(order) = 0)
    /\ (Len(runs) > 0 => runs[1].first = 1 /\ runs[Len(runs)].first + runs[Len(runs)].count - 1 = Len(order))
    /\ \A x \in 1..(Len(runs) - 1) : runs[x + 1].first = runs[x].first + runs[x].count)

RunOneDest == AtEnd(
    \A x \in 1..Len(runs) : \A a, b \in 1..runs[x].count :
        runs[x].first + runs[x].count - 1 <= Len(order) =>
            dest[Members(runs[x])[a]] = dest[Members(runs[x])[b]])

RunBytes(r) == LET m == Members(r) IN
    LET Sum[x \in 0..r.count] == IF x = 0 THEN 0 ELSE Sum[x - 1] + size[m[x]] IN Sum[r.count]

RunCaps == AtEnd(
    \A x \in 1..Len(runs) :
        /\ runs[x].count <= MaxSeg
        /\ (runs[x].count = 1 \/ RunBytes(runs[x]) <= MaxBytes))

RunShape == AtEnd(
    \A x \in 1..Len(runs) : LET m == Members(runs[x]) IN
        /\ runs[x].seg = size[m[1]]
        /\ runs[x].last = size[m[runs[x].count]]
        /\ \A a \in 1..(runs[x].count - 1) : size[m[a]] = runs[x].seg
        /\ size[m[runs[x].count]] <= runs[x].seg)

\* the gather always ends
Terminates == <>(pc = "done")

=============================================================================
