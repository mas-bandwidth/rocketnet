-------------------------- MODULE RNCryptoSource --------------------------
(* The bundled sodium's random source, opened before and after rocketnet_init
   (sdk/rocketnet.cpp: rocketnet_crypto_ready, rocketnet_crypto_close; the
   bundled sodium's randombytes_sysrandom_stir and randombytes_sysrandom_buf).

   The source is a flag (sodium's stream.initialized, here src) and a
   descriptor. Sodium opens it in two steps with no lock: it reads the flag
   and, when clear, opens a descriptor and sets the flag. A draw
   (randombytes_buf) does that on its own whenever the flag is clear.

   A generator call (rocketnet_generate_server_keypair and the others, and
   rocketnet_init) runs rocketnet_crypto_ready first: one load of open, the
   library's published flag; when it is clear, the lock, a second load, the
   sodium_init and randombytes_stir that open the source, and the publish.
   Then the call draws. Term (rocketnet_term on macOS) takes the same lock,
   closes the source and clears open. Term never runs while a generator call
   is in flight: the header states it.

   Threads are generator calls; each makes one call per cycle. A term starts
   the next cycle. The model interleaves whole steps; open is an
   acquire/release atomic and every other variable is written under the lock
   or by sodium's unlocked two steps, which the model keeps apart.

   Defect selects the reversed witness:
     "nolock"     a call draws with no rocketnet_crypto_ready (the code before
                  the guard): AtMostOneSource
     "noclear"    term closes the source but leaves open set, so the next
                  cycle's calls skip the lock: AtMostOneSource
     "nostir"     rocketnet_crypto_ready runs sodium_init alone, which opens
                  nothing once sodium is initialized: AtMostOneSource *)
EXTENDS Integers
CONSTANTS Threads, MaxTerms, Defect
ASSUME Threads \in Nat \ {0} /\ MaxTerms \in Nat
ASSUME Defect \subseteq {"nolock", "noclear", "nostir"}

T == 1..Threads

VARIABLES open,     \* rocketnet_crypto_open
          src,      \* sodium's stream.initialized
          fds,      \* descriptors the source holds
          sodium,   \* sodium_init has run
          lock,     \* rocketnet_crypto_mutex holder, 0 when free
          pc,       \* each thread's step
          terms     \* terms completed
vars == <<open, src, fds, sodium, lock, pc, terms>>

Init ==
    /\ open = FALSE /\ src = FALSE /\ fds = 0 /\ sodium = FALSE
    /\ lock = 0
    /\ pc = [t \in T |-> "idle"]
    /\ terms = 0

(* rocketnet_crypto_ready: the first load of open *)
Start(t) ==
    /\ pc[t] = "idle"
    /\ pc' = [pc EXCEPT ![t] = IF "nolock" \in Defect \/ open THEN "draw" ELSE "wantlock"]
    /\ UNCHANGED <<open, src, fds, sodium, lock, terms>>

Acquire(t) ==
    /\ pc[t] = "wantlock" /\ lock = 0
    /\ lock' = t /\ pc' = [pc EXCEPT ![t] = "locked"]
    /\ UNCHANGED <<open, src, fds, sodium, terms>>

(* the second load of open, under the lock *)
Recheck(t) ==
    /\ pc[t] = "locked"
    /\ IF open
       THEN /\ lock' = 0 /\ pc' = [pc EXCEPT ![t] = "draw"]
       ELSE /\ lock' = lock /\ pc' = [pc EXCEPT ![t] = "stir"]
    /\ UNCHANGED <<open, src, fds, sodium, terms>>

(* sodium_init then randombytes_stir: each reads the flag first; sodium_init
   stirs only the first time it runs *)
StirCheck(t) ==
    /\ pc[t] = "stir"
    /\ LET stirs == ~sodium \/ "nostir" \notin Defect
       IN pc' = [pc EXCEPT ![t] = IF stirs /\ ~src THEN "stiropen" ELSE "publish"]
    /\ sodium' = TRUE
    /\ UNCHANGED <<open, src, fds, lock, terms>>

StirOpen(t) ==
    /\ pc[t] = "stiropen"
    /\ fds' = fds + 1 /\ src' = TRUE
    /\ pc' = [pc EXCEPT ![t] = "publish"]
    /\ UNCHANGED <<open, sodium, lock, terms>>

Publish(t) ==
    /\ pc[t] = "publish"
    /\ open' = TRUE /\ lock' = 0
    /\ pc' = [pc EXCEPT ![t] = "draw"]
    /\ UNCHANGED <<src, fds, sodium, terms>>

(* randombytes_buf: sodium's own unlocked open when the flag is clear *)
DrawCheck(t) ==
    /\ pc[t] = "draw"
    /\ pc' = [pc EXCEPT ![t] = IF src THEN "done" ELSE "drawopen"]
    /\ UNCHANGED <<open, src, fds, sodium, lock, terms>>

DrawOpen(t) ==
    /\ pc[t] = "drawopen"
    /\ fds' = fds + 1 /\ src' = TRUE
    /\ pc' = [pc EXCEPT ![t] = "done"]
    /\ UNCHANGED <<open, sodium, lock, terms>>

(* rocketnet_crypto_close from rocketnet_term: under the lock, with no call in
   flight; randombytes_close closes the one descriptor it holds *)
Term ==
    /\ terms < MaxTerms
    /\ lock = 0
    /\ \A t \in T : pc[t] \in {"idle", "done"}
    /\ fds' = IF src THEN fds - 1 ELSE fds
    /\ src' = FALSE
    /\ open' = IF "noclear" \in Defect THEN open ELSE FALSE
    /\ pc' = [t \in T |-> "idle"]
    /\ terms' = terms + 1
    /\ UNCHANGED <<sodium, lock>>

Done ==
    /\ terms = MaxTerms
    /\ \A t \in T : pc[t] = "done"
    /\ UNCHANGED vars

Next ==
    \/ \E t \in T : Start(t) \/ Acquire(t) \/ Recheck(t) \/ StirCheck(t)
                    \/ StirOpen(t) \/ Publish(t) \/ DrawCheck(t) \/ DrawOpen(t)
    \/ Term
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ open \in BOOLEAN /\ src \in BOOLEAN /\ sodium \in BOOLEAN
    /\ fds \in Nat
    /\ lock \in 0..Threads
    /\ pc \in [T -> {"idle", "wantlock", "locked", "stir", "stiropen", "publish", "draw", "drawopen", "done"}]
    /\ terms \in 0..MaxTerms

(* the source holds at most one descriptor: no open is lost *)
AtMostOneSource == fds <= 1

(* a published open flag means the source is open *)
OpenMeansSource == open => src

(* every draw that completes finds the source open *)
DrawnFromSource == \A t \in T : pc[t] = "done" => src
=============================================================================
