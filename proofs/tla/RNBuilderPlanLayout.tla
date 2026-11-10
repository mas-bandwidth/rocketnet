------------------------ MODULE RNBuilderPlanLayout ------------------------
(***************************************************************************)
(* The packet builder's flat plan at its layout                            *)
(* (sdk/rocketnet_packet_builder.cpp, rn_builder_plan, rn_builder_radix_   *)
(* pass, rn_builder_next_in_class, rn_builder_take_head and the refused    *)
(* packet's undo in rn_packet_builder_write).  RNScheduler checks the      *)
(* order items are first written as sequences; this model checks the      *)
(* hand-indexed arrays that carry it, which RNScheduler cannot see.        *)
(*                                                                         *)
(* The writable item indices are copied into sort_indices, then sorted by  *)
(* three stable radix passes (the channel's low byte, its high byte, the   *)
(* class), each counting keys into a bucket table, turning the counts      *)
(* into exclusive prefix offsets and scattering each index to its          *)
(* bucket's next slot, between sort_indices and alternate.  A walk over    *)
(* alternate cuts it into contiguous groups of one class and channel,      *)
(* each with begin, cursor and end, and fills sequence[] with the item and *)
(* its group; a reverse walk over each group stores its suffix priority    *)
(* minimum.  A class's groups are the contiguous range class_group_begin   *)
(* .. class_group_end.  A take reads sequence[cursor] of a group of the    *)
(* lowest live class and advances the cursor; a packet's taken positions   *)
(* are kept, and a refused packet walks them backwards, stepping each      *)
(* group's cursor back and clearing the item's taken flag.                 *)
(*                                                                         *)
(* The radix uses Radix-wide digits (the code's 256); with Radix = 2 and   *)
(* channels 1 and 2 the two channel passes disagree, so the high pass has  *)
(* to reorder what the low pass produced.  The scheduler's choice of       *)
(* group inside a class (clocks, round robin) is left free: any live head  *)
(* of the lowest live class may be taken.                                  *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   NoOverlap        every scatter writes a distinct slot inside          *)
(*                    0 .. count - 1.                                      *)
(*   PlanSorted       after the third pass alternate holds every writable  *)
(*                    item once, ordered by class, channel, caller index.  *)
(*   GroupsExact      the groups tile 0 .. count - 1, every slot names its *)
(*                    group and item, and no two groups share a key.       *)
(*   ClassRanges      a class's group range holds exactly its groups.      *)
(*   SuffixMinima     sequence[s].suffix_level is the least priority from  *)
(*                    s to its group's end.                                *)
(*   CursorMatchesTaken  a group's taken items are exactly begin .. cursor.*)
(*   ChannelOrderKept each channel's items written, in flight and still    *)
(*                    planned are its items in caller order, none lost and *)
(*                    none twice.                                          *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   Restore       the refused packet's cursor step back; FALSE skips it.  *)
(*   PrefixSum     "exclusive" is the code; "inclusive" stores offset + n. *)
(*   ScatterOrder  "forward" is the code; "reverse" scatters from the end, *)
(*                 which is no longer stable.                              *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS N,           \* writable items in the call (count)
          Radix,       \* bucket table size: the code's 256
          Channels,    \* channel numbers the items may carry
          Classes,     \* send classes 0 .. Classes - 1
          Levels,      \* ROCKETNET_PACKET_BUILDER_LEVELS
          MaxRecords,  \* ROCKETNET_PACKET_BUILDER_MAX_RECORDS
          Restore, PrefixSum, ScatterOrder

ASSUME N >= 1 /\ Radix >= 2 /\ Classes >= 1 /\ Classes <= Radix
ASSUME Levels >= 1 /\ MaxRecords >= 1
ASSUME Channels # {} /\ Channels \subseteq 0..(Radix * Radix - 1)
ASSUME Restore \in BOOLEAN
ASSUME PrefixSum \in {"exclusive", "inclusive"}
ASSUME ScatterOrder \in {"forward", "reverse"}

Items == 0..N - 1
Slots == 0..N - 1
Buckets == 0..Radix - 1
ClassIds == 0..Classes - 1
LevelIds == 0..Levels - 1

VARIABLES cls, chn, pri,   \* items[x].send_class, .channel, .priority
          taken,           \* items[x].taken
          pc,
          i,               \* the filter, count and scatter loops' index
          count,
          sortIdx,         \* plan->sort_indices
          alt,             \* alternate
          pass,
          bkt,             \* buckets[]
          b, offset,       \* the prefix loop's b and offset
          wrote,           \* ghost: the slots this pass's scatter wrote
          gcount,          \* plan->group_count
          gbegin, gend, gcursor, gchan, gcls,   \* plan->groups[g]
          seqItem, seqGroup, seqSuffix,         \* plan->sequence[s]
          position,        \* the group walk's position
          p, suffix,       \* the suffix walk's p and suffix
          c, nextGroup,    \* the class range loop's c and next_group
          cgBegin, cgEnd,  \* plan->class_group_begin, class_group_end
          packet,          \* taken_positions[0 .. records - 1]
          r,               \* the undo loop's r
          sent             \* ghost: the items of written packets, in order

itemv  == <<cls, chn, pri>>
sortv  == <<i, count, sortIdx, alt, pass, bkt, b, offset, wrote>>
groupv == <<gcount, gbegin, gend, gchan, gcls, seqItem, seqGroup, seqSuffix,
            position, p, suffix>>
rangev == <<c, nextGroup, cgBegin, cgEnd>>
writev == <<taken, gcursor, packet, r, sent>>
vars   == <<itemv, pc, sortv, groupv, rangev, writev>>

\* rn_builder_key_byte: the low byte, the high byte, the class
Key(x, ps) ==
    IF ps = 0 THEN chn[x] % Radix
    ELSE IF ps = 1 THEN (chn[x] \div Radix) % Radix
    ELSE cls[x]

\* the pass's input array: passes 0 and 2 read sort_indices, pass 1 alternate
Input == IF pass = 1 THEN alt ELSE sortIdx

Init ==
    /\ cls \in [Items -> ClassIds]
    /\ chn \in [Items -> Channels]
    /\ pri \in [Items -> LevelIds]
    /\ taken = [x \in Items |-> FALSE]
    /\ pc = "filter"
    /\ i = 0
    /\ count = 0
    /\ sortIdx = [s \in Slots |-> 0]
    /\ alt = [s \in Slots |-> 0]
    /\ pass = 0
    /\ bkt = [k \in Buckets |-> 0]
    /\ b = 0
    /\ offset = 0
    /\ wrote = <<>>
    /\ gcount = 0
    /\ gbegin = [g \in Slots |-> 0]
    /\ gend = [g \in Slots |-> 0]
    /\ gcursor = [g \in Slots |-> 0]
    /\ gchan = [g \in Slots |-> 0]
    /\ gcls = [g \in Slots |-> 0]
    /\ seqItem = [s \in Slots |-> 0]
    /\ seqGroup = [s \in Slots |-> 0]
    /\ seqSuffix = [s \in Slots |-> 0]
    /\ position = 0
    /\ p = 0
    /\ suffix = 0
    /\ c = 0
    /\ nextGroup = 0
    /\ cgBegin = [k \in ClassIds |-> 0]
    /\ cgEnd = [k \in ClassIds |-> 0]
    /\ packet = <<>>
    /\ r = 0
    /\ sent = <<>>

\* rn_builder_plan's first loop: sort_indices[count++] = i for each writable
\* item (every item here is writable and of a budget class)
Filter ==
    /\ pc = "filter"
    /\ IF i < N
         THEN /\ sortIdx' = [sortIdx EXCEPT ![count] = i]
              /\ count' = count + 1
              /\ i' = i + 1
              /\ UNCHANGED pc
         ELSE /\ pc' = "count"
              /\ i' = 0
              /\ UNCHANGED <<sortIdx, count>>
    /\ UNCHANGED <<itemv, alt, pass, bkt, b, offset, wrote, groupv, rangev, writev>>

\* rn_builder_radix_pass's count loop: buckets[key]++
Count ==
    /\ pc = "count"
    /\ IF i < count
         THEN /\ bkt' = [bkt EXCEPT ![Key(Input[i], pass)] = @ + 1]
              /\ i' = i + 1
              /\ UNCHANGED <<pc, b, offset>>
         ELSE /\ pc' = "prefix"
              /\ b' = 0
              /\ offset' = 0
              /\ UNCHANGED <<bkt, i>>
    /\ UNCHANGED <<itemv, count, sortIdx, alt, pass, wrote, groupv, rangev, writev>>

\* the offset loop: n = buckets[b]; buckets[b] = offset; offset += n
Prefix ==
    /\ pc = "prefix"
    /\ IF b < Radix
         THEN LET n == bkt[b] IN
              /\ bkt' = [bkt EXCEPT ![b] = IF PrefixSum = "exclusive" THEN offset ELSE offset + n]
              /\ offset' = offset + n
              /\ b' = b + 1
              /\ UNCHANGED <<pc, i, wrote>>
         ELSE /\ pc' = "scatter"
              /\ i' = 0
              /\ wrote' = <<>>
              /\ UNCHANGED <<bkt, b, offset>>
    /\ UNCHANGED <<itemv, count, sortIdx, alt, pass, groupv, rangev, writev>>

\* the scatter loop: output[buckets[key]++] = input[i].  A slot outside the
\* array is not written (the model's stand-in for the overrun).  After the
\* third pass the plan walks alternate.
Scatter ==
    /\ pc = "scatter"
    /\ IF i < count
         THEN LET x  == IF ScatterOrder = "forward" THEN Input[i] ELSE Input[count - 1 - i]
                  k  == Key(x, pass)
                  at == bkt[k]
              IN /\ IF pass = 1
                      THEN /\ sortIdx' = IF at \in Slots THEN [sortIdx EXCEPT ![at] = x] ELSE sortIdx
                           /\ UNCHANGED alt
                      ELSE /\ alt' = IF at \in Slots THEN [alt EXCEPT ![at] = x] ELSE alt
                           /\ UNCHANGED sortIdx
                 /\ bkt' = [bkt EXCEPT ![k] = @ + 1]
                 /\ wrote' = Append(wrote, at)
                 /\ i' = i + 1
                 /\ UNCHANGED <<pc, pass>>
         ELSE /\ IF pass < 2
                   THEN /\ pass' = pass + 1
                        /\ pc' = "count"
                        /\ bkt' = [k \in Buckets |-> 0]
                   ELSE /\ pc' = "group"
                        /\ UNCHANGED <<pass, bkt>>
              /\ i' = 0
              /\ UNCHANGED <<sortIdx, alt, wrote>>
    /\ UNCHANGED <<itemv, count, b, offset, groupv, rangev, writev>>

\* the group walk's outer loop: a new group at position (begin = cursor =
\* position, its channel and class from alternate[position]), or the walk's end
Group ==
    /\ pc = "group"
    /\ IF position < count
         THEN LET x == alt[position] IN
              /\ gbegin' = [gbegin EXCEPT ![gcount] = position]
              /\ gcursor' = [gcursor EXCEPT ![gcount] = position]
              /\ gchan' = [gchan EXCEPT ![gcount] = chn[x]]
              /\ gcls' = [gcls EXCEPT ![gcount] = cls[x]]
              /\ gcount' = gcount + 1
              /\ pc' = "member"
              /\ UNCHANGED <<c, nextGroup>>
         ELSE /\ pc' = "ranges"
              /\ c' = 0
              /\ nextGroup' = 0
              /\ UNCHANGED <<gbegin, gcursor, gchan, gcls, gcount>>
    /\ UNCHANGED <<itemv, sortv, gend, seqItem, seqGroup, seqSuffix, position,
                   p, suffix, cgBegin, cgEnd, taken, packet, r, sent>>

\* the do-while body: sequence[position] = { alternate[position], group },
\* position++, while the next item has the group's class and channel; at the
\* group's end, end = position and the suffix walk starts
Member ==
    /\ pc = "member"
    /\ LET g    == gcount - 1
           more == /\ position + 1 < count
                   /\ cls[alt[position + 1]] = gcls[g]
                   /\ chn[alt[position + 1]] = gchan[g]
       IN /\ seqItem' = [seqItem EXCEPT ![position] = alt[position]]
          /\ seqGroup' = [seqGroup EXCEPT ![position] = g]
          /\ position' = position + 1
          /\ IF more
               THEN /\ UNCHANGED <<pc, gend, p, suffix>>
               ELSE /\ gend' = [gend EXCEPT ![g] = position + 1]
                    /\ p' = position + 1
                    /\ suffix' = Levels
                    /\ pc' = "suffix"
    /\ UNCHANGED <<itemv, sortv, gcount, gbegin, gchan, gcls, seqSuffix,
                   rangev, writev>>

\* for ( p = end; p > begin; p-- ): suffix = min( suffix, priority of
\* sequence[p - 1] ); sequence[p - 1].suffix_level = suffix
Suffix ==
    /\ pc = "suffix"
    /\ IF p > gbegin[gcount - 1]
         THEN LET q == pri[seqItem[p - 1]]
                  s == IF q < suffix THEN q ELSE suffix
              IN /\ suffix' = s
                 /\ seqSuffix' = [seqSuffix EXCEPT ![p - 1] = s]
                 /\ p' = p - 1
                 /\ UNCHANGED pc
         ELSE /\ pc' = "group"
              /\ UNCHANGED <<suffix, seqSuffix, p>>
    /\ UNCHANGED <<itemv, sortv, gcount, gbegin, gend, gchan, gcls, seqItem,
                   seqGroup, position, rangev, writev>>

\* the class range loop: class_group_begin[c] = next_group; while the next
\* group is of class c, next_group++; class_group_end[c] = next_group
Range ==
    \/ /\ pc = "ranges"
       /\ IF c < Classes
            THEN /\ cgBegin' = [cgBegin EXCEPT ![c] = nextGroup]
                 /\ pc' = "rangeloop"
            ELSE /\ pc' = "fill"
                 /\ UNCHANGED cgBegin
       /\ UNCHANGED <<itemv, sortv, groupv, c, nextGroup, cgEnd, writev>>
    \/ /\ pc = "rangeloop"
       /\ IF nextGroup < gcount /\ gcls[nextGroup] = c
            THEN /\ nextGroup' = nextGroup + 1
                 /\ UNCHANGED <<pc, c, cgEnd>>
            ELSE /\ cgEnd' = [cgEnd EXCEPT ![c] = nextGroup]
                 /\ c' = c + 1
                 /\ pc' = "ranges"
                 /\ UNCHANGED nextGroup
       /\ UNCHANGED <<itemv, sortv, groupv, cgBegin, writev>>

\* rn_builder_next_in_class's offer: a group with cursor < end whose
\* sequence[cursor] names it
HeadAt(g) == gcursor[g] < gend[g] /\ seqGroup[gcursor[g]] = g

Live(k) == \E g \in cgBegin[k]..(cgEnd[k] - 1) : HeadAt(g)

\* a head of the lowest live class taken: taken_positions[records++] =
\* rn_builder_take_head (cursor++), items[i].taken = 1
Take ==
    /\ pc = "fill"
    /\ Len(packet) < MaxRecords
    /\ \E k \in ClassIds :
         /\ Live(k)
         /\ \A k2 \in 0..(k - 1) : ~Live(k2)
         /\ \E g \in cgBegin[k]..(cgEnd[k] - 1) :
              /\ HeadAt(g)
              /\ packet' = Append(packet, gcursor[g])
              /\ gcursor' = [gcursor EXCEPT ![g] = @ + 1]
              /\ taken' = [taken EXCEPT ![seqItem[gcursor[g]]] = TRUE]
    /\ UNCHANGED <<itemv, pc, sortv, groupv, rangev, r, sent>>

\* the packet is admitted and written: its items stay taken
Commit ==
    /\ pc = "fill"
    /\ packet # <<>>
    /\ sent' = sent \o [j \in 1..Len(packet) |-> seqItem[packet[j]]]
    /\ packet' = <<>>
    /\ UNCHANGED <<itemv, pc, sortv, groupv, rangev, taken, gcursor, r>>

\* an empty packet, or the payloads spent: the call ends
Stop ==
    /\ pc = "fill"
    /\ packet = <<>>
    /\ pc' = "done"
    /\ UNCHANGED <<itemv, sortv, groupv, rangev, writev>>

\* the budget refuses the packet: the undo walk starts at r = records
Refuse ==
    /\ pc = "fill"
    /\ packet # <<>>
    /\ pc' = "undo"
    /\ r' = Len(packet)
    /\ UNCHANGED <<itemv, sortv, groupv, rangev, taken, gcursor, packet, sent>>

\* for ( r = records; r > 0; r-- ): the group of sequence[taken_positions[r -
\* 1]] steps its cursor back and the item is no longer taken; then break
Undo ==
    /\ pc = "undo"
    /\ IF r > 0
         THEN LET e == packet[r]
                  g == seqGroup[e]
              IN /\ gcursor' = IF Restore THEN [gcursor EXCEPT ![g] = @ - 1] ELSE gcursor
                 /\ taken' = [taken EXCEPT ![seqItem[e]] = FALSE]
                 /\ r' = r - 1
                 /\ UNCHANGED <<pc, packet>>
         ELSE /\ packet' = <<>>
              /\ pc' = "done"
              /\ UNCHANGED <<gcursor, taken, r>>
    /\ UNCHANGED <<itemv, sortv, groupv, rangev, sent>>

Next ==
    \/ Filter \/ Count \/ Prefix \/ Scatter
    \/ Group \/ Member \/ Suffix \/ Range
    \/ Take \/ Commit \/ Stop \/ Refuse \/ Undo

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

Sorted   == pc \notin {"filter", "count", "prefix", "scatter"}
Planned  == pc \in {"fill", "undo", "done"}
GroupIds == 0..(gcount - 1)

Min(S) == CHOOSE m \in S : \A n \in S : m <= n

\* class, then channel, then the caller's index
Before(x, y) ==
    \/ cls[x] < cls[y]
    \/ cls[x] = cls[y] /\ chn[x] < chn[y]
    \/ cls[x] = cls[y] /\ chn[x] = chn[y] /\ x < y

\* a class and channel's items in the caller's order
CallerOrder(k, h) == SelectSeq([j \in 1..N |-> j - 1], LAMBDA x : cls[x] = k /\ chn[x] = h)

TypeOK ==
    /\ cls \in [Items -> ClassIds]
    /\ chn \in [Items -> Channels]
    /\ pri \in [Items -> LevelIds]
    /\ taken \in [Items -> BOOLEAN]
    /\ count \in 0..N
    /\ sortIdx \in [Slots -> Items]
    /\ alt \in [Slots -> Items]
    /\ pass \in 0..2
    /\ bkt \in [Buckets -> Nat]
    /\ gcount \in 0..N
    /\ seqSuffix \in [Slots -> 0..Levels]
    /\ packet \in Seq(Slots)
    /\ Len(packet) <= MaxRecords
    /\ r \in 0..MaxRecords

NoOverlap ==
    \A j \in 1..Len(wrote) :
        /\ wrote[j] \in 0..(count - 1)
        /\ \A j2 \in 1..(j - 1) : wrote[j2] # wrote[j]

PlanSorted ==
    Sorted =>
        /\ {alt[s] : s \in 0..(count - 1)} = Items
        /\ \A s \in 0..(count - 2) : Before(alt[s], alt[s + 1])

GroupsExact ==
    Planned =>
        /\ gcount >= 1
        /\ gbegin[0] = 0
        /\ gend[gcount - 1] = count
        /\ \A g \in GroupIds :
             /\ gbegin[g] < gend[g]
             /\ g < gcount - 1 => gend[g] = gbegin[g + 1]
             /\ \A s \in gbegin[g]..(gend[g] - 1) :
                  /\ seqGroup[s] = g
                  /\ seqItem[s] = alt[s]
                  /\ cls[seqItem[s]] = gcls[g]
                  /\ chn[seqItem[s]] = gchan[g]
        /\ \A g, h \in GroupIds : g # h => <<gcls[g], gchan[g]>> # <<gcls[h], gchan[h]>>

ClassRanges ==
    Planned =>
        \A k \in ClassIds, g \in GroupIds :
            (cgBegin[k] <= g /\ g < cgEnd[k]) <=> gcls[g] = k

SuffixMinima ==
    Planned =>
        \A g \in GroupIds, s \in Slots :
            (gbegin[g] <= s /\ s < gend[g]) =>
                seqSuffix[s] = Min({pri[seqItem[q]] : q \in s..(gend[g] - 1)})

CursorMatchesTaken ==
    Planned =>
        \A g \in GroupIds :
            /\ gbegin[g] <= gcursor[g]
            /\ gcursor[g] <= gend[g]
            /\ \A s \in gbegin[g]..(gend[g] - 1) : taken[seqItem[s]] <=> s < gcursor[g]

ChannelOrderKept ==
    Planned =>
        LET flight == IF pc = "undo" THEN SubSeq(packet, 1, r) ELSE packet
            flow   == sent \o [j \in 1..Len(flight) |-> seqItem[flight[j]]]
        IN \A g \in GroupIds :
             LET rest == [j \in 1..(gend[g] - gcursor[g]) |-> seqItem[gcursor[g] + j - 1]]
             IN SelectSeq(flow, LAMBDA x : cls[x] = gcls[g] /\ chn[x] = gchan[g]) \o rest
                    = CallerOrder(gcls[g], gchan[g])

=============================================================================
