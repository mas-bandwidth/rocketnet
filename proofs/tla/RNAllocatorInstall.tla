-------------------------- MODULE RNAllocatorInstall --------------------------
(* The installed allocator pair and its live count with no lock
   (sdk/rocketnet.cpp: rn_allocator_install_locked, rocketnet_malloc,
   rocketnet_free).

   A call counts itself, reads the pair pointer, and if it is not the
   installing sentinel copies the pair and allocates with it; its header
   keeps the pair's release. A failed allocation uncounts the call; a free
   releases through the header and uncounts it. Install (under the
   lifecycle mutex, so one at a time) stores the sentinel, reads the count,
   and either puts the old pair back and refuses (LIVE_ALLOCATIONS) or
   publishes the new pair. Every step is one sequentially consistent
   operation, as the code's are.

   Pairs are named 1 and 2; Installing is 0. Install alternates the pair.

   Defect selects the reversed witness:
     "nosentinel"  install reads the count and then publishes, with no
                   sentinel:
                   ReleasedThroughItsPair and InstallOnlyWhenIdle *)
EXTENDS Integers, FiniteSets
CONSTANTS Calls, MaxInstalls, Defect
ASSUME Calls \in Nat \ {0} /\ MaxInstalls \in Nat
ASSUME Defect \subseteq {"nosentinel"}

Installing == 0
Callers == 1..Calls

VARIABLES pair,       \* the published pair pointer: 1, 2 or Installing
          count,      \* the live count
          pc,         \* each call's step
          read,       \* the pair each call read
          allocatedBy,\* the pair each live allocation came from (0 none)
          releaseOf,  \* the release each live allocation's header keeps
          ipc,        \* install's step
          iold,       \* the pair install found
          installs,   \* installs completed
          swappedLive \* an install published while an allocation of the old pair was live
vars == <<pair, count, pc, read, allocatedBy, releaseOf, ipc, iold, installs, swappedLive>>

Other(p) == IF p = 1 THEN 2 ELSE 1

Init ==
    /\ pair = 1 /\ count = 0
    /\ pc = [c \in Callers |-> "idle"]
    /\ read = [c \in Callers |-> 0]
    /\ allocatedBy = [c \in Callers |-> 0]
    /\ releaseOf = [c \in Callers |-> 0]
    /\ ipc = "idle" /\ iold = 0 /\ installs = 0 /\ swappedLive = FALSE

(* rocketnet_malloc: count, then read *)
CallCount(c) ==
    /\ pc[c] = "idle"
    /\ count' = count + 1 /\ pc' = [pc EXCEPT ![c] = "counted"]
    /\ UNCHANGED <<pair, read, allocatedBy, releaseOf, ipc, iold, installs, swappedLive>>

CallRead(c) ==
    /\ pc[c] = "counted"
    /\ IF pair = Installing
       THEN /\ count' = count - 1 /\ pc' = [pc EXCEPT ![c] = "idle"]
            /\ UNCHANGED read
       ELSE /\ read' = [read EXCEPT ![c] = pair] /\ pc' = [pc EXCEPT ![c] = "read"]
            /\ UNCHANGED count
    /\ UNCHANGED <<pair, allocatedBy, releaseOf, ipc, iold, installs, swappedLive>>

(* the allocator call with the copied pair: it succeeds, or fails and uncounts *)
CallAllocate(c) ==
    /\ pc[c] = "read"
    /\ \/ /\ allocatedBy' = [allocatedBy EXCEPT ![c] = read[c]]
          /\ releaseOf' = [releaseOf EXCEPT ![c] = read[c]]
          /\ pc' = [pc EXCEPT ![c] = "live"] /\ UNCHANGED count
       \/ /\ count' = count - 1 /\ pc' = [pc EXCEPT ![c] = "idle"]
          /\ UNCHANGED <<allocatedBy, releaseOf>>
    /\ UNCHANGED <<pair, read, ipc, iold, installs, swappedLive>>

(* rocketnet_free: through the header's release, then uncount *)
CallFree(c) ==
    /\ pc[c] = "live"
    /\ allocatedBy' = [allocatedBy EXCEPT ![c] = 0] /\ releaseOf' = [releaseOf EXCEPT ![c] = 0]
    /\ count' = count - 1 /\ pc' = [pc EXCEPT ![c] = "done"]
    /\ UNCHANGED <<pair, read, ipc, iold, installs, swappedLive>>

(* rn_allocator_install_locked *)
InstallBegin ==
    /\ ipc = "idle" /\ installs < MaxInstalls
    /\ iold' = pair
    /\ IF "nosentinel" \in Defect
       THEN UNCHANGED pair
       ELSE pair' = Installing
    /\ ipc' = "closed"
    /\ UNCHANGED <<count, pc, read, allocatedBy, releaseOf, installs, swappedLive>>

InstallCheck ==
    /\ ipc = "closed"
    /\ IF count # 0
       THEN /\ pair' = iold /\ ipc' = "idle" /\ installs' = installs + 1
            /\ UNCHANGED swappedLive
       ELSE /\ ipc' = "publish"
            /\ UNCHANGED <<pair, installs, swappedLive>>
    /\ UNCHANGED <<count, pc, read, allocatedBy, releaseOf, iold>>

InstallPublish ==
    /\ ipc = "publish"
    /\ pair' = Other(iold)
    /\ swappedLive' = (swappedLive \/ \E c \in Callers : allocatedBy[c] = iold \/ (pc[c] = "read" /\ read[c] = iold))
    /\ ipc' = "idle" /\ installs' = installs + 1
    /\ UNCHANGED <<count, pc, read, allocatedBy, releaseOf, iold>>

Done ==
    /\ \A c \in Callers : pc[c] \in {"idle", "done"}
    /\ ipc = "idle"
    /\ UNCHANGED vars

Next ==
    \/ \E c \in Callers : CallCount(c) \/ CallRead(c) \/ CallAllocate(c) \/ CallFree(c)
    \/ InstallBegin \/ InstallCheck \/ InstallPublish
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ pair \in {Installing, 1, 2} /\ count \in 0..Calls
    /\ pc \in [Callers -> {"idle", "counted", "read", "live", "done"}]
    /\ read \in [Callers -> 0..2] /\ allocatedBy \in [Callers -> 0..2] /\ releaseOf \in [Callers -> 0..2]
    /\ ipc \in {"idle", "closed", "publish"} /\ iold \in 0..2
    /\ installs \in 0..MaxInstalls /\ swappedLive \in BOOLEAN

(* every live allocation is released through the pair it came from, and
   that pair is the one published, or install is mid-way and will put it back *)
ReleasedThroughItsPair == \A c \in Callers : allocatedBy[c] # 0 => releaseOf[c] = allocatedBy[c] /\ (pair = allocatedBy[c] \/ (pair = Installing /\ iold = allocatedBy[c]))
(* the count covers every call between its count and its uncount *)
CountCoversCalls == count = Cardinality({c \in Callers : pc[c] \in {"counted", "read", "live"}})
(* install publishes a new pair only with nothing of the old pair live or in flight *)
InstallOnlyWhenIdle == ~swappedLive
=============================================================================
