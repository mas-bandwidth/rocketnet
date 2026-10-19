-------------------------- MODULE RNOutgoingBuild --------------------------
(***************************************************************************)
(* The outgoing build of one connection (docs/OUTGOING-BUILD.md): how a    *)
(* message the game starts, writes and finishes reaches the transport.     *)
(*                                                                         *)
(* A fitting unreliable message (UNRELIABLE, its receipt twin, and         *)
(* UNRELIABLE_SEQUENCED, every header and the form byte inside one wire    *)
(* frame) is written in place: start takes a transport frame and hands     *)
(* out a pointer inside it, finish seals and stages that same frame        *)
(* (sdk/rocketnet_message_server.cpp, rn_message_object_start and          *)
(* rn_message_object_finish; sdk/rocketnet_endpoint.cpp,                   *)
(* rn_endpoint_in_place_header and rn_endpoint_in_place_finish).  Every    *)
(* other message -- a reliable one, an oversize unreliable one, and a      *)
(* fitting unreliable one that fell back because the transport gave no     *)
(* frame or unreliable messages already wait in the queue -- is written    *)
(* into its region record and copied into a frame by the write pass in     *)
(* send_packets (rn_message_connection_write).                             *)
(*                                                                         *)
(* Buffers have identities: frames 1..NumFrames, and the region record of  *)
(* message m is NumFrames + m.  A message's bytes are one value, Data(m),  *)
(* which the game writes once.  Acks are the transport's: each packet      *)
(* sealed carries the acks due at its seal.                                *)
(*                                                                         *)
(* CopyStep is the rule switch of the reversed witness: TRUE restores the  *)
(* build before this change, in which a fitting unreliable message is      *)
(* written into its region record and copied into the frame at             *)
(* send_packets.                                                           *)
(*                                                                         *)
(* One update is: BeginUpdate (the packet allowance resets), the game's    *)
(* starts and finishes, and one or more send_packets flushes (BeginFlush,  *)
(* the write pass's packets, EndFlush).  The write pass sends reliable     *)
(* work first (its reliable phase) and unreliable records after.           *)
(* SharedQuota FALSE gives in-place packets an allowance of their own      *)
(* (PacketQuota fails); Reserve FALSE lets an in-place packet take the     *)
(* last of the allowance while reliable work is due                        *)
(* (ReliableSentWithinOneUpdate fails).                                    *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    N,              \* messages 1..N, started in this order (the game's send order)
    Kind,           \* Kind[m] \in {"U", "S", "R"}: unreliable (with or without receipt), sequenced, reliable
    Fits,           \* Fits[m]: every header and the form byte fit one wire frame
    NumFrames,      \* the frames the transport can have out at once
    MaxPackets,     \* the queued and in-place packet allowance in one update
    MaxAcks,        \* the acks the peer makes due in a run
    MaxFaults,      \* the faults in a run: a frame refused at start, an abort, an over-budget drop
    CopyStep,       \* TRUE: the copy step of the build before this change
    SharedQuota,    \* TRUE: in-place packets draw on the write pass's allowance (FALSE: their own)
    Reserve         \* TRUE: one packet of the allowance stays for due reliable work

Msgs == 1..N
Frames == 1..NumFrames
None == 0
Region(m) == NumFrames + m
Data(m) == m
Unreliable == {"U", "S"}
FitUnr(m) == Kind[m] \in Unreliable /\ Fits[m]
Outcomes == {"none", "started", "queued", "staged", "sent", "dropped", "aborted"}

VARIABLES
    next,           \* the next message the game starts
    pending,        \* the message whose start is pending, or None
    handed,         \* handed[m]: the buffer start returned for m, or None
    fellback,       \* fellback[m]: a fitting unreliable m took the queued path (no frame, or unreliable queued ahead)
    frameRecs,      \* frameRecs[f]: the records in frame f, a sequence of [msg, data]
    regionData,     \* regionData[m]: the bytes in m's region record (0 unwritten)
    frameFree,      \* frames the transport can hand out now
    queue,          \* the queued path: finished messages waiting for the write pass, in finish order
    staged,         \* sealed packets waiting for the flush: [buf, recs, acks]
    wire,           \* packets the transport sent, in order
    acksDue,        \* acks due and not yet carried
    acksMade,       \* acks the peer made due in all
    finished,       \* messages finished (not aborted), in finish order
    faults,         \* the faults so far
    outcome,
    packetsThisUpdate, \* queued and in-place packet attempts since BeginUpdate
    flushing,       \* a send_packets flush is running
    promised        \* the oldest reliable message due while the allowance had room, or None

vars == << next, pending, handed, fellback, frameRecs, regionData, frameFree, queue,
           staged, wire, acksDue, acksMade, finished, faults, outcome, packetsThisUpdate,
           flushing, promised >>

Rec == [msg : Msgs, data : 0..N]
Packet == [buf : Frames, recs : Seq(Rec), acks : 0..MaxAcks]

TypeOK ==
    /\ next \in 1..(N + 1)
    /\ pending \in Msgs \cup {None}
    /\ handed \in [Msgs -> Frames \cup {Region(m) : m \in Msgs} \cup {None}]
    /\ fellback \in [Msgs -> BOOLEAN]
    /\ frameRecs \in [Frames -> Seq(Rec)]
    /\ regionData \in [Msgs -> 0..N]
    /\ frameFree \subseteq Frames
    /\ queue \in Seq(Msgs)
    /\ staged \in Seq(Packet)
    /\ wire \in Seq(Packet)
    /\ acksDue \in 0..MaxAcks
    /\ acksMade \in 0..MaxAcks
    /\ finished \in Seq(Msgs)
    /\ faults \in 0..MaxFaults
    /\ outcome \in [Msgs -> Outcomes]
    /\ packetsThisUpdate \in Nat
    /\ flushing \in BOOLEAN
    /\ promised \in Msgs \cup {None}

Init ==
    /\ next = 1
    /\ pending = None
    /\ handed = [m \in Msgs |-> None]
    /\ fellback = [m \in Msgs |-> FALSE]
    /\ frameRecs = [f \in Frames |-> << >>]
    /\ regionData = [m \in Msgs |-> 0]
    /\ frameFree = Frames
    /\ queue = << >>
    /\ staged = << >>
    /\ wire = << >>
    /\ acksDue = 0
    /\ acksMade = 0
    /\ finished = << >>
    /\ faults = 0
    /\ outcome = [m \in Msgs |-> "none"]
    /\ packetsThisUpdate = 0
    /\ flushing = FALSE
    /\ promised = None

UnreliableQueued == \E i \in 1..Len(queue) : Kind[queue[i]] \in Unreliable
ReliableQueued == \E i \in 1..Len(queue) : Kind[queue[i]] = "R"
FirstReliable == IF ReliableQueued
                 THEN queue[CHOOSE i \in 1..Len(queue) : Kind[queue[i]] = "R" /\ \A j \in 1..(i - 1) : Kind[queue[j]] # "R"]
                 ELSE None

\* the allowance an in-place packet may take (rn_endpoint_in_place_header):
\* the shared one, less one packet while reliable work is due
InPlaceRoom ==
    IF ~SharedQuota THEN TRUE
    ELSE packetsThisUpdate + (IF Reserve /\ ReliableQueued THEN 1 ELSE 0) < MaxPackets

\* start: a fitting unreliable message takes a frame when the transport has
\* one and no unreliable message waits ahead of it in the queue.
StartInPlace ==
    /\ next <= N /\ pending = None
    /\ ~CopyStep
    /\ ~flushing
    /\ FitUnr(next)
    /\ InPlaceRoom
    /\ ~UnreliableQueued
    /\ \E f \in frameFree :
        /\ frameFree' = frameFree \ {f}
        /\ handed' = [handed EXCEPT ![next] = f]
        /\ frameRecs' = [frameRecs EXCEPT ![f] = << [msg |-> next, data |-> 0] >>]
    /\ pending' = next
    /\ next' = next + 1
    /\ outcome' = [outcome EXCEPT ![next] = "started"]
    /\ UNCHANGED << fellback, regionData, queue, staged, wire, acksDue, acksMade, finished, faults, packetsThisUpdate, flushing, promised >>

\* start on the queued path: a reliable or oversize message always; a fitting
\* unreliable one when the transport gave no frame (it may refuse at any
\* time: a full ring, a client not yet connected), when unreliable messages
\* wait ahead of it, when the allowance has no room for it (InPlaceRoom),
\* or, under CopyStep, always.
StartQueued ==
    /\ next <= N /\ pending = None /\ ~flushing
    /\ LET refused == FitUnr(next) /\ ~CopyStep /\ frameFree # {} /\ ~UnreliableQueued /\ InPlaceRoom IN
       /\ refused => faults < MaxFaults
       /\ faults' = IF refused THEN faults + 1 ELSE faults
    /\ handed' = [handed EXCEPT ![next] = Region(next)]
    /\ fellback' = [fellback EXCEPT ![next] = FitUnr(next) /\ ~CopyStep]
    /\ pending' = next
    /\ next' = next + 1
    /\ outcome' = [outcome EXCEPT ![next] = "started"]
    /\ UNCHANGED << frameRecs, regionData, frameFree, queue, staged, wire, acksDue, acksMade, finished, packetsThisUpdate, flushing, promised >>

\* the game writes its bytes at the pointer start returned, once
Write ==
    /\ pending # None /\ ~flushing
    /\ LET m == pending IN
       IF handed[m] \in Frames
       THEN /\ frameRecs[handed[m]][1].data = 0
            /\ frameRecs' = [frameRecs EXCEPT ![handed[m]] = << [msg |-> m, data |-> Data(m)] >>]
            /\ UNCHANGED regionData
       ELSE /\ regionData[m] = 0
            /\ regionData' = [regionData EXCEPT ![m] = Data(m)]
            /\ UNCHANGED frameRecs
    /\ UNCHANGED << next, pending, handed, fellback, frameFree, queue, staged, wire, acksDue, acksMade, finished, faults, outcome, packetsThisUpdate, flushing, promised >>

Written(m) == IF handed[m] \in Frames THEN frameRecs[handed[m]][1].data # 0 ELSE regionData[m] # 0

\* finish of an in-place message: the header and form byte are written
\* around the bytes and the same frame is sealed and staged; the acks due
\* ride in it (finish_packet writes the ack block)
FinishInPlace ==
    /\ pending # None /\ ~flushing /\ handed[pending] \in Frames /\ Written(pending)
    /\ LET m == pending
           f == handed[m] IN
       /\ staged' = Append(staged, [buf |-> f, recs |-> frameRecs[f], acks |-> acksDue])
       /\ acksDue' = 0
       /\ finished' = Append(finished, m)
       /\ outcome' = [outcome EXCEPT ![m] = "staged"]
    /\ packetsThisUpdate' = packetsThisUpdate + 1
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, frameRecs, regionData, frameFree, queue, wire, acksMade, faults, flushing, promised >>

\* finish of an in-place message the send budget cannot pay: dropped and
\* counted (MESSAGE_UNRELIABLE_OVER_BUDGET), the frame given back
FinishInPlaceOverBudget ==
    /\ pending # None /\ ~flushing /\ handed[pending] \in Frames /\ Written(pending)
    /\ faults < MaxFaults
    /\ faults' = faults + 1
    /\ LET m == pending IN
       /\ frameFree' = frameFree \cup {handed[m]}
       /\ frameRecs' = [frameRecs EXCEPT ![handed[m]] = << >>]
       /\ finished' = Append(finished, m)
       /\ outcome' = [outcome EXCEPT ![m] = "dropped"]
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, regionData, queue, staged, wire, acksDue, acksMade, packetsThisUpdate, flushing, promised >>

\* a reliable message queued while the allowance has room is due work the
\* update owes a packet: it is promised
FinishQueued ==
    /\ pending # None /\ ~flushing /\ handed[pending] \notin Frames /\ Written(pending)
    /\ queue' = Append(queue, pending)
    /\ finished' = Append(finished, pending)
    /\ outcome' = [outcome EXCEPT ![pending] = "queued"]
    /\ promised' = IF promised = None /\ Kind[pending] = "R" /\ packetsThisUpdate < MaxPackets
                   THEN pending ELSE promised
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, frameRecs, regionData, frameFree, staged, wire, acksDue, acksMade, faults, packetsThisUpdate, flushing >>

\* abort: an in-place frame goes back to the transport (abort_packet); a
\* region record's room goes back to the region
Abort ==
    /\ pending # None /\ ~flushing
    /\ faults < MaxFaults
    /\ faults' = faults + 1
    /\ IF handed[pending] \in Frames
       THEN /\ frameFree' = frameFree \cup {handed[pending]}
            /\ frameRecs' = [frameRecs EXCEPT ![handed[pending]] = << >>]
            /\ UNCHANGED regionData
       ELSE /\ regionData' = [regionData EXCEPT ![pending] = 0]
            /\ UNCHANGED << frameFree, frameRecs >>
    /\ outcome' = [outcome EXCEPT ![pending] = "aborted"]
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, queue, staged, wire, acksDue, acksMade, finished, packetsThisUpdate, flushing, promised >>

\* send_packets begins: the write pass.  No in-place start is pending here:
\* the transport forbids a flush with a fill open, so the object aborts it
\* first (Abort).
BeginFlush ==
    /\ ~flushing
    /\ queue # << >> \/ staged # << >>                \* an empty flush changes nothing
    /\ IF pending = None THEN TRUE ELSE handed[pending] \notin Frames
    /\ flushing' = TRUE
    /\ UNCHANGED << next, pending, handed, fellback, frameRecs, regionData, frameFree, queue,
                    staged, wire, acksDue, acksMade, finished, faults, outcome, packetsThisUpdate, promised >>

\* one packet of the write pass, from the queued records at positions idx
\* (in queue order), copied from their region records into a frame, sealed
\* and staged with the acks due
WritePacket(idx) ==
    \E f \in frameFree :
        LET recs == [i \in 1..Len(idx) |-> [msg |-> queue[idx[i]], data |-> regionData[queue[idx[i]]]]]
            taken == {queue[idx[i]] : i \in 1..Len(idx)} IN
        /\ frameFree' = frameFree \ {f}
        /\ frameRecs' = [frameRecs EXCEPT ![f] = recs]
        /\ staged' = Append(staged, [buf |-> f, recs |-> recs, acks |-> acksDue])
        /\ queue' = SelectSeq(queue, LAMBDA m : m \notin taken)
        /\ regionData' = [m \in Msgs |-> IF m \in taken THEN 0 ELSE regionData[m]]
        /\ outcome' = [m \in Msgs |-> IF m \in taken THEN "staged" ELSE outcome[m]]
        /\ acksDue' = 0
        /\ packetsThisUpdate' = packetsThisUpdate + 1
        /\ UNCHANGED << next, pending, handed, fellback, wire, acksMade, finished, faults, flushing, promised >>

RIndexes == SelectSeq([i \in 1..Len(queue) |-> i], LAMBDA i : Kind[queue[i]] = "R")

\* the reliable phase: due reliable work first, the oldest reliable record
\* alone, or every queued reliable record (a shorter packet is the packet full)
SendReliable ==
    /\ flushing
    /\ packetsThisUpdate < MaxPackets
    /\ ReliableQueued
    /\ \E k \in {1, Len(RIndexes)} : WritePacket(SubSeq(RIndexes, 1, k))

\* the unreliable phase, once no reliable work is due: the first queued
\* record alone, or the whole queue
SendUnreliable ==
    /\ flushing
    /\ packetsThisUpdate < MaxPackets
    /\ queue # << >> /\ ~ReliableQueued
    /\ \E k \in {1, Len(queue)} : WritePacket([i \in 1..k |-> i])

\* send_packets returns once the transport has every packet and the write
\* pass has nothing more it may send this update
EndFlush ==
    /\ flushing
    /\ staged = << >>
    /\ queue = << >> \/ packetsThisUpdate >= MaxPackets
    /\ flushing' = FALSE
    /\ promised' = IF promised # None /\ outcome[promised] = "queued" THEN promised ELSE None
    /\ UNCHANGED << next, pending, handed, fellback, frameRecs, regionData, frameFree, queue,
                    staged, wire, acksDue, acksMade, finished, faults, outcome, packetsThisUpdate >>

\* the transport sends every staged packet, in staging order, and their
\* frames are free again; staged packets go out at the flush (send_packets)
TransportSend ==
    /\ flushing
    /\ staged # << >>
    /\ LET bufs == {staged[i].buf : i \in 1..Len(staged)}
           msgs == UNION {{staged[i].recs[j].msg : j \in 1..Len(staged[i].recs)} : i \in 1..Len(staged)} IN
       /\ wire' = wire \o staged
       /\ staged' = << >>
       /\ frameFree' = frameFree \cup bufs
       /\ frameRecs' = [f \in Frames |-> IF f \in bufs THEN << >> ELSE frameRecs[f]]
       /\ outcome' = [m \in Msgs |-> IF m \in msgs THEN "sent" ELSE outcome[m]]
    /\ UNCHANGED << next, pending, handed, fellback, regionData, queue, acksDue, acksMade, finished, faults, packetsThisUpdate, flushing, promised >>

\* the next update (rn_endpoint_begin_update): the allowance resets, and a
\* reliable message still queued is due work this update owes a packet.
\* The transport's update reads the peer's packets, so this is where acks
\* become due (at most MaxAcks in a run), never inside a start or a flush.
BeginUpdate ==
    /\ pending = None /\ ~flushing
    /\ packetsThisUpdate > 0
    /\ packetsThisUpdate' = 0
    /\ promised' = IF promised = None THEN FirstReliable ELSE promised
    /\ \/ UNCHANGED << acksDue, acksMade >>
       \/ /\ acksMade < MaxAcks
          /\ acksDue' = acksDue + 1
          /\ acksMade' = acksMade + 1
    /\ UNCHANGED << next, pending, handed, fellback, frameRecs, regionData, frameFree, queue, staged, wire, finished, faults, outcome, flushing >>

Next ==
    \/ StartInPlace \/ StartQueued \/ Write
    \/ FinishInPlace \/ FinishInPlaceOverBudget \/ FinishQueued \/ Abort
    \/ BeginFlush \/ SendReliable \/ SendUnreliable \/ EndFlush
    \/ TransportSend \/ BeginUpdate

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* the packets the transport has: staged, then sent

Out == wire \o staged

RECURSIVE SumAcks(_)
SumAcks(s) == IF s = << >> THEN 0 ELSE Head(s).acks + SumAcks(Tail(s))

RECURSIVE Flat(_)
Flat(s) == IF s = << >> THEN << >> ELSE [i \in 1..Len(Head(s).recs) |-> Head(s).recs[i].msg] \o Flat(Tail(s))

OutMsgs == Flat(wire) \o Flat(staged)

OfKind(s, k) == SelectSeq(s, LAMBDA m : Kind[m] = k)
Delivered(s) == SelectSeq(s, LAMBDA m : outcome[m] # "dropped")

IsPrefix(a, b) == Len(a) <= Len(b) /\ a = SubSeq(b, 1, Len(a))

\* in-place and queued packets together stay within the allowance
PacketQuota == packetsThisUpdate <= MaxPackets

\* due reliable work is sent within one update: a reliable message queued
\* while the update's allowance had room, or still queued when the next
\* update began, is with the transport when send_packets returns.  A
\* bounded response ("within one update"), so TLC checks it as an action
\* property; an unbounded ~> would hold in a finite run even without the
\* reservation, because the game's in-place sends run out.
ReliableSentWithinOneUpdate ==
    [][flushing /\ ~flushing' => (promised = None \/ outcome[promised] # "queued")]_vars

\* every byte the game wrote reaches the transport unchanged

BytesUnchangedInPlace ==
    LET out == Out IN
    \A i \in 1..Len(out) : \A j \in 1..Len(out[i].recs) :
        out[i].recs[j].data = Data(out[i].recs[j].msg)

\* a fitting unreliable message that did not fall back is in the very
\* buffer start handed out: buffer identity, not equal contents
NoCopy ==
    LET out == Out IN
    \A i \in 1..Len(out) : \A j \in 1..Len(out[i].recs) :
        LET m == out[i].recs[j].msg IN
        FitUnr(m) /\ ~fellback[m] => out[i].buf = handed[m]

\* every ack made due is still due or rides a staged or sent packet
AcksNeverLost == acksMade = acksDue + SumAcks(wire) + SumAcks(staged)

\* no message is lost or duplicated, and each mode keeps its order: within
\* each kind the messages the transport has are a prefix of that kind's
\* finished, undropped messages in finish order (the sequenced drop rule is
\* the receiver's and starts from this order); every finished, undropped
\* message is queued or with the transport
ModeOrder ==
    LET out == OutMsgs
        outSet == {out[b] : b \in 1..Len(out)}
        queued == {queue[a] : a \in 1..Len(queue)}
        delivered == Delivered(finished) IN
    /\ \A k \in {"U", "S", "R"} : IsPrefix(OfKind(out, k), OfKind(delivered, k))
    /\ \A i \in 1..Len(finished) :
        LET m == finished[i] IN
        outcome[m] # "dropped" => m \in queued \/ m \in outSet
    /\ Cardinality(outSet) = Len(out)
=============================================================================
