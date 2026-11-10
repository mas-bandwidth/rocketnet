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
(* still holds position - Capacity (the queue is full, or a pop of that   *)
(* round has not freed it) and push returns false; above zero means      *)
(* another producer claimed the position, and push reloads enqueue.  A    *)
(* failed CAS reloads the position                                        *)
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
(* Producers push fresh tokens, each an operation released to the queue. *)
(* Ops bounds the tokens live at once (pushed or being pushed, not yet    *)
(* taken): the code sizes each queue to hold every operation it carries   *)
(* (free_sends has send_queue_size cells for send_queue_size sends,       *)
(* retry_receives at least every receive), so its controls take Ops at   *)
(* most Cap; Ops = MaxTok leaves the bound off.  Ghost state: inq, the    *)
(* tokens published and not yet taken; lost, the tokens a publish wrote   *)
(* over while they were unread; phantom, the values a pop returned that   *)
(* were never published or were already taken; refused, the tokens whose  *)
(* push was refused; popped, the tokens a pop took.                       *)
(*                                                                         *)
(* A stalled pop refuses a push with the queue not full: a consumer has   *)
(* claimed position q (dequeue is past it) and not yet stored its         *)
(* sequence q + Capacity, so the push at position q + Capacity reads      *)
(* sequence q + 1 and finds the difference below zero.  Every other      *)
(* operation can have gone round the queue meanwhile, so this needs only  *)
(* two live operations, and it holds whatever the capacity.  With Ops at  *)
(* most Cap it is the only refusal (RefusalOnlyWhilePopStalled).  A       *)
(* refused push keeps its operation and retries from a fresh enqueue     *)
(* load until the stalled pop has stored its sequence and the cell is    *)
(* free (PushRead's refusal branch): RefusedNeverLost, and under the      *)
(* fair spec every operation released is queued and popped               *)
(* (EveryOperationPopped).  DropRefused = TRUE is the reversed witness:   *)
(* the push's caller takes the refusal (return false, line 113 of the     *)
(* file) as final, as rocketnet_iocp_release_send (lines 446-449) and     *)
(* rocketnet_iocp_post_receive (lines 683-686) do when they mark the     *)
(* operation RETIRED, and the operation is in no cell, no thread and no  *)
(* pop (RefusedNeverLost).  Those callers do that today: the code is     *)
(* behind this model, and its retry is owed in the code.                 *)
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
    Cap,         \* cells: queue->capacity, a power of two set at create
    PosMod,      \* positions and sequences wrap here (2^64 in the code)
    InitSlack,   \* 0 is the code's initialize; 1 is the off-by-one
    Producers,   \* threads that push
    Consumers,   \* threads that pop
    MaxTok,      \* exploration bound: operations released
    Ops,         \* operations live at once: at most Cap in the code
    DropRefused  \* defect: a refused push drops its operation

ASSUME Cap >= 1 /\ PosMod >= 2 /\ PosMod % 2 = 0 /\ InitSlack \in 0..1 /\ MaxTok >= 1
ASSUME Ops \in 1 .. MaxTok /\ DropRefused \in BOOLEAN

RECURSIVE BitAnd(_, _)
BitAnd(a, b) ==
    IF a = 0 \/ b = 0 THEN 0
    ELSE 2 * BitAnd(a \div 2, b \div 2) + (IF a % 2 = 1 /\ b % 2 = 1 THEN 1 ELSE 0)

Cells == 0 .. Cap - 1
\* queue->cells[position & ( queue->capacity - 1 )]
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
    refused,   \* ghost: tokens whose push was refused
    popped     \* ghost: tokens a pop took

vars == << seq, ref, enqueue, dequeue, ppc, ppos, ptok, cpc, cpos, nextTok,
           inq, lost, phantom, refused, popped >>

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
    /\ inq = {} /\ lost = {} /\ phantom = {} /\ refused = {} /\ popped = {}

(* the tokens released and not yet taken *)
Live == Cardinality((1 .. nextTok - 1) \ popped)

(* push starts: position = enqueue.load *)
PushStart(p) ==
    /\ ppc[p] = "idle" /\ nextTok <= MaxTok /\ Live < Ops
    /\ ptok' = [ ptok EXCEPT ![p] = nextTok ]
    /\ nextTok' = nextTok + 1
    /\ ppos' = [ ppos EXCEPT ![p] = enqueue ]
    /\ ppc' = [ ppc EXCEPT ![p] = "read" ]
    /\ UNCHANGED << seq, ref, enqueue, dequeue, cpc, cpos, inq, lost, phantom, refused, popped >>

(* push reads its cell's sequence: free, full or passed.  A refused push *)
(* keeps its operation and reloads enqueue, as a passed one does, until   *)
(* the cell is free; DropRefused returns false to a caller that drops it *)
PushRead(p) ==
    /\ ppc[p] = "read"
    /\ LET d == SDiff(seq[Index(ppos[p])], ppos[p]) IN
         IF d = 0 THEN
              /\ ppc' = [ ppc EXCEPT ![p] = "cas" ]
              /\ UNCHANGED << ppos, refused >>
         ELSE IF d < 0 THEN
              /\ refused' = refused \cup { ptok[p] }
              /\ IF DropRefused
                   THEN /\ ppc' = [ ppc EXCEPT ![p] = "idle" ]
                        /\ UNCHANGED ppos
                   ELSE /\ ppos' = [ ppos EXCEPT ![p] = enqueue ]
                        /\ UNCHANGED ppc
         ELSE
              /\ ppos' = [ ppos EXCEPT ![p] = enqueue ]
              /\ UNCHANGED << ppc, refused >>
    /\ UNCHANGED << seq, ref, enqueue, dequeue, ptok, cpc, cpos, nextTok, inq, lost, phantom, popped >>

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
    /\ UNCHANGED << seq, ref, dequeue, ptok, cpc, cpos, nextTok, inq, lost, phantom, refused, popped >>

(* the winner writes its reference and publishes sequence position + 1 *)
PushPublish(p) ==
    /\ ppc[p] = "publish"
    /\ LET i == Index(ppos[p]) IN
         /\ ref' = [ ref EXCEPT ![i] = ptok[p] ]
         /\ seq' = [ seq EXCEPT ![i] = Wrap(ppos[p] + 1) ]
         /\ lost' = IF ref[i] \in inq THEN lost \cup { ref[i] } ELSE lost
         /\ inq' = (inq \ { ref[i] }) \cup { ptok[p] }
    /\ ppc' = [ ppc EXCEPT ![p] = "idle" ]
    /\ UNCHANGED << enqueue, dequeue, ppos, ptok, cpc, cpos, nextTok, phantom, refused, popped >>

(* pop starts: position = dequeue.load *)
PopStart(c) ==
    /\ cpc[c] = "idle"
    /\ cpos' = [ cpos EXCEPT ![c] = dequeue ]
    /\ cpc' = [ cpc EXCEPT ![c] = "read" ]
    /\ UNCHANGED << seq, ref, enqueue, dequeue, ppc, ppos, ptok, nextTok, inq, lost, phantom, refused, popped >>

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
    /\ UNCHANGED << seq, ref, enqueue, dequeue, ppc, ppos, ptok, nextTok, inq, lost, phantom, refused, popped >>

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
    /\ UNCHANGED << seq, ref, enqueue, ppc, ppos, ptok, nextTok, inq, lost, phantom, refused, popped >>

(* the winner reads the reference and frees the cell for one round later *)
PopTake(c) ==
    /\ cpc[c] = "take"
    /\ LET i == Index(cpos[c]) IN
         /\ seq' = [ seq EXCEPT ![i] = Wrap(cpos[c] + Cap) ]
         /\ phantom' = IF ref[i] \in inq THEN phantom ELSE phantom \cup { ref[i] }
         /\ inq' = inq \ { ref[i] }
         /\ popped' = popped \cup ({ ref[i] } \cap inq)
    /\ cpc' = [ cpc EXCEPT ![c] = "idle" ]
    /\ UNCHANGED << ref, enqueue, dequeue, ppc, ppos, ptok, cpos, nextTok, lost, refused >>

Next ==
    \/ \E p \in Producers : PushStart(p) \/ PushRead(p) \/ PushClaim(p) \/ PushPublish(p)
    \/ \E c \in Consumers : PopStart(c) \/ PopRead(c) \/ PopClaim(c) \/ PopTake(c)

Spec == Init /\ [][Next]_vars

(* every thread keeps taking steps *)
SpecFair ==
    /\ Spec
    /\ \A p \in Producers : WF_vars(PushStart(p) \/ PushRead(p) \/ PushClaim(p) \/ PushPublish(p))
    /\ \A c \in Consumers : WF_vars(PopStart(c) \/ PopRead(c) \/ PopClaim(c) \/ PopTake(c))

----------------------------------------------------------------------------

TypeOK ==
    /\ seq \in [ Cells -> 0 .. PosMod - 1 ]
    /\ ref \in [ Cells -> 0 .. MaxTok ]
    /\ enqueue \in 0 .. PosMod - 1 /\ dequeue \in 0 .. PosMod - 1
    /\ ppc \in [ Producers -> { "idle", "read", "cas", "publish" } ]
    /\ cpc \in [ Consumers -> { "idle", "read", "cas", "take" } ]
    /\ nextTok \in 1 .. MaxTok + 1
    /\ popped \subseteq 1 .. MaxTok

(* no publish writes over a reference that is still unread *)
NoOverwrite == lost = {}

(* every pop returns a reference that was published and not yet taken *)
NoPhantom == phantom = {}

(* In one modeled state, claimed positions differ by 0 .. Capacity.    *)
(* rocketnet_iocp_queue_count samples the two cursors separately;        *)
(* concurrent movement can put that observed unsigned difference outside *)
(* the range. Its clamped advisory count is not this single-state       *)
(* occupancy invariant.                                                *)
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

(* a refused push's operation is still held by its producer, queued or *)
(* taken: the refusal never drops it                                    *)
RefusedNeverLost ==
    \A t \in refused :
        \/ t \in popped
        \/ t \in inq
        \/ \E p \in Producers : ppc[p] # "idle" /\ ptok[p] = t

(* with the queue sized to its operations, a push is refused only while  *)
(* a pop has claimed the position one round behind (the cell's last      *)
(* round) and not yet stored the sequence that frees the cell            *)
RefusalOnlyWhilePopStalled ==
    Ops <= Cap =>
        \A p \in Producers :
            (ppc[p] = "read" /\ SDiff(seq[Index(ppos[p])], ppos[p]) < 0) =>
                \E c \in Consumers : cpc[c] = "take" /\ cpos[c] = Wrap(ppos[p] - Cap)

(* under SpecFair: every operation released is queued and popped *)
EveryOperationPopped == <>(popped = 1 .. MaxTok)

=============================================================================
