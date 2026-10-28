--------------------------- MODULE RNClientIoDecrypt ---------------------------
(***************************************************************************)
(* Decrypt on the client I/O thread, as docs/CLIENT-IO-DECRYPT.md, If      *)
(* decrypt moves, designs it.  The I/O thread opens each session datagram  *)
(* in the receive buffer it fills, before it publishes that buffer, with   *)
(* its own copy of the open key and its own replay window; update consumes *)
(* the records in arrival order and keeps every counter, the integrity     *)
(* limit, the session state and replay_session.                            *)
(*                                                                         *)
(* The code the design changes: sdk/rocketnet_client_protocol.cpp,         *)
(* rocketnet_client_open_session (the state check, the sequence     *)
(* reconstruction, rocketnet_replay_already_received on replay_session,    *)
(* the AEAD open, rocketnet_replay_advance right after it),                *)
(* rocketnet_client_integrity_failure (the limit, the state change and the *)
(* zeroing of client->keys) and rocketnet_client_process_challenge (where  *)
(* the session keys are derived).                                          *)
(*                                                                         *)
(* The receive half of tla/RNClientIo.tla is restated here, not EXTENDed:  *)
(* the send rings, the transport's stop and join are left out.  Each       *)
(* restated action names the RNClientIo action it restates.  The two      *)
(* buffers, the Fill/Ready/Exposed/Free handshake, the owed flag and the   *)
(* level-triggered wake are RNClientIo's; a buffer now holds records (a    *)
(* datagram, its 64-bit sequence and its verdict) instead of a count.      *)
(*                                                                         *)
(* Each window is a mark and a set, as tla/ReplayWindow.tla abstracts the  *)
(* ring: a sequence at or below mark - W is too old, a sequence in the set *)
(* has opened.  The AEAD is a predicate on the datagram (genuine opens,    *)
(* forged does not); the key copy is the boolean ioKeys.                   *)
(*                                                                         *)
(* Four switches restore the defects the reversed witnesses must catch:    *)
(* OpenBeforeReplay (the I/O thread opens before its replay check),        *)
(* SharedWindow (the I/O thread's open also advances replay_session),     *)
(* NoRetire (the I/O thread keeps its copy after it reads retired) and     *)
(* CountOnIo (the I/O thread counts a refusal itself).                     *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS BufferCap,         \* records one receive buffer holds
          MaxDatagrams,      \* datagrams that arrive in one behaviour
          MaxSeq,            \* genuine session sequences the server sends
          W,                 \* the replay window's width
          MaxReplays,        \* copies of sent genuine datagrams (duplicate or too old)
          MaxForged,         \* forged session datagrams
          IntegrityLimit,    \* ROCKETNET_INTEGRITY_LIMIT
          OpenBeforeReplay,  \* defect: the AEAD before the I/O replay check
          SharedWindow,      \* defect: the I/O thread advances mainWin
          NoRetire,          \* defect: the I/O thread keeps its copy after retired
          CountOnIo          \* defect: the I/O thread counts a refusal

ASSUME /\ BufferCap \in Nat \ {0} /\ MaxDatagrams \in Nat
       /\ MaxSeq \in Nat \ {0} /\ W \in Nat \ {0}
       /\ MaxReplays \in Nat /\ MaxForged \in Nat
       /\ IntegrityLimit \in Nat \ {0}
       /\ OpenBeforeReplay \in BOOLEAN /\ SharedWindow \in BOOLEAN
       /\ NoRetire \in BOOLEAN /\ CountOnIo \in BOOLEAN

Seqs    == 1..MaxSeq
Ids     == 1..MaxDatagrams
Buffers == {0, 1}
BufferStates == {"Fill", "Ready", "Exposed", "Free"}

\* a record's verdict: "new" and "checked" are the I/O thread's working
\* states; "opened", "replay" (refused by the I/O replay check), "forged"
\* (did not open) and "raw" (passed through unopened) are final
Verdicts == {"new", "checked", "opened", "replay", "forged", "raw"}
Final    == {"opened", "replay", "forged", "raw"}

Datagrams == [id : Ids, kind : {"handshake", "session"},
              genuine : BOOLEAN, seq : 0..MaxSeq]
Records   == [id : Ids, kind : {"handshake", "session"},
              genuine : BOOLEAN, seq : 0..MaxSeq, v : Verdicts]
Windows   == [mark : 0..MaxSeq, set : SUBSET Seqs]
Writers   == {"none", "update", "io", "net"}

VARIABLES socket,        \* datagrams readable on the socket, in read order
          rec,           \* the records of each receive buffer
          buf,           \* each receive buffer's state (RNClientIo)
          snap,          \* the exposed buffer's records at Expose (RNClientIo)
          cur,           \* records of the exposed buffer update has consumed
          owed,          \* RNClientIo: a publish is owed
          wake,          \* RNClientIo: the level-triggered wake
          waiting,       \* RNClientIo: the I/O thread is blocked in its wait
          waitReadable,  \* RNClientIo: the blocked wait also returns on readable
          fresh,         \* the I/O thread is at the top of a drain
          ioWin,         \* the I/O thread's own replay window
          mainWin,       \* replay_session, update's window
          epoch,         \* the open epoch update publishes
          ioKeys,        \* the I/O thread holds a live copy of the open key
          ioSeen,        \* the epoch the I/O thread read last
          session,       \* the client protocol's state, abstracted
          nCounted,      \* the counters: verdicts counted
          integrity,     \* integrity_failures
          nArrived, nextSeq, nReplay, nForged, challengeSent, \* the network's budget
          crypto,        \* ghost: ids of datagrams that reached the AEAD
          cheapRefused,  \* ghost: ids refused by a rung before the AEAD
          accepted,      \* ghost: sequences update accepted
          acceptedTwice, \* ghost: update accepted a sequence it had accepted
          openNoKeys,    \* ghost: an IoOpen ran without a live copy
          ackMark,       \* the mark a fill thread last read from mainWin
          wMain, wCount, wSession, wIo \* ghost: the last writer of each

ghostVars == <<crypto, cheapRefused, accepted, acceptedTwice, openNoKeys,
               ackMark>>
netVars   == <<nArrived, nextSeq, nReplay, nForged, challengeSent>>
writerVars == <<wMain, wCount, wSession, wIo>>

vars == <<socket, rec, buf, snap, cur, owed, wake, waiting, waitReadable,
          fresh, ioWin, mainWin, epoch, ioKeys, ioSeen, session, nCounted,
          integrity, nArrived, nextSeq, nReplay, nForged, challengeSent,
          crypto, cheapRefused, accepted, acceptedTwice, openNoKeys,
          ackMark, wMain, wCount, wSession, wIo>>

\* everything but the writer ghosts, which Next sets after the step: the
\* subscript of the fairness conditions
modelVars == <<socket, rec, buf, snap, cur, owed, wake, waiting, waitReadable,
               fresh, ioWin, mainWin, epoch, ioKeys, ioSeen, session, nCounted,
               integrity, nArrived, nextSeq, nReplay, nForged, challengeSent,
               crypto, cheapRefused, accepted, acceptedTwice, openNoKeys,
               ackMark>>

Max(a, b) == IF a >= b THEN a ELSE b

(* ---- the window, as tla/ReplayWindow.tla abstracts it ---- *)

EmptyWin == [mark |-> 0, set |-> {}]

\* rocketnet_replay_already_received: too old, or opened
AlreadyReceived(w, s) == s <= w.mark - W \/ s \in w.set

\* rocketnet_replay_advance: raise the mark, forget what left the range
Advance(w, s) ==
    LET m == Max(w.mark, s) IN
    [mark |-> m, set |-> {t \in w.set \cup {s} : t + W > m}]

RECURSIVE AdvanceAll(_, _)
AdvanceAll(w, ss) ==
    IF ss = <<>> THEN w ELSE AdvanceAll(Advance(w, Head(ss)), Tail(ss))

TypeOK ==
    /\ socket \in Seq(Datagrams) /\ Len(socket) <= MaxDatagrams
    /\ \A b \in Buffers : rec[b] \in Seq(Records) /\ Len(rec[b]) <= BufferCap
    /\ buf \in [Buffers -> BufferStates]
    /\ snap \in Seq(Records)
    /\ cur \in 0..BufferCap
    /\ owed \in BOOLEAN /\ wake \in BOOLEAN /\ waiting \in BOOLEAN
    /\ waitReadable \in BOOLEAN /\ fresh \in BOOLEAN
    /\ ioWin \in Windows /\ mainWin \in Windows
    /\ epoch \in {"none", "live", "retired"}
    /\ ioKeys \in BOOLEAN
    /\ ioSeen \in {"none", "live", "retired"}
    /\ session \in {"handshake", "connected", "ended"}
    /\ nCounted \in 0..MaxDatagrams
    /\ integrity \in 0..IntegrityLimit
    /\ nArrived \in 0..MaxDatagrams /\ nextSeq \in 1..MaxSeq + 1
    /\ nReplay \in 0..MaxReplays /\ nForged \in 0..MaxForged
    /\ challengeSent \in BOOLEAN
    /\ crypto \subseteq Ids /\ cheapRefused \subseteq Ids
    /\ accepted \subseteq Seqs
    /\ acceptedTwice \in BOOLEAN /\ openNoKeys \in BOOLEAN
    /\ ackMark \in 0..MaxSeq
    /\ {wMain, wCount, wSession, wIo} \subseteq Writers

Init ==
    /\ socket = <<>>
    /\ rec = [b \in Buffers |-> <<>>]
    /\ buf = [b \in Buffers |-> IF b = 0 THEN "Fill" ELSE "Free"]
    /\ snap = <<>> /\ cur = 0
    /\ owed = FALSE /\ wake = FALSE /\ waiting = FALSE
    /\ waitReadable = TRUE /\ fresh = TRUE
    /\ ioWin = EmptyWin /\ mainWin = EmptyWin
    /\ epoch = "none" /\ ioKeys = FALSE /\ ioSeen = "none"
    /\ session = "handshake"
    /\ nCounted = 0 /\ integrity = 0
    /\ nArrived = 0 /\ nextSeq = 1 /\ nReplay = 0 /\ nForged = 0
    /\ challengeSent = FALSE
    /\ crypto = {} /\ cheapRefused = {} /\ accepted = {}
    /\ acceptedTwice = FALSE /\ openNoKeys = FALSE /\ ackMark = 0
    /\ wMain = "none" /\ wCount = "none" /\ wSession = "none" /\ wIo = "none"

FillBuffer    == CHOOSE b \in Buffers : buf[b] = "Fill"
HasExposed    == \E b \in Buffers : buf[b] = "Exposed"
ExposedBuffer == CHOOSE b \in Buffers : buf[b] = "Exposed"
HasReady      == \E b \in Buffers : buf[b] = "Ready"
ReadyBuffer   == CHOOSE b \in Buffers : buf[b] = "Ready"
HasFree       == \E b \in Buffers : buf[b] = "Free"

FillRecs == rec[FillBuffer]
\* the I/O thread is between datagrams: its last record has a final verdict
Between  == IF FillRecs = <<>> THEN TRUE ELSE FillRecs[Len(FillRecs)].v \in Final
LastIdx  == Len(FillRecs)
Last     == FillRecs[LastIdx]
SetLast(r) == rec' = [rec EXCEPT ![FillBuffer][LastIdx] = r]

\* the records update has not consumed yet, oldest first: the rest of the
\* exposed buffer, then the Ready buffer, then the Fill buffer
Unconsumed ==
    (IF HasExposed THEN SubSeq(rec[ExposedBuffer], cur + 1, Len(rec[ExposedBuffer]))
                   ELSE <<>>)
    \o (IF HasReady THEN rec[ReadyBuffer] ELSE <<>>)
    \o FillRecs

\* the sequences the I/O thread opened that update has not consumed, in order
RECURSIVE OpenedSeqs(_)
OpenedSeqs(rs) ==
    IF rs = <<>> THEN <<>>
    ELSE (IF Head(rs).v = "opened" THEN <<Head(rs).seq>> ELSE <<>>)
         \o OpenedSeqs(Tail(rs))

(* ---- who writes what: Next records each step's thread as the writer of ---- *)
(* ---- every variable the step changes                                 ---- *)

Tag(who) ==
    /\ wMain'    = IF mainWin' # mainWin THEN who ELSE wMain
    /\ wCount'   = IF nCounted' # nCounted \/ integrity' # integrity
                     THEN who ELSE wCount
    /\ wSession' = IF session' # session THEN who ELSE wSession
    /\ wIo'      = IF ioWin' # ioWin THEN who ELSE wIo

(* ---- the network ---- *)

Push(d) ==
    /\ nArrived < MaxDatagrams
    /\ socket' = Append(socket, [d EXCEPT !.id = nArrived + 1])
    /\ nArrived' = nArrived + 1

\* the server's challenge, once
ArriveChallenge ==
    /\ ~challengeSent
    /\ Push([id |-> 1, kind |-> "handshake", genuine |-> TRUE, seq |-> 0])
    /\ challengeSent' = TRUE
    /\ UNCHANGED <<nextSeq, nReplay, nForged>>

\* the server's next session packet.  The server sends session traffic only
\* after the connection response, which update commits after it has
\* published the keys and the I/O thread sends at the end of a drain, so the
\* I/O thread has read the live epoch before any genuine session datagram
\* reaches the socket (ioSeen # "none").  The server sends in increasing
\* order; reordering is the replay copy below.
ArriveGenuine ==
    /\ ioSeen # "none"
    /\ nextSeq <= MaxSeq
    /\ Push([id |-> 1, kind |-> "session", genuine |-> TRUE, seq |-> nextSeq])
    /\ nextSeq' = nextSeq + 1
    /\ UNCHANGED <<nReplay, nForged, challengeSent>>

\* a copy of a session datagram already sent: a duplicate, or too old once
\* the window has moved past it
ArriveReplay ==
    /\ nReplay < MaxReplays
    /\ \E s \in 1..(nextSeq - 1) :
         Push([id |-> 1, kind |-> "session", genuine |-> TRUE, seq |-> s])
    /\ nReplay' = nReplay + 1
    /\ UNCHANGED <<nextSeq, nForged, challengeSent>>

\* a forged session datagram with any sequence
ArriveForged ==
    /\ nForged < MaxForged
    /\ \E s \in Seqs :
         Push([id |-> 1, kind |-> "session", genuine |-> FALSE, seq |-> s])
    /\ nForged' = nForged + 1
    /\ UNCHANGED <<nextSeq, nReplay, challengeSent>>

Arrive ==
    /\ \/ ArriveChallenge \/ ArriveGenuine \/ ArriveReplay \/ ArriveForged
    /\ UNCHANGED <<rec, buf, snap, cur, owed, wake, waiting, waitReadable,
                   fresh, ioWin, mainWin, epoch, ioKeys, ioSeen, session,
                   nCounted, integrity>>
    /\ UNCHANGED ghostVars

(* ---- update: the keys ---- *)

\* update stores the epoch as retired (seq_cst) before it zeroes its own
\* keys, ends the session and wakes the I/O thread so it reads the epoch
Retire ==
    /\ epoch' = "retired"
    /\ session' = "ended"
    /\ wake' = TRUE

\* update consumes the challenge (rocketnet_client_process_challenge): it
\* derives the keys, copies the open key and the immutable inputs into the
\* I/O thread's open block and stores the epoch live (release).  The
\* connection response update commits next is handed off with the wake
\* (RNClientIo HandOff), folded into this step: the I/O thread starts a
\* drain, reads the live epoch at its top and then sends the response.
PublishKeys ==
    /\ HasExposed
    /\ cur < Len(rec[ExposedBuffer])
    /\ LET r == rec[ExposedBuffer][cur + 1] IN
         /\ r.kind = "handshake"
         /\ session = "handshake"
    /\ cur' = cur + 1
    /\ nCounted' = nCounted + 1
    /\ epoch' = "live"
    /\ session' = "connected"
    /\ wake' = TRUE
    /\ UNCHANGED <<socket, rec, buf, snap, owed, waiting, waitReadable,
                   fresh, ioWin, mainWin, ioKeys, ioSeen, integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* disconnect or destroy (rocketnet_client_protocol_disconnect)
RetireKeys ==
    /\ session = "connected"
    /\ Retire
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, waiting, waitReadable,
                   fresh, ioWin, mainWin, ioKeys, ioSeen, nCounted, integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

(* ---- update: the records ---- *)

\* update consumes the next record of the exposed buffer, in arrival order.
\* Every verdict is counted with today's counter (nCounted).
\*   opened:  the state check, then update's own replay check on mainWin and
\*            rocketnet_replay_advance; the sequence is accepted.
\*   replay:  counted as already received.
\*   forged:  counted as did not open, and in its position
\*            rocketnet_client_integrity_failure, which may end the session.
\*   raw:     processed as at BASE: a handshake record outside the
\*            handshake is counted as wrong state; a session record gets
\*            the state check, the replay check on mainWin and the AEAD on
\*            the main thread.
UpdateConsume ==
    /\ HasExposed
    /\ cur < Len(rec[ExposedBuffer])
    /\ LET r == rec[ExposedBuffer][cur + 1]
           live == session = "connected"
           Fail == IF integrity + 1 >= IntegrityLimit
                     THEN /\ integrity' = IntegrityLimit
                          /\ Retire
                     ELSE /\ integrity' = integrity + 1
                          /\ UNCHANGED <<epoch, session, wake>>
           Accept(s) ==
               /\ mainWin' = Advance(mainWin, s)
               /\ accepted' = accepted \cup {s}
               /\ acceptedTwice' = (acceptedTwice \/ s \in accepted)
       IN
       /\ ~(r.kind = "handshake" /\ session = "handshake")   \* PublishKeys
       /\ cur' = cur + 1
       /\ nCounted' = nCounted + 1
       /\ CASE r.kind = "handshake" ->
                 /\ UNCHANGED <<mainWin, integrity, epoch, session, wake>>
                 /\ UNCHANGED <<crypto, cheapRefused, accepted, acceptedTwice>>
            [] r.v = "opened" ->
                 /\ IF live /\ ~AlreadyReceived(mainWin, r.seq)
                      THEN Accept(r.seq)
                      ELSE UNCHANGED <<mainWin, accepted, acceptedTwice>>
                 /\ UNCHANGED <<integrity, epoch, session, wake>>
                 /\ UNCHANGED <<crypto, cheapRefused>>
            [] r.v = "replay" ->
                 /\ UNCHANGED <<mainWin, integrity, epoch, session, wake>>
                 /\ UNCHANGED <<crypto, cheapRefused, accepted, acceptedTwice>>
            [] r.v = "forged" ->
                 /\ IF live THEN Fail
                            ELSE UNCHANGED <<integrity, epoch, session, wake>>
                 /\ UNCHANGED mainWin
                 /\ UNCHANGED <<crypto, cheapRefused, accepted, acceptedTwice>>
            [] r.v = "raw" ->
                 IF ~live \/ AlreadyReceived(mainWin, r.seq)
                   THEN /\ cheapRefused' = cheapRefused \cup {r.id}
                        /\ UNCHANGED <<mainWin, integrity, epoch, session, wake>>
                        /\ UNCHANGED <<crypto, accepted, acceptedTwice>>
                   ELSE /\ crypto' = crypto \cup {r.id}
                        /\ UNCHANGED cheapRefused
                        /\ IF r.genuine
                             THEN /\ Accept(r.seq)
                                  /\ UNCHANGED <<integrity, epoch, session, wake>>
                             ELSE /\ Fail
                                  /\ UNCHANGED <<mainWin, accepted, acceptedTwice>>
    /\ UNCHANGED <<socket, rec, buf, snap, owed, waiting, waitReadable, fresh,
                   ioWin, ioKeys, ioSeen, openNoKeys, ackMark>>
    /\ UNCHANGED netVars

\* a fill thread builds an ack block from mainWin (rocketnet_ack_block_build
\* under the phase contract): it reads mainWin and writes nothing the
\* threads share (ackMark is the mark it read).  It is why mainWin must keep
\* one writer, update, which the phase contract orders against the fills;
\* the I/O thread is outside that contract.
FillAckBuild ==
    /\ session = "connected"
    /\ ackMark # mainWin.mark
    /\ ackMark' = mainWin.mark
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, wake, waiting,
                   waitReadable, fresh, ioWin, mainWin, epoch, ioKeys, ioSeen,
                   session, nCounted, integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED <<crypto, cheapRefused, accepted, acceptedTwice, openNoKeys>>

(* ---- update: the receive handshake, restated from RNClientIo ---- *)

\* RNClientIo Expose: update exposes the Ready buffer (acquire)
Expose ==
    /\ HasReady
    /\ ~HasExposed
    /\ buf' = [buf EXCEPT ![ReadyBuffer] = "Exposed"]
    /\ snap' = rec[ReadyBuffer]
    /\ cur' = 0
    /\ UNCHANGED <<socket, rec, owed, wake, waiting, waitReadable, fresh,
                   ioWin, mainWin, epoch, ioKeys, ioSeen, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* RNClientIo Return: at the end of update, once every record is consumed,
\* the buffer is returned as Free (seq_cst) and the owed flag wakes the I/O
\* thread
Return ==
    /\ HasExposed
    /\ cur = Len(rec[ExposedBuffer])
    /\ buf' = [buf EXCEPT ![ExposedBuffer] = "Free"]
    /\ wake' = (wake \/ owed)
    /\ UNCHANGED <<socket, rec, snap, cur, owed, waiting, waitReadable, fresh,
                   ioWin, mainWin, epoch, ioKeys, ioSeen, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

(* ---- the I/O thread ---- *)

IoReady == ~waiting

\* the top of a drain: the epoch is loaded (acquire) and the copy taken
IoTakeKeys ==
    /\ IoReady /\ fresh /\ Between
    /\ epoch = "live" /\ ioSeen # "live"
    /\ ioKeys' = TRUE
    /\ ioSeen' = "live"
    /\ fresh' = FALSE
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, wake, waiting,
                   waitReadable, ioWin, mainWin, epoch, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* the top of a drain: the epoch reads retired and the copy is zeroed.
\* Under NoRetire the copy is kept.
IoDropKeys ==
    /\ IoReady /\ fresh /\ Between
    /\ epoch = "retired" /\ ioSeen # "retired"
    /\ ioKeys' = (NoRetire /\ ioKeys)
    /\ ioSeen' = "retired"
    /\ fresh' = FALSE
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, wake, waiting,
                   waitReadable, ioWin, mainWin, epoch, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* the top of a drain: the epoch has not changed since the last read
IoKeepKeys ==
    /\ IoReady /\ fresh /\ Between
    /\ epoch = ioSeen
    /\ fresh' = FALSE
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, wake, waiting,
                   waitReadable, ioWin, mainWin, epoch, ioKeys, ioSeen,
                   session, nCounted, integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* RNClientIo IoReceive: read one datagram into the Fill buffer while it
\* has room; its record starts "new" with the sequence it carries (the
\* reconstruction from the I/O thread's own mark is abstracted: the record
\* holds the exact 64-bit sequence)
IoReceive ==
    /\ IoReady /\ ~fresh /\ Between
    /\ socket # <<>>
    /\ Len(FillRecs) < BufferCap
    /\ rec' = [rec EXCEPT ![FillBuffer] =
                 Append(FillRecs, [id |-> Head(socket).id,
                                   kind |-> Head(socket).kind,
                                   genuine |-> Head(socket).genuine,
                                   seq |-> Head(socket).seq, v |-> "new"])]
    /\ socket' = Tail(socket)
    /\ UNCHANGED <<buf, snap, cur, owed, wake, waiting, waitReadable, fresh,
                   ioWin, mainWin, epoch, ioKeys, ioSeen, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* the cheap rungs, the ioWin replay check last.  A handshake datagram, or
\* any datagram while the I/O thread holds no live copy, passes through raw.
\* Under OpenBeforeReplay the replay check is skipped here and runs after
\* the open.  Under CountOnIo the I/O thread counts its replay refusal.
IoCheck ==
    /\ IoReady
    /\ FillRecs # <<>>
    /\ Last.v = "new"
    /\ IF Last.kind = "handshake" \/ ~ioKeys
         THEN /\ SetLast([Last EXCEPT !.v = "raw"])
              /\ UNCHANGED <<cheapRefused, nCounted>>
         ELSE IF ~OpenBeforeReplay /\ AlreadyReceived(ioWin, Last.seq)
         THEN /\ SetLast([Last EXCEPT !.v = "replay"])
              /\ cheapRefused' = cheapRefused \cup {Last.id}
              /\ nCounted' = IF CountOnIo THEN nCounted + 1 ELSE nCounted
         ELSE /\ SetLast([Last EXCEPT !.v = "checked"])
              /\ UNCHANGED <<cheapRefused, nCounted>>
    /\ UNCHANGED <<socket, buf, snap, cur, owed, wake, waiting, waitReadable,
                   fresh, ioWin, mainWin, epoch, ioKeys, ioSeen, session,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED <<crypto, accepted, acceptedTwice, openNoKeys, ackMark>>

\* the AEAD on the I/O thread's copy: a genuine datagram opens and advances
\* ioWin (rocketnet_replay_advance right after the open), a forged one does
\* not open.  Under OpenBeforeReplay the replay check runs here, after the
\* AEAD.  Under SharedWindow the open also advances mainWin.
IoOpen ==
    /\ IoReady
    /\ FillRecs # <<>>
    /\ Last.v = "checked"
    /\ crypto' = crypto \cup {Last.id}
    /\ openNoKeys' = (openNoKeys \/ ~ioKeys)
    /\ IF Last.genuine /\ ~(OpenBeforeReplay /\ AlreadyReceived(ioWin, Last.seq))
         THEN /\ SetLast([Last EXCEPT !.v = "opened"])
              /\ ioWin' = Advance(ioWin, Last.seq)
              /\ mainWin' = IF SharedWindow THEN Advance(mainWin, Last.seq)
                                            ELSE mainWin
              /\ UNCHANGED cheapRefused
         ELSE IF Last.genuine
         THEN /\ SetLast([Last EXCEPT !.v = "replay"])
              /\ cheapRefused' = cheapRefused \cup {Last.id}
              /\ UNCHANGED <<ioWin, mainWin>>
         ELSE /\ SetLast([Last EXCEPT !.v = "forged"])
              /\ UNCHANGED <<ioWin, mainWin, cheapRefused>>
    /\ UNCHANGED <<socket, buf, snap, cur, owed, wake, waiting, waitReadable,
                   fresh, epoch, ioKeys, ioSeen, session, nCounted, integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED <<accepted, acceptedTwice, ackMark>>

\* RNClientIo IoPublish: at a batch end, between datagrams, publish the Fill
\* buffer (release) when a Free one is there to fill next: the plaintext,
\* the sequences and the verdicts ride this store
IoPublish ==
    /\ IoReady /\ Between
    /\ FillRecs # <<>>
    /\ HasFree
    /\ LET fb == FillBuffer
           nb == CHOOSE b \in Buffers : buf[b] = "Free" IN
         /\ buf' = [buf EXCEPT ![fb] = "Ready", ![nb] = "Fill"]
         /\ rec' = [rec EXCEPT ![nb] = <<>>]
    /\ owed' = FALSE
    /\ UNCHANGED <<socket, snap, cur, wake, waiting, waitReadable, fresh,
                   ioWin, mainWin, epoch, ioKeys, ioSeen, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* RNClientIo IoOwe: a batch with no Free buffer records the owed publish
IoOwe ==
    /\ IoReady /\ Between
    /\ FillRecs # <<>>
    /\ ~HasFree
    /\ ~owed
    /\ owed' = TRUE
    /\ UNCHANGED <<socket, rec, buf, snap, cur, wake, waiting, waitReadable,
                   fresh, ioWin, mainWin, epoch, ioKeys, ioSeen, session,
                   nCounted, integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* RNClientIo IoWait: the end of a drain.  Blocks only between datagrams,
\* after the epoch read of this drain, never while it could publish, with an
\* unpublished batch only once it is owed, with datagrams in the socket only
\* when the buffer is full; a full buffer waits on the wake alone
IoWait ==
    /\ IoReady /\ ~fresh /\ Between
    /\ ~wake
    /\ ~(FillRecs # <<>> /\ HasFree)
    /\ FillRecs # <<>> => owed
    /\ socket = <<>> \/ Len(FillRecs) = BufferCap
    /\ waiting' = TRUE
    /\ waitReadable' = (Len(FillRecs) < BufferCap)
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, wake, fresh, ioWin,
                   mainWin, epoch, ioKeys, ioSeen, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* RNClientIo IoWake: the wait returns; the next drain starts at its top
IoWake ==
    /\ waiting
    /\ wake \/ (waitReadable /\ socket # <<>>)
    /\ waiting' = FALSE
    /\ wake' = FALSE
    /\ fresh' = TRUE
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, waitReadable, ioWin,
                   mainWin, epoch, ioKeys, ioSeen, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* RNClientIo IoClearWake: a running thread's wait returns at once on a set
\* wake, which it consumes; the next drain starts at its top
IoClearWake ==
    /\ IoReady /\ ~fresh /\ Between
    /\ wake
    /\ wake' = FALSE
    /\ fresh' = TRUE
    /\ UNCHANGED <<socket, rec, buf, snap, cur, owed, waiting, waitReadable,
                   ioWin, mainWin, epoch, ioKeys, ioSeen, session, nCounted,
                   integrity>>
    /\ UNCHANGED netVars
    /\ UNCHANGED ghostVars

\* the end of a behaviour: every datagram consumed, the socket empty and the
\* I/O thread blocked.  Any other state with no step is a deadlock.
Done ==
    /\ socket = <<>>
    /\ waiting
    /\ Unconsumed = <<>>
    /\ ~HasExposed /\ ~HasReady
    /\ UNCHANGED vars

UpdateNext == PublishKeys \/ RetireKeys \/ UpdateConsume \/ FillAckBuild
              \/ Expose \/ Return

IoNext == IoTakeKeys \/ IoDropKeys \/ IoKeepKeys \/ IoReceive \/ IoCheck
          \/ IoOpen \/ IoPublish \/ IoOwe \/ IoWait \/ IoWake \/ IoClearWake

\* each step is one thread's, and Tag records it as the writer of what it
\* changed
Next == \/ UpdateNext /\ Tag("update")
        \/ IoNext /\ Tag("io")
        \/ Arrive /\ Tag("net")
        \/ Done

Spec == Init /\ [][Next]_vars

\* RNClientIo's fairness on the receive half (IoReceive, IoPublish, IoOwe,
\* IoWake, Expose, Return), and the same for the new steps of the two
\* threads: the I/O thread's epoch read, check, open and the consumption of
\* a set wake; update's consumption of each record.  The game calls update
\* every frame.  RetireKeys, Arrive and FillAckBuild carry no fairness.
SpecFair ==
    /\ Spec
    /\ WF_modelVars(IoReceive) /\ WF_modelVars(IoPublish) /\ WF_modelVars(IoOwe)
    /\ WF_modelVars(IoWake) /\ WF_modelVars(IoClearWake)
    /\ WF_modelVars(IoTakeKeys) /\ WF_modelVars(IoDropKeys) /\ WF_modelVars(IoKeepKeys)
    /\ WF_modelVars(IoCheck) /\ WF_modelVars(IoOpen)
    /\ WF_modelVars(Expose) /\ WF_modelVars(Return)
    /\ WF_modelVars(UpdateConsume) /\ WF_modelVars(PublishKeys)

(* ---- what the design must keep ---- *)

\* tla/ReceiveOrder.tla's rule: no datagram refused by a rung before the
\* AEAD, the replay check included, reached the AEAD
CheapRefusalsFirst == cheapRefused \cap crypto = {}

\* the I/O thread never opens without a live copy, and never holds one once
\* it has read retired
NoOpenWithoutKeys ==
    /\ ~openNoKeys
    /\ ioSeen = "retired" => ~ioKeys

\* while the session has not ended, update's window advanced by the
\* sequences the I/O thread opened and update has not consumed yet is the
\* I/O thread's window: the two are the same function of one sequence
\* stream, update's lagging by the unconsumed records
MirrorAgrees ==
    session # "ended" => AdvanceAll(mainWin, OpenedSeqs(Unconsumed)) = ioWin

\* update accepts a sequence at most once, and both windows refuse every
\* sequence update accepted (ReplayWindow's ReceivedIsRefused for each)
AcceptedOnce ==
    /\ ~acceptedTwice
    /\ \A s \in accepted : AlreadyReceived(mainWin, s) /\ AlreadyReceived(ioWin, s)

\* mainWin, the counters and the session change only in update's steps,
\* ioWin only in the I/O thread's
SingleWriter ==
    /\ wMain \in {"none", "update"}
    /\ wCount \in {"none", "update"}
    /\ wSession \in {"none", "update"}
    /\ wIo \in {"none", "io"}

\* RNClientIo ExposedStable: the exposed buffer's records do not change
\* between Expose and Return
ExposedStable ==
    \A b \in Buffers : buf[b] = "Exposed" => rec[b] = snap

\* RNClientIo NoReceiveStall, with the two new kinds of step: records in
\* the Fill buffer always have a next step toward update
NoReceiveStall ==
    FillRecs # <<>> =>
        ENABLED (IoCheck \/ IoOpen \/ IoPublish \/ Expose \/ UpdateConsume
                 \/ PublishKeys \/ Return \/ IoWake)

\* a genuine session datagram the server sent is accepted, unless the
\* session ends first
GenuineEventuallyAccepted ==
    \A s \in Seqs : (nextSeq > s) ~> (s \in accepted \/ session = "ended")

\* after the retire the I/O thread's copy is eventually dropped
RetiredEventuallyZeroed ==
    (epoch = "retired") ~> (~ioKeys /\ ioSeen = "retired")

=============================================================================
