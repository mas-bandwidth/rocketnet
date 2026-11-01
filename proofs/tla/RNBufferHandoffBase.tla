------------------------- MODULE RNBufferHandoffBase -------------------------
(* BASE refinement: one producer, N-1 READY slots and a batch of consumer  *)
(* buffers. Update takes the maximal FIFO prefix fitting its packet budget *)
(* in one successful exchange. Processing/clearing is outside the mutex;   *)
(* the final exchange releases ONLY. A busy try-lock preserves ownership,  *)
(* including cleared deferred returns, for the next successful expose.    *)
(* PacketWeight scales each model item to packets; the checked instance   *)
(* has four 256-packet items per 1024-packet update. It is a bounded        *)
(* quotient, not exhaustive enumeration of every possible packet count.  *)
(* Reversed-witness switches, each FALSE in the code: TakeOnlyOne,        *)
(* IgnoreBudget, LoseDeferred, MainBlocks (update's try-lock becomes a     *)
(* blocking acquire that waits in take or release while the worker holds  *)
(* the mutex) and WorkUnderLock (Expose keeps the mutex and Process reads  *)
(* and clears the buffers before releasing it).                            *)
EXTENDS Naturals, Sequences, FiniteSets
CONSTANTS N, Cap, MaxItems, PacketWeight, PacketBudget,
          TakeOnlyOne, IgnoreBudget, LoseDeferred, MainBlocks, WorkUnderLock
ASSUME N >= 3 /\ Cap >= 1 /\ Cap * PacketWeight <= PacketBudget
Buffers == 0 .. N-1
VARIABLES lock, phase, producer, ready, free, held, content, produced,
          consumed, view, batchPackets, incompleteDrain, lockedWork
vars == <<lock, phase, producer, ready, free, held, content, produced,
          consumed, view, batchPackets, incompleteDrain, lockedWork>>
Range(s) == {s[i] : i \in 1 .. Len(s)}
RECURSIVE Items(_)
Items(s) == IF s = <<>> THEN <<>> ELSE content[Head(s)] \o Items(Tail(s))
Packets(s) == Len(Items(s)) * PacketWeight
Prefix(s,k) == SubSeq(s,1,k)
TakeCount == CHOOSE k \in 0 .. Len(ready) :
    /\ Packets(Prefix(ready,k)) <= PacketBudget
    /\ (k = Len(ready) \/ Packets(Prefix(ready,k+1)) > PacketBudget)
Init ==
    /\ lock = "none" /\ phase = "expose"
    /\ producer = 0 /\ ready = <<>>
    /\ free = [i \in 1 .. N-1 |-> i]
    /\ held = <<>> /\ content = [b \in Buffers |-> <<>>]
    /\ produced = 0 /\ consumed = <<>> /\ view = <<>>
    /\ batchPackets = 0 /\ incompleteDrain = FALSE /\ lockedWork = FALSE
Produce ==
    /\ lock # "worker" /\ produced < MaxItems
    /\ Len(content[producer]) < Cap
    /\ content' = [content EXCEPT ![producer] = Append(@, produced+1)]
    /\ produced' = produced+1
    /\ UNCHANGED <<lock,phase,producer,ready,free,held,consumed,view,
                    batchPackets,incompleteDrain,lockedWork>>
WorkerLock ==
    /\ lock = "none" /\ content[producer] # <<>> /\ free # <<>>
    /\ lock' = "worker"
    /\ UNCHANGED <<phase,producer,ready,free,held,content,produced,consumed,
                    view,batchPackets,incompleteDrain,lockedWork>>
Publish ==
    /\ lock = "worker"
    /\ ready' = Append(ready,producer)
    /\ producer' = Head(free) /\ free' = Tail(free)
    /\ lock' = "none"
    /\ UNCHANGED <<phase,held,content,produced,consumed,view,batchPackets,
                    incompleteDrain,lockedWork>>
MainTry ==
    /\ phase \in {"expose","return"}
    /\ IF lock = "none"
       THEN /\ lock' = "main"
            /\ phase' = IF phase = "expose" THEN "take" ELSE "release"
            /\ held' = held
       ELSE /\ lock' = lock
            /\ phase' = IF MainBlocks
                         THEN IF phase = "expose" THEN "take" ELSE "release"
                         ELSE "end"
            /\ held' = IF ~MainBlocks /\ LoseDeferred /\ phase = "return" THEN <<>> ELSE held
    /\ UNCHANGED <<producer,ready,free,content,produced,consumed,view,
                    batchPackets,incompleteDrain,lockedWork>>
(* MainBlocks only: the blocked update takes the mutex once it is free.  *)
MainLockWait ==
    /\ MainBlocks
    /\ phase \in {"take","release"} /\ lock = "none"
    /\ lock' = "main"
    /\ UNCHANGED <<phase,producer,ready,free,held,content,produced,consumed,
                    view,batchPackets,incompleteDrain,lockedWork>>
Expose ==
    /\ phase = "take" /\ lock = "main"
    /\ LET k == IF IgnoreBudget THEN Len(ready)
                ELSE IF TakeOnlyOne /\ TakeCount > 0 THEN 1 ELSE TakeCount
           taken == Prefix(ready,k)
       IN /\ free' = free \o held
          /\ held' = taken /\ ready' = SubSeq(ready,k+1,Len(ready))
          /\ view' = Items(taken) /\ batchPackets' = Packets(taken)
          /\ incompleteDrain' = (incompleteDrain \/ k < TakeCount)
    /\ phase' = "process" /\ lock' = IF WorkUnderLock THEN "main" ELSE "none"
    /\ UNCHANGED <<producer,content,produced,consumed,lockedWork>>
Process ==
    /\ phase = "process"
    /\ consumed' = consumed \o Items(held)
    /\ content' = [b \in Buffers |-> IF b \in Range(held) THEN <<>> ELSE content[b]]
    /\ lockedWork' = (lockedWork \/ lock = "main")
    /\ view' = <<>> /\ phase' = "return"
    /\ lock' = IF WorkUnderLock THEN "none" ELSE lock
    /\ UNCHANGED <<producer,ready,free,held,produced,batchPackets,incompleteDrain>>
ReleaseOnly ==
    /\ phase = "release" /\ lock = "main"
    /\ free' = free \o held /\ held' = <<>>
    /\ phase' = "end" /\ lock' = "none"
    /\ UNCHANGED <<producer,ready,content,produced,consumed,view,
                    batchPackets,incompleteDrain,lockedWork>>
EndUpdate ==
    /\ phase = "end"
    /\ phase' = "expose" /\ batchPackets' = 0
    /\ UNCHANGED <<lock,producer,ready,free,held,content,produced,consumed,
                    view,incompleteDrain,lockedWork>>
Next == Produce \/ WorkerLock \/ Publish \/ MainTry \/ MainLockWait \/ Expose \/ Process \/ ReleaseOnly \/ EndUpdate
Spec == Init /\ [][Next]_vars
TypeOK ==
    /\ lock \in {"none","main","worker"}
    /\ phase \in {"expose","take","process","return","release","end"}
    /\ producer \in Buffers
    /\ ready \in Seq(Buffers) /\ free \in Seq(Buffers) /\ held \in Seq(Buffers)
    /\ produced \in 0 .. MaxItems
OneOwner ==
    \A b \in Buffers :
      (IF producer = b THEN 1 ELSE 0)
      + Len(SelectSeq(ready,LAMBDA x : x=b))
      + Len(SelectSeq(free,LAMBDA x : x=b))
      + Len(SelectSeq(held,LAMBDA x : x=b)) = 1
ReadyBound == Len(ready) <= N-1
FreeIsEmpty == \A b \in Range(free) : content[b] = <<>>
ViewStable == Items(held) = view
InOrder == consumed = [i \in 1 .. Len(consumed) |-> i]
BudgetBound == batchPackets <= PacketBudget
MaximalDrain == ~incompleteDrain
(* Process (the reads and clears) never runs while update holds the mutex. *)
NoWorkUnderLock == ~lockedWork
(* No wait state: a failed try-lock advances to end, preserving held, so   *)
(* update is never in take or release while the worker holds the mutex.   *)
MainNeverWaits == lock = "worker" => phase \notin {"take","release"}
=============================================================================
