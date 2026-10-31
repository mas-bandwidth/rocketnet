-------------------------- MODULE RNCryptoSource --------------------------
(* The bundled sodium's random source, opened and closed beside the key and
   token generators (sdk/rocketnet.cpp: rocketnet_crypto_acquire,
   rocketnet_crypto_release, rocketnet_crypto_open_locked,
   rocketnet_crypto_close; the bundled sodium's randombytes_sysrandom_stir and
   randombytes_sysrandom_buf).

   The source is sodium's flag (stream.initialized, here src) and a
   descriptor. Sodium opens it in two unlocked steps: it reads the flag and,
   when clear, opens a descriptor and sets the flag. A draw (randombytes_buf)
   does that on its own whenever the flag is clear.

   A generator call takes rocketnet_crypto_mutex (waiting while a term is
   closing), opens the source when open is clear (sodium_init, which stirs
   only the first time it runs, then randombytes_stir), counts itself a
   holder and lets the mutex go. It draws, then takes the mutex to uncount
   itself. rocketnet_init opens the source the same way without holding it.
   Term (rocketnet_term on macOS) takes the mutex, marks itself closing, waits
   for zero holders, closes the source and clears open, at any time.

   Each thread makes MaxCalls calls; terms happen at most MaxTerms times. The
   mutex is excl; the holder count is the set holders.

   Defect selects the reversed witness:
     "nolock"     a call draws with no lock and no open (the code before the
                  guard): AtMostOneSource
     "nohold"     a call opens the source but does not count itself a holder,
                  so a term closes the source under its draw: AtMostOneSource
     "noclear"    term closes the source but leaves open set: AtMostOneSource
     "nostir"     the open runs sodium_init alone, which opens nothing once
                  sodium is initialized: AtMostOneSource *)
EXTENDS Integers, FiniteSets
CONSTANTS Threads, MaxCalls, MaxTerms, Defect
ASSUME Threads \in Nat \ {0} /\ MaxCalls \in Nat \ {0} /\ MaxTerms \in Nat
ASSUME Defect \subseteq {"nolock", "nohold", "noclear", "nostir"}

T == 1..Threads
Free == 0

VARIABLES open,     \* rocketnet_crypto_open, read and written under the mutex
          src,      \* sodium's stream.initialized
          fds,      \* descriptors the source holds
          sodium,   \* sodium_init has run
          holders,  \* threads counted in rocketnet_crypto_holders
          excl,     \* the mutex holder: a thread, or Free
          pc,       \* each thread's step
          calls,    \* calls each thread has finished
          terms     \* terms completed
vars == <<open, src, fds, sodium, holders, excl, pc, calls, terms>>

Init ==
    /\ open = FALSE /\ src = FALSE /\ fds = 0 /\ sodium = FALSE
    /\ holders = {} /\ excl = Free
    /\ pc = [t \in T |-> "idle"]
    /\ calls = [t \in T |-> 0]
    /\ terms = 0

(* rocketnet_crypto_acquire: lock the mutex once no term is closing (Term
   is one step, so a closing term never leaves the mutex free) *)
Acquire(t) ==
    /\ pc[t] = "idle" /\ calls[t] < MaxCalls
    /\ IF "nolock" \in Defect
       THEN /\ pc' = [pc EXCEPT ![t] = "draw"] /\ UNCHANGED excl
       ELSE /\ excl = Free
            /\ excl' = t /\ pc' = [pc EXCEPT ![t] = "locked"]
    /\ UNCHANGED <<open, src, fds, sodium, holders, calls, terms>>

Hold(t) == IF "nohold" \in Defect THEN holders ELSE holders \cup {t}

(* rocketnet_crypto_open_locked: read open; when set, count the holder and
   unlock *)
Recheck(t) ==
    /\ pc[t] = "locked"
    /\ IF open
       THEN /\ holders' = Hold(t) /\ excl' = Free /\ pc' = [pc EXCEPT ![t] = "draw"]
       ELSE /\ UNCHANGED <<holders, excl>> /\ pc' = [pc EXCEPT ![t] = "stir"]
    /\ UNCHANGED <<open, src, fds, sodium, calls, terms>>

