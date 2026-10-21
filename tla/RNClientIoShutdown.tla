------------------------- MODULE RNClientIoShutdown -------------------------
(* Request/completion ownership abstraction for the 2026-10-21 API.
   This is an additive model, NOT a proved refinement/composition of RNClientIo,
   RNClientReceiveQuit, ClientSession or ServerSession. Those data-path models
   retain their original synchronous shutdown assumptions and measurements.

   Request publishes the final client Filling ring even while the other ring
   is Sending. It never reopens either ring. Worker drains both, wipes its own
   key copy, then acknowledges quiescence. Poll only observes acknowledgement.
   A permanently stuck backend retains ownership, while request/poll return.
   Synchronous destroy/restart joins the OS thread after quiescence; the shim
   may finish after acknowledgement. No elapsed-time bound is modeled.

   One endpoint owner serializes requests, queries, restart and destroy. Any
   producer tasks have already joined before request (the existing phase
   contract). Callback invocation itself and native cancel/syscall semantics
   are outside this abstraction; callback affinity/count and retained context
   are represented. Backend-specific RIO/uring/IOCP algorithms need separate
   correspondence and native tests. *)
EXTENDS Naturals, FiniteSets
CONSTANTS Endpoint, NumWorkers, MaxGenerations, StuckWorker, Defect
ASSUME Endpoint \in {"clientio", "receive", "client", "server"}
ASSUME NumWorkers \in Nat \ {0} /\ MaxGenerations \in Nat \ {0}
ASSUME StuckWorker \in 0..NumWorkers
ASSUME Defect \in {"none", "requestwait", "requestcomplete", "freeearly",
                  "reuseearly", "ackbeforewipe", "repeatcallback"}
Workers == 1..NumWorkers
Generations == 1..MaxGenerations
Rings == {0, 1}
HasRings == Endpoint = "clientio"
HasKeys == Endpoint \in {"clientio", "client"}
Phases == {"stopped", "running", "stopping", "restarting", "destroying", "freed"}
WorkerStates == {"idle", "working", "quiescent", "exited", "joined"}
RingStates == {"empty", "filling", "handedoff", "sending", "sent"}
CallStates == {"idle", "entered", "returned"}
VARIABLES phase, generation, worker, keys, ring, ioRing, request, poll,
          pollValue, callbacks, contextLive, badReuse
vars == <<phase, generation, worker, keys, ring, ioRing, request, poll,
          pollValue, callbacks, contextLive, badReuse>>
IdleCaller == request # "entered" /\ poll # "entered"
Quiescent == \A w \in Workers : worker[w] \in {"idle", "quiescent", "exited", "joined"}
Joined == \A w \in Workers : worker[w] \in {"idle", "joined"}
NoWorkerKeys == \A w \in Workers : ~keys[w]
NoRingOwner == \A r \in Rings : ring[r] \in {"empty", "sent"}
Complete == phase = "stopped" \/ (phase = "stopping" /\ Quiescent)
Init ==
    /\ phase = "stopped" /\ generation = 0
    /\ worker = [w \in Workers |-> "idle"]
    /\ keys = [w \in Workers |-> FALSE]
    /\ ring = [r \in Rings |-> "empty"] /\ ioRing = -1
    /\ request = "idle" /\ poll = "idle" /\ pollValue = FALSE
    /\ callbacks = [g \in Generations |-> 0]
    /\ contextLive = TRUE /\ badReuse = FALSE

