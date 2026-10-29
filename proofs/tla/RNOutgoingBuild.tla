-------------------------- MODULE RNOutgoingBuild --------------------------
(***************************************************************************)
(* The outgoing build of one connection (docs/OUTGOING-BUILD.md): how a    *)
(* message the game starts, writes and finishes reaches the transport.     *)
(*                                                                         *)
(* A fitting unreliable message (UNRELIABLE, its receipt twin, and         *)
(* UNRELIABLE_SEQUENCED, every header and the form byte inside one wire    *)
(* frame) is written in place, packed: the connection keeps one open       *)
(* transport frame, its build, which holds a committed prefix of records   *)
(* back to back.  Start reserves the message's declared maximum at the     *)
(* committed end (the offset) and hands out a pointer there; finish        *)
(* commits the bytes actually written, so a short finish returns the       *)
(* rest of the reservation, and an abort returns all of it; no committed   *)
(* record moves.  Later records append to the same frame until it is full  *)
(* (the next declared maximum does not fit) or a flush seals it:           *)
(* send_packets, update, or a start that falls back to the queued path,    *)
(* which must not overtake the frame's records                             *)
(* (sdk/rocketnet_message_server.cpp, rn_message_object_start,             *)
(* rn_message_object_finish, rn_message_connection_cancel_pending and      *)
(* rn_message_flush_unreliable; sdk/rocketnet_endpoint.cpp,                *)
(* rn_endpoint_in_place_header and rn_endpoint_in_place_affordable).  The  *)
(* whole-frame path the code keeps for a plain message that fits SINGLE    *)
(* but not with a record header is the case of a declared maximum equal   *)
(* to the frame's room: it opens a frame and fills it.  Every other        *)
(* message -- a reliable one, an oversize unreliable one, and a fitting    *)
(* unreliable one that fell back because the transport gave no frame or    *)
(* unreliable messages already wait in the queue -- is written into its    *)
(* region record and copied into a frame by the write pass in              *)
(* send_packets (rn_message_connection_write).                             *)
(*                                                                         *)
(* Buffers have identities: frames 1..NumFrames, and the region record of  *)
(* message m is NumFrames + m.  A frame's payload is Cap units; message m  *)
(* declares Decl[m] units and the game writes 1..Decl[m] of them, each     *)
(* unit holding Data(m).  Acks are the transport's: each packet sealed     *)
(* carries the acks due at its seal.                                       *)
(*                                                                         *)
(* CopyStep is the rule switch of the reversed witness: TRUE restores the  *)
(* build before the in-place change, in which a fitting unreliable         *)
(* message is written into its region record and copied into the frame at *)
(* send_packets.  MovePrior and ReuseCommitted are the two packing         *)
(* witnesses: MovePrior puts each new record at the front of the frame,    *)
(* moving the committed ones behind it (PriorRecordsUnmoved fails);        *)
(* ReuseCommitted leaves the committed end where it was at finish, so the  *)
(* next reservation overlays committed bytes (BytesUnchangedInPlace        *)
(* fails).                                                                 *)
(*                                                                         *)
(* One update is: BeginUpdate (the open frame sealed, the packet allowance *)
(* reset), the game's starts and finishes, and one or more send_packets    *)
(* flushes (BeginFlush seals the open frame, then the write pass's         *)
(* packets, EndFlush).  The write pass sends reliable work first (its      *)
(* reliable phase) and unreliable records after.  SharedQuota FALSE gives  *)
(* in-place packets an allowance of their own (PacketQuota fails);         *)
(* Reserve FALSE lets an in-place packet take the last of the allowance    *)
(* while reliable work is due (ReliableSentWithinOneUpdate fails).         *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    N,              \* messages 1..N, started in this order (the game's send order)
    Kind,           \* Kind[m] \in {"U", "S", "R"}: unreliable (with or without receipt), sequenced, reliable
    Fits,           \* Fits[m]: every header and the form byte fit one wire frame
    Decl,           \* Decl[m]: the declared maximum, header included, in units of the frame's room
    Cap,            \* the room of one frame's payload, the form byte excluded
    NumFrames,      \* the frames the transport can have out at once
    MaxPackets,     \* the queued and in-place packet allowance in one update
    MaxAcks,        \* the acks the other end makes due in a run
    MaxFaults,      \* the faults in a run: a frame refused at start, an abort, an over-budget drop
    CopyStep,       \* TRUE: the copy step of the build before the in-place change
    SharedQuota,    \* TRUE: in-place packets draw on the write pass's allowance (FALSE: their own)
    Reserve,        \* TRUE: one packet of the allowance stays for due reliable work
    MovePrior,      \* TRUE: a finish moves the committed records (witness)
    ReuseCommitted  \* TRUE: a finish does not advance the committed end (witness)

ASSUME \A m \in 1..N : Decl[m] \in 1..Cap

Msgs == 1..N
Frames == 1..NumFrames
Units == 1..Cap
None == 0
Region(m) == NumFrames + m
Data(m) == m
Unreliable == {"U", "S"}
FitUnr(m) == Kind[m] \in Unreliable /\ Fits[m]
Outcomes == {"none", "started", "packed", "queued", "staged", "sent", "dropped", "aborted"}
Zero == [p \in Units |-> 0]

VARIABLES
    next,           \* the next message the game starts
    pending,        \* the message whose start is pending, or None
    handed,         \* handed[m]: the buffer start returned for m, or None
    fellback,       \* fellback[m]: a fitting unreliable m took the queued path (no frame, or unreliable queued ahead)
    open,           \* the frame of the connection's open build, or None
    offset,         \* the committed end of the open build (0 when none is open)
    resAt,          \* the offset of the pending in-place reservation
    wrote,          \* the units the game wrote at the pending in-place reservation (0: not yet)
    frameRecs,      \* frameRecs[f]: the committed records of the open build in f, [msg, off, len] in offset order
    bytes,          \* bytes[f][p]: the unit at position p of frame f's payload
    regionData,     \* regionData[m]: the bytes in m's region record (0 unwritten)
    frameFree,      \* frames the transport can hand out now
    queue,          \* the queued path: finished messages waiting for the write pass, in finish order
    staged,         \* sealed packets waiting for the flush: [buf, recs, acks]
    wire,           \* packets the transport sent, in order
    acksDue,        \* acks due and not yet carried
    acksMade,       \* acks the other end made due in all
    finished,       \* messages finished (not aborted), in finish order
    faults,         \* the faults so far
    outcome,
    packetsThisUpdate, \* queued and in-place packets sealed since BeginUpdate
    flushing,       \* a send_packets flush is running
    promised        \* the oldest reliable message due while the allowance had room, or None

vars == << next, pending, handed, fellback, open, offset, resAt, wrote, frameRecs, bytes,
           regionData, frameFree, queue, staged, wire, acksDue, acksMade, finished, faults,
           outcome, packetsThisUpdate, flushing, promised >>

sealVars == << open, offset, frameRecs, bytes, frameFree, staged, outcome, faults >>

FrameRec == [msg : Msgs, off : 0..Cap, len : Units]
Rec == [msg : Msgs, data : 0..N]
Packet == [buf : Frames, recs : Seq(Rec), acks : 0..MaxAcks]

TypeOK ==
    /\ next \in 1..(N + 1)
    /\ pending \in Msgs \cup {None}
    /\ handed \in [Msgs -> Frames \cup {Region(m) : m \in Msgs} \cup {None}]
    /\ fellback \in [Msgs -> BOOLEAN]
    /\ open \in Frames \cup {None}
    /\ offset \in 0..Cap
    /\ resAt \in 0..Cap
    /\ wrote \in 0..Cap
    /\ frameRecs \in [Frames -> Seq(FrameRec)]
    /\ bytes \in [Frames -> [Units -> 0..N]]
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
    /\ open = None
    /\ offset = 0
    /\ resAt = 0
    /\ wrote = 0
    /\ frameRecs = [f \in Frames |-> << >>]
    /\ bytes = [f \in Frames |-> Zero]
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

\* the open build holds committed records: a packet the update will seal
Committed == open # None /\ frameRecs[open] # << >>
FrameMsgs == IF open = None THEN {} ELSE {frameRecs[open][i].msg : i \in 1..Len(frameRecs[open])}

\* the allowance spent: sealed packets, and the open build's packet
Spent == packetsThisUpdate + (IF Committed THEN 1 ELSE 0)

\* the allowance an in-place start may take (rn_endpoint_in_place_header):
\* the shared one, less one packet while reliable work is due
InPlaceRoom ==
    IF ~SharedQuota THEN TRUE
    ELSE packetsThisUpdate + (IF Reserve /\ ReliableQueued THEN 1 ELSE 0) < MaxPackets

\* message m may be written in place: a fitting unreliable message, room in
\* the allowance, and no unreliable message queued ahead of it
CanPack(m) == ~CopyStep /\ FitUnr(m) /\ InPlaceRoom /\ ~UnreliableQueued

\* the next start seals the open build first: the frame is full, or the
\* next message is unreliable and cannot be appended (its declared maximum
\* does not fit the rest of the frame, or it falls back to the queued path,
\* which must not overtake the frame's records)
NextStartSeals ==
    \/ offset = Cap
    \/ /\ next <= N /\ Kind[next] \in Unreliable
       /\ ~(CanPack(next) /\ Decl[next] <= Cap - offset)

\* a committed record as the packet carries it: the units at its place
ReadRec(f, r) ==
    [msg |-> r.msg,
     data |-> IF \A p \in (r.off + 1)..(r.off + r.len) : bytes[f][p] = Data(r.msg)
              THEN Data(r.msg) ELSE 0]

\* seal the open build (rn_message_flush_unreliable): its records go to the
\* transport as one packet carrying the acks due, or, when the send budget
\* cannot pay it (drop), they are dropped and counted
\* (MESSAGE_UNRELIABLE_OVER_BUDGET) and the frame goes back.  Either way
\* the packet counts against the allowance (the caller adds it).
Seal(drop) ==
    LET f == open
        msgs == FrameMsgs IN
    /\ open' = None
    /\ offset' = 0
    /\ frameRecs' = [frameRecs EXCEPT ![f] = << >>]
    /\ bytes' = [bytes EXCEPT ![f] = Zero]
    /\ IF drop
       THEN /\ faults' = faults + 1
            /\ frameFree' = frameFree \cup {f}
            /\ outcome' = [m \in Msgs |-> IF m \in msgs THEN "dropped" ELSE outcome[m]]
            /\ UNCHANGED staged
       ELSE /\ staged' = Append(staged, [buf |-> f, acks |-> acksDue,
                                         recs |-> [i \in 1..Len(frameRecs[f]) |-> ReadRec(f, frameRecs[f][i])]])
            /\ outcome' = [m \in Msgs |-> IF m \in msgs THEN "staged" ELSE outcome[m]]
            /\ UNCHANGED << faults, frameFree >>

\* the acks due after a seal: a sent packet carries them
SealAcks(drop) == IF drop THEN acksDue ELSE 0

\* start in place: a fitting unreliable message reserves its declared
\* maximum at the committed end of the open build, or, with none open,
\* takes a frame and reserves at its start
StartInPlace ==
    /\ next <= N /\ pending = None /\ ~flushing
    /\ CanPack(next)
    /\ \/ /\ open # None
          /\ Decl[next] <= Cap - offset
          /\ handed' = [handed EXCEPT ![next] = open]
          /\ UNCHANGED << open, frameFree >>
       \/ /\ open = None
          /\ \E f \in frameFree :
              /\ frameFree' = frameFree \ {f}
              /\ open' = f
              /\ handed' = [handed EXCEPT ![next] = f]
    /\ resAt' = offset
    /\ pending' = next
    /\ next' = next + 1
    /\ outcome' = [outcome EXCEPT ![next] = "started"]
    /\ UNCHANGED << fellback, offset, wrote, frameRecs, bytes, regionData, queue, staged, wire,
                    acksDue, acksMade, finished, faults, packetsThisUpdate, flushing, promised >>

\* start on the queued path: a reliable or oversize message always; a fitting
\* unreliable one when the transport gave no frame (it may refuse at any
\* time: a full ring, a client not yet connected), when unreliable messages
\* wait ahead of it, when the allowance has no room for it (InPlaceRoom),
\* or, under CopyStep, always.  An unreliable start reaches it only with no
\* build open (SealBuild sealed it first); a reliable start leaves the open
\* build as it is.
StartQueued ==
    /\ next <= N /\ pending = None /\ ~flushing
    /\ Kind[next] \in Unreliable => open = None
    /\ LET refused == CanPack(next) /\ frameFree # {} IN
       /\ refused => faults < MaxFaults
       /\ faults' = IF refused THEN faults + 1 ELSE faults
    /\ handed' = [handed EXCEPT ![next] = Region(next)]
    /\ fellback' = [fellback EXCEPT ![next] = FitUnr(next) /\ ~CopyStep]
    /\ pending' = next
    /\ next' = next + 1
    /\ outcome' = [outcome EXCEPT ![next] = "started"]
    /\ UNCHANGED << open, offset, resAt, wrote, frameRecs, bytes, regionData, frameFree, queue,
                    staged, wire, acksDue, acksMade, finished, packetsThisUpdate, flushing, promised >>

\* the game writes its bytes at the pointer start returned, once: in place,
\* 1..Decl[m] units at the reservation
Write ==
    /\ pending # None /\ ~flushing
    /\ LET m == pending IN
       IF handed[m] \in Frames
       THEN /\ wrote = 0
            /\ \E w \in 1..Decl[m] :
                /\ wrote' = w
                /\ bytes' = [bytes EXCEPT ![handed[m]] =
                               [p \in Units |-> IF p > resAt /\ p <= resAt + w THEN Data(m) ELSE bytes[handed[m]][p]]]
            /\ UNCHANGED regionData
       ELSE /\ regionData[m] = 0
            /\ regionData' = [regionData EXCEPT ![m] = Data(m)]
            /\ UNCHANGED << wrote, bytes >>
    /\ UNCHANGED << next, pending, handed, fellback, open, offset, resAt, frameRecs, frameFree, queue,
                    staged, wire, acksDue, acksMade, finished, faults, outcome, packetsThisUpdate,
                    flushing, promised >>

Written(m) == IF handed[m] \in Frames THEN wrote > 0 ELSE regionData[m] # 0

\* finish of an in-place message: its header written in front of the bytes
\* and the record committed at the reservation; the committed end advances
\* by the bytes written, so a short finish returns the rest of the
\* reservation.  The build stays open for later records.
FinishInPlace ==
    /\ pending # None /\ ~flushing /\ handed[pending] \in Frames /\ Written(pending)
    /\ LET m == pending
           f == open
           w == wrote IN
       /\ IF MovePrior
          THEN /\ frameRecs' = [frameRecs EXCEPT ![f] =
                     << [msg |-> m, off |-> 0, len |-> w] >> \o
                     [i \in 1..Len(frameRecs[f]) |-> [frameRecs[f][i] EXCEPT !.off = frameRecs[f][i].off + w]]]
               /\ bytes' = [bytes EXCEPT ![f] =
                     [p \in Units |-> IF p <= w THEN Data(m)
                                      ELSE IF p <= offset + w THEN bytes[f][p - w] ELSE bytes[f][p]]]
          ELSE /\ frameRecs' = [frameRecs EXCEPT ![f] = Append(frameRecs[f], [msg |-> m, off |-> resAt, len |-> w])]
               /\ UNCHANGED bytes
       /\ offset' = IF ReuseCommitted THEN offset ELSE offset + w
       /\ finished' = Append(finished, m)
       /\ outcome' = [outcome EXCEPT ![m] = "packed"]
    /\ pending' = None
    /\ resAt' = 0
    /\ wrote' = 0
    /\ UNCHANGED << next, handed, fellback, open, regionData, frameFree, queue, staged, wire,
                    acksDue, acksMade, faults, packetsThisUpdate, flushing, promised >>

\* a reliable message queued while the allowance has room is due work the
\* update owes a packet: it is promised
FinishQueued ==
    /\ pending # None /\ ~flushing /\ handed[pending] \notin Frames /\ Written(pending)
    /\ queue' = Append(queue, pending)
    /\ finished' = Append(finished, pending)
    /\ outcome' = [outcome EXCEPT ![pending] = "queued"]
    /\ promised' = IF promised = None /\ Kind[pending] = "R" /\ Spent < MaxPackets
                   THEN pending ELSE promised
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, open, offset, resAt, wrote, frameRecs, bytes, regionData,
                    frameFree, staged, wire, acksDue, acksMade, faults, packetsThisUpdate, flushing >>

\* abort: an in-place reservation is returned (the uncommitted suffix); a
\* build with no committed record goes back to the transport
\* (abort_packet); a region record's room goes back to the region
Abort ==
    /\ pending # None /\ ~flushing
    /\ faults < MaxFaults
    /\ faults' = faults + 1
    /\ IF handed[pending] \in Frames
       THEN IF frameRecs[open] = << >>
            THEN /\ frameFree' = frameFree \cup {open}
                 /\ open' = None
                 /\ bytes' = [bytes EXCEPT ![open] = Zero]
            ELSE /\ bytes' = [bytes EXCEPT ![open] = [p \in Units |-> IF p > offset THEN 0 ELSE bytes[open][p]]]
                 /\ UNCHANGED << frameFree, open >>
       ELSE /\ UNCHANGED << frameFree, open, bytes >>
    /\ regionData' = [regionData EXCEPT ![pending] = 0]
    /\ outcome' = [outcome EXCEPT ![pending] = "aborted"]
    /\ pending' = None
    /\ resAt' = 0
    /\ wrote' = 0
    /\ UNCHANGED << next, handed, fellback, offset, frameRecs, queue, staged, wire, acksDue, acksMade,
                    finished, packetsThisUpdate, flushing, promised >>

\* the start of the next message seals the open build first
\* (rn_message_object_start: rn_message_flush_unreliable when the declared
\* maximum does not fit, and before the queued path)
SealBuild ==
    /\ Committed /\ pending = None /\ ~flushing
    /\ NextStartSeals
    /\ \E drop \in BOOLEAN :
        /\ drop => faults < MaxFaults
        /\ Seal(drop)
        /\ acksDue' = SealAcks(drop)
    /\ packetsThisUpdate' = packetsThisUpdate + 1
    /\ UNCHANGED << next, pending, handed, fellback, resAt, wrote, regionData, queue, wire, acksMade,
                    finished, flushing, promised >>

\* send_packets begins: the open build is sealed, then the write pass.  No
\* in-place start is pending here: the transport forbids a flush with a
\* fill open, so the object aborts it first (Abort).
BeginFlush ==
    /\ ~flushing
    /\ queue # << >> \/ staged # << >> \/ Committed     \* an empty flush changes nothing
    /\ IF pending = None THEN TRUE ELSE handed[pending] \notin Frames
    /\ flushing' = TRUE
    /\ IF Committed
       THEN \E drop \in BOOLEAN :
              /\ drop => faults < MaxFaults
              /\ Seal(drop)
              /\ acksDue' = SealAcks(drop)
              /\ packetsThisUpdate' = packetsThisUpdate + 1
       ELSE UNCHANGED << sealVars, acksDue, packetsThisUpdate >>
    /\ UNCHANGED << next, pending, handed, fellback, resAt, wrote, regionData, queue,
                    wire, acksMade, finished, promised >>

\* one packet of the write pass, from the queued records at positions idx
\* (in queue order), copied from their region records into a frame, sealed
\* and staged with the acks due
WritePacket(idx) ==
    \E f \in frameFree :
        LET recs == [i \in 1..Len(idx) |-> [msg |-> queue[idx[i]], data |-> regionData[queue[idx[i]]]]]
            taken == {queue[idx[i]] : i \in 1..Len(idx)} IN
        /\ frameFree' = frameFree \ {f}
        /\ staged' = Append(staged, [buf |-> f, recs |-> recs, acks |-> acksDue])
        /\ queue' = SelectSeq(queue, LAMBDA m : m \notin taken)
        /\ regionData' = [m \in Msgs |-> IF m \in taken THEN 0 ELSE regionData[m]]
        /\ outcome' = [m \in Msgs |-> IF m \in taken THEN "staged" ELSE outcome[m]]
        /\ acksDue' = 0
        /\ packetsThisUpdate' = packetsThisUpdate + 1
        /\ UNCHANGED << next, pending, handed, fellback, open, offset, resAt, wrote, frameRecs, bytes,
                        wire, acksMade, finished, faults, flushing, promised >>

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
    /\ UNCHANGED << next, pending, handed, fellback, open, offset, resAt, wrote, frameRecs, bytes,
                    regionData, frameFree, queue, staged, wire, acksDue, acksMade, finished, faults,
                    outcome, packetsThisUpdate >>

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
       /\ outcome' = [m \in Msgs |-> IF m \in msgs THEN "sent" ELSE outcome[m]]
    /\ UNCHANGED << next, pending, handed, fellback, open, offset, resAt, wrote, frameRecs, bytes,
                    regionData, queue, acksDue, acksMade, finished, faults, packetsThisUpdate,
                    flushing, promised >>

\* the next update (rn_message_object_update, rn_endpoint_begin_update):
\* the open build is sealed, the allowance resets, and a reliable message
\* still queued is due work this update owes a packet.  The transport's
\* update reads the other end's packets, so this is where acks become due
\* (at most MaxAcks in a run), never inside a start or a flush.
BeginUpdate ==
    /\ pending = None /\ ~flushing
    /\ packetsThisUpdate > 0 \/ Committed
    /\ packetsThisUpdate' = 0
    /\ promised' = IF promised = None THEN FirstReliable ELSE promised
    /\ \E drop \in BOOLEAN :
        /\ drop => Committed /\ faults < MaxFaults
        /\ IF Committed THEN Seal(drop) ELSE UNCHANGED sealVars
        /\ LET base == IF Committed THEN SealAcks(drop) ELSE acksDue IN
           \/ /\ acksDue' = base
              /\ UNCHANGED acksMade
           \/ /\ acksMade < MaxAcks
              /\ acksDue' = base + 1
              /\ acksMade' = acksMade + 1
    /\ UNCHANGED << next, pending, handed, fellback, resAt, wrote, regionData, queue, wire,
                    finished, flushing >>

Next ==
    \/ StartInPlace \/ StartQueued \/ Write
    \/ FinishInPlace \/ FinishQueued \/ Abort \/ SealBuild
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
\* message is queued, committed in the open build, or with the transport
ModeOrder ==
    LET out == OutMsgs
        outSet == {out[b] : b \in 1..Len(out)}
        queued == {queue[a] : a \in 1..Len(queue)}
        delivered == Delivered(finished) IN
    /\ \A k \in {"U", "S", "R"} : IsPrefix(OfKind(out, k), OfKind(delivered, k))
    /\ \A i \in 1..Len(finished) :
        LET m == finished[i] IN
        outcome[m] # "dropped" => m \in queued \/ m \in outSet \/ m \in FrameMsgs
    /\ Cardinality(outSet) = Len(out)

\* a committed record never moves: until its frame is sealed, every
\* committed record keeps its place (its offset and length, in offset
\* order) while later records append behind it.  Its bytes are
\* BytesUnchangedInPlace's.
PriorRecordsUnmoved ==
    [][\A f \in Frames : frameRecs'[f] = << >> \/ IsPrefix(frameRecs[f], frameRecs'[f])]_vars

\* a frame holding committed records closes only when it is full for the
\* next start (or its records fill it), or at a flush of the build:
\* send_packets (BeginFlush), update (BeginUpdate), or the start of an
\* unreliable message on the queued path, which must not overtake it.
\* Never at a finish, an abort, an append that fits, or a reliable start.
FrameClosedOnlyByFullOrFlush ==
    [][Committed /\ open' # open =>
        \/ NextStartSeals /\ UNCHANGED << next, pending >>
        \/ ~flushing /\ flushing'
        \/ packetsThisUpdate' = 0]_vars
=============================================================================
