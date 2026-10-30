--------------------------- MODULE RNIocpRefQueue ---------------------------
(***************************************************************************)
(* The bounded MPMC reference queue of the IOCP server transport as the   *)
(* code lays it out (sdk/rocketnet_server_iocp_windows.cpp,               *)
(* rocketnet_iocp_ref_queue_t, rocketnet_iocp_queue_initialize,           *)
(* rocketnet_iocp_queue_push and rocketnet_iocp_queue_pop).  The queue is *)
(* Capacity cells, each a sequence number and a reference, and two free- *)
(* running position counters, enqueue and dequeue.  The cell of position  *)
(* p is p & (Capacity - 1).  Initialize stores sequence i in cell i.      *)
(*                                                                         *)
(* Push loads enqueue into its local position, reads the cell's sequence  *)
(* and takes the signed difference sequence - position: zero means the   *)
(* cell is free for this position and push tries to claim it with a CAS   *)
(* of enqueue from position to position + 1; below zero means the cell   *)
(* still holds the reference of position - Capacity, the queue is full    *)
(* and push returns false; above zero means another producer claimed the *)
(* position, and push reloads enqueue.  A failed CAS reloads the position *)
(* (compare_exchange_weak writes the current value back).  The winner     *)
(* writes the reference and publishes sequence position + 1.  Pop is the  *)
(* mirror: it claims when sequence - (position + 1) is zero, finds the    *)
(* queue empty below zero, and after reading the reference publishes      *)
(* sequence position + Capacity, freeing the cell for the push one round  *)
(* later.  Every thread's read, CAS and publish is its own step, so a     *)
(* thread can stall between reading a sequence and its CAS while the      *)
(* others run, which is where ABA would hide.                             *)
(*                                                                         *)
(* The counters and sequences are uint64_t in the code and wrap at        *)
(* PosMod here; the signed difference is the code's (int64_t) cast.  The  *)
(* passing controls take PosMod beyond every position the exploration     *)
(* reaches, which is the code's 2^64: a counter wrap needs 2^63 queue     *)
(* operations while one thread stalls inside a single push.               *)
(*                                                                         *)
(* Producers push fresh tokens (one per push attempt).  Ghost state: inq, *)
(* the tokens published and not yet taken; lost, the tokens a publish     *)
(* wrote over while they were unread; phantom, the values a pop returned  *)
(* that were never published or were already taken.                       *)
(*                                                                         *)
(* Reversed witnesses: Cap = 3 is a capacity one below a power of two,    *)
(* which the code's static_assert refuses: p & 2 sends positions 0 and 1  *)
(* to the same cell, so the second push overwrites the first (NoOverwrite)*)
(* .  InitSlack = 1 is initialize off by one (sequence i + 1 in cell i):  *)
(* an empty cell reads as full and pop returns a reference never pushed  *)
(* (NoPhantom).  PosMod = 2 * Cap is the counter wrap: a producer stalls  *)
(* between its sequence read and its CAS while the others advance enqueue *)
(* a whole wrap, the stale CAS succeeds (ABA) and its publish overwrites  *)
(* an unread reference (NoOverwrite).                                     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Cap,         \* cells: the code's Capacity
    PosMod,      \* positions and sequences wrap here (2^64 in the code)
    InitSlack,   \* 0 is the code's initialize; 1 is the off-by-one
    Producers,   \* threads that push
    Consumers,   \* threads that pop
    MaxTok       \* exploration bound: push attempts

ASSUME Cap >= 1 /\ PosMod >= 2 /\ PosMod % 2 = 0 /\ InitSlack \in 0..1 /\ MaxTok >= 1

RECURSIVE BitAnd(_, _)
BitAnd(a, b) ==
    IF a = 0 \/ b = 0 THEN 0
    ELSE 2 * BitAnd(a \div 2, b \div 2) + (IF a % 2 = 1 /\ b % 2 = 1 THEN 1 ELSE 0)

Cells == 0 .. Cap - 1
\* queue->cells[position & ( Capacity - 1 )]
Index(p) == BitAnd(p, Cap - 1)
\* uint64_t arithmetic, wrapping at PosMod
Wrap(x) == ((x % PosMod) + PosMod) % PosMod
\* (int64_t) ( a - b )
SDiff(a, b) == LET d == Wrap(a - b) IN IF d >= PosMod \div 2 THEN d - PosMod ELSE d

VARIABLES
    seq,       \* each cell's sequence
    ref,       \* each cell's reference: a token, 0 never pushed
    enqueue,   \* the enqueue position counter
    dequeue,   \* the dequeue position counter
    ppc,       \* each producer: "idle", "read", "cas" or "publish"
    ppos,      \* each producer's local position
    ptok,      \* each producer's token
    cpc,       \* each consumer: "idle", "read", "cas" or "take"
    cpos,      \* each consumer's local position
    nextTok,   \* the next token
    inq,       \* ghost: tokens published and not yet taken
    lost,      \* ghost: unread tokens a publish overwrote
    phantom,   \* ghost: values a pop returned that were not in inq
    refused    \* ghost: tokens whose push found the queue full

vars == << seq, ref, enqueue, dequeue, ppc, ppos, ptok, cpc, cpos, nextTok,
           inq, lost, phantom, refused >>

Init ==
    /\ seq = [ i \in Cells |-> Wrap(i + InitSlack) ]
    /\ ref = [ i \in Cells |-> 0 ]
    /\ enqueue = 0 /\ dequeue = 0
    /\ ppc = [ p \in Producers |-> "idle" ]
    /\ ppos = [ p \in Producers |-> 0 ]
    /\ ptok = [ p \in Producers |-> 0 ]
    /\ cpc = [ c \in Consumers |-> "idle" ]
    /\ cpos = [ c \in Consumers |-> 0 ]
    /\ nextTok = 1
    /\ inq = {} /\ lost = {} /\ phantom = {} /\ refused = {}

(* push starts: position = enqueue.load *)
PushStart(p) ==
    /\ ppc[p] = "idle" /\ nextTok <= MaxTok
    /\ ptok' = [ ptok EXCEPT ![p] = nextTok ]
    /\ nextTok' = nextTok + 1
    /\ ppos' = [ ppos EXCEPT ![p] = enqueue ]
    /\ ppc' = [ ppc EXCEPT ![p] = "read" ]
    /\ UNCHANGED << seq, ref, enqueue, dequeue, cpc, cpos, inq, lost, phantom, refused >>

(* push reads its cell's sequence: free, full or passed *)
PushRead(p) ==
    /\ ppc[p] = "read"
    /\ LET d == SDiff(seq[Index(ppos[p])], ppos[p]) IN
         IF d = 0 THEN
              /\ ppc' = [ ppc EXCEPT ![p] = "cas" ]
              /\ UNCHANGED << ppos, refused >>
         ELSE IF d < 0 THEN
              /\ ppc' = [ ppc EXCEPT ![p] = "idle" ]
              /\ refused' = refused \cup { ptok[p] }
              /\ UNCHANGED ppos
         ELSE
              /\ ppos' = [ ppos EXCEPT ![p] = enqueue ]
              /\ UNCHANGED << ppc, refused >>
    /\ UNCHANGED << seq, ref, enqueue, dequeue, ptok, cpc, cpos, nextTok, inq, lost, phantom >>

(* push's CAS of enqueue from its position; a failure reloads the position *)
PushClaim(p) ==
    /\ ppc[p] = "cas"
    /\ IF enqueue = ppos[p]
         THEN /\ enqueue' = Wrap(ppos[p] + 1)
              /\ ppc' = [ ppc EXCEPT ![p] = "publish" ]
              /\ UNCHANGED ppos
         ELSE /\ ppos' = [ ppos EXCEPT ![p] = enqueue ]
              /\ ppc' = [ ppc EXCEPT ![p] = "read" ]
              /\ UNCHANGED enqueue
    /\ UNCHANGED << seq, ref, dequeue, ptok, cpc, cpos, nextTok, inq, lost, phantom, refused >>

(* the winner writes its reference and publishes sequence position + 1 *)
PushPublish(p) ==
    /\ ppc[p] = "publish"
    /\ LET i == Index(ppos[p]) IN
         /\ ref' = [ ref EXCEPT ![i] = ptok[p] ]
         /\ seq' = [ seq EXCEPT ![i] = Wrap(ppos[p] + 1) ]
         /\ lost' = IF ref[i] \in inq THEN lost \cup { ref[i] } ELSE lost
         /\ inq' = (inq \ { ref[i] }) \cup { ptok[p] }
    /\ ppc' = [ ppc EXCEPT ![p] = "idle" ]
    /\ UNCHANGED << enqueue, dequeue, ppos, ptok, cpc, cpos, nextTok, phantom, refused >>

(* pop starts: position = dequeue.load *)
PopStart(c) ==
    /\ cpc[c] = "idle"
    /\ cpos' = [ cpos EXCEPT ![c] = dequeue ]
    /\ cpc' = [ cpc EXCEPT ![c] = "read" ]
    /\ UNCHANGED << seq, ref, enqueue, dequeue, ppc, ppos, ptok, nextTok, inq, lost, phantom, refused >>

(* pop reads its cell's sequence: published, empty or passed *)
PopRead(c) ==
    /\ cpc[c] = "read"
    /\ LET d == SDiff(seq[Index(cpos[c])], cpos[c] + 1) IN
         IF d = 0 THEN
              /\ cpc' = [ cpc EXCEPT ![c] = "cas" ]
              /\ UNCHANGED cpos
         ELSE IF d < 0 THEN
              /\ cpc' = [ cpc EXCEPT ![c] = "idle" ]
              /\ UNCHANGED cpos
         ELSE
              /\ cpos' = [ cpos EXCEPT ![c] = dequeue ]
              /\ UNCHANGED cpc
    /\ UNCHANGED << seq, ref, enqueue, dequeue, ppc, ppos, ptok, nextTok, inq, lost, phantom, refused >>

(* pop's CAS of dequeue from its position; a failure reloads the position *)
PopClaim(c) ==
    /\ cpc[c] = "cas"
    /\ IF dequeue = cpos[c]
         THEN /\ dequeue' = Wrap(cpos[c] + 1)
              /\ cpc' = [ cpc EXCEPT ![c] = "take" ]
              /\ UNCHANGED cpos
         ELSE /\ cpos' = [ cpos EXCEPT ![c] = dequeue ]
              /\ cpc' = [ cpc EXCEPT ![c] = "read" ]
              /\ UNCHANGED dequeue
    /\ UNCHANGED << seq, ref, enqueue, ppc, ppos, ptok, nextTok, inq, lost, phantom, refused >>

(* the winner reads the reference and frees the cell for one round later *)
PopTake(c) ==
    /\ cpc[c] = "take"
    /\ LET i == Index(cpos[c]) IN
         /\ seq' = [ seq EXCEPT ![i] = Wrap(cpos[c] + Cap) ]
         /\ phantom' = IF ref[i] \in inq THEN phantom ELSE phantom \cup { ref[i] }
         /\ inq' = inq \ { ref[i] }
    /\ cpc' = [ cpc EXCEPT ![c] = "idle" ]
    /\ UNCHANGED << ref, enqueue, dequeue, ppc, ppos, ptok, cpos, nextTok, lost, refused >>

Next ==
    \/ \E p \in Producers : PushStart(p) \/ PushRead(p) \/ PushClaim(p) \/ PushPublish(p)
    \/ \E c \in Consumers : PopStart(c) \/ PopRead(c) \/ PopClaim(c) \/ PopTake(c)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ seq \in [ Cells -> 0 .. PosMod - 1 ]
    /\ ref \in [ Cells -> 0 .. MaxTok ]
    /\ enqueue \in 0 .. PosMod - 1 /\ dequeue \in 0 .. PosMod - 1
    /\ ppc \in [ Producers -> { "idle", "read", "cas", "publish" } ]
    /\ cpc \in [ Consumers -> { "idle", "read", "cas", "take" } ]
    /\ nextTok \in 1 .. MaxTok + 1

(* no publish writes over a reference that is still unread *)
NoOverwrite == lost = {}

(* every pop returns a reference that was published and not yet taken *)
NoPhantom == phantom = {}

(* the claimed positions never exceed the cells: enqueue - dequeue is in *)
(* 0 .. Capacity (the clamp in rocketnet_iocp_queue_count never fires)    *)
WithinCapacity == SDiff(enqueue, dequeue) \in 0 .. Cap

(* the layout: every position claimed for push and not yet claimed for    *)
(* pop sits in its own cell, which holds that position's sequence:        *)
(* position (claimed, being written) or position + 1 (published)          *)
CellsMatchPositions ==
    \A j \in 0 .. SDiff(enqueue, dequeue) - 1 :
        LET q == Wrap(dequeue + j) IN seq[Index(q)] \in { q, Wrap(q + 1) }

(* the tokens published and not yet taken are exactly the references of  *)
(* the published cells, one per cell                                     *)
InqIsCells ==
    /\ Cardinality(inq) <= Cap
    /\ \A t \in inq : \E i \in Cells : ref[i] = t

=============================================================================