(* sodium_init then randombytes_stir: the stir reads the flag first *)
StirCheck(t) ==
    /\ pc[t] = "stir"
    /\ LET stirs == ~sodium \/ "nostir" \notin Defect
       IN pc' = [pc EXCEPT ![t] = IF stirs /\ ~src THEN "stiropen" ELSE "publish"]
    /\ sodium' = TRUE
    /\ UNCHANGED <<open, src, fds, holders, excl, calls, terms>>

StirOpen(t) ==
    /\ pc[t] = "stiropen"
    /\ fds' = fds + 1 /\ src' = TRUE
    /\ pc' = [pc EXCEPT ![t] = "publish"]
    /\ UNCHANGED <<open, sodium, holders, excl, calls, terms>>

(* set open, count the holder, unlock *)
Publish(t) ==
    /\ pc[t] = "publish"
    /\ open' = TRUE /\ holders' = Hold(t) /\ excl' = Free
    /\ pc' = [pc EXCEPT ![t] = "draw"]
    /\ UNCHANGED <<src, fds, sodium, calls, terms>>

(* randombytes_buf: sodium's own unlocked open when the flag is clear *)
DrawCheck(t) ==
    /\ pc[t] = "draw"
    /\ pc' = [pc EXCEPT ![t] = IF src THEN "drawn" ELSE "drawopen"]
    /\ UNCHANGED <<open, src, fds, sodium, holders, excl, calls, terms>>

DrawOpen(t) ==
    /\ pc[t] = "drawopen"
    /\ fds' = fds + 1 /\ src' = TRUE
    /\ pc' = [pc EXCEPT ![t] = "drawn"]
    /\ UNCHANGED <<open, sodium, holders, excl, calls, terms>>

(* rocketnet_crypto_release: uncount the holder under the mutex *)
Release(t) ==
    /\ pc[t] = "drawn" /\ excl = Free
    /\ holders' = holders \ {t}
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ calls' = [calls EXCEPT ![t] = calls[t] + 1]
    /\ UNCHANGED <<open, src, fds, sodium, excl, terms>>

(* rocketnet_crypto_close from rocketnet_term, at any time: with the mutex
   and no holder left, close and clear open *)
Term ==
    /\ terms < MaxTerms
    /\ excl = Free /\ holders = {}
    /\ fds' = IF src THEN fds - 1 ELSE fds
    /\ src' = FALSE
    /\ open' = IF "noclear" \in Defect THEN open ELSE FALSE
    /\ terms' = terms + 1
    /\ UNCHANGED <<sodium, holders, excl, pc, calls>>

Done ==
    /\ \A t \in T : pc[t] = "idle" /\ calls[t] = MaxCalls
    /\ UNCHANGED vars

Next ==
    \/ \E t \in T : Acquire(t) \/ Recheck(t) \/ StirCheck(t)
                    \/ StirOpen(t) \/ Publish(t) \/ DrawCheck(t) \/ DrawOpen(t) \/ Release(t)
    \/ Term
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ open \in BOOLEAN /\ src \in BOOLEAN /\ sodium \in BOOLEAN
    /\ fds \in Nat
    /\ holders \subseteq T
    /\ excl \in 0..Threads
    /\ pc \in [T -> {"idle", "locked", "stir", "stiropen", "publish", "draw", "drawopen", "drawn"}]
    /\ calls \in [T -> 0..MaxCalls]
    /\ terms \in 0..MaxTerms

(* the source holds at most one descriptor: no open is lost *)
AtMostOneSource == fds <= 1

(* open set means the source is open *)
OpenMeansSource == open => src

(* a counted draw finds the source open until it releases *)
HeldDrawFromSource == \A t \in holders : pc[t] = "drawn" => src
=============================================================================
