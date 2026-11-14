------------------------- MODULE RNClientIoShutdown -------------------------
(* Request/completion ownership abstraction for the 2026-10-21 API.
   This is an additive model, NOT a proved refinement/composition of RNClientIo,
   RNClientReceiveQuit, ClientSession or ServerSession. Those data-path models
   retain their original synchronous shutdown assumptions and measurements.

   Request publishes the final client Filling ring while one ring is half-sent
   (Sending) and one is ready (HandedOff). It never reopens a ring. Worker
   drains every send ring, wipes its own key copy, then acknowledges
   quiescence. Poll only observes acknowledgement.
   A permanently stuck backend retains ownership, while request/poll return.
   Synchronous destroy/restart joins the OS thread after quiescence; the shim
   may finish after acknowledgement. No elapsed-time bound is modeled.

   One endpoint owner serializes requests, queries, restart and destroy. Any
   producer tasks have already joined before request (the existing phase
   contract). Callback invocation itself and native cancel/syscall semantics
   are outside this abstraction; callback affinity/count and retained context
   are represented. Backend-specific RIO/uring/IOCP algorithms need separate
   correspondence and native tests.

   The client I/O endpoint's socket (sock, queued) and the ghosts lost and
   closeLoss: a datagram that arrives on the open socket queues until a read
   takes it. The worker, at its exit (sdk/rocketnet_client_io.h
   rocketnet_client_io_discard_at_stop), drains the socket in reads it
   counts as discarded at stop (drain, reads), then closes the socket
   before it acknowledges quiescence; a later datagram meets a closed port.
   The drain is bounded: it ends at an empty read, at a read the transport
   refuses (a counted transport fault) or after
   ROCKETNET_CLIENT_IO_DISCARD_MAX datagrams (DiscardCap here), and it
   waits for stop_woken between its last read and the close. A datagram
   that arrives in that gap, or past the cap from a sender that keeps the
   queue full, is still queued at the close and goes with it, unread and
   uncounted: closeLoss records it, and the model allows it, as the code
   does (README, "The exit's bounded drain"). lost records a queue that
   destroy or the next connection's start freed unread, which nothing
   counts; the close comes before quiescence, so that never happens.

   wake: the stop request stores quit, then wakes the worker through the
   endpoint's wake handle, which the worker's exit closes with the socket.
   wake is "pending" from the quit store until the request's wake call has
   returned (rocketnet_client_io_stop stores stop_woken after it); the
   worker closes only after that, so the request never writes to a closed,
   possibly reused, handle. The closebeforewake defect closes while the
   wake call is still to come. *)
EXTENDS Integers, FiniteSets
CONSTANTS Endpoint, NumWorkers, MaxGenerations, StuckWorker, Defect
ASSUME Endpoint \in {"clientio", "receive", "client", "server"}
ASSUME NumWorkers \in Nat \ {0} /\ MaxGenerations \in Nat \ {0}
ASSUME StuckWorker \in 0..NumWorkers
ASSUME Defect \in {"none", "requestwait", "requestcomplete", "freeearly",
                  "reuseearly", "ackbeforewipe", "repeatcallback",
                  "exitunread", "closebeforewake"}
Workers == 1..NumWorkers
Generations == 1..MaxGenerations
(* Three send rings: sdk/rocketnet_client_io.h
   ROCKETNET_CLIENT_IO_NUM_RINGS is 3. *)
Rings == {0, 1, 2}
(* the exit's drain reads at most this many datagrams: it stands for
   ROCKETNET_CLIENT_IO_DISCARD_MAX, which a sender that keeps the one-slot
   queue here full reaches *)
DiscardCap == 2
HasRings == Endpoint = "clientio"
HasKeys == Endpoint \in {"clientio", "client"}
Phases == {"stopped", "running", "stopping", "restarting", "destroying", "freed"}
WorkerStates == {"idle", "working", "quiescent", "exited", "joined"}
RingStates == {"empty", "filling", "handedoff", "sending", "sent"}
CallStates == {"idle", "entered", "returned"}
VARIABLES phase, generation, worker, keys, ring, ioRing, request, poll,
          pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake,
          drain, reads, closeLoss
