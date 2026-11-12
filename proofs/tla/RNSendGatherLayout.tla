------------------------- MODULE RNSendGatherLayout -------------------------
(***************************************************************************)
(* The transport-neutral send gather (X2, sdk/rocketnet_send_gather.cpp)   *)
(* at the code's layout.  A send worker hands it one bank of S staged      *)
(* slots, each with a destination and a size (0: aborted).  The gather     *)
(* makes two passes over the slots:                                        *)
(*                                                                         *)
(*   scan  slot by slot, a destination's first slot (its head) takes a     *)
(*         bucket of an open-addressed table of Buckets entries, found by  *)
(*         linear probing from Hash(dest) with the index taken modulo      *)
(*         Buckets; a later slot to the same destination is appended to    *)
(*         its head's chain (next[tail[head]] = slot, tail[head] = slot).  *)
(*         An aborted slot is skipped.                                     *)
(*   emit  slot by slot again, each head's chain is written to order and   *)
(*         cut into runs: a run takes the next packet of its chain while   *)
(*         it holds fewer than MaxSeg, the total stays within MaxBytes and *)
(*         the packet is no larger than the run's first; a smaller packet  *)
(*         ends the run.  A packet larger than MaxBytes is a run of its    *)
(*         own.                                                            *)
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
(*   AppendRule  "tail" appends at the chain's tail (the code); "head"     *)
(*               pushes after the head, reversing a destination's later    *)
(*               slots (Grouped).                                          *)
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
          ProbeRule, MatchRule, AppendRule, CapRule, ShortRule

ASSUME S >= 1 /\ Buckets >= S + 1 /\ MaxSeg >= 1 /\ MaxBytes >= 1
ASSUME 0 \in Sizes /\ \A d \in Dests : Hash(d) \in 0..(Buckets - 1)
ASSUME ProbeRule \in {"mod", "nomod"} /\ MatchRule \in {"dest", "any"}
ASSUME AppendRule \in {"tail", "head"} /\ CapRule \in {"bytes", "none"}
ASSUME ShortRule \in {"close", "open"}

Slots == 0..(S - 1)
NoRun == [count |-> 0]

VARIABLES dest, size,       \* the bank: each slot's destination and size
          bucket,           \* the table: a head slot or -1
          next, tail,       \* chains: the next slot, and a head's last slot (-1: not a head)
          pc, i, k,         \* the pass, its slot, and the probe's bucket
          c,                \* the slot emit is at in a chain, or -1
          order, runs,      \* the output
          run,              \* the run being built: first, count, seg, total, last, closed
          oob               \* a probe read outside the table

vars == <<dest, size, bucket, next, tail, pc, i, k, c, order, runs, run, oob>>

Init ==
    /\ dest \in [Slots -> Dests]
    /\ size \in [Slots -> Sizes]
    /\ bucket = [b \in 0..(Buckets - 1) |-> -1]
    /\ next = [s \in Slots |-> -1]
    /\ tail = [s \in Slots |-> -1]
    /\ pc = "scan" /\ i = 0 /\ k = 0 /\ c = -1
    /\ order = <<>> /\ runs = <<>> /\ run = NoRun
    /\ oob = FALSE

Fixed == UNCHANGED <<dest, size>>

\* scan: skip an aborted slot, start probing for a staged one
Scan ==
    /\ pc = "scan"
    /\ IF i = S
         THEN /\ pc' = "emit" /\ i' = 0
              /\ UNCHANGED <<k>>
         ELSE IF size[i] = 0
                THEN /\ i' = i + 1 /\ UNCHANGED <<pc, k>>
                ELSE /\ pc' = "probe" /\ k' = Hash(dest[i]) /\ UNCHANGED <<i>>
    /\ Fixed
    /\ UNCHANGED <<bucket, next, tail, c, order, runs, run, oob>>

Owner(b) == IF b \in DOMAIN bucket THEN bucket[b] ELSE -1

\* probe bucket k for slot i's destination
Probe ==
    /\ pc = "probe"
    /\ LET h == Owner(k)
       IN IF k \notin DOMAIN bucket
            THEN /\ oob' = TRUE /\ pc' = "done"
                 /\ UNCHANGED <<bucket, next, tail, i, k>>
          ELSE IF h = -1
            THEN \* a new destination: slot i is its head
                 /\ bucket' = [bucket EXCEPT ![k] = i]
                 /\ tail' = [tail EXCEPT ![i] = i]
                 /\ i' = i + 1 /\ pc' = "scan"
                 /\ UNCHANGED <<next, k, oob>>
          ELSE IF MatchRule = "any" \/ dest[h] = dest[i]
            THEN \* slot i joins head h's chain
                 /\ IF AppendRule = "tail"
                      THEN /\ next' = [next EXCEPT ![tail[h]] = i]
                           /\ tail' = [tail EXCEPT ![h] = i]
                      ELSE /\ next' = [next EXCEPT ![h] = i, ![i] = next[h]]
                           /\ tail' = tail
                 /\ i' = i + 1 /\ pc' = "scan"
                 /\ UNCHANGED <<bucket, k, oob>>
            ELSE \* another destination's bucket: probe on
                 /\ k' = IF ProbeRule = "mod" THEN (k + 1) % Buckets ELSE k + 1
                 /\ UNCHANGED <<bucket, next, tail, i, pc, oob>>
    /\ Fixed
    /\ UNCHANGED <<c, order, runs, run>>

Close(r, rs) == IF r.count > 0 THEN Append(rs, [first |-> r.first, count |-> r.count,
                                                 seg |-> r.seg, last |-> r.last]) ELSE rs

\* emit: at a head, start its chain with a fresh run; past the last slot, done
Emit ==
    /\ pc = "emit"
    /\ IF i = S
         THEN /\ pc' = "done" /\ UNCHANGED <<i, c>>
         ELSE IF tail[i] # -1
                THEN /\ pc' = "walk" /\ c' = i /\ UNCHANGED i
                ELSE /\ i' = i + 1 /\ UNCHANGED <<pc, c>>
    /\ Fixed
    /\ UNCHANGED <<bucket, next, tail, k, order, runs, run, oob>>

Fits(b) ==
    /\ run.count > 0
    /\ ~run.closed
    /\ run.count < MaxSeg
    /\ (CapRule = "none" \/ run.total + b <= MaxBytes)
    /\ b <= run.seg

\* walk: slot c goes to order and into the run, or ends the run and starts one
Walk ==
    /\ pc = "walk"
    /\ LET b == size[c]
           at == Len(order) + 1
           r2 == IF Fits(b)
                   THEN [run EXCEPT !.count = @ + 1, !.total = @ + b, !.last = b,
                                    !.closed = (ShortRule = "close" /\ b < run.seg)]
                   ELSE [first |-> at, count |-> 1, seg |-> b, total |-> b, last |-> b, closed |-> FALSE]
           rs2 == IF Fits(b) THEN runs ELSE Close(run, runs)
       IN /\ order' = Append(order, c)
          /\ IF next[c] = -1
               THEN \* the chain ends: so does its run
                    /\ runs' = Close(r2, rs2) /\ run' = NoRun
                    /\ c' = -1 /\ i' = i + 1 /\ pc' = "emit"
               ELSE /\ runs' = rs2 /\ run' = r2
                    /\ c' = next[c] /\ UNCHANGED <<i, pc>>
    /\ Fixed
    /\ UNCHANGED <<bucket, next, tail, k, oob>>

Done ==
    /\ pc = "done"
    /\ UNCHANGED vars

Next == Scan \/ Probe \/ Emit \/ Walk \/ Done

Spec == Init /\ [][Next]_vars

LiveSpec == Spec /\ WF_vars(Next)

-----------------------------------------------------------------------------

Staged == {s \in Slots : size[s] > 0}
Pos == 1..Len(order)
Members(r) == [j \in 1..r.count |-> order[r.first + j - 1]]
FirstSlot(d) == CHOOSE s \in Staged : dest[s] = d /\ \A t \in Staged : dest[t] = d => s <= t

AtEnd(P) == pc = "done" /\ ~oob => P

InBounds == ~oob

Permutation == AtEnd(
    /\ Len(order) = Cardinality(Staged)
    /\ {order[j] : j \in Pos} = Staged)

Grouped == AtEnd(
    /\ \A a, b, e \in Pos : a < b /\ b < e /\ dest[order[a]] = dest[order[e]] => dest[order[b]] = dest[order[a]]
    /\ \A a, b \in Pos : a < b /\ dest[order[a]] = dest[order[b]] => order[a] < order[b]
    /\ \A a, b \in Pos : a < b /\ dest[order[a]] # dest[order[b]] => FirstSlot(dest[order[a]]) < FirstSlot(dest[order[b]]))

RunsTile == AtEnd(
    /\ \A j \in 1..Len(runs) : runs[j].count >= 1
    /\ (Len(runs) = 0 => Len(order) = 0)
    /\ (Len(runs) > 0 => runs[1].first = 1 /\ runs[Len(runs)].first + runs[Len(runs)].count - 1 = Len(order))
    /\ \A j \in 1..(Len(runs) - 1) : runs[j + 1].first = runs[j].first + runs[j].count)

RunOneDest == AtEnd(
    \A j \in 1..Len(runs) : \A a, b \in 1..runs[j].count :
        runs[j].first + runs[j].count - 1 <= Len(order) =>
            dest[Members(runs[j])[a]] = dest[Members(runs[j])[b]])

RunBytes(r) == LET m == Members(r) IN
    LET Sum[j \in 0..r.count] == IF j = 0 THEN 0 ELSE Sum[j - 1] + size[m[j]] IN Sum[r.count]

RunCaps == AtEnd(
    \A j \in 1..Len(runs) :
        /\ runs[j].count <= MaxSeg
        /\ (runs[j].count = 1 \/ RunBytes(runs[j]) <= MaxBytes))

RunShape == AtEnd(
    \A j \in 1..Len(runs) : LET m == Members(runs[j]) IN
        /\ runs[j].seg = size[m[1]]
        /\ runs[j].last = size[m[runs[j].count]]
        /\ \A a \in 1..(runs[j].count - 1) : size[m[a]] = runs[j].seg
        /\ size[m[runs[j].count]] <= runs[j].seg)

\* the gather always ends
Terminates == <>(pc = "done")

=============================================================================
