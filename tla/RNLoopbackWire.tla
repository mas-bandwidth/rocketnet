---------------------------- MODULE RNLoopbackWire ----------------------------
(***************************************************************************)
(* The loopback wire of sdk/rocketnet_driver_loopback.cpp: one receiving   *)
(* endpoint's inbox and the caller threads that send to it.  Senders (a    *)
(* client in its send_packets, or the server in its send_packets, each on  *)
(* its own thread) produce; the receiver (in its update, on its thread)    *)
(* consumes.  There is no thread of the transport's own and no wake: the   *)
(* receiver takes what is ready at its next update.                        *)
(*                                                                         *)
(* The inbox is N slots of one datagram, preallocated with the receiving   *)
(* endpoint, with the free and ready queues of                             *)
(* sdk/rocketnet_buffer_handoff.h.  Each slot is owned by exactly one of   *)
(* the free queue, one sender (reserved, being filled), the ready queue    *)
(* (filled) and the receiver (taken, being read).  Only indices move, in   *)
(* one exchange under the inbox mutex (one atomic action here); the        *)
(* datagram is written after the sender's exchange and read after the      *)
(* receiver's, outside the mutex.  The sender's exchange reserves a free   *)
(* slot and appends it to the ready queue in the same step, so the queue   *)
(* keeps send order; the slot's filled flag (a release store after the     *)
(* write, an acquire load before the take) hands it to the receiver, which *)
(* takes only the filled prefix of the queue.                              *)
(*                                                                         *)
(* The inbox lives in the receiving endpoint, so a datagram survives its   *)
(* sender's destroy, as it does in a socket buffer.  A sender pins the     *)
(* receiving endpoint before its exchange and checks it is live after the  *)
(* pin; the receiver's destroy ends liveness under the table mutex and     *)
(* frees the inbox only once no pin is held (loopback_pin, loopback_destroy).*)
(*                                                                         *)
(* Defect switches, FALSE in the passing run, each restored by one         *)
(* reversed witness: FillLate (send returns before its datagram is filled; *)
(* a later send fills it), CheckBeforePin (liveness is read before the pin *)
(* is taken), ReleaseBeforeCopy (the receiver frees what it took before    *)
(* reading it), TakeUnfilled (the receiver takes a reserved slot that is   *)
(* not filled yet), NoDestroyWait (destroy frees with a pin held) and      *)
(* DropWithSender (the sender's destroy discards its datagrams in flight,  *)
(* the inbox kept with the sender).                                        *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Senders,      \* the sending threads
          N,            \* slots in the inbox
          K,            \* slots the receiver takes in one exchange
          MaxItems,     \* datagrams each sender sends in one behaviour
          FillLate, CheckBeforePin, ReleaseBeforeCopy, TakeUnfilled,
          NoDestroyWait, DropWithSender

ASSUME N >= 1 /\ K >= 1 /\ MaxItems >= 1

Slots == 0 .. N - 1
Datagrams == Senders \X ( 1 .. MaxItems )
NOTHING == << 0, 0 >>

VARIABLES
    spc,        \* per sender: "idle", "checked", "pinned", "verified", "reserved", "late", "gone"
    sslot,      \* per sender: the slot it reserved, or N
    sent,       \* per sender: datagrams sent so far
    rpc,        \* receiver: "take", "copy", "release", "copied", "waitpins", "freed"
    ready,      \* the ready queue of slots, oldest first (filled or being filled)
    free,       \* the free queue
    taken,      \* the slots the receiver's last exchange took
    filled,     \* per slot: the sender finished writing it
    content,    \* per slot: the datagram in it
    consumed,   \* the datagrams the receiver read, in order
    dropped,    \* the datagrams refused at a full inbox or a dead receiver
    lost,       \* the datagrams discarded by DropWithSender
    live,       \* the receiving endpoint is live
    pins,       \* pins senders hold on it
    freed,      \* its inbox was freed
    touchedFreed \* a sender wrote or moved inbox state after the free

vars == << spc, sslot, sent, rpc, ready, free, taken, filled, content, consumed,
           dropped, lost, live, pins, freed, touchedFreed >>

Range(s) == { s[i] : i \in 1 .. Len(s) }
RECURSIVE Items(_)
Items(s) == IF s = << >> THEN << >> ELSE << content[Head(s)] >> \o Items(Tail(s))

\* the longest prefix of the ready queue that is filled, at most K long
RECURSIVE FilledPrefix(_, _)
FilledPrefix(s, k) ==
    IF s = << >> \/ k = 0 \/ ~( filled[Head(s)] \/ TakeUnfilled ) THEN << >>
    ELSE << Head(s) >> \o FilledPrefix( Tail(s), k - 1 )

Init ==
    /\ spc = [ p \in Senders |-> "idle" ]
    /\ sslot = [ p \in Senders |-> N ]
    /\ sent = [ p \in Senders |-> 0 ]
    /\ rpc = "take"
    /\ ready = << >>
    /\ free = [ i \in 1 .. N |-> i - 1 ]
    /\ taken = << >>
    /\ filled = [ s \in Slots |-> FALSE ]
    /\ content = [ s \in Slots |-> NOTHING ]
    /\ consumed = << >>
    /\ dropped = {}
    /\ lost = {}
    /\ live = TRUE
    /\ pins = 0
    /\ freed = FALSE
    /\ touchedFreed = FALSE

(* ------------------------------------------------------------------ *)
(* a sender: loopback_deliver                                           *)

Pin(p) ==
    /\ spc[p] = "idle"
    /\ sent[p] < MaxItems
    /\ IF CheckBeforePin
       THEN /\ live
            /\ spc' = [ spc EXCEPT ![p] = "checked" ]
            /\ UNCHANGED pins
       ELSE /\ spc' = [ spc EXCEPT ![p] = "pinned" ]
            /\ pins' = pins + 1
    /\ UNCHANGED << sslot, sent, rpc, ready, free, taken, filled, content, consumed, dropped, lost, live, freed, touchedFreed >>

(* CheckBeforePin: the pin follows a liveness read it does not repeat   *)
PinAfterCheck(p) ==
    /\ spc[p] = "checked"
    /\ spc' = [ spc EXCEPT ![p] = "verified" ]
    /\ pins' = pins + 1
    /\ UNCHANGED << sslot, sent, rpc, ready, free, taken, filled, content, consumed, dropped, lost, live, freed, touchedFreed >>

(* liveness is read after the pin; a dead receiver drops the datagram   *)
Verify(p) ==
    /\ spc[p] = "pinned"
    /\ IF live
       THEN /\ spc' = [ spc EXCEPT ![p] = "verified" ]
            /\ UNCHANGED << pins, sent, dropped >>
       ELSE /\ spc' = [ spc EXCEPT ![p] = "idle" ]
            /\ pins' = pins - 1
            /\ sent' = [ sent EXCEPT ![p] = @ + 1 ]
            /\ dropped' = dropped \cup { << p, sent[p] + 1 >> }
    /\ UNCHANGED << sslot, rpc, ready, free, taken, filled, content, consumed, lost, live, freed, touchedFreed >>

(* the exchange: a free slot is reserved and appended to the ready      *)
(* queue; a full inbox drops the datagram                               *)
Reserve(p) ==
    /\ spc[p] = "verified"
    /\ touchedFreed' = ( touchedFreed \/ freed )
    /\ IF Len( free ) > 0
       THEN /\ sslot' = [ sslot EXCEPT ![p] = Head( free ) ]
            /\ free' = Tail( free )
            /\ ready' = Append( ready, Head( free ) )
            /\ spc' = [ spc EXCEPT ![p] = "reserved" ]
            /\ UNCHANGED << sent, dropped, pins >>
       ELSE /\ spc' = [ spc EXCEPT ![p] = "idle" ]
            /\ sent' = [ sent EXCEPT ![p] = @ + 1 ]
            /\ dropped' = dropped \cup { << p, sent[p] + 1 >> }
            /\ pins' = pins - 1
            /\ UNCHANGED << sslot, free, ready >>
    /\ UNCHANGED << rpc, taken, filled, content, consumed, lost, live, freed >>

(* the write and the filled flag, outside the mutex; then the unpin and *)
(* send returns                                                          *)
Fill(p) ==
    /\ spc[p] \in { "reserved", "late" }
    /\ touchedFreed' = ( touchedFreed \/ freed )
    /\ content' = [ content EXCEPT ![sslot[p]] = << p, sent[p] + 1 >> ]
    /\ filled' = [ filled EXCEPT ![sslot[p]] = TRUE ]
    /\ sent' = [ sent EXCEPT ![p] = @ + 1 ]
    /\ sslot' = [ sslot EXCEPT ![p] = N ]
    /\ spc' = [ spc EXCEPT ![p] = "idle" ]
    /\ pins' = pins - 1
    /\ UNCHANGED << rpc, ready, free, taken, consumed, dropped, lost, live, freed >>

(* FillLate: send returns with its slot reserved and empty              *)
Return(p) ==
    /\ FillLate
    /\ spc[p] = "reserved"
    /\ spc' = [ spc EXCEPT ![p] = "late" ]
    /\ UNCHANGED << sslot, sent, rpc, ready, free, taken, filled, content, consumed, dropped, lost, live, pins, freed, touchedFreed >>

(* the sender's endpoint is destroyed between its calls.  Its datagrams *)
(* in the inbox stay; DropWithSender discards them                      *)
Leave(p) ==
    /\ spc[p] = "idle"
    /\ spc' = [ spc EXCEPT ![p] = "gone" ]
    /\ IF DropWithSender
       THEN LET mine == { i \in 1 .. Len( ready ) : content[ready[i]][1] = p }
                keep == SelectSeq( ready, LAMBDA s : content[s][1] # p )
            IN /\ lost' = lost \cup { content[ready[i]] : i \in mine }
               /\ ready' = keep
               /\ free' = free \o SelectSeq( ready, LAMBDA s : content[s][1] = p )
               /\ filled' = [ s \in Slots |-> IF s \in Range( ready ) /\ content[s][1] = p THEN FALSE ELSE filled[s] ]
               /\ content' = [ s \in Slots |-> IF s \in Range( ready ) /\ content[s][1] = p THEN NOTHING ELSE content[s] ]
       ELSE UNCHANGED << lost, ready, free, filled, content >>
    /\ UNCHANGED << sslot, sent, rpc, taken, consumed, dropped, live, pins, freed, touchedFreed >>

(* ------------------------------------------------------------------ *)
(* the receiver: loopback_take, in its update                            *)

Take ==
    /\ rpc = "take"
    /\ LET batch == FilledPrefix( ready, K )
       IN /\ taken' = batch
          /\ ready' = SubSeq( ready, Len( batch ) + 1, Len( ready ) )
    /\ rpc' = IF ReleaseBeforeCopy THEN "release" ELSE "copy"
    /\ UNCHANGED << spc, sslot, sent, free, filled, content, consumed, dropped, lost, live, pins, freed, touchedFreed >>

Copy ==
    /\ rpc \in { "copy", "copied" }
    /\ consumed' = consumed \o Items( taken )
    /\ rpc' = IF ReleaseBeforeCopy THEN "take" ELSE "release"
    /\ taken' = IF ReleaseBeforeCopy THEN << >> ELSE taken
    /\ UNCHANGED << spc, sslot, sent, ready, free, filled, content, dropped, lost, live, pins, freed, touchedFreed >>

(* the flags are cleared before the exchange that frees the slots       *)
Release ==
    /\ rpc = "release"
    /\ free' = free \o taken
    /\ filled' = [ s \in Slots |-> IF s \in Range( taken ) THEN FALSE ELSE filled[s] ]
    /\ taken' = IF ReleaseBeforeCopy THEN taken ELSE << >>
    /\ rpc' = IF ReleaseBeforeCopy THEN "copied" ELSE "take"
    /\ UNCHANGED << spc, sslot, sent, ready, content, consumed, dropped, lost, live, pins, freed, touchedFreed >>

(* the receiver's destroy, between its updates                          *)
Destroy ==
    /\ rpc = "take"
    /\ live' = FALSE
    /\ rpc' = "waitpins"
    /\ UNCHANGED << spc, sslot, sent, ready, free, taken, filled, content, consumed, dropped, lost, pins, freed, touchedFreed >>

Free ==
    /\ rpc = "waitpins"
    /\ pins = 0 \/ NoDestroyWait
    /\ freed' = TRUE
    /\ rpc' = "freed"
    /\ UNCHANGED << spc, sslot, sent, ready, free, taken, filled, content, consumed, dropped, lost, live, pins, touchedFreed >>

(* every thread has finished: the behaviour stutters here               *)
Done ==
    /\ \A p \in Senders : spc[p] = "gone" \/ ( spc[p] = "idle" /\ sent[p] = MaxItems )
    /\ rpc \in { "take", "freed" }
    /\ UNCHANGED vars

Next ==
    \/ Done
    \/ \E p \in Senders : Pin(p) \/ PinAfterCheck(p) \/ Verify(p) \/ Reserve(p) \/ Fill(p) \/ Return(p) \/ Leave(p)
    \/ Take \/ Copy \/ Release \/ Destroy \/ Free

Spec == Init /\ [][Next]_vars

(* senders finish what they start and the receiver keeps updating;     *)
(* neither leaving nor destroy is forced                                 *)
SpecFair ==
    /\ Spec
    /\ \A p \in Senders : WF_vars( Verify(p) ) /\ WF_vars( Reserve(p) ) /\ WF_vars( Fill(p) )
    /\ WF_vars( Take ) /\ WF_vars( Copy ) /\ WF_vars( Release ) /\ WF_vars( Free )

----------------------------------------------------------------------------

TypeOK ==
    /\ spc \in [ Senders -> { "idle", "checked", "pinned", "verified", "reserved", "late", "gone" } ]
    /\ sent \in [ Senders -> 0 .. MaxItems ]
    /\ pins \in 0 .. Cardinality( Senders )

(* every slot has exactly one owner: free, ready (a sender owns it      *)
(* while it is unfilled) or taken                                       *)
OneOwner ==
    LET owners(s) == Len( SelectSeq( free, LAMBDA x : x = s ) )
                   + Len( SelectSeq( ready, LAMBDA x : x = s ) )
                   + Len( SelectSeq( taken, LAMBDA x : x = s ) )
    IN \A s \in Slots : owners(s) = 1

(* no slot is written by a sender while the receiver may read it, and   *)
(* no two senders hold one slot                                          *)
NoOverlap ==
    /\ \A p \in Senders : sslot[p] # N => sslot[p] \notin Range( taken )
    /\ \A p, q \in Senders : ( p # q /\ sslot[p] # N ) => sslot[p] # sslot[q]

(* the receiver reads only datagrams, each sender's in its send order,  *)
(* each once                                                             *)
InOrder ==
    /\ \A i \in 1 .. Len( consumed ) : consumed[i] # NOTHING
    /\ \A i, j \in 1 .. Len( consumed ) :
         ( i < j /\ consumed[i][1] = consumed[j][1] ) => consumed[i][2] < consumed[j][2]

(* nothing is lost: every datagram sent is in the inbox (filled), being *)
(* read, read, or dropped at the send; a sender's destroy loses nothing *)
NoLoss ==
    /\ lost = {}
    /\ \A p \in Senders : \A n \in 1 .. sent[p] :
         LET d == << p, n >>
         IN \/ d \in dropped
            \/ d \in Range( consumed )
            \/ \E s \in Range( ready ) \cup Range( taken ) : content[s] = d
            \/ freed

(* same frame: when send returns its datagram is filled and ready for   *)
(* the receiver's next exchange                                          *)
SentIsVisible == \A p \in Senders : spc[p] \in { "idle", "gone", "late" } => sslot[p] = N

(* no sender touches the inbox after the receiver's destroy freed it    *)
NoUseAfterFree == ~touchedFreed

(* while the receiver lives, every filled datagram is read               *)
FilledRead == \A s \in Slots : ( live /\ filled[s] ) ~> ( ~filled[s] \/ ~live )

=============================================================================