vars == <<phase, generation, worker, keys, ring, ioRing, request, poll,
          pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake,
          drain, reads, closeLoss>>
(* the exit's drain (drain, reads) and the ghost of datagrams the close
   took unread (closeLoss); only the client I/O worker's exit changes them *)
exitVars == <<drain, reads, closeLoss>>
SockStates == {"none", "open", "closed"}
WakeStates == {"none", "pending", "done"}
DrainStates == {"none", "reading", "done"}
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
    /\ sock = "none" /\ queued = FALSE /\ lost = FALSE /\ wake = "none"
    /\ drain = "none" /\ reads = 0 /\ closeLoss = FALSE

(* Freshly created, never-started endpoints already report completion.
   Restart's synchronous finalization joins the prior generation first. *)
BeginRestart ==
    /\ IdleCaller /\ generation > 0 /\ generation < MaxGenerations
    /\ phase = "stopped" \/ (Defect = "reuseearly" /\ phase = "stopping")
    /\ phase' = "restarting"
    /\ badReuse' = (badReuse \/ ~Quiescent)
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, sock, queued, lost, wake, exitVars>>
Start ==
    /\ IdleCaller /\ generation < MaxGenerations
    /\ (phase = "stopped" /\ generation = 0) \/
       (phase = "restarting" /\ (Joined \/ Defect = "reuseearly"))
    /\ phase' = "running" /\ generation' = generation + 1
    /\ worker' = [w \in Workers |-> "working"]
    /\ keys' = [w \in Workers |-> HasKeys]
    (* Worst-case shutdown start for ROCKETNET_CLIENT_IO_NUM_RINGS (3):
       ring 0 is half-sent (Sending, still in the I/O hand), ring 1 is ready
       in the queue (HandedOff), and ring 2 is Filling so RequestStop hands
       it over as the final ring. No new producer runs during shutdown. *)
    /\ ring' = [r \in Rings |-> IF ~HasRings THEN "empty"
                              ELSE IF r = 0 THEN "sending"
                              ELSE IF r = 1 THEN "handedoff"
                              ELSE "filling"]
    /\ ioRing' = IF HasRings THEN 0 ELSE -1
    /\ request' = "idle" /\ poll' = "idle" /\ pollValue' = FALSE
    /\ badReuse' = (badReuse \/ ~Joined)
    /\ sock' = IF HasRings THEN "open" ELSE "none"
    /\ queued' = FALSE /\ lost' = (lost \/ queued) /\ wake' = "none"
    /\ drain' = "none" /\ reads' = 0
    /\ UNCHANGED <<callbacks, contextLive, closeLoss>>
(* The request does no worker's work and waits on no worker. On the client
   I/O path one backend call does run inside it: a link simulator (a
   development build's simulated network) is the caller's, never the I/O
   thread's, so the request first sends every copy it still holds
   (rocketnet_client_transport_stop calls rocketnet_client_link_drain at
   DBL_MAX, sdk/rocketnet_client_transport.cpp; RNClientLinkStop Stop,
   path "iocore"). Those sends are synchronous calls that do not wait on
   the worker; this model has no packets, so they are not a step here. *)
RequestStop ==
    /\ IdleCaller /\ phase = "running"
    /\ phase' = "stopping" /\ request' = "entered"
    (* The half-sent ring, the queued ring and the final ring may now
       belong to I/O. Publication does not inspect or wait for a ring
       already sending or queued, and never changes a worker-owned ring.
       The Filling ring is handed over as final. *)
    /\ ring' = [r \in Rings |-> IF ring[r] = "filling" THEN "handedoff" ELSE ring[r]]
    /\ callbacks' = [callbacks EXCEPT ![generation] = @ + 1]
    /\ pollValue' = FALSE
    (* quit is stored; the wake call that follows it has not returned *)
    /\ wake' = IF HasRings THEN "pending" ELSE wake
    /\ UNCHANGED <<generation, worker, keys, ioRing, poll, contextLive, badReuse, sock, queued, lost, exitVars>>
RepeatStop ==
    /\ IdleCaller /\ phase \in {"stopping", "stopped"}
    /\ request' = "entered"
    /\ callbacks' = IF Defect = "repeatcallback" /\ generation > 0
                     THEN [callbacks EXCEPT ![generation] = 2] ELSE callbacks
    /\ UNCHANGED <<phase, generation, worker, keys, ring, ioRing, poll,
                    pollValue, contextLive, badReuse, sock, queued, lost, wake, exitVars>>
RequestReturn ==
    /\ request = "entered"
    /\ Defect # "requestwait" \/ Quiescent
    /\ request' = "returned"
    (* the wake call returns, then stop_woken is stored *)
    /\ wake' = IF wake = "pending" THEN "done" ELSE wake
    /\ UNCHANGED <<phase, generation, worker, keys, ring, ioRing, poll,
                    pollValue, callbacks, contextLive, badReuse, sock, queued, lost, exitVars>>
BeginPoll ==
    /\ IdleCaller /\ phase \in {"stopping", "stopped"}
    /\ poll' = "entered"
    /\ UNCHANGED <<phase, generation, worker, keys, ring, ioRing, request,
                    pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake, exitVars>>
PollReturn ==
    /\ poll = "entered"
    /\ LET result == Complete \/ (Defect = "requestcomplete" /\ phase = "stopping") IN
       /\ pollValue' = result
       /\ phase' = IF result THEN "stopped" ELSE phase
    /\ poll' = "returned"
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request,
                    callbacks, contextLive, badReuse, sock, queued, lost, wake, exitVars>>

(* A stuck worker has no finish/quiesce/exit action. No fair backend return
   is assumed in the stuck-owner control. The caller actions still run. *)
FinishRing ==
    /\ HasRings /\ StuckWorker # 1 /\ worker[1] = "working"
    /\ ioRing \in Rings /\ ring[ioRing] = "sending"
    /\ ring' = [ring EXCEPT ![ioRing] = "sent"] /\ ioRing' = -1
    /\ UNCHANGED <<phase, generation, worker, keys, request, poll,
                    pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake, exitVars>>
TakeRing(r) ==
    /\ HasRings /\ StuckWorker # 1 /\ worker[1] = "working"
    /\ ioRing = -1 /\ ring[r] = "handedoff"
    /\ ring' = [ring EXCEPT ![r] = "sending"] /\ ioRing' = r
    /\ UNCHANGED <<phase, generation, worker, keys, request, poll,
                    pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake, exitVars>>
(* The worker's quiescence. For the client I/O worker (w = 1 with rings)
   it is the exit of rocketnet_client_io_step after its last send, in three
   kinds of step (rocketnet_client_io_discard_at_stop):
     a read     takes one queued datagram and counts it discarded at stop,
                while fewer than DiscardCap were read;
     the end    of the drain: an empty read, the cap, or a read the
                transport refuses (a counted transport fault, which can
                come at any read), so the drain may end with a datagram
                still queued;
     the close  after stop_woken (the exit waits for the stop request's
                wake call to return before it closes the wake handle):
                the socket closes, and what is still queued goes with it
                unread and uncounted (closeLoss); the worker acknowledges.
   Every other worker, and the exitunread defect (no drain, no close),
   acknowledges in one step. *)
QuiesceStep(w) ==
    /\ keys' = IF Defect = "ackbeforewipe" THEN keys ELSE [keys EXCEPT ![w] = FALSE]
    /\ worker' = [worker EXCEPT ![w] = "quiescent"]
WorkerQuiesce(w) ==
    /\ phase = "stopping" /\ w # StuckWorker /\ worker[w] = "working"
    /\ ~HasRings \/ w # 1 \/ (NoRingOwner /\ ioRing = -1)
    /\ IF HasRings /\ w = 1 /\ Defect # "exitunread"
       THEN \/ /\ drain # "done" /\ queued /\ reads < DiscardCap
               /\ queued' = FALSE /\ reads' = reads + 1 /\ drain' = "reading"
               /\ UNCHANGED <<worker, keys, sock, closeLoss>>
            \/ /\ drain # "done"
               /\ drain' = "done"
               /\ UNCHANGED <<worker, keys, sock, queued, reads, closeLoss>>
            \/ /\ drain = "done"
               /\ wake # "pending" \/ Defect = "closebeforewake"
               /\ QuiesceStep(w)
               /\ sock' = "closed" /\ queued' = FALSE
               /\ closeLoss' = (closeLoss \/ queued)
               /\ UNCHANGED <<drain, reads>>
       ELSE /\ ~HasRings \/ w # 1 \/ wake # "pending" \/ Defect = "closebeforewake"
            /\ QuiesceStep(w)
            /\ UNCHANGED <<sock, queued, exitVars>>
    /\ UNCHANGED <<phase, generation, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse, lost, wake>>
WorkerExit(w) ==
    /\ worker[w] = "quiescent"
    /\ worker' = [worker EXCEPT ![w] = "exited"]
    /\ UNCHANGED <<phase, generation, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake, exitVars>>
Join(w) ==
    /\ phase \in {"destroying", "restarting"} /\ worker[w] = "exited"
    /\ worker' = [worker EXCEPT ![w] = "joined"]
    /\ UNCHANGED <<phase, generation, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake, exitVars>>
DestroyBegin ==
    /\ IdleCaller
    /\ phase = "stopped" \/ (Defect = "freeearly" /\ phase = "stopping")
    /\ phase' = "destroying"
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse, sock, queued, lost, wake, exitVars>>
Free ==
    /\ phase = "destroying" /\ (Joined \/ Defect = "freeearly")
    /\ phase' = "freed" /\ contextLive' = FALSE
    /\ lost' = (lost \/ queued)
    /\ UNCHANGED <<generation, worker, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, badReuse, sock, queued, wake, exitVars>>
(* A datagram from the server reaches the client's open socket and
   queues; at a closed socket the operating system counts it instead. *)
Arrive ==
    /\ HasRings /\ sock = "open" /\ ~queued
    /\ queued' = TRUE
    /\ UNCHANGED <<phase, generation, worker, keys, ring, ioRing, request, poll,
                    pollValue, callbacks, contextLive, badReuse, sock, lost, wake, exitVars>>
Done == phase = "freed" /\ UNCHANGED vars
Next == Start \/ BeginRestart \/ RequestStop \/ RepeatStop \/ RequestReturn
        \/ BeginPoll \/ PollReturn \/ FinishRing \/ (\E r \in Rings : TakeRing(r))
        \/ (\E w \in Workers : WorkerQuiesce(w) \/ WorkerExit(w) \/ Join(w))
        \/ DestroyBegin \/ Free \/ Arrive \/ Done
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
    /\ ring \in [Rings -> RingStates] /\ ioRing \in {-1} \cup Rings
    /\ request \in CallStates /\ poll \in CallStates /\ pollValue \in BOOLEAN
    /\ callbacks \in [Generations -> 0..2] /\ contextLive \in BOOLEAN /\ badReuse \in BOOLEAN
    /\ sock \in SockStates /\ queued \in BOOLEAN /\ lost \in BOOLEAN
    /\ wake \in WakeStates
    /\ drain \in DrainStates /\ reads \in 0..DiscardCap /\ closeLoss \in BOOLEAN
CompletionMeansQuiescent == pollValue => Quiescent /\ NoWorkerKeys /\ NoRingOwner
FreedOnlyAfterJoin == phase = "freed" => Joined /\ NoWorkerKeys /\ NoRingOwner
RetainedUntilQuiescent == ~contextLive => Quiescent
(* the stop a poll reports has closed the socket, and nothing waits on it:
   the exit's drain read and counted what it read, and the close took the
   rest (closeLoss), so no datagram waits for destroy *)
StoppedSocketDrained == HasRings /\ pollValue => sock # "open" /\ ~queued
(* no queue is freed unread by destroy or the next start *)
NoUnreadDiscard == ~lost
(* not a property of the code: the reach case
   MCRNClientIoShutdownReachCloseDrops.cfg breaks it to show that the
   bounded drain's gap is in the model (a datagram queued after the drain's
   last read, or past its cap, goes with the close uncounted) *)
NoCloseLoss == ~closeLoss
(* the stop request's wake call never meets a handle the worker closed *)
NoWakeAfterClose == ~(wake = "pending" /\ sock = "closed")
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