(* Freshly created, never-started endpoints already report completion.
   Restart's synchronous finalization joins the prior generation first. *)
BeginRestart ==
    /\ IdleCaller /\ generation > 0 /\ generation < MaxGenerations
    /\ phase = "stopped" \/ (Defect = "reuseearly" /\ phase = "stopping")
    /\ phase' = "restarting"
    /\ badReuse' = badReuse \/ ~Quiescent
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive>>
Start ==
    /\ IdleCaller /\ generation < MaxGenerations
    /\ (phase = "stopped" /\ generation = 0) \/
       (phase = "restarting" /\ (Joined \/ Defect = "reuseearly"))
    /\ phase' = "running" /\ generation' = generation + 1
    /\ worker' = [w \in Workers |-> "working"]
    /\ keys' = [w \in Workers |-> HasKeys]
    (* The hardest client handoff: other ring already Sending, final ring
       Filling with committed data. No new producer runs during shutdown. *)
    /\ ring' = [r \in Rings |-> IF ~HasRings THEN "empty"
                              ELSE IF r = 0 THEN "sending" ELSE "filling"]
    /\ ioRing' = IF HasRings THEN 0 ELSE -1
    /\ request' = "idle" /\ poll' = "idle" /\ pollValue' = FALSE
    /\ badReuse' = badReuse \/ ~Joined
    /\ UNCHANGED <<callbacks, contextLive>>
RequestStop ==
    /\ IdleCaller /\ phase = "running"
    /\ phase' = "stopping" /\ request' = "entered"
    (* Both rings may now belong to I/O. Publication does not inspect or
       wait for the other ring, and never changes a worker-owned ring. *)
    /\ ring' = [r \in Rings |-> IF ring[r] = "filling" THEN "handedoff" ELSE ring[r]]
    /\ callbacks' = [callbacks EXCEPT ![generation] = @ + 1]
    /\ pollValue' = FALSE
    /\ UNCHANGED <<generation, worker, keys, ioRing, poll, contextLive, badReuse>>
RepeatStop ==
    /\ IdleCaller /\ phase \in {"stopping", "stopped"}
    /\ request' = "entered"
    /\ callbacks' = IF Defect = "repeatcallback" /\ generation > 0
                     THEN [callbacks EXCEPT ![generation] = 2] ELSE callbacks
    /\ UNCHANGED <<phase, generation, worker, keys, ring, ioRing, poll,
                    pollValue, contextLive, badReuse>>
RequestReturn ==
    /\ request = "entered"
    /\ Defect # "requestwait" \/ Quiescent
    /\ request' = "returned"
    /\ UNCHANGED <<phase, generation, worker, keys, ring, ioRing, poll,
                    pollValue, callbacks, contextLive, badReuse>>
BeginPoll ==
    /\ IdleCaller /\ phase \in {"stopping", "stopped"}
    /\ poll' = "entered"
    /\ UNCHANGED <<phase, generation, worker, keys, ring, ioRing, request,
                    pollValue, callbacks, contextLive, badReuse>>
PollReturn ==
    /\ poll = "entered"
    /\ LET result == Complete \/ (Defect = "requestcomplete" /\ phase = "stopping") IN
       /\ pollValue' = result
       /\ phase' = IF result THEN "stopped" ELSE phase
    /\ poll' = "returned"
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request,
                    callbacks, contextLive, badReuse>>

(* A stuck worker has no finish/quiesce/exit action. No fair backend return
   is assumed in the stuck-owner control. The caller actions still run. *)
FinishRing ==
    /\ HasRings /\ StuckWorker # 1 /\ worker[1] = "working"
    /\ ioRing \in Rings /\ ring[ioRing] = "sending"
    /\ ring' = [ring EXCEPT ![ioRing] = "sent"] /\ ioRing' = -1
    /\ UNCHANGED <<phase, generation, worker, keys, request, poll,
                    pollValue, callbacks, contextLive, badReuse>>
TakeRing(r) ==
    /\ HasRings /\ StuckWorker # 1 /\ worker[1] = "working"
    /\ ioRing = -1 /\ ring[r] = "handedoff"
    /\ ring' = [ring EXCEPT ![r] = "sending"] /\ ioRing' = r
    /\ UNCHANGED <<phase, generation, worker, keys, request, poll,
                    pollValue, callbacks, contextLive, badReuse>>
