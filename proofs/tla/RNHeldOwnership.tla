-------------------------- MODULE RNHeldOwnership --------------------------
(***************************************************************************)
(* Receive-descriptor ownership and the held-ordered index, as the code    *)
(* lays them out (sdk/rocketnet_channel.cpp, the held index and its drain; *)
(* sdk/rocketnet_channel_sequenced.cpp, the code-4 block release).         *)
(*                                                                         *)
(* The receiver's descriptors are W slots indexed number % W.  A slot is   *)
(* owned by what took its number: a held code-3 message (its held key), a  *)
(* code-4 block's fragment (OB) or a message delivered at once (OD,        *)
(* freed when the update ends).  The window takes the next number only     *)
(* into a free slot; a live slot is a SLOT_COLLISION, which ends the       *)
(* connection.                                                             *)
(*                                                                         *)
(* The held index is N = 2W open-addressed cells, linear probing from a    *)
(* key's home, deletion by backward shift that clears the final hole last  *)
(* (RNOpenAddress, ClearLast TRUE).  Each entry binds the number of the    *)
(* descriptor it holds (ent): the slot is ent % W and the identity is the  *)
(* number itself.                                                          *)
(*                                                                         *)
(* One code-4 block at a time collects fragments over its claimed span     *)
(* first .. first + count - 1, chosen by the sender.  Supersede cancels it *)
(* while incomplete and releases the span.  A number in the span owned by *)
(* anything but the block is a sender that lied: a protocol violation.     *)
(*                                                                         *)
(* err is the connection's sticky protocol violation (or exhaustion): the  *)
(* connection ends there, so every action but the erase's own steps needs  *)
(* ~err.  Ending is a terminal state, not a deadlock.                      *)
(*                                                                         *)
(* Invariants: ReleaseOwned (a release frees only the releasing block's    *)
(* descriptors), HeldNamesLive (every held entry names a live descriptor   *)
(* of matching number and owner), HeldBound (at most W entries),           *)
(* EraseEnds (an erase examines at most N cells), NoStaleDelivery (a       *)
(* drain delivers only the descriptor its entry names).                    *)
(*                                                                         *)
(* Reversed witnesses: NumberOnlyRelease TRUE is the code before the fix:  *)
(* the release checks a slot's number and not its owner (ReleaseOwned,     *)
(* HeldNamesLive).  With the other three guards also removed it reaches    *)
(* the stale delivery (NoStaleDelivery) and the full index whose erase     *)
(* circles (EraseEnds).  With NumberOnlyRelease TRUE and the guards        *)
(* (NoOccupancyBound, NoIdentityCheck, UnboundedErase FALSE) the bound,    *)
(* the stale-delivery refusal and the erase's end still hold; with only   *)
(* the bounded erase left, the index fills and the erase still ends.       *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS W, Keys, Homes, MaxNum, MaxCount,
          NumberOnlyRelease, NoOccupancyBound, NoIdentityCheck, UnboundedErase

N == 2 * W
EMPTY == 0
Slots == 0..(W - 1)
Cells == 0..(N - 1)
\* owners are integers, as the keys are: a key (1..), or one of
OB == -1        \* a code-4 block fragment
OD == -2        \* a message delivered at once
OF == -3        \* free
Owners == Keys \cup {OB, OD, OF}

VARIABLES
    home,           \* each key's home cell, fixed for the run (one of Homes)
    desc,           \* slot -> [live, num, owner]
    next,           \* the next number the sender uses
    cell,           \* cell -> key or EMPTY
    ent,            \* cell -> the number its entry binds
    block,          \* [active, first, count]: the one collecting code-4 block
    pc, hole, scan, scanned,
    err,            \* the connection ended (protocol violation, exhaustion)
    foreignFreed,   \* a release freed a descriptor the block did not own
    staleDelivered  \* a drain delivered a descriptor its entry did not name

vars == <<home, desc, next, cell, ent, block, pc, hole, scan, scanned, err, foreignFreed, staleDelivered>>

Next1(x) == (x + 1) % N
Free == [live |-> FALSE, num |-> 0, owner |-> OF]
Used == Cardinality({ c \in Cells : cell[c] # EMPTY })
HeldKeys == { cell[c] : c \in Cells } \ {EMPTY}

InCyclic(h, lo, hi) == IF lo <= hi THEN lo < h /\ h <= hi ELSE lo < h \/ h <= hi

RECURSIVE FirstEmpty(_, _)
FirstEmpty(at, steps) == IF steps = N \/ cell[at] = EMPTY THEN at ELSE FirstEmpty(Next1(at), steps + 1)

RECURSIVE CellOf(_, _, _)
CellOf(k, at, steps) == IF steps = N \/ cell[at] = k THEN at ELSE CellOf(k, Next1(at), steps + 1)

Init ==
    /\ home \in Homes
    /\ desc = [ s \in Slots |-> Free ]
    /\ next = 0
    /\ cell = [ c \in Cells |-> EMPTY ]
    /\ ent = [ c \in Cells |-> 0 ]
    /\ block = [active |-> FALSE, first |-> 0, count |-> 0]
    /\ pc = "idle" /\ hole = 0 /\ scan = 0 /\ scanned = 0
    /\ err = FALSE /\ foreignFreed = FALSE /\ staleDelivered = FALSE

\* the window takes the next number into a free slot (RNWindow Accept)
CanTake == ~err /\ pc = "idle" /\ next <= MaxNum /\ ~desc[next % W].live
Take(owner) == /\ desc' = [ desc EXCEPT ![next % W] = [live |-> TRUE, num |-> next, owner |-> owner] ]
               /\ next' = next + 1

\* the next number's slot is still live: SLOT_COLLISION ends the connection
Collide ==
    /\ ~err /\ pc = "idle" /\ next <= MaxNum /\ desc[next % W].live
    /\ err' = TRUE
    /\ UNCHANGED <<home, desc, next, cell, ent, block, pc, hole, scan, scanned, foreignFreed, staleDelivered>>

\* a code-3 message ahead of its turn: held under key k (rn_held_insert).
\* At W entries the insert is refused as a violation; with no empty cell the
\* region is exhausted.
Hold(k) ==
    /\ CanTake
    /\ k \notin HeldKeys
    /\ IF ~NoOccupancyBound /\ Used >= W
       THEN /\ err' = TRUE
            /\ UNCHANGED <<desc, next, cell, ent>>
       ELSE IF \A c \in Cells : cell[c] # EMPTY
       THEN /\ err' = TRUE
            /\ UNCHANGED <<desc, next, cell, ent>>
       ELSE LET at == FirstEmpty(home[k], 0)
            IN /\ Take(k)
               /\ cell' = [ cell EXCEPT ![at] = k ]
               /\ ent' = [ ent EXCEPT ![at] = next ]
               /\ UNCHANGED err
    /\ UNCHANGED <<home, block, pc, hole, scan, scanned, foreignFreed, staleDelivered>>

\* the first fragment of a code-4 block: fragment i of count, its span
\* claimed from the sender's numbers (rn_sequenced_block)
Fragment(i, count) ==
    /\ CanTake
    /\ ~block.active
    /\ i < count /\ next - i >= 0
    /\ Take(OB)
    /\ block' = [active |-> TRUE, first |-> next - i, count |-> count]
    /\ UNCHANGED <<home, cell, ent, pc, hole, scan, scanned, err, foreignFreed, staleDelivered>>

\* a later fragment of the same block joins it
Join ==
    /\ CanTake
    /\ block.active
    /\ block.first <= next /\ next < block.first + block.count
    /\ Take(OB)
    /\ UNCHANGED <<home, block, cell, ent, pc, hole, scan, scanned, err, foreignFreed, staleDelivered>>

\* a message delivered at once (code 2, or code 4 in turn)
Direct ==
    /\ CanTake
    /\ Take(OD)
    /\ UNCHANGED <<home, block, cell, ent, pc, hole, scan, scanned, err, foreignFreed, staleDelivered>>

\* RNChannel EndUpdate: the update's deliveries expire
EndUpdate ==
    /\ ~err /\ pc = "idle"
    /\ \E s \in Slots : desc[s].live /\ desc[s].owner = OD
    /\ desc' = [ s \in Slots |-> IF desc[s].live /\ desc[s].owner = OD THEN Free ELSE desc[s] ]
    /\ UNCHANGED <<home, next, block, cell, ent, pc, hole, scan, scanned, err, foreignFreed, staleDelivered>>

\* a newer state cancels the incomplete block and releases its span
\* (rn_sequenced_release_slot).  The fix frees only the block's own
\* descriptors and ends the connection on a foreign one; NumberOnlyRelease
\* frees every slot whose number is in the span.
Supersede ==
    /\ ~err /\ pc = "idle"
    /\ block.active
    /\ LET span == block.first..(block.first + block.count - 1)
           hit == { s \in Slots : desc[s].live /\ desc[s].num \in span }
           owned == { s \in hit : desc[s].owner = OB }
           foreign == hit \ owned
           freed == IF NumberOnlyRelease THEN hit ELSE owned
       IN /\ desc' = [ s \in Slots |-> IF s \in freed THEN Free ELSE desc[s] ]
          /\ foreignFreed' = (foreignFreed \/ (freed \cap foreign) # {})
          /\ err' = (~NumberOnlyRelease /\ foreign # {})
    /\ block' = [active |-> FALSE, first |-> 0, count |-> 0]
    /\ UNCHANGED <<home, next, cell, ent, pc, hole, scan, scanned, staleDelivered>>

\* the ordered drain reaches key k (rn_channels_drain): the entry's
\* descriptor must be live, of the entry's number and owned by k.  Without
\* the check the drain delivers whatever occupies the slot, and the update's
\* end frees that occupant.
Deliver(k) ==
    /\ ~err /\ pc = "idle"
    /\ k \in HeldKeys
    /\ LET c == CellOf(k, home[k], 0)
           e == ent[c]
           s == e % W
           ok == desc[s].live /\ desc[s].num = e /\ desc[s].owner = k
       IN IF ~NoIdentityCheck /\ ~ok
          THEN /\ err' = TRUE
               /\ UNCHANGED <<desc, pc, hole, scan, scanned, staleDelivered>>
          ELSE /\ staleDelivered' = (staleDelivered \/ ~ok)
               /\ desc' = [ desc EXCEPT ![s] = Free ]
               /\ pc' = "erase" /\ hole' = c /\ scan' = c /\ scanned' = 0
               /\ UNCHANGED err
    /\ UNCHANGED <<home, next, cell, ent, block, foreignFreed>>

\* one backward-shift step (rn_held_erase); the fix bounds the scan at N
\* cells, UnboundedErase is the code before it
EraseStep ==
    /\ pc = "erase"
    /\ IF UnboundedErase THEN scanned <= N ELSE scanned < N - 1
    /\ LET s == Next1(scan)
           k == cell[s]
       IN IF k = EMPTY
          THEN /\ pc' = "idle"
               /\ scan' = s
               /\ cell' = [ cell EXCEPT ![hole] = EMPTY ]
               /\ ent' = [ ent EXCEPT ![hole] = 0 ]
               /\ UNCHANGED hole
          ELSE IF InCyclic(home[k], hole, s)
          THEN /\ scan' = s
               /\ UNCHANGED <<cell, ent, hole, pc>>
          ELSE /\ cell' = [ cell EXCEPT ![hole] = k ]
               /\ ent' = [ ent EXCEPT ![hole] = ent[s] ]
               /\ hole' = s
               /\ scan' = s
               /\ UNCHANGED pc
    /\ scanned' = scanned + 1
    /\ UNCHANGED <<home, desc, next, block, err, foreignFreed, staleDelivered>>

\* the bounded erase examined the N - 1 other cells and met no empty one
\* (the code's loop from step 1 below the entry count): an invariant
\* failure, the final hole is cleared and the connection ends
EraseBound ==
    /\ pc = "erase"
    /\ ~UnboundedErase
    /\ scanned = N - 1
    /\ cell' = [ cell EXCEPT ![hole] = EMPTY ]
    /\ ent' = [ ent EXCEPT ![hole] = 0 ]
    /\ pc' = "idle"
    /\ err' = TRUE
    /\ UNCHANGED <<home, desc, next, block, hole, scan, scanned, foreignFreed, staleDelivered>>

Next ==
    \/ Collide
    \/ \E k \in Keys : Hold(k) \/ Deliver(k)
    \/ \E count \in 2..MaxCount : \E i \in 0..(count - 1) : Fragment(i, count)
    \/ Join
    \/ Direct
    \/ EndUpdate
    \/ Supersede
    \/ EraseStep
    \/ EraseBound

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ desc \in [Slots -> [live : BOOLEAN, num : 0..MaxNum, owner : Owners]]
    /\ cell \in [Cells -> Keys \cup {EMPTY}]
    /\ pc \in {"idle", "erase"}

ReleaseOwned == ~foreignFreed

HeldNamesLive ==
    pc = "idle" =>
        \A c \in Cells : cell[c] # EMPTY =>
            LET d == desc[ent[c] % W]
            IN d.live /\ d.num = ent[c] /\ d.owner = cell[c]

HeldBound == Used <= W

EraseEnds == scanned <= N

NoStaleDelivery == ~staleDelivered
=============================================================================
