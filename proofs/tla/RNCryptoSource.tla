-------------------------- MODULE RNCryptoSource --------------------------
(* The bundled sodium's random source, opened and closed beside the key and
   token generators (sdk/rocketnet.cpp: rocketnet_crypto_acquire,
   rocketnet_crypto_release, rocketnet_crypto_open_source,
   rocketnet_crypto_close; the bundled sodium's randombytes_sysrandom_stir and
   randombytes_sysrandom_buf).

   The source is sodium's flag (stream.initialized, here src) and a
   descriptor. Sodium opens it in two unlocked steps: it reads the flag and,
   when clear, opens a descriptor and sets the flag. A draw (randombytes_buf)
   does that on its own whenever the flag is clear.

   A generator call takes rocketnet_crypto_mutex shared and reads open; when
   open is clear it lets go, opens the source with the mutex exclusive
   (sodium_init, which stirs only the first time it runs, then
   randombytes_stir) and starts again. Holding the mutex shared it draws, then
   releases. rocketnet_init opens the source the same exclusive way. Term
   (rocketnet_term on macOS) takes the mutex exclusive, closes the source and
   clears open, at any time.

   Each thread makes MaxCalls calls; terms happen at most MaxTerms times.

   Defect selects the reversed witness:
     "nolock"     a call draws with no lock and no open (the code before the
                  guard): AtMostOneSource
     "noshared"   a call opens the source but does not hold the mutex across
                  its draw, so a term closes the source under it:
                  AtMostOneSource
     "noclear"    term closes the source but leaves open set: AtMostOneSource
     "nostir"     the open runs sodium_init alone, which opens nothing once
                  sodium is initialized: AtMostOneSource *)
EXTENDS Integers, FiniteSets
CONSTANTS Threads, MaxCalls, MaxTerms, Defect
ASSUME Threads \in Nat \ {0} /\ MaxCalls \in Nat \ {0} /\ MaxTerms \in Nat
ASSUME Defect \subseteq {"nolock", "noshared", "noclear", "nostir"}

T == 1..Threads
Free == 0
TermHolder == Threads + 1

VARIABLES open,     \* rocketnet_crypto_open, read and written under the mutex
          src,      \* sodium's stream.initialized
          fds,      \* descriptors the source holds
          sodium,   \* sodium_init has run
          shared,   \* threads holding the mutex shared
          excl,     \* the exclusive holder: a thread, TermHolder, or Free
          pc,       \* each thread's step
          calls,    \* calls each thread has finished
          terms     \* terms completed
vars == <<open, src, fds, sodium, shared, excl, pc, calls, terms>>

Init ==
    /\ open = FALSE /\ src = FALSE /\ fds = 0 /\ sodium = FALSE
    /\ shared = {} /\ excl = Free
    /\ pc = [t \in T |-> "idle"]
    /\ calls = [t \in T |-> 0]
    /\ terms = 0

(* rocketnet_crypto_acquire: lock_shared and read open; on a clear flag
   unlock_shared and go to the exclusive open *)
AcquireShared(t) ==
    /\ pc[t] = "idle" /\ calls[t] < MaxCalls
    /\ IF "nolock" \in Defect
       THEN /\ pc' = [pc EXCEPT ![t] = "draw"] /\ UNCHANGED shared
       ELSE /\ excl = Free
            /\ IF open
               THEN /\ shared' = IF "noshared" \in Defect THEN shared ELSE shared \cup {t}
                    /\ pc' = [pc EXCEPT ![t] = "draw"]
               ELSE /\ pc' = [pc EXCEPT ![t] = "wantexcl"] /\ UNCHANGED shared
    /\ UNCHANGED <<open, src, fds, sodium, excl, calls, terms>>

(* rocketnet_crypto_open_source: lock exclusive *)
AcquireExclusive(t) ==
    /\ pc[t] = "wantexcl" /\ excl = Free /\ shared = {}
    /\ excl' = t /\ pc' = [pc EXCEPT ![t] = "locked"]
    /\ UNCHANGED <<open, src, fds, sodium, shared, calls, terms>>

