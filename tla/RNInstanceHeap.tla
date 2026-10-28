--------------------------- MODULE RNInstanceHeap ---------------------------
(* The instance heap's ownership over a client's or server's lifecycle
   (sdk/rocketnet_allocator.h, the instance heap; sdk/rocketnet_api.cpp).

   Create takes one block from the installed allocator: the object, its
   connection memory and its heap, sized from the transport plan of the
   largest candidate the ladder can grant (rocketnet_heap_required_bytes). A
   connect (a server's start) makes one transport generation from the heap,
   allocation by allocation; disconnect (stop) asks its worker to stop and
   retires it; the next connect, once the worker has stopped, frees the
   retired generation into the heap before it makes the next; destroy frees
   whatever generation is left and then the block.

   The heap is abstracted to a count of units: Plan units hold one generation
   (Allocations allocations of one unit each), and the heap holds Plan. A
   refusal is an allocation that finds the heap full. Every allocation and
   free is made by the caller's thread; the worker never touches the heap
   (RNClientRace.tla FreedOnlyByCaller is the race's half of that).

   Defect selects the reversed witnesses:
     "allocbeforefree"   connect makes the next generation before freeing the
                         retired one: NoRefusal
     "connectusesuser"   connect takes its generation from the installed
                         allocator, as before the heap: InstalledOnlyAtCreateDestroy
     "destroyleaks"      destroy frees the block with a generation still live
                         in the heap: NothingLiveAtFree
     "freewhilerunning"  connect frees the retired generation before its
                         worker has stopped: FreedOnlyStopped *)
EXTENDS Integers, FiniteSets
CONSTANTS Allocations, MaxGenerations, Defect
ASSUME Allocations \in Nat \ {0} /\ MaxGenerations \in Nat \ {0}
ASSUME Defect \subseteq {"allocbeforefree", "connectusesuser", "destroyleaks", "freewhilerunning"}

Plan == Allocations
Phases == {"none", "created", "destroyed"}
GenStates == {"absent", "building", "live", "retiring", "stopped"}

VARIABLES phase,        \* the object: none, created, destroyed
          current,      \* the connected generation's state
          retired,      \* the retired generation's state
          currentUnits, \* heap units the current generation holds
          retiredUnits, \* heap units the retired generation holds
          refused,      \* the heap refused an allocation
          installed,    \* installed-allocator calls: <<call, phase-at-call>>
          generations,  \* connects made
          freedLive,    \* the block was freed with units live in the heap
          freedRunning  \* a generation was freed while its worker ran
vars == <<phase, current, retired, currentUnits, retiredUnits, refused, installed, generations, freedLive, freedRunning>>

HeapUsed == currentUnits + retiredUnits

Init ==
    /\ phase = "none" /\ current = "absent" /\ retired = "absent"
    /\ currentUnits = 0 /\ retiredUnits = 0 /\ refused = FALSE
    /\ installed = {} /\ generations = 0
    /\ freedLive = FALSE /\ freedRunning = FALSE

(* rocketnet_client_create: the one block from the installed allocator *)
Create ==
    /\ phase = "none"
    /\ phase' = "created"
    /\ installed' = installed \cup {<<"malloc", "create">>}
    /\ UNCHANGED <<current, retired, currentUnits, retiredUnits, refused, generations, freedLive, freedRunning>>

(* rocketnet_client_connect: refused while the retired worker runs; else
   the retired generation goes back to the heap first, then building starts *)
Connect ==
    /\ phase = "created" /\ current = "absent" /\ generations < MaxGenerations
    /\ retired \in {"absent", "stopped"} \/ ("freewhilerunning" \in Defect /\ retired = "retiring")
    /\ generations' = generations + 1
    /\ current' = "building"
    /\ IF "allocbeforefree" \in Defect
       THEN UNCHANGED <<retired, retiredUnits, freedRunning>>
       ELSE /\ freedRunning' = (freedRunning \/ retired = "retiring")
            /\ retired' = "absent"
            /\ retiredUnits' = 0
    /\ installed' = IF "connectusesuser" \in Defect THEN installed \cup {<<"malloc", "connect">>} ELSE installed
    /\ UNCHANGED <<phase, currentUnits, refused, freedLive>>

(* one allocation of the building generation, from the heap; a refusal
   gives back what the connect built, and the connect is refused *)
Allocate ==
    /\ current = "building" /\ currentUnits < Allocations
    /\ IF "connectusesuser" \in Defect \/ HeapUsed < Plan
       THEN /\ currentUnits' = currentUnits + 1
            /\ UNCHANGED <<current, refused>>
       ELSE /\ refused' = TRUE
            /\ current' = "absent" /\ currentUnits' = 0
    /\ UNCHANGED <<phase, retired, retiredUnits, installed, generations, freedLive, freedRunning>>

Built ==
    /\ current = "building" /\ currentUnits = Allocations
    /\ current' = "live"
    /\ UNCHANGED <<phase, retired, currentUnits, retiredUnits, refused, installed, generations, freedLive, freedRunning>>

(* rocketnet_client_disconnect: the generation retires, its worker asked to stop *)
Disconnect ==
    /\ current = "live" /\ retired = "absent"
    /\ retired' = "retiring" /\ retiredUnits' = currentUnits
    /\ current' = "absent" /\ currentUnits' = 0
    /\ UNCHANGED <<phase, refused, installed, generations, freedLive, freedRunning>>

(* the worker's completion: the environment *)
WorkerStops ==
    /\ retired = "retiring"
    /\ retired' = "stopped"
    /\ UNCHANGED <<phase, current, currentUnits, retiredUnits, refused, installed, generations, freedLive, freedRunning>>

(* rocketnet_client_destroy: disconnected and stopped, the retired
   generation back to the heap, then the block to the installed allocator *)
Destroy ==
    /\ phase = "created" /\ current = "absent" /\ retired \in {"absent", "stopped"}
    /\ phase' = "destroyed"
    /\ IF "destroyleaks" \in Defect
       THEN /\ freedLive' = (HeapUsed > 0)
            /\ UNCHANGED <<retired, retiredUnits>>
       ELSE /\ freedLive' = FALSE
            /\ retired' = "absent" /\ retiredUnits' = 0
    /\ installed' = installed \cup {<<"free", "destroy">>}
    /\ UNCHANGED <<current, currentUnits, refused, generations, freedRunning>>

Done ==
    /\ phase = "destroyed"
    /\ UNCHANGED vars

Next ==
    \/ Create \/ Connect \/ Allocate \/ Built \/ Disconnect
    \/ WorkerStops \/ Destroy \/ Done

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ WF_vars(Allocate) /\ WF_vars(Built) /\ WF_vars(WorkerStops)

TypeOK ==
    /\ phase \in Phases /\ current \in GenStates /\ retired \in GenStates
    /\ currentUnits \in 0..Allocations /\ retiredUnits \in 0..Allocations
    /\ refused \in BOOLEAN /\ generations \in 0..MaxGenerations
    /\ installed \subseteq ({"malloc", "free"} \X {"create", "connect", "destroy"})
    /\ freedLive \in BOOLEAN /\ freedRunning \in BOOLEAN

(* the heap never refuses: a plan-sized heap holds every generation *)
NoRefusal == ~refused
(* the heap never holds more than the plan *)
WithinPlan == HeapUsed <= Plan
(* the installed allocator is called at create and destroy only *)
InstalledOnlyAtCreateDestroy == \A call \in installed : call[2] \in {"create", "destroy"}
(* the block goes back with nothing live in its heap *)
NothingLiveAtFree == ~freedLive
(* a generation goes back to the heap only after its worker stopped *)
FreedOnlyStopped == ~freedRunning

(* with the worker fair, a connect is built *)
ConnectBuilt == current = "building" ~> current \in {"live", "absent"}
=============================================================================
