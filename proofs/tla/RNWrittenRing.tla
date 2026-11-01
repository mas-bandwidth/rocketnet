---------------------------- MODULE RNWrittenRing ----------------------------
(***************************************************************************)
(* A ring of N records whose emptiness is a written bit, not zeroed bytes  *)
(* (sdk/rocketnet_ack.cpp: the ack endpoint's sent and received rings,    *)
(* rocketnet_ack_endpoint_reset and every reader;                         *)
(* rocketnet_ack_endpoint_state_equal).                                    *)
(*                                                                         *)
(* The reference ring zeroes every record at reset, so a record is empty   *)
(* (sequence 0) until it is written. Reset leaves the modeled record      *)
(* fields unchanged: it clears one bit per record, sets a record's bit     *)
(* when it writes the record, and reads a record whose bit is clear as the *)
(* empty record without reading its bytes, which keep whatever the         *)
(* previous connection left (mem).  Sequences restart at every reset, so a *)
(* stale record can hold exactly the sequence a reader asks for: only the  *)
(* bit tells it apart. The SDK also clears 64 generation tags and a cached *)
(* base (520 bytes) in reclaimed received-record summary words; these are *)
(* outside this written-bit model. Sequence/time/byte fields stay intact. *)
(*                                                                         *)
(* ReadsAsReference: every read returns what the zeroing ring returns.     *)
(* NoUnwrittenRead: no read takes bytes from a record not written since    *)
(* the reset.                                                              *)
(*                                                                         *)
(* GuardRead = FALSE reads a record's bytes whatever its bit (a reader     *)
(* that skips the check); ClearOnReset = FALSE keeps the bits across a     *)
(* reset.  Each must fail: the reversed witnesses.                         *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS N,             \* the ring's records
          MaxSeq,        \* sequences 1 .. MaxSeq per connection
          GuardRead,     \* readers check the written bit first
          ClearOnReset   \* reset clears the written bits

ASSUME N \in Nat \ {0} /\ MaxSeq \in Nat \ {0}
ASSUME GuardRead \in BOOLEAN /\ ClearOnReset \in BOOLEAN

Slots == 0..(N - 1)
EMPTY == 0

VARIABLES ref,      \* the reference: the record each slot holds in a ring zeroed at reset
          mem,      \* the implementation's bytes: the sequence each slot's bytes hold
          written,  \* the slots whose bit is set
          next,     \* the next sequence this connection writes
          lastRead, \* the last read: [slot, got, want], got by the implementation, want by the reference
          rawRead   \* TRUE when the last read took an unwritten record's bytes

vars == <<ref, mem, written, next, lastRead, rawRead>>

Init ==
    /\ ref = [ s \in Slots |-> EMPTY ]
    /\ mem = [ s \in Slots |-> EMPTY ]       \* any stale value; zero is one
    /\ written = {}
    /\ next = 1
    /\ lastRead = [ slot |-> 0, got |-> EMPTY, want |-> EMPTY ]
    /\ rawRead = FALSE

\* record_sent / record_received: the record, then its bit
Write ==
    /\ next <= MaxSeq
    /\ LET s == next % N
       IN /\ ref' = [ ref EXCEPT ![s] = next ]
          /\ mem' = [ mem EXCEPT ![s] = next ]
          /\ written' = written \cup {s}
    /\ next' = next + 1
    /\ UNCHANGED <<lastRead, rawRead>>

\* any reader: a record read through its bit (the empty record when clear)
Read(s) ==
    /\ LET got == IF GuardRead /\ s \notin written THEN EMPTY ELSE mem[s]
       IN lastRead' = [ slot |-> s, got |-> got, want |-> ref[s] ]
    /\ rawRead' = ( s \notin written /\ ~GuardRead )
    /\ UNCHANGED <<ref, mem, written, next>>

\* rocketnet_ack_endpoint_reset: the bits cleared, the bytes kept; the
\* reference zeroes every record; the next connection starts at sequence 1
Reset ==
    /\ ref' = [ s \in Slots |-> EMPTY ]
    /\ written' = IF ClearOnReset THEN {} ELSE written
    /\ next' = 1
    /\ UNCHANGED <<mem, lastRead, rawRead>>

Next == Write \/ Reset \/ \E s \in Slots : Read(s)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ ref \in [ Slots -> 0..MaxSeq ]
    /\ mem \in [ Slots -> 0..MaxSeq ]
    /\ written \subseteq Slots
    /\ next \in 1..(MaxSeq + 1)

ReadsAsReference == lastRead.got = lastRead.want

NoUnwrittenRead == ~rawRead

\* the bits name exactly the records written since the reset
BitsMatchWrites == \A s \in Slots : ( s \in written ) <=> ( ref[s] # EMPTY )
=============================================================================