Recheck(t) ==
    /\ pc[t] = "locked"
    /\ IF open
       THEN /\ excl' = Free /\ pc' = [pc EXCEPT ![t] = "idle"]
       ELSE /\ excl' = excl /\ pc' = [pc EXCEPT ![t] = "stir"]
    /\ UNCHANGED <<open, src, fds, sodium, shared, calls, terms>>

(* sodium_init then randombytes_stir: the stir reads the flag first *)
StirCheck(t) ==
    /\ pc[t] = "stir"
    /\ LET stirs == ~sodium \/ "nostir" \notin Defect
       IN pc' = [pc EXCEPT ![t] = IF stirs /\ ~src THEN "stiropen" ELSE "publish"]
    /\ sodium' = TRUE
    /\ UNCHANGED <<open, src, fds, shared, excl, calls, terms>>

StirOpen(t) ==
    /\ pc[t] = "stiropen"
    /\ fds' = fds + 1 /\ src' = TRUE
    /\ pc' = [pc EXCEPT ![t] = "publish"]
    /\ UNCHANGED <<open, sodium, shared, excl, calls, terms>>

(* set open and unlock; the acquire starts again from lock_shared *)
Publish(t) ==
    /\ pc[t] = "publish"
    /\ open' = TRUE /\ excl' = Free
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<src, fds, sodium, shared, calls, terms>>

(* randombytes_buf: sodium's own unlocked open when the flag is clear *)
DrawCheck(t) ==
    /\ pc[t] = "draw"
    /\ pc' = [pc EXCEPT ![t] = IF src THEN "drawn" ELSE "drawopen"]
    /\ UNCHANGED <<open, src, fds, sodium, shared, excl, calls, terms>>

DrawOpen(t) ==
    /\ pc[t] = "drawopen"
    /\ fds' = fds + 1 /\ src' = TRUE
    /\ pc' = [pc EXCEPT ![t] = "drawn"]
    /\ UNCHANGED <<open, sodium, shared, excl, calls, terms>>

(* rocketnet_crypto_release: unlock_shared *)
Release(t) ==
    /\ pc[t] = "drawn"
    /\ shared' = shared \ {t}
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ calls' = [calls EXCEPT ![t] = calls[t] + 1]
    /\ UNCHANGED <<open, src, fds, sodium, excl, terms>>

(* rocketnet_crypto_close from rocketnet_term: exclusive, at any time *)
Term ==
    /\ terms < MaxTerms
    /\ excl = Free /\ shared = {}
    /\ fds' = IF src THEN fds - 1 ELSE fds
    /\ src' = FALSE
    /\ open' = IF "noclear" \in Defect THEN open ELSE FALSE
    /\ terms' = terms + 1
    /\ UNCHANGED <<sodium, shared, excl, pc, calls>>

Done ==
    /\ \A t \in T : pc[t] = "idle" /\ calls[t] = MaxCalls
    /\ UNCHANGED vars

Next ==
    \/ \E t \in T : AcquireShared(t) \/ AcquireExclusive(t) \/ Recheck(t) \/ StirCheck(t)
                    \/ StirOpen(t) \/ Publish(t) \/ DrawCheck(t) \/ DrawOpen(t) \/ Release(t)
    \/ Term
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ open \in BOOLEAN /\ src \in BOOLEAN /\ sodium \in BOOLEAN
    /\ fds \in Nat
    /\ shared \subseteq T
    /\ excl \in 0..TermHolder
    /\ pc \in [T -> {"idle", "wantexcl", "locked", "stir", "stiropen", "publish", "draw", "drawopen", "drawn"}]
    /\ calls \in [T -> 0..MaxCalls]
    /\ terms \in 0..MaxTerms

(* the source holds at most one descriptor: no open is lost *)
AtMostOneSource == fds <= 1

(* open set means the source is open *)
OpenMeansSource == open => src

(* a draw that holds the mutex finds the source open until it releases *)
HeldDrawFromSource == \A t \in shared : pc[t] = "drawn" => src
=============================================================================
