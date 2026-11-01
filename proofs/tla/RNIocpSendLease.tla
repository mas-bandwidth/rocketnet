--------------------------- MODULE RNIocpSendLease ---------------------------
(***************************************************************************)
(* The generation tags of the IOCP server transport's send operations     *)
(* (sdk/rocketnet_server_iocp_windows.cpp): every send operation is a     *)
(* fixed record named by index and generation, the free list free_sends   *)
(* holds full references (index, generation), and a caller's packet id is *)
(* the generation above sixteen index bits.  RNIocpRefQueue checks the    *)
(* free list's cells and counters; here it is the FIFO it implements, and *)
(* this module checks the tags that ride in it.  The owner epoch is one   *)
(* instance's constant and is left out.                                   *)
(*                                                                         *)
(* rocketnet_iocp_acquire_send pops a reference and accepts it only when  *)
(* the operation is FREE at that generation (rocketnet_iocp_send_from_ref)*)
(* ; it then refuses the generation GenMax (UINT64_MAX >> 16, the largest *)
(* that fits a packet id) by retiring the operation, and otherwise        *)
(* increments the generation and stages it.  rocketnet_iocp_post_send     *)
(* posts a staged operation; the completion (rocketnet_iocp_retire_send)  *)
(* and the refused paths release it through rocketnet_iocp_release_send,  *)
(* which marks it FREE and pushes its reference, unchanged generation.    *)
(* The queue's capacity is the operation count, so the push never fails.  *)
(*                                                                         *)
(* A caller keeps the packet id it was given after the lease ends and may *)
(* present it again to rocketnet_server_iocp_finish_packet or abort_packet*)
(* (rocketnet_iocp_packet_ref, then send_from_ref STAGED).  The tag must  *)
(* refuse it: StaleNeverNamesLease.                                       *)
(*                                                                         *)
(* Reversed witness: GenWrap = TRUE drops the GenMax refusal and lets the *)
(* generation wrap modulo GenMax + 1, as the 48 generation bits of a      *)
(* packet id would; after a wrap a stale packet id names the new lease    *)
(* (StaleNeverNamesLease).                                                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    NumOps,     \* send operations: send_queue_size, rocketnet_server_iocp_send_queue_size( max_clients )
    Callers,    \* threads that start packets
    GenMax,     \* the largest generation: UINT64_MAX >> 16 in the code
    MaxLeases,  \* exploration bound: acquisitions
    GenWrap     \* defect: no GenMax refusal, the generation wraps

ASSUME NumOps >= 1 /\ GenMax >= 1 /\ MaxLeases >= 1 /\ GenWrap \in BOOLEAN

Ops == 0 .. NumOps - 1
NoRef == [ op |-> -1, g |-> 0 ]
Ref(o, g) == [ op |-> o, g |-> g ]

VARIABLES
    gen,      \* each operation's generation
    state,    \* each operation: "free", "staged", "posted" or "retired"
    free,     \* free_sends: references, oldest first
    held,     \* each caller's current lease, or NoRef
    posted,   \* references posted and not yet completed
    stale,    \* each caller's last ended packet id, or NoRef
    leases    \* acquisitions so far

vars == << gen, state, free, held, posted, stale, leases >>

(* rocketnet_iocp_send_from_ref: the reference names the operation at     *)
(* its current generation and in the expected state                       *)
SendFromRef(r, s) == r.op \in Ops /\ gen[r.op] = r.g /\ state[r.op] = s

(* create pushes every operation at generation 0, in index order *)
Init ==
    /\ gen = [ o \in Ops |-> 0 ]
    /\ state = [ o \in Ops |-> "free" ]
    /\ free = [ k \in 1 .. NumOps |-> Ref(k - 1, 0) ]
    /\ held = [ t \in Callers |-> NoRef ]
    /\ posted = {}
    /\ stale = [ t \in Callers |-> NoRef ]
    /\ leases = 0

(* one pop of rocketnet_iocp_acquire_send's loop: skip a reference that   *)
(* does not name a FREE operation, retire one at GenMax, else stage it    *)
Acquire(t) ==
    /\ held[t] = NoRef /\ free # << >> /\ leases < MaxLeases
    /\ LET r == Head(free)
           o == r.op
       IN /\ free' = Tail(free)
          /\ IF ~SendFromRef(r, "free") THEN
                  UNCHANGED << gen, state, held, leases >>
             ELSE IF ~GenWrap /\ gen[o] = GenMax THEN
                  /\ state' = [ state EXCEPT ![o] = "retired" ]
                  /\ UNCHANGED << gen, held, leases >>
             ELSE
                  /\ gen' = [ gen EXCEPT ![o] = (@ + 1) % (GenMax + 1) ]
                  /\ state' = [ state EXCEPT ![o] = "staged" ]
                  /\ held' = [ held EXCEPT ![t] = Ref(o, (gen[o] + 1) % (GenMax + 1)) ]
                  /\ leases' = leases + 1
    /\ UNCHANGED << posted, stale >>

(* rocketnet_iocp_release_send from STAGED: the start_packet refusals *)
ReleaseStaged(t) ==
    /\ held[t] # NoRef /\ SendFromRef(held[t], "staged")
    /\ state' = [ state EXCEPT ![held[t].op] = "free" ]
    /\ free' = Append(free, held[t])
    /\ stale' = [ stale EXCEPT ![t] = held[t] ]
    /\ held' = [ held EXCEPT ![t] = NoRef ]
    /\ UNCHANGED << gen, posted, leases >>

(* rocketnet_iocp_post_send: the staged operation is posted; the caller  *)
(* still has its packet id                                                *)
Post(t) ==
    /\ held[t] # NoRef /\ SendFromRef(held[t], "staged")
    /\ state' = [ state EXCEPT ![held[t].op] = "posted" ]
    /\ posted' = posted \cup { held[t] }
    /\ stale' = [ stale EXCEPT ![t] = held[t] ]
    /\ held' = [ held EXCEPT ![t] = NoRef ]
    /\ UNCHANGED << gen, free, leases >>

(* the completion: rocketnet_iocp_retire_send validates the completion's *)
(* reference POSTED and releases it                                       *)
Retire(r) ==
    /\ r \in posted /\ SendFromRef(r, "posted")
    /\ state' = [ state EXCEPT ![r.op] = "free" ]
    /\ free' = Append(free, r)
    /\ posted' = posted \ { r }
    /\ UNCHANGED << gen, held, stale, leases >>

Next ==
    \/ \E t \in Callers : Acquire(t) \/ ReleaseStaged(t) \/ Post(t)
    \/ \E r \in posted : Retire(r)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ gen \in [ Ops -> 0 .. GenMax ]
    /\ state \in [ Ops -> { "free", "staged", "posted", "retired" } ]
    /\ leases \in 0 .. MaxLeases

(* a packet id the caller kept after its lease ended never names a live   *)
(* staged lease: finish_packet and abort_packet refuse it                 *)
StaleNeverNamesLease ==
    \A t \in Callers : stale[t] # NoRef => ~SendFromRef(stale[t], "staged")

(* every reference on the free list names a FREE operation at its current *)
(* generation, and no operation is listed twice                           *)
QueueRefsCurrent ==
    /\ \A k \in 1 .. Len(free) : SendFromRef(free[k], "free")
    /\ \A j, k \in 1 .. Len(free) : j # k => free[j].op # free[k].op

(* every operation has exactly one owner: the free list, one caller, the *)
(* kernel (posted), or nobody once retired                                *)
OneOwner ==
    \A o \in Ops :
        LET inFree == Cardinality({ k \in 1 .. Len(free) : free[k].op = o })
            inHeld == Cardinality({ t \in Callers : held[t].op = o })
            inPost == Cardinality({ r \in posted : r.op = o })
            gone   == IF state[o] = "retired" THEN 1 ELSE 0
        IN inFree + inHeld + inPost + gone = 1

(* a lease's packet id decodes: generation 1 .. GenMax, never 0, which    *)
(* rocketnet_iocp_packet_ref refuses                                      *)
PacketIdsDecode ==
    \A t \in Callers : held[t] # NoRef => held[t].g \in 1 .. GenMax

=============================================================================
