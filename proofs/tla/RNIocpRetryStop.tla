--------------------------- MODULE RNIocpRetryStop ---------------------------
(***************************************************************************)
(* The stop-ticketed retry queue of the IOCP server transport's receives  *)
(* (sdk/rocketnet_server_iocp_windows.cpp: retry_receives,                 *)
(* rocketnet_iocp_post_receive, rocketnet_iocp_queue_pop_before and the    *)
(* retry pass of rocketnet_iocp_worker), with the queue's own cells,       *)
(* sequences and position counters (RNIocpRefQueue checks the cell         *)
(* protocol with every read and CAS a step of its own; here a claim is one *)
(* step, and the pass's stop ticket is what is checked).                   *)
(*                                                                         *)
(* A receive operation is posted, completes on whichever worker dequeues   *)
(* it, and is reposted by that worker.  rocketnet_iocp_post_receive        *)
(* retires the operation once stopping is stored; otherwise it increments  *)
(* the generation, marks the operation posted and counts it pending, then  *)
(* issues the receive.  A refused post has no completion: pending drops,   *)
(* the operation is marked RETRY and its reference (index, generation) is  *)
(* pushed onto retry_receives.  After each completion batch a worker runs  *)
(* a retry pass: it captures stop = enqueue, then pops with                *)
(* rocketnet_iocp_queue_pop_before until it returns false, reposting every *)
(* reference that still names a RETRY operation at its generation.        *)
(* pop_before returns false when (int64_t) ( stop - position ) <= 0, when  *)
(* the cell at position is not published (empty, or claimed by a push not *)
(* yet published), and otherwise claims the position by CAS.  A refused    *)
(* repost inside a pass requeues at a position at or beyond stop, so a     *)
(* pass is bounded.  A worker exits once stopping is stored and pending is *)
(* zero; stop cancels every posted receive, whose completion arrives on a  *)
(* worker still running.  Retries still queued at exit are abandoned with  *)
(* the transport, which frees every operation at destroy.                  *)
(*                                                                         *)
(* Scope.  The code's worker also waits for send_exited before it cancels  *)
(* and tests pending (rocketnet_iocp_worker, the stopping && send_exited   *)
(* test); Exit here is stopping and pending = 0 alone, so it over-         *)
(* approximates exit.  The TX-exit dependency is RNBufferHandoff's         *)
(* (WorkerExit), not StopCompletes'.  post_receive's own generation guard  *)
(* (generation == UINT64_MAX retires the operation) is not modelled: one   *)
(* operation cannot be posted 2^64 times, so it is unreachable.            *)
(*                                                                         *)
(* Positions are uint64_t in the code and wrap at PosMod here; the passing *)
(* controls take PosMod beyond every position the exploration reaches.     *)
(* Ghost absEnq, absDeq, absStop and absPos are the unwrapped positions.   *)
(*                                                                         *)
(* Reversed witnesses: StopSlack = 1 is the stop test off by one           *)
(* (stop - position < 0): a pass claims the position at its stop, which    *)
(* its own refused repost just pushed (PassBounded).  ClaimBeforeStop is   *)
(* the stop tested after the claim: a pass claims the position at its      *)
(* stop and returns false without reposting, and that retry is lost        *)
(* (RetryOwned).  PosMod = 2 * Cap is a stop ticket that wraps: a pass     *)
(* stalls after capturing stop while the others advance dequeue past it   *)
(* by more than half the wrap, so stop - position looks positive and the   *)
(* pass claims beyond its stop (PassBounded).                              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    NumOps,           \* receive operations
    Workers,          \* completion workers
    Cap,              \* retry queue cells, a power of two at least NumOps
    PosMod,           \* positions and sequences wrap here (2^64 in the code)
    MaxFail,          \* exploration bound: refused posts
    MaxComplete,      \* exploration bound: completions while running
    StopSlack,        \* 0 is the code's stop test; 1 is the off-by-one
    ClaimBeforeStop   \* defect: the stop test after the claim

ASSUME NumOps >= 1 /\ Cap >= NumOps /\ PosMod >= 2 * Cap /\ PosMod % Cap = 0
ASSUME MaxFail >= 1 /\ MaxComplete >= 0 /\ StopSlack \in 0 .. 1 /\ ClaimBeforeStop \in BOOLEAN

Ops == 0 .. NumOps - 1
Cells == 0 .. Cap - 1
NONE == -1
NoRef == [ op |-> NONE, g |-> 0 ]
Wrap(x) == ((x % PosMod) + PosMod) % PosMod
SDiff(a, b) == LET d == Wrap(a - b) IN IF d >= PosMod \div 2 THEN d - PosMod ELSE d
\* Cap is a power of two, so position & ( Capacity - 1 ) is position % Cap
Index(p) == p % Cap
\* pop_before's stop test: (int64_t) ( stop - position ) <= 0
PastStop(stop, p) == IF StopSlack = 0 THEN SDiff(stop, p) <= 0 ELSE SDiff(stop, p) < 0

VARIABLES
    state,      \* each operation: "posted", "idle", "retry" or "retired"
    gen,        \* each operation's generation
    pending,    \* posted receives not yet completed
    stopping,   \* stop stored
    seq, ref,   \* retry_receives cells
    enqueue, dequeue,
    wpc,        \* each worker: "loop", "post", "issue", "push", "publish", "pass", "take", "exited"
    cur,        \* the operation a worker is reposting, or NONE
    ret,        \* where a worker's post returns: "loop" or "pass"
    pos,        \* a worker's claimed queue position (push or pop)
    stop,       \* a worker's captured stop ticket
    absEnq, absDeq,   \* ghost: unwrapped counters
    absPos, absStop,  \* ghost: unwrapped claimed position and stop
    fails,      \* refused posts so far
    completes   \* completions while running so far

vars == << state, gen, pending, stopping, seq, ref, enqueue, dequeue, wpc, cur, ret,
           pos, stop, absEnq, absDeq, absPos, absStop, fails, completes >>

Init ==
    /\ state = [ o \in Ops |-> "posted" ]
    /\ gen = [ o \in Ops |-> 1 ]
    /\ pending = NumOps
    /\ stopping = FALSE
    /\ seq = [ i \in Cells |-> i ]
    /\ ref = [ i \in Cells |-> NoRef ]
    /\ enqueue = 0 /\ dequeue = 0
    /\ wpc = [ w \in Workers |-> "loop" ]
    /\ cur = [ w \in Workers |-> NONE ]
    /\ ret = [ w \in Workers |-> "loop" ]
    /\ pos = [ w \in Workers |-> 0 ]
    /\ stop = [ w \in Workers |-> 0 ]
    /\ absEnq = 0 /\ absDeq = 0
    /\ absPos = [ w \in Workers |-> 0 ]
    /\ absStop = [ w \in Workers |-> 0 ]
    /\ fails = 0 /\ completes = 0

Issuing(o) == \E w \in Workers : wpc[w] = "issue" /\ cur[w] = o

(* a worker dequeues one posted receive's completion (a datagram while    *)
(* running, the cancellation once stopping) and reposts the operation     *)
Complete(w, o) ==
    /\ wpc[w] = "loop"
    /\ state[o] = "posted" /\ ~Issuing(o)
    /\ ( stopping \/ completes < MaxComplete )
    /\ completes' = IF stopping THEN completes ELSE completes + 1
    /\ state' = [ state EXCEPT ![o] = "idle" ]
    /\ pending' = pending - 1
    /\ cur' = [ cur EXCEPT ![w] = o ]
    /\ ret' = [ ret EXCEPT ![w] = "loop" ]
    /\ wpc' = [ wpc EXCEPT ![w] = "post" ]
    /\ UNCHANGED << gen, stopping, seq, ref, enqueue, dequeue, pos, stop, absEnq, absDeq,
                    absPos, absStop, fails >>

(* rocketnet_iocp_post_receive up to the issue: retire once stopping is  *)
(* stored, else a new generation, posted and pending                      *)
PostStart(w) ==
    /\ wpc[w] = "post"
    /\ LET o == cur[w] IN
         IF stopping
         THEN /\ state' = [ state EXCEPT ![o] = "retired" ]
              /\ wpc' = [ wpc EXCEPT ![w] = ret[w] ]
              /\ cur' = [ cur EXCEPT ![w] = NONE ]
              /\ UNCHANGED << gen, pending >>
         ELSE /\ gen' = [ gen EXCEPT ![o] = @ + 1 ]
              /\ state' = [ state EXCEPT ![o] = "posted" ]
              /\ pending' = pending + 1
              /\ wpc' = [ wpc EXCEPT ![w] = "issue" ]
              /\ UNCHANGED cur
    /\ UNCHANGED << stopping, seq, ref, enqueue, dequeue, ret, pos, stop, absEnq, absDeq,
                    absPos, absStop, fails, completes >>

(* the receive is accepted: one completion will follow *)
PostAccepted(w) ==
    /\ wpc[w] = "issue"
    /\ wpc' = [ wpc EXCEPT ![w] = ret[w] ]
    /\ cur' = [ cur EXCEPT ![w] = NONE ]
    /\ UNCHANGED << state, gen, pending, stopping, seq, ref, enqueue, dequeue, ret, pos, stop,
                    absEnq, absDeq, absPos, absStop, fails, completes >>

(* the receive is refused: no completion; pending drops, RETRY, push *)
PostRefused(w) ==
    /\ wpc[w] = "issue" /\ fails < MaxFail
    /\ fails' = fails + 1
    /\ pending' = pending - 1
    /\ state' = [ state EXCEPT ![cur[w]] = "retry" ]
    /\ wpc' = [ wpc EXCEPT ![w] = "push" ]
    /\ UNCHANGED << gen, stopping, seq, ref, enqueue, dequeue, cur, ret, pos, stop, absEnq,
                    absDeq, absPos, absStop, completes >>

(* rocketnet_iocp_queue_push's claim: the cell at enqueue is free, CAS;  *)
(* a full queue retires the operation                                     *)
PushClaim(w) ==
    /\ wpc[w] = "push"
    /\ IF SDiff(seq[Index(enqueue)], enqueue) = 0
       THEN /\ pos' = [ pos EXCEPT ![w] = enqueue ]
            /\ enqueue' = Wrap(enqueue + 1)
            /\ absEnq' = absEnq + 1
            /\ wpc' = [ wpc EXCEPT ![w] = "publish" ]
            /\ UNCHANGED << state, cur >>
       ELSE /\ state' = [ state EXCEPT ![cur[w]] = "retired" ]
            /\ wpc' = [ wpc EXCEPT ![w] = ret[w] ]
            /\ cur' = [ cur EXCEPT ![w] = NONE ]
            /\ UNCHANGED << pos, enqueue, absEnq >>
    /\ UNCHANGED << gen, pending, stopping, seq, ref, dequeue, ret, stop, absDeq, absPos,
                    absStop, fails, completes >>

(* the push writes the reference and publishes position + 1 *)
PushPublish(w) ==
    /\ wpc[w] = "publish"
    /\ ref' = [ ref EXCEPT ![Index(pos[w])] = [ op |-> cur[w], g |-> gen[cur[w]] ] ]
    /\ seq' = [ seq EXCEPT ![Index(pos[w])] = Wrap(pos[w] + 1) ]
    /\ wpc' = [ wpc EXCEPT ![w] = ret[w] ]
    /\ cur' = [ cur EXCEPT ![w] = NONE ]
    /\ UNCHANGED << state, gen, pending, stopping, enqueue, dequeue, ret, pos, stop, absEnq,
                    absDeq, absPos, absStop, fails, completes >>

(* the retry pass starts: retry_stop = retry_receives.enqueue.load *)
PassStart(w) ==
    /\ wpc[w] = "loop"
    /\ stop' = [ stop EXCEPT ![w] = enqueue ]
    /\ absStop' = [ absStop EXCEPT ![w] = absEnq ]
    /\ wpc' = [ wpc EXCEPT ![w] = "pass" ]
    /\ UNCHANGED << state, gen, pending, stopping, seq, ref, enqueue, dequeue, cur, ret, pos,
                    absEnq, absDeq, absPos, fails, completes >>

(* one rocketnet_iocp_queue_pop_before: stop test, published test, CAS *)
PassClaim(w) ==
    /\ wpc[w] = "pass"
    /\ LET p == dequeue
           published == SDiff(seq[Index(p)], p + 1) = 0
       IN IF ~ClaimBeforeStop /\ PastStop(stop[w], p) THEN
               /\ wpc' = [ wpc EXCEPT ![w] = "loop" ]
               /\ UNCHANGED << dequeue, absDeq, pos, absPos >>
          ELSE IF ~published THEN
               /\ wpc' = [ wpc EXCEPT ![w] = "loop" ]
               /\ UNCHANGED << dequeue, absDeq, pos, absPos >>
          ELSE
               /\ dequeue' = Wrap(p + 1)
               /\ absDeq' = absDeq + 1
               /\ IF ClaimBeforeStop /\ PastStop(stop[w], p)
                  THEN /\ wpc' = [ wpc EXCEPT ![w] = "loop" ]
                       /\ UNCHANGED << pos, absPos >>
                  ELSE /\ pos' = [ pos EXCEPT ![w] = p ]
                       /\ absPos' = [ absPos EXCEPT ![w] = absDeq ]
                       /\ wpc' = [ wpc EXCEPT ![w] = "take" ]
    /\ UNCHANGED << state, gen, pending, stopping, seq, ref, enqueue, cur, ret, stop, absEnq,
                    absStop, fails, completes >>

(* the pop reads the reference and frees the cell; a reference that      *)
(* still names a RETRY operation at its generation is reposted            *)
PassTake(w) ==
    /\ wpc[w] = "take"
    /\ LET i == Index(pos[w])
           r == ref[i]
       IN /\ seq' = [ seq EXCEPT ![i] = Wrap(pos[w] + Cap) ]
          /\ IF r.op \in Ops /\ state[r.op] = "retry" /\ gen[r.op] = r.g
             THEN /\ cur' = [ cur EXCEPT ![w] = r.op ]
                  /\ ret' = [ ret EXCEPT ![w] = "pass" ]
                  /\ wpc' = [ wpc EXCEPT ![w] = "post" ]
             ELSE /\ wpc' = [ wpc EXCEPT ![w] = "pass" ]
                  /\ UNCHANGED << cur, ret >>
    /\ UNCHANGED << state, gen, pending, stopping, ref, enqueue, dequeue, pos, stop, absEnq,
                    absDeq, absPos, absStop, fails, completes >>

(* rocketnet_server_iocp_stop stores stopping *)
Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ UNCHANGED << state, gen, pending, seq, ref, enqueue, dequeue, wpc, cur, ret, pos, stop,
                    absEnq, absDeq, absPos, absStop, fails, completes >>

(* the worker's exit test at the top of its loop *)
Exit(w) ==
    /\ wpc[w] = "loop" /\ stopping /\ pending = 0
    /\ wpc' = [ wpc EXCEPT ![w] = "exited" ]
    /\ UNCHANGED << state, gen, pending, stopping, seq, ref, enqueue, dequeue, cur, ret, pos,
                    stop, absEnq, absDeq, absPos, absStop, fails, completes >>

(* every worker exited: the behaviour ends *)
Done ==
    /\ \A w \in Workers : wpc[w] = "exited"
    /\ UNCHANGED vars

Next ==
    \/ \E w \in Workers :
          \/ \E o \in Ops : Complete(w, o)
          \/ PostStart(w) \/ PostAccepted(w) \/ PostRefused(w)
          \/ PushClaim(w) \/ PushPublish(w)
          \/ PassStart(w) \/ PassClaim(w) \/ PassTake(w)
          \/ Exit(w)
    \/ Stop
    \/ Done

Spec == Init /\ [][Next]_vars

(* workers keep looping, cancelled receives complete, posts finish *)
Fairness ==
    \A w \in Workers :
        /\ WF_vars(PostStart(w)) /\ WF_vars(PostAccepted(w))
        /\ WF_vars(PushClaim(w)) /\ WF_vars(PushPublish(w))
        /\ WF_vars(PassClaim(w)) /\ WF_vars(PassTake(w))
        /\ SF_vars(PassStart(w)) /\ SF_vars(Exit(w))
        /\ \A o \in Ops : SF_vars(Complete(w, o))

SpecFair == Spec /\ Fairness

----------------------------------------------------------------------------

TypeOK ==
    /\ state \in [ Ops -> { "posted", "idle", "retry", "retired" } ]
    /\ pending \in 0 .. NumOps
    /\ enqueue \in 0 .. PosMod - 1 /\ dequeue \in 0 .. PosMod - 1
    /\ wpc \in [ Workers -> { "loop", "post", "issue", "push", "publish", "pass", "take", "exited" } ]

(* a pass never claims a position at or beyond the stop it captured *)
PassBounded ==
    \A w \in Workers : wpc[w] = "take" => absPos[w] < absStop[w]

(* no lost retry: every RETRY operation is being pushed, sits published   *)
(* in a queued cell at its generation, or is held by a pass that claimed  *)
(* its cell                                                               *)
RetryOwned ==
    \A o \in Ops : state[o] = "retry" =>
        \/ \E w \in Workers : cur[w] = o /\ wpc[w] \in { "post", "push", "publish" }
        \/ \E k \in absDeq .. absEnq - 1 :
               /\ ref[Index(Wrap(k))] = [ op |-> o, g |-> gen[o] ]
               /\ seq[Index(Wrap(k))] = Wrap(k + 1)
        \/ \E w \in Workers : wpc[w] = "take" /\ ref[Index(pos[w])] = [ op |-> o, g |-> gen[o] ]

(* pending counts exactly the posted receives *)
PendingIsPosted ==
    pending = Cardinality({ o \in Ops : state[o] = "posted" })

(* Single-state occupancy is bounded. This does not constrain queue_count's *)
(* separate atomic reads: their sampled difference can need clamping.  *)
(* It does not prove the clamp or RETIRED fallback unreachable in the code. *)
WithinCapacity == absEnq - absDeq \in 0 .. Cap

(* once every worker exited nothing is posted: every completion was taken *)
ExitQuiescent ==
    ( \A w \in Workers : wpc[w] = "exited" ) => \A o \in Ops : state[o] # "posted"

(* while running, every retry is eventually reposted *)
RetryResolves ==
    \A o \in Ops : ( state[o] = "retry" /\ ~stopping ) ~> ( state[o] # "retry" \/ stopping )

(* a stop completes: every worker exits *)
StopCompletes == stopping ~> \A w \in Workers : wpc[w] = "exited"

=============================================================================
