-------------------------- MODULE XdpHardwareBench --------------------------
(***************************************************************************)
(* The dedicated XDP test machine as a lease: one run at a time holds it,  *)
(* verifies it, attaches, runs, and restores it, and restore is            *)
(* unconditional.  bench/xdp_hardware/hold.sh and restore.sh implement     *)
(* this model; README-XdpHardwareBench.md maps each action to its lines.   *)
(*                                                                         *)
(* The machine's states, derived from the variables (MachineState):        *)
(*   Free       no holder, nothing attached, the baseline intact           *)
(*   Held       a run holds the lock and has not verified yet              *)
(*   Verified   the holder's `rocketnet-provision verify` passed           *)
(*   Attached   the holder's receiver attached its program and bound       *)
(*   Running    the holder's sender and receiver are measuring             *)
(*   Restoring  the holder (or a recovering hold) is putting it back       *)
(*   Dirty      no holder, but a crashed run left state behind; the lock   *)
(*              file still names the dead run                              *)
(*                                                                         *)
(* A crash is a run that dies without its EXIT trap (kill -9, power).  The *)
(* lock is an flock, so the kernel releases it with the dead process, but  *)
(* the lock file keeps naming the dead run, and the next hold restores     *)
(* first (Recover, RecoverRestore).  An INT or TERM is not a crash: the    *)
(* trap runs restore, which is the Finish/Restore path.                    *)
(*                                                                         *)
(* Constants:                                                              *)
(*   Runs          the run identities (two are enough to contend)          *)
(*   UseLock       hold takes the exclusive lock; FALSE is the             *)
(*                 TwoRuns reversed witness                                *)
(*   MarkCrash     a crash leaves the lock file naming the dead run, so    *)
(*                 the next hold restores first; FALSE is the NoRestore    *)
(*                 reversed witness                                        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Runs, UseLock, MarkCrash

NoRun    == "none"
Recovery == "recovery"   \* a hold.sh that finds a dead run's lock file

VARIABLES
    pc,          \* pc[r]: where run r is
    lock,        \* who holds the flock: a run, Recovery, or NoRun
    dirty,       \* the lock file names a dead run (restore owed)
    attachedBy,  \* the runs whose programs and sockets are on the NIC
    baseline,    \* the host snapshot equals the one taken at hold
    verified,    \* verified[r]: run r's verify passed (history)
    recovering   \* Recovery is mid-restore

vars == << pc, lock, dirty, attachedBy, baseline, verified, recovering >>

States == { "idle", "held", "verified", "attached", "running",
            "restoring", "done", "refused", "crashed" }

\* the states in which a run believes it holds the machine
Holding == { "held", "verified", "attached", "running", "restoring" }

TypeOK ==
    /\ pc \in [Runs -> States]
    /\ lock \in Runs \cup { NoRun, Recovery }
    /\ dirty \in BOOLEAN
    /\ attachedBy \subseteq Runs
    /\ baseline \in BOOLEAN
    /\ verified \in [Runs -> BOOLEAN]
    /\ recovering \in BOOLEAN

Init ==
    /\ pc = [r \in Runs |-> "idle"]
    /\ lock = NoRun
    /\ dirty = FALSE
    /\ attachedBy = {}
    /\ baseline = TRUE
    /\ verified = [r \in Runs |-> FALSE]
    /\ recovering = FALSE

-----------------------------------------------------------------------------
(* hold.sh: flock -n on the lock file.  With the lock it refuses while     *)
(* another run holds, naming the holder; while the lock file names a dead  *)
(* run, hold restores first (Recover below) and only then hands the        *)
(* machine on.                                                             *)

Hold(r) ==
    /\ pc[r] = "idle"
    /\ UseLock => (lock = NoRun /\ ~dirty)
    /\ pc' = [pc EXCEPT ![r] = "held"]
    /\ lock' = r
    /\ UNCHANGED << dirty, attachedBy, baseline, verified, recovering >>

(* the second hold, refused while the first holds: it changes nothing     *)
Refused(r) ==
    /\ UseLock
    /\ pc[r] = "idle"
    /\ lock \notin { NoRun }
    /\ pc' = [pc EXCEPT ![r] = "refused"]
    /\ UNCHANGED << lock, dirty, attachedBy, baseline, verified, recovering >>

(* run.sh's first row, preflight_verify.  verify reads R3 (no program     *)
(* attached) among its conditions, so it can pass only on a machine with  *)
(* nothing attached; a failed verify ends the run through restore.        *)
Verify(r) ==
    /\ pc[r] = "held"
    /\ \/ /\ attachedBy = {} /\ baseline
          /\ pc' = [pc EXCEPT ![r] = "verified"]
          /\ verified' = [verified EXCEPT ![r] = TRUE]
       \/ /\ pc' = [pc EXCEPT ![r] = "restoring"]
          /\ UNCHANGED verified
    /\ UNCHANGED << lock, dirty, attachedBy, baseline, recovering >>

(* the receiver starts: the library attaches natively and binds AF_XDP    *)
Attach(r) ==
    /\ pc[r] = "verified"
    /\ pc' = [pc EXCEPT ![r] = "attached"]
    /\ attachedBy' = attachedBy \cup { r }
    /\ baseline' = FALSE
    /\ UNCHANGED << lock, dirty, verified, recovering >>

Run(r) ==
    /\ pc[r] = "attached"
    /\ pc' = [pc EXCEPT ![r] = "running"]
    /\ UNCHANGED << lock, dirty, attachedBy, baseline, verified, recovering >>

(* the rows end, pass or fail; the EXIT trap starts restore              *)
Finish(r) ==
    /\ pc[r] = "running"
    /\ pc' = [pc EXCEPT ![r] = "restoring"]
    /\ UNCHANGED << lock, dirty, attachedBy, baseline, verified, recovering >>

(* restore.sh: stop the per-run processes, detach, close, sweep, verify,  *)
(* diff the snapshot; then the lock is released.  Restore puts back       *)
(* everything any run attached, since the sweep is by pin root, not by    *)
(* run, and the snapshot diff is of the whole host.                       *)
Restore(r) ==
    /\ pc[r] = "restoring"
    /\ pc' = [pc EXCEPT ![r] = "done"]
    /\ attachedBy' = {}
    /\ baseline' = TRUE
    /\ dirty' = FALSE
    /\ lock' = IF lock = r THEN NoRun ELSE lock
    /\ UNCHANGED << verified, recovering >>

(* a run dies without its trap, at any point while it holds: the kernel  *)
(* drops the flock; the lock file still names it when MarkCrash.          *)
Crash(r) ==
    /\ pc[r] \in Holding
    /\ pc' = [pc EXCEPT ![r] = "crashed"]
    /\ lock' = IF lock = r THEN NoRun ELSE lock
    /\ dirty' = IF MarkCrash THEN TRUE ELSE dirty
    /\ UNCHANGED << attachedBy, baseline, verified, recovering >>

(* the next hold.sh (or hold.sh --recover) finds the dead run's lock      *)
(* file, takes the lock, and runs restore before anything else            *)
Recover ==
    /\ dirty
    /\ lock = NoRun
    /\ ~recovering
    /\ lock' = Recovery
    /\ recovering' = TRUE
    /\ UNCHANGED << pc, dirty, attachedBy, baseline, verified >>

RecoverRestore ==
    /\ recovering
    /\ lock = Recovery
    /\ attachedBy' = {}
    /\ baseline' = TRUE
    /\ dirty' = FALSE
    /\ lock' = NoRun
    /\ recovering' = FALSE
    /\ UNCHANGED << pc, verified >>

(* every run has ended and the machine is quiet: stutter, not deadlock    *)
Quiet ==
    /\ \A r \in Runs : pc[r] \in { "done", "refused", "crashed" }
    /\ ~recovering
    /\ ~dirty
    /\ UNCHANGED vars

Next ==
    \/ \E r \in Runs :
        \/ Hold(r) \/ Refused(r) \/ Verify(r) \/ Attach(r) \/ Run(r)
        \/ Finish(r) \/ Restore(r) \/ Crash(r)
    \/ Recover \/ RecoverRestore
    \/ Quiet

(* Weak fairness of restore (and of the run's own steps, which a run      *)
(* takes unless it crashes): a holder that keeps the chance to move,      *)
(* moves.  Hold and Crash are never forced.                               *)
Fairness ==
    /\ \A r \in Runs :
        /\ WF_vars(Verify(r))
        /\ WF_vars(Attach(r))
        /\ WF_vars(Run(r))
        /\ WF_vars(Finish(r))
        /\ WF_vars(Restore(r))
    /\ WF_vars(Recover)
    /\ WF_vars(RecoverRestore)

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
MachineFree == lock = NoRun /\ ~dirty

MachineState ==
    IF lock = NoRun
    THEN IF dirty THEN "Dirty" ELSE "Free"
    ELSE IF lock = Recovery THEN "Restoring"
    ELSE CASE pc[lock] = "held"      -> "Held"
           [] pc[lock] = "verified"  -> "Verified"
           [] pc[lock] = "attached"  -> "Attached"
           [] pc[lock] = "running"   -> "Running"
           [] OTHER                  -> "Restoring"

(* at most one run holds the machine *)
OneRunAtATime ==
    Cardinality({ r \in Runs : pc[r] \in Holding }) + (IF recovering THEN 1 ELSE 0) <= 1

(* no program is attached by a run whose verify did not pass *)
AttachOnlyAfterVerify ==
    \A r \in attachedBy : verified[r]

(* Free means nothing attached and the baseline intact *)
FreeMeansRestored ==
    MachineState = "Free" => (attachedBy = {} /\ baseline)

(* every hold ends with the machine Free again *)
EveryHoldEndsFree ==
    \A r \in Runs : (pc[r] = "held") ~> MachineFree

=============================================================================