WorkerQuiesce(w) ==
    /\ phase = "stopping" /\ w # StuckWorker /\ worker[w] = "working"
    /\ ~HasRings \/ w # 1 \/ (NoRingOwner /\ ioRing = -1)
    /\ keys' = IF Defect = "ackbeforewipe" THEN keys ELSE [keys EXCEPT ![w] = FALSE]
    /\ worker' = [worker EXCEPT ![w] = "quiescent"]
    /\ UNCHANGED <<phase, generation, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse>>
WorkerExit(w) ==
    /\ worker[w] = "quiescent"
    /\ worker' = [worker EXCEPT ![w] = "exited"]
    /\ UNCHANGED <<phase, generation, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse>>
Join(w) ==
    /\ phase \in {"destroying", "restarting"} /\ worker[w] = "exited"
    /\ worker' = [worker EXCEPT ![w] = "joined"]
    /\ UNCHANGED <<phase, generation, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse>>
DestroyBegin ==
    /\ IdleCaller
    /\ phase = "stopped" \/ (Defect = "freeearly" /\ phase = "stopping")
    /\ phase' = "destroying"
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse>>
Free ==
    /\ phase = "destroying" /\ (Joined \/ Defect = "freeearly")
    /\ phase' = "freed" /\ contextLive' = FALSE
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, badReuse>>
Done == phase = "freed" /\ UNCHANGED vars
Next == Start \/ BeginRestart \/ RequestStop \/ RepeatStop \/ RequestReturn
        \/ BeginPoll \/ PollReturn \/ FinishRing \/ (\E r \in Rings : TakeRing(r))
        \/ (\E w \in Workers : WorkerQuiesce(w) \/ WorkerExit(w) \/ Join(w))
        \/ DestroyBegin \/ Free \/ Done
Spec == Init /\ [][Next]_vars
(* Caller return fairness concerns only caller execution, not I/O progress.
   Strong poll fairness permits applications that also repeat the request.
   Nothing requires the application to initiate shutdown or destroy. *)
FairSpec == Spec
    /\ WF_vars(RequestReturn) /\ WF_vars(PollReturn) /\ SF_vars(BeginPoll)
    /\ WF_vars(FinishRing) /\ (\A r \in Rings : WF_vars(TakeRing(r)))
    /\ (\A w \in Workers : WF_vars(WorkerQuiesce(w)) /\ WF_vars(WorkerExit(w)) /\ WF_vars(Join(w)))
    /\ WF_vars(Free)
TypeOK ==
    /\ phase \in Phases /\ generation \in 0..MaxGenerations
    /\ worker \in [Workers -> WorkerStates] /\ keys \in [Workers -> BOOLEAN]
    /\ ring \in [Rings -> RingStates] /\ ioRing \in {-1, 0, 1}
    /\ request \in CallStates /\ poll \in CallStates /\ pollValue \in BOOLEAN
    /\ callbacks \in [Generations -> 0..2] /\ contextLive \in BOOLEAN /\ badReuse \in BOOLEAN
CompletionMeansQuiescent == pollValue => Quiescent /\ NoWorkerKeys /\ NoRingOwner
FreedOnlyAfterJoin == phase = "freed" => Joined /\ NoWorkerKeys /\ NoRingOwner
RetainedUntilQuiescent == ~contextLive => Quiescent
NoReuseBeforeComplete == ~badReuse
CallbackOnce == \A g \in Generations : callbacks[g] <= 1
NeverStartedComplete == generation = 0 /\ phase = "stopped" => Complete
RequestNeverWaits == request = "entered" => ENABLED RequestReturn
PollNeverWaits == poll = "entered" => ENABLED PollReturn
StuckRemainsPending == StuckWorker # 0 /\ generation > 0 =>
    phase \notin {"stopped", "restarting", "destroying", "freed"}
RequestReturns == request = "entered" ~> request = "returned"
PollReturns == poll = "entered" ~> poll = "returned"
StopCompletes == phase = "stopping" ~> phase = "stopped"
DestroyCompletes == phase = "destroying" ~> phase = "freed"
=============================================================================
