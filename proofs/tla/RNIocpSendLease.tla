--------------------------- MODULE RNIocpSendLease ---------------------------
(***************************************************************************)
(* The generation tags of the IOCP server transport's send operations     *)
(* (sdk/rocketnet_server_iocp_windows.cpp): every send operation is a     *)
(* fixed record named by index and generation, the free list free_sends   *)
(* holds full references (index, generation), and a caller's packet id is *)
(* the generation above sixteen index bits.  The owner epoch is one       *)
(* instance's constant and is left out.                                   *)
(*                                                                         *)
(* free_sends is laid out as the code lays it out: NumOps cells, each a   *)
(* sequence number and a reference, and the free-running enqueue and      *)
(* dequeue positions; the cell of position p is p & (NumOps - 1).  Its    *)
(* capacity is send_queue_size, the operation count, a power of two.      *)
(* RNIocpRefQueue checks every step of the queue's push and pop on their  *)
(* own (each sequence read, CAS and store, and the counter wrap); here a  *)
(* push's sequence read and CAS are one step, and so are a pop's, while a *)
(* push's publish and a pop's sequence store are each a step of their     *)
(* own, so a pop can stall between its CAS and the store that frees its   *)
(* cell.  Positions do not wrap (2^64 in the code).                       *)
(*                                                                         *)
(* rocketnet_iocp_acquire_send pops a reference (AcquireClaim, the pop's  *)
(* CAS and read; Acquire, its sequence store and the checks after it)     *)
(* and accepts it only when the operation is FREE at that generation      *)
(* (rocketnet_iocp_send_from_ref); it then refuses the generation GenMax  *)
(* (UINT64_MAX >> 16, the largest that fits a packet id) by retiring the *)
(* operation, and otherwise increments the generation and stages it.      *)
(* rocketnet_iocp_post_send posts a staged operation; the completion      *)
(* (rocketnet_iocp_retire_send) and the refused paths release it through *)
(* rocketnet_iocp_release_send, which marks it FREE and pushes its        *)
(* reference, unchanged generation (rocketnet_iocp_queue_release_send:    *)
(* ReleasePush claims a position, ReleasePublish writes the cell).        *)
(*                                                                         *)
(* The push can be refused with the queue not full: a pop has claimed     *)
(* position q (dequeue is past it) and not yet stored its sequence        *)
(* q + NumOps, so the push at q + NumOps reads sequence q + 1 and finds   *)
(* the difference below zero (RefusalOnlyWhilePopStalled).  The code      *)
(* retries the push until that store lands, so ReleasePush is simply not *)
(* enabled while the cell is not free, and under the threads' fairness   *)
(* every release is published (ReleaseCompletes).                         *)
(*                                                                         *)
(* A caller keeps the packet id it was given after the lease ends and may *)
(* present it again to rocketnet_server_iocp_finish_packet or abort_packet*)
(* (rocketnet_iocp_packet_ref, then send_from_ref STAGED).  The tag must  *)
(* refuse it: StaleNeverNamesLease.                                       *)
(*                                                                         *)
(* Reversed witnesses: GenWrap = TRUE drops the GenMax refusal and lets   *)
(* the generation wrap modulo GenMax + 1, as the 48 generation bits of a  *)
(* packet id would; after a wrap a stale packet id names the new lease    *)
(* (StaleNeverNamesLease).  RetireRefused = TRUE takes a refused push as  *)
(* final and marks the operation RETIRED, as rocketnet_iocp_release_send  *)
(* did before the retry: an operation below GenMax is retired for good,  *)
(* free_sends never again holds every operation and send_idle never      *)
(* becomes true (RetiredOnlyAtGenMax).                                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    NumOps,        \* send operations and free_sends cells: send_queue_size, a power of two
    Callers,       \* threads that start packets
    Workers,       \* threads that complete posted sends
    GenMax,        \* the largest generation: UINT64_MAX >> 16 in the code
    MaxLeases,     \* exploration bound: acquisitions
    GenWrap,       \* defect: no GenMax refusal, the generation wraps
    RetireRefused  \* defect: a refused release push retires the operation

ASSUME NumOps >= 1 /\ GenMax >= 1 /\ MaxLeases >= 1
ASSUME GenWrap \in BOOLEAN /\ RetireRefused \in BOOLEAN
ASSUME Callers \cap Workers = {}

Ops == 0 .. NumOps - 1
Cells == 0 .. NumOps - 1
Threads == Callers \cup Workers
NoRef == [ op |-> -1, g |-> 0 ]
Ref(o, g) == [ op |-> o, g |-> g ]

RECURSIVE BitAnd(_, _)
BitAnd(a, b) ==
    IF a = 0 \/ b = 0 THEN 0
    ELSE 2 * BitAnd(a \div 2, b \div 2) + (IF a % 2 = 1 /\ b % 2 = 1 THEN 1 ELSE 0)

\* queue->cells[position & ( queue->capacity - 1 )]
Index(p) == BitAnd(p, NumOps - 1)

VARIABLES
    gen,      \* each operation's generation
    state,    \* each operation: "free", "staged", "posted" or "retired"
    seq,      \* free_sends: each cell's sequence
    cref,     \* free_sends: each cell's reference
    enqueue,  \* free_sends: the enqueue position
    dequeue,  \* free_sends: the dequeue position
    pc,       \* each thread: "idle", "pop" (claimed, sequence not stored), "push" or "publish"
    pos,      \* each thread's claimed position
    pref,     \* each thread's reference in flight: popped, or being pushed
    held,     \* each caller's current lease, or NoRef
    posted,   \* references posted and not yet completed
    stale,    \* each caller's last ended packet id, or NoRef
    leases    \* acquisitions so far

vars == << gen, state, seq, cref, enqueue, dequeue, pc, pos, pref, held, posted, stale, leases >>

(* rocketnet_iocp_send_from_ref: the reference names the operation at     *)
(* its current generation and in the expected state                       *)
SendFromRef(r, s) == r.op \in Ops /\ gen[r.op] = r.g /\ state[r.op] = s

(* the positions published and not yet claimed by a pop *)
Queued == { q \in dequeue .. enqueue - 1 : seq[Index(q)] = q + 1 }

(* create initializes sequence i in cell i, then pushes every operation  *)
(* at generation 0 in index order                                        *)
Init ==
    /\ gen = [ o \in Ops |-> 0 ]
    /\ state = [ o \in Ops |-> "free" ]
    /\ seq = [ i \in Cells |-> i + 1 ]
    /\ cref = [ i \in Cells |-> Ref(i, 0) ]
    /\ enqueue = NumOps
    /\ dequeue = 0
    /\ pc = [ x \in Threads |-> "idle" ]
    /\ pos = [ x \in Threads |-> 0 ]
    /\ pref = [ x \in Threads |-> NoRef ]
    /\ held = [ t \in Callers |-> NoRef ]
    /\ posted = {}
    /\ stale = [ t \in Callers |-> NoRef ]
    /\ leases = 0

(* rocketnet_iocp_queue_pop's sequence read and CAS: the cell at dequeue *)
(* is published, the pop claims it and reads its reference; the store    *)
(* that frees the cell is Acquire                                         *)
AcquireClaim(t) ==
    /\ pc[t] = "idle" /\ held[t] = NoRef /\ leases < MaxLeases
    /\ LET i == Index(dequeue) IN
         /\ seq[i] = dequeue + 1
         /\ dequeue' = dequeue + 1
         /\ pos' = [ pos EXCEPT ![t] = dequeue ]
         /\ pref' = [ pref EXCEPT ![t] = cref[i] ]
    /\ pc' = [ pc EXCEPT ![t] = "pop" ]
    /\ UNCHANGED << gen, state, seq, cref, enqueue, held, posted, stale, leases >>

(* the pop stores sequence position + NumOps, freeing the cell for the    *)
(* push one round later; then one pass of rocketnet_iocp_acquire_send's   *)
(* loop: skip a reference that does not name a FREE operation, retire one *)
(* at GenMax, else stage it                                               *)
Acquire(t) ==
    /\ pc[t] = "pop"
    /\ seq' = [ seq EXCEPT ![Index(pos[t])] = pos[t] + NumOps ]
    /\ pc' = [ pc EXCEPT ![t] = "idle" ]
    /\ pref' = [ pref EXCEPT ![t] = NoRef ]
    /\ LET r == pref[t]
           o == r.op
       IN IF ~SendFromRef(r, "free") THEN
               UNCHANGED << gen, state, held, leases >>
          ELSE IF ~GenWrap /\ gen[o] = GenMax THEN
               /\ state' = [ state EXCEPT ![o] = "retired" ]
               /\ UNCHANGED << gen, held, leases >>
          ELSE
               /\ gen' = [ gen EXCEPT ![o] = (@ + 1) % (GenMax + 1) ]
               /\ state' = [ state EXCEPT ![o] = "staged" ]
               /\ held' = [ held EXCEPT ![t] = Ref(o, (gen[o] + 1) % (GenMax + 1)) ]
               /\ leases' = leases + 1
    /\ UNCHANGED << cref, enqueue, dequeue, pos, posted, stale >>

(* rocketnet_iocp_release_send from STAGED: the start_packet refusals;   *)
(* FREE, then the push                                                   *)
ReleaseStaged(t) ==
    /\ pc[t] = "idle" /\ held[t] # NoRef /\ SendFromRef(held[t], "staged")
    /\ state' = [ state EXCEPT ![held[t].op] = "free" ]
    /\ pc' = [ pc EXCEPT ![t] = "push" ]
    /\ pref' = [ pref EXCEPT ![t] = held[t] ]
    /\ stale' = [ stale EXCEPT ![t] = held[t] ]
    /\ held' = [ held EXCEPT ![t] = NoRef ]
    /\ UNCHANGED << gen, seq, cref, enqueue, dequeue, pos, posted, leases >>

(* rocketnet_iocp_post_send: the staged operation is posted; the caller  *)
(* still has its packet id                                                *)
Post(t) ==
    /\ pc[t] = "idle" /\ held[t] # NoRef /\ SendFromRef(held[t], "staged")
    /\ state' = [ state EXCEPT ![held[t].op] = "posted" ]
    /\ posted' = posted \cup { held[t] }
    /\ stale' = [ stale EXCEPT ![t] = held[t] ]
    /\ held' = [ held EXCEPT ![t] = NoRef ]
    /\ UNCHANGED << gen, seq, cref, enqueue, dequeue, pc, pos, pref, leases >>

(* the completion: rocketnet_iocp_retire_send validates the completion's *)
(* reference POSTED and releases it: FREE, then the push                 *)
Retire(w, r) ==
    /\ pc[w] = "idle" /\ r \in posted /\ SendFromRef(r, "posted")
    /\ state' = [ state EXCEPT ![r.op] = "free" ]
    /\ posted' = posted \ { r }
    /\ pc' = [ pc EXCEPT ![w] = "push" ]
    /\ pref' = [ pref EXCEPT ![w] = r ]
    /\ UNCHANGED << gen, seq, cref, enqueue, dequeue, pos, held, stale, leases >>

(* rocketnet_iocp_queue_push's sequence read and CAS at enqueue: zero     *)
(* difference claims the position.  Below zero the push is refused and    *)
(* rocketnet_iocp_queue_release_send retries it from a fresh enqueue, so  *)
(* the step waits for the stalled pop's store; RetireRefused instead      *)
(* retires the operation, as the code did before the retry.               *)
ReleasePush(x) ==
    /\ pc[x] = "push"
    /\ LET d == seq[Index(enqueue)] - enqueue IN
         IF d = 0 THEN
              /\ enqueue' = enqueue + 1
              /\ pos' = [ pos EXCEPT ![x] = enqueue ]
              /\ pc' = [ pc EXCEPT ![x] = "publish" ]
              /\ UNCHANGED << state, pref >>
         ELSE
              /\ d < 0 /\ RetireRefused
              /\ state' = [ state EXCEPT ![pref[x].op] = "retired" ]
              /\ pc' = [ pc EXCEPT ![x] = "idle" ]
              /\ pref' = [ pref EXCEPT ![x] = NoRef ]
              /\ UNCHANGED << enqueue, pos >>
    /\ UNCHANGED << gen, seq, cref, dequeue, held, posted, stale, leases >>

(* the push writes its reference and publishes sequence position + 1 *)
ReleasePublish(x) ==
    /\ pc[x] = "publish"
    /\ cref' = [ cref EXCEPT ![Index(pos[x])] = pref[x] ]
    /\ seq' = [ seq EXCEPT ![Index(pos[x])] = pos[x] + 1 ]
    /\ pc' = [ pc EXCEPT ![x] = "idle" ]
    /\ pref' = [ pref EXCEPT ![x] = NoRef ]
    /\ UNCHANGED << gen, state, enqueue, dequeue, pos, held, posted, stale, leases >>

Next ==
    \/ \E t \in Callers : AcquireClaim(t) \/ Acquire(t) \/ ReleaseStaged(t) \/ Post(t)
    \/ \E w \in Workers : \E r \in posted : Retire(w, r)
    \/ \E x \in Threads : ReleasePush(x) \/ ReleasePublish(x)

Spec == Init /\ [][Next]_vars

(* a thread that has claimed a position finishes with it *)
SpecFair ==
    /\ Spec
    /\ \A t \in Callers : WF_vars(Acquire(t))
    /\ \A x \in Threads : WF_vars(ReleasePush(x)) /\ WF_vars(ReleasePublish(x))

----------------------------------------------------------------------------

TypeOK ==
    /\ gen \in [ Ops -> 0 .. GenMax ]
    /\ state \in [ Ops -> { "free", "staged", "posted", "retired" } ]
    /\ pc \in [ Threads -> { "idle", "pop", "push", "publish" } ]
    /\ dequeue \in Nat /\ enqueue \in Nat
    /\ leases \in 0 .. MaxLeases

(* a packet id the caller kept after its lease ended never names a live   *)
(* staged lease: finish_packet and abort_packet refuse it                 *)
StaleNeverNamesLease ==
    \A t \in Callers : stale[t] # NoRef => ~SendFromRef(stale[t], "staged")

(* the layout: claimed positions differ by 0 .. NumOps, and every one     *)
(* claimed for push and not yet for pop sits in its own cell, holding     *)
(* that position's sequence (position: being written, position + 1:       *)
(* published); the cell at enqueue is never a round ahead                 *)
CellsMatchPositions ==
    /\ enqueue - dequeue \in 0 .. NumOps
    /\ \A q \in dequeue .. enqueue - 1 : seq[Index(q)] \in { q, q + 1 }
    /\ seq[Index(enqueue)] <= enqueue

(* every reference queued names a FREE operation at its current           *)
(* generation, and no operation is queued twice                           *)
QueueRefsCurrent ==
    /\ \A q \in Queued : SendFromRef(cref[Index(q)], "free")
    /\ \A q1, q2 \in Queued : q1 # q2 => cref[Index(q1)].op # cref[Index(q2)].op

(* every operation has exactly one owner: free_sends, a thread popping or *)
(* pushing it, one caller's lease, the kernel (posted), or nobody once    *)
(* retired                                                                *)
OneOwner ==
    \A o \in Ops :
        LET inFree == Cardinality({ q \in Queued : cref[Index(q)].op = o })
            inFly  == Cardinality({ x \in Threads : pref[x].op = o })
            inHeld == Cardinality({ t \in Callers : held[t].op = o })
            inPost == Cardinality({ r \in posted : r.op = o })
            gone   == IF state[o] = "retired" THEN 1 ELSE 0
        IN inFree + inFly + inHeld + inPost + gone = 1

(* a lease's packet id decodes: generation 1 .. GenMax, never 0, which    *)
(* rocketnet_iocp_packet_ref refuses                                      *)
PacketIdsDecode ==
    \A t \in Callers : held[t] # NoRef => held[t].g \in 1 .. GenMax

(* an operation is retired only at GenMax: a release never loses one, so  *)
(* free_sends holds every live operation again once sends finish          *)
RetiredOnlyAtGenMax ==
    \A o \in Ops : state[o] = "retired" => gen[o] = GenMax

(* with the queue sized to its operations, a release push is refused only *)
(* while a pop has claimed the position one round behind and not yet      *)
(* stored the sequence that frees the cell                                *)
RefusalOnlyWhilePopStalled ==
    \A x \in Threads :
        (pc[x] = "push" /\ seq[Index(enqueue)] < enqueue) =>
            \E t \in Callers : pc[t] = "pop" /\ pos[t] = enqueue - NumOps

(* under SpecFair: every release push is published *)
ReleaseCompletes == \A x \in Threads : pc[x] = "push" ~> pc[x] = "idle"

=============================================================================
