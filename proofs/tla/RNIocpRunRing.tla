---------------------------- MODULE RNIocpRunRing ----------------------------
(***************************************************************************)
(* The run-ticket ring of the IOCP server transport's USO sends as the    *)
(* code lays it out (sdk/rocketnet_server_iocp_windows.cpp, run_refs,     *)
(* next_run_ticket, rocketnet_iocp_allocate_run and                        *)
(* rocketnet_iocp_take_run).  The ring is N fixed slots (N =               *)
(* ROCKETNET_IOCP_SEND_QUEUE_SIZE), each a reference, an owner epoch, a    *)
(* ticket and an occupied flag.  Ticket t selects slot (t - 1) % N.  The   *)
(* TX worker allocates a run of count members (2 .. MaxSeg) as the         *)
(* tickets next_run_ticket .. next_run_ticket + count - 1: only when       *)
(* first # 0, first <= TicketMax - count (the uint64_t overflow guard)     *)
(* and every one of those count slots is free; it then writes all count   *)
(* slots and advances next_run_ticket by count.  Otherwise it refuses and  *)
(* the run is sent frame by frame.  A completion (or the refused-USO       *)
(* path) takes the run by (first, count): only when every slot is          *)
(* occupied and carries exactly its ticket, and it then returns the refs   *)
(* and frees the slots.  Both run under run_mutex, so each is one step.   *)
(* The owner epoch is one instance's constant and is left out.            *)
(*                                                                         *)
(* Runs complete in any order, so a slot is reused while older slots are  *)
(* still held.  Any handle ever issued may be presented again (a stale or *)
(* repeated completion); the ticket compare must refuse every one that is *)
(* no longer live.                                                         *)
(*                                                                         *)
(* Ghost state: runs, every handle issued with its id and whether it is   *)
(* live; overwrite, set when an allocation writes an occupied slot (or    *)
(* one slot twice); staleTake, set when a take succeeds for a handle that *)
(* is not live; wrongRefs, set when a take returns refs not its own.      *)
(*                                                                         *)
(* Reversed witnesses: CheckShort = 1 is the free-slot check off by one   *)
(* at capacity (it checks count - 1 slots and writes count): a run whose  *)
(* last slot wraps onto a held run overwrites it (NoOverwrite).           *)
(* TicketWrap = TRUE drops the overflow guard and lets tickets wrap from  *)
(* TicketMax back to 1: a new run reissues an old run's tickets in the    *)
(* same slots and a stale handle takes it (TakeOnlyLive).                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    N,           \* slots: ROCKETNET_IOCP_SEND_QUEUE_SIZE
    MaxSeg,      \* the longest run: ROCKETNET_IOCP_USO_MAX_SEGMENTS, at most N
    TicketMax,   \* the largest ticket: UINT64_MAX in the code
    MaxRuns,     \* exploration bound: allocation attempts
    CheckShort,  \* 0 is the code's check of every slot; 1 is the off-by-one
    TicketWrap   \* defect: no overflow guard, tickets wrap to 1

ASSUME N >= 2 /\ MaxSeg \in 2 .. N /\ TicketMax >= MaxSeg /\ MaxRuns >= 1
ASSUME CheckShort \in 0 .. 1 /\ TicketWrap \in BOOLEAN

Slots == 0 .. N - 1
\* run_refs[( ticket - 1 ) % ROCKETNET_IOCP_SEND_QUEUE_SIZE]
SlotOf(t) == (t - 1) % N
\* first + i, wrapping into 1 .. TicketMax under the defect
TicketAt(first, i) == IF TicketWrap THEN ((first + i - 1) % TicketMax) + 1 ELSE first + i
NoMember == << 0, 0 >>

VARIABLES
    slotRef,      \* each slot's reference: << run id, member >>
    slotTicket,   \* each slot's ticket
    slotOcc,      \* each slot's occupied flag
    next,         \* next_run_ticket
    runs,         \* ghost: every handle issued: [id, first, count, live]
    attempts,     \* ghost: allocation attempts so far
    overwrite,    \* ghost: an allocation wrote an occupied slot
    staleTake,    \* ghost: a take succeeded for a handle not live
    wrongRefs     \* ghost: a take returned refs not its own

vars == << slotRef, slotTicket, slotOcc, next, runs, attempts, overwrite, staleTake, wrongRefs >>

Init ==
    /\ slotRef = [ s \in Slots |-> NoMember ]
    /\ slotTicket = [ s \in Slots |-> 0 ]
    /\ slotOcc = [ s \in Slots |-> FALSE ]
    /\ next = 1
    /\ runs = {}
    /\ attempts = 0
    /\ overwrite = FALSE /\ staleTake = FALSE /\ wrongRefs = FALSE

RunSlots(first, count) == { SlotOf(TicketAt(first, i)) : i \in 0 .. count - 1 }

(* rocketnet_iocp_allocate_run, under run_mutex *)
Allocate(count) ==
    /\ attempts < MaxRuns
    /\ attempts' = attempts + 1
    /\ LET first == next
           available ==
               /\ first # 0
               /\ ( TicketWrap \/ first <= TicketMax - count )
               /\ \A i \in 0 .. count - 1 - CheckShort : ~slotOcc[SlotOf(TicketAt(first, i))]
           id == attempts + 1
       IN IF available
          THEN /\ slotOcc' = [ s \in Slots |-> slotOcc[s] \/ s \in RunSlots(first, count) ]
               /\ slotTicket' = [ s \in Slots |->
                                  IF \E i \in 0 .. count - 1 : SlotOf(TicketAt(first, i)) = s
                                  THEN CHOOSE t \in { TicketAt(first, i) : i \in 0 .. count - 1 } : SlotOf(t) = s
                                  ELSE slotTicket[s] ]
               /\ slotRef' = [ s \in Slots |->
                               IF \E i \in 0 .. count - 1 : SlotOf(TicketAt(first, i)) = s
                               THEN << id, CHOOSE i \in 0 .. count - 1 : SlotOf(TicketAt(first, i)) = s >>
                               ELSE slotRef[s] ]
               /\ next' = TicketAt(first, count)
               /\ runs' = runs \cup { [ id |-> id, first |-> first, count |-> count, live |-> TRUE ] }
               /\ overwrite' = ( overwrite
                                 \/ \E s \in RunSlots(first, count) : slotOcc[s]
                                 \/ Cardinality(RunSlots(first, count)) < count )
          ELSE UNCHANGED << slotRef, slotTicket, slotOcc, next, runs, overwrite >>
    /\ UNCHANGED << staleTake, wrongRefs >>

(* rocketnet_iocp_take_run, under run_mutex, for any handle ever issued: *)
(* refused (no change) unless every slot holds its ticket                 *)
Take(r) ==
    /\ LET valid == \A i \in 0 .. r.count - 1 :
                        LET s == SlotOf(TicketAt(r.first, i)) IN
                            slotOcc[s] /\ slotTicket[s] = TicketAt(r.first, i)
       IN /\ valid
          /\ slotOcc' = [ s \in Slots |-> slotOcc[s] /\ s \notin RunSlots(r.first, r.count) ]
          /\ staleTake' = ( staleTake \/ ~r.live )
          /\ wrongRefs' = ( wrongRefs \/ \E i \in 0 .. r.count - 1 :
                                slotRef[SlotOf(TicketAt(r.first, i))] # << r.id, i >> )
          /\ runs' = { IF x.id = r.id THEN [ x EXCEPT !.live = FALSE ] ELSE x : x \in runs }
    /\ UNCHANGED << slotRef, slotTicket, next, attempts, overwrite >>

Next ==
    \/ \E c \in 2 .. MaxSeg : Allocate(c)
    \/ \E r \in runs : Take(r)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ slotOcc \in [ Slots -> BOOLEAN ]
    /\ slotTicket \in [ Slots -> 0 .. TicketMax ]
    /\ next \in 1 .. TicketMax
    /\ attempts \in 0 .. MaxRuns

(* no allocation writes a slot that still holds a run *)
NoOverwrite == ~overwrite

(* a take succeeds only for a live run: a stale or repeated completion is *)
(* refused by the ticket compare                                          *)
TakeOnlyLive == ~staleTake

(* a take returns exactly the run's own references, in order *)
TakeReturnsOwnRefs == ~wrongRefs

(* the layout: every live run's slots are occupied and carry its tickets  *)
(* and refs, so its completion will be accepted                           *)
LiveRunsIntact ==
    \A r \in runs : r.live =>
        \A i \in 0 .. r.count - 1 :
            LET s == SlotOf(TicketAt(r.first, i)) IN
                /\ slotOcc[s]
                /\ slotTicket[s] = TicketAt(r.first, i)
                /\ slotRef[s] = << r.id, i >>

(* every occupied slot belongs to exactly one live run *)
OccupiedIsLive ==
    \A s \in Slots : slotOcc[s] =>
        Cardinality({ r \in runs : r.live /\ s \in RunSlots(r.first, r.count) }) = 1

=============================================================================
