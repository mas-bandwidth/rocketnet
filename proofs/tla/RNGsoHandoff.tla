--------------------------- MODULE RNGsoHandoff ---------------------------
(***************************************************************************)
(* The cross-thread bank handoff of the GSO server transport               *)
(* (sdk/rocketnet_server_gso_linux.cpp), one receive queue plus the        *)
(* transmit bank group.                                                    *)
(*                                                                         *)
(* Receive (per queue, two banks plus one unused spare): the worker        *)
(* appends to its bank under the receive mutex and never publishes; main   *)
(* holds one bank as its stable view and swaps it for the worker's filled  *)
(* bank, under the same mutex, only once it has consumed every packet of   *)
(* the bank it holds, so a partially consumed retained tail is never       *)
(* recycled under it and every packet staged before the claim is in that   *)
(* claim's view.  Main's swap is a try-acquire: on contention it returns   *)
(* and retries next update.  A worker pass that stages nothing while a     *)
(* datagram waits (its bank is full until main's swap; a failed receive   *)
(* takes the same path in the code and is not modelled) is a stall, and   *)
(* the worker sleeps 1ms before its next pass instead of spinning on poll  *)
(* and the mutex (RxStall, RxSleep, RxBacksOff).                           *)
(*                                                                         *)
(* Transmit (three banks): game threads stage into the producer bank;      *)
(* send_packets try-publishes it into a bounded FIFO and takes a free      *)
(* bank, retaining the producer when the FIFO's free list is empty or      *)
(* the mutex is contended.  The TX worker takes banks FIFO outside the     *)
(* mutex, sends them, and returns each bank only after its synchronous     *)
(* sends finish.  A full socket takes a prefix of the held bank (possibly  *)
(* none); the worker keeps the unsent rest, in order, and sends it before  *)
(* taking any later bank, so nothing staged is dropped for lack of room.  *)
(* Stop hands the worker the final unpublished producer                   *)
(* after every ready bank; the worker sends it last and exits without      *)
(* returning it.  A socket that stays full after stop is tried once and    *)
(* then the worker leaves the held bank's unsent rest (and any later banks *)
(* and the producer) and exits, so stop completes without waiting for the  *)
(* socket to drain (rocketnet_server_gso_send_worker's post-quit retry     *)
(* bound).                                                                 *)
(*                                                                         *)
(* Stop and free: stop stores quit for both workers; the RX worker exits   *)
(* after its drain, the TX worker after its drain or its one post-quit     *)
(* attempt on a full socket; destroy joins both threads before freeing any *)
(* storage, and refuses (does not free) while a worker has not exited.     *)
(*                                                                         *)
(* Defect switches, FALSE in the passing run: ClaimStale (the RX claim     *)
(* leaves the most recently staged packet behind for a later claim, so a   *)
(* packet staged before the claim is not in its view),                     *)
(* PublishWhileHeld (the RX worker writes a datagram into the buffer main  *)
(* currently holds, as if it published over main's view), MainBlocks       *)
(* (main acquires the TX mutex with a blocking wait instead of a try),     *)
(* FullDrops (a full socket drops packets after the prefix it took and the *)
(* worker goes on with the rest of the bank) and CarryBehind (the unsent   *)
(* rest is queued behind the banks published since, instead of being       *)
(* kept).  The reversed witnesses MCRNGsoHandoffBrokenPublishWhileHeld.cfg, *)
(* MCRNGsoHandoffBrokenFullDrops.cfg and                                   *)
(* MCRNGsoHandoffBrokenCarryBehind.cfg restore one each and violate        *)
(* NoPublishWhileHeld, SentOrHeld and TxInOrder.  The operator switches    *)
(* ClaimStale and NoBackoff (below) are reversed by                        *)
(* MCRNGsoHandoffBrokenClaimStale.cfg and MCRNGsoHandoffBrokenNoBackoff.cfg *)
(* and violate StagedBeforeClaimInView and RxBacksOff.                     *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS RxCap,            \* packets one receive buffer holds
          TxCap,            \* packets one transmit bank holds
          MaxRx,            \* datagrams that arrive in one behaviour
          MaxTx,            \* packets staged in one behaviour
          PublishWhileHeld, \* defect: worker writes into main's buffer
          MainBlocks,       \* defect: main waits on the TX mutex
          FullDrops,        \* defect: a full socket drops the rest of its batch
          CarryBehind       \* defect: the unsent rest queues behind newer banks

ASSUME RxCap >= 1 /\ TxCap >= 1 /\ MaxRx >= 1 /\ MaxTx >= 1

NONE == 3
RxBuffers == 0 .. 1
Buffers == 0 .. 2
Threads == { "main", "worker" }

(* Rule switches (operators, overridden in a cfg with <-): ClaimStale      *)
(* makes the RX claim leave the most recently staged packet behind in the *)
(* worker's next bank, deferred to a later claim as the worker-driven      *)
(* publish deferred a packet staged after it; NoBackoff sends the RX      *)
(* worker straight back to poll after a stalled pass instead of sleeping  *)
(* 1ms. FALSE in the passing run; the reversed witnesses                  *)
(* MCRNGsoHandoffBrokenClaimStale.cfg and MCRNGsoHandoffBrokenNoBackoff.cfg *)
(* set each TRUE and violate StagedBeforeClaimInView and RxBacksOff.      *)
ClaimStale == FALSE
NoBackoff == FALSE

Range(s) == { s[i] : i \in DOMAIN s }

VARIABLES
    lock,        \* "none", "main" or "worker": who holds the TX mutex
    pc,          \* main and the TX worker: "run", "locked", "unlock", "waitlock", "sending", "done"
    pcRx,        \* the RX worker: "run" or "done" (it never touches the TX mutex)
    rxWorker,    \* the RX buffer the worker fills
    rxMain,      \* the RX buffer main holds
    rxContent,   \* the packets staged in each RX buffer
    rxCursor,    \* packets of rxMain main has consumed (the retained tail follows)
    rxView,      \* rxMain's content as main claimed it
    rxStagedClaim, \* ghost: datagrams staged (arrived) before the last claim
    rxBackoff,   \* owed: the last RX pass stalled and a 1ms sleep is owed
                 \* before the next; spun (ghost): a pass ran while one was owed
    arrived,     \* datagrams arrived: the next is arrived + 1
    rxConsumed,  \* packets main consumed, in order
    txProducer,  \* the TX bank main's fill threads stage into, or NONE
    txFree,      \* the free TX banks, oldest first
    txReady,     \* the published TX banks, oldest first
    txHeld,      \* the TX bank the worker holds, or NONE
    txFinal,     \* txHeld is stop's final producer
    txContent,   \* the packets staged in each TX bank
    staged,      \* packets staged: the next is staged + 1
    txSent,      \* packets the worker sent, in order
    quit,        \* main requested stop
    rxExited,    \* the RX worker exited
    txExited,    \* the TX worker exited
    joined,      \* destroy joined both workers
    freed        \* destroy freed the transport storage

vars == << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
           rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer, txFree, txReady,
           txHeld, txFinal, txContent, staged, txSent, quit, rxExited,
           txExited, joined, freed >>

Init ==
    /\ lock = "none"
    /\ pc = [ t \in Threads |-> "run" ]
    /\ pcRx = "run"
    /\ rxWorker = 0 /\ rxMain = 1
    /\ rxContent = [ b \in RxBuffers |-> << >> ]
    /\ rxCursor = 0 /\ rxView = << >> /\ rxStagedClaim = 0
    /\ rxBackoff = [ owed |-> FALSE, spun |-> FALSE ]
    /\ arrived = 0 /\ rxConsumed = << >>
    /\ txProducer = 0 /\ txFree = << 1, 2 >> /\ txReady = << >> /\ txHeld = NONE
    /\ txFinal = FALSE
    /\ txContent = [ b \in Buffers |-> << >> ]
    /\ staged = 0 /\ txSent = << >>
    /\ quit = FALSE /\ rxExited = FALSE /\ txExited = FALSE
    /\ joined = FALSE /\ freed = FALSE

(* the RX worker stages one arrived datagram into its own buffer, under the *)
(* mutex (folded into this atomic step); a full buffer has no fill step      *)
(* enabled, which is the queue's bounded backpressure; after a stalled pass *)
(* the worker sleeps (RxSleep) before its next pass                         *)
RxFill ==
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ ~rxBackoff.owed
    /\ Len(rxContent[rxWorker]) < RxCap
    /\ arrived < MaxRx
    /\ rxContent' = [ rxContent EXCEPT ![rxWorker] = Append( @, arrived + 1 ) ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxCursor, rxView,
                    rxStagedClaim, rxBackoff, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, joined, freed >>

(* a stalled pass: a datagram waits, so poll returns at once, but the     *)
(* worker's bank is full (it stays full until main's swap) and the pass    *)
(* stages nothing. The worker then owes a 1ms sleep before its next pass;  *)
(* NoBackoff instead lets it go straight back to poll and the mutex, the   *)
(* spin, and a stall while a sleep is still owed is recorded as one. A     *)
(* failed receive takes the same path in the code; it is not modelled      *)
(* separately (test_server_gso_receive_backoff covers it)                  *)
RxStall ==
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ Len(rxContent[rxWorker]) = RxCap
    /\ arrived < MaxRx
    /\ NoBackoff \/ ~rxBackoff.owed
    /\ rxBackoff' = [ owed |-> TRUE, spun |-> rxBackoff.spun \/ rxBackoff.owed ]
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, txExited, joined, freed >>

(* the 1ms sleep after a stalled pass, outside the mutex                    *)
RxSleep ==
    /\ pcRx = "run" /\ rxBackoff.owed
    /\ rxBackoff' = [ rxBackoff EXCEPT !.owed = FALSE ]
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, txExited, joined, freed >>

(* defect: the worker writes into the buffer main holds, as if a publish   *)
(* landed on main's view instead of the worker's own bank                 *)
WriteHeld ==
    /\ PublishWhileHeld
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ Len(rxContent[rxMain]) < RxCap
    /\ arrived < MaxRx
    /\ rxContent' = [ rxContent EXCEPT ![rxMain] = Append( @, arrived + 1 ) ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxCursor, rxView,
                    rxStagedClaim, rxBackoff, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, joined, freed >>

(* main claims only once the buffer it holds is fully consumed: its old    *)
(* buffer becomes the worker's empty one, and the worker's filled buffer   *)
(* becomes main's view -- every packet staged before the claim is in it.   *)
(* An empty worker buffer is claimed too: the code's counters-only bank    *)
(* (rocketnet_server_gso_claim_receive folds its counters and returns it). *)
RxClaim ==
    /\ pc["main"] = "run" /\ ~quit
    /\ rxCursor = Len(rxContent[rxMain])
    /\ LET c     == rxContent[rxWorker]
           stale == ClaimStale /\ c # << >>
           taken == IF stale THEN SubSeq( c, 1, Len(c) - 1 ) ELSE c
           left  == IF stale THEN << c[Len(c)] >> ELSE << >>
       IN  /\ rxMain' = rxWorker /\ rxWorker' = rxMain
           /\ rxView' = taken /\ rxCursor' = 0
           /\ rxStagedClaim' = arrived
           /\ rxContent' = [ rxContent EXCEPT ![rxWorker] = taken, ![rxMain] = left ]
    /\ UNCHANGED << lock, pc, pcRx, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, txExited, joined, freed >>

(* main consumes one packet of the buffer it holds; between the first and *)
(* the last packet the unconsumed tail stays retained in main's buffer    *)
RxConsume ==
    /\ pc["main"] = "run"
    /\ rxCursor < Len(rxContent[rxMain])
    /\ rxConsumed' = Append( rxConsumed, rxContent[rxMain][rxCursor + 1] )
    /\ rxCursor' = rxCursor + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxView,
                    rxStagedClaim, rxBackoff, arrived, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, joined, freed >>

(* a fill thread stages one packet into the producer bank, outside the    *)
(* mutex                                                                  *)
TxStage ==
    /\ pc["main"] = "run" /\ ~quit
    /\ txProducer # NONE /\ Len(txContent[txProducer]) < TxCap
    /\ staged < MaxTx
    /\ txContent' = [ txContent EXCEPT ![txProducer] = Append( @, staged + 1 ) ]
    /\ staged' = staged + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txSent, quit, rxExited,
                    txExited, joined, freed >>

(* main's try-acquire of the TX mutex for a publish: it takes a free mutex *)
(* or defers this frame; with MainBlocks it waits for a held one instead  *)
TxMainLock ==
    /\ pc["main"] = "run" /\ ~quit
    /\ IF lock = "none"
       THEN /\ lock' = "main"
            /\ pc' = [ pc EXCEPT !["main"] = "locked" ]
       ELSE /\ MainBlocks
            /\ lock' = lock
            /\ pc' = [ pc EXCEPT !["main"] = "waitlock" ]
    /\ UNCHANGED << pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent,
                    quit, rxExited, txExited, joined, freed >>

TxMainWait ==
    /\ pc["main"] = "waitlock" /\ lock = "none"
    /\ lock' = "main"
    /\ pc' = [ pc EXCEPT !["main"] = "locked" ]
    /\ UNCHANGED << pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent,
                    quit, rxExited, txExited, joined, freed >>

(* the publish exchange, mutex held: a non-empty producer joins the ready *)
(* FIFO and main takes the oldest free bank; with no bank free main       *)
(* retains its producer and the publish is deferred, never waited on      *)
TxPublish ==
    /\ pc["main"] = "locked"
    /\ IF txProducer # NONE /\ txContent[txProducer] # << >> /\ txFree # << >>
       THEN /\ txReady' = Append( txReady, txProducer )
            /\ txProducer' = Head( txFree )
            /\ txFree' = Tail( txFree )
       ELSE UNCHANGED << txReady, txProducer, txFree >>
    /\ pc' = [ pc EXCEPT !["main"] = "unlock" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txHeld,
                    txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

(* the TX worker acquires the mutex for a take or a return                *)
TxWorkerLock ==
    /\ pc["worker"] = "run" /\ ~txExited
    /\ lock = "none"
    /\ lock' = "worker"
    /\ pc' = [ pc EXCEPT !["worker"] = "locked" ]
    /\ UNCHANGED << pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent,
                    quit, rxExited, txExited, joined, freed >>

(* the take, mutex held: the oldest ready bank first; at quit, with the   *)
(* ready FIFO drained, stop's final producer                             *)
TxTake ==
    /\ pc["worker"] = "locked" /\ txHeld = NONE
    /\ \/ /\ txReady # << >>
          /\ txHeld' = Head( txReady ) /\ txReady' = Tail( txReady )
          /\ txFinal' = FALSE
          /\ UNCHANGED << txProducer >>
       \/ /\ txReady = << >> /\ quit /\ txProducer # NONE
          /\ txHeld' = txProducer /\ txProducer' = NONE
          /\ txFinal' = TRUE
          /\ UNCHANGED txReady
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txFree,
                    txContent, staged, txSent, quit, rxExited, txExited,
                    joined, freed >>

(* the take that finds nothing, mutex held: no bank moves and the worker *)
(* drops the mutex and waits for the next wake                            *)
TxTakeNone ==
    /\ pc["worker"] = "locked" /\ txHeld = NONE
    /\ txReady = << >>
    /\ ( ~quit \/ txProducer = NONE )
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, txExited, joined, freed >>

(* either thread drops the mutex; a publisher wakes the worker after the  *)
(* drop, never under it (the wake is not state this model needs)          *)
Unlock(t) ==
    /\ pc[t] = "unlock" /\ lock = t
    /\ lock' = "none"
    /\ pc' = [ pc EXCEPT ![t] = IF t = "worker" /\ txHeld # NONE THEN "sending" ELSE "run" ]
    /\ UNCHANGED << pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent,
                    quit, rxExited, txExited, joined, freed >>

(* the worker sends the bank it holds, outside the mutex, and clears it;  *)
(* stop's final bank is sent last and ends the worker instead of being    *)
(* returned                                                               *)
TxSend ==
    /\ pc["worker"] = "sending" /\ txHeld # NONE
    /\ txSent' = txSent \o txContent[txHeld]
    /\ txContent' = [ txContent EXCEPT ![txHeld] = << >> ]
    /\ IF txFinal
       THEN /\ txExited' = TRUE
            /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
            \* the final bank stays the exited worker's: it is retired
            \* unsent-to-free and freed with the transport at destroy
            /\ UNCHANGED << txHeld, txFinal >>
       ELSE /\ pc' = [ pc EXCEPT !["worker"] = "run" ]
            /\ UNCHANGED << txExited, txHeld, txFinal >>
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, staged, quit, rxExited, joined, freed >>

(* a full socket: it takes the first k packets of the held bank, k less   *)
(* than the bank holds (rocketnet_server_gso_flush returning the first     *)
(* unsent slot with full set). The worker keeps the rest in the bank and   *)
(* stays sending (rocketnet_server_gso_send_bank's caller waits for main's *)
(* next update, then sends from that slot). After stop, though, the        *)
(* worker tries the held bank once and leaves its unsent rest instead of   *)
(* waiting for the socket to drain, as ROCKETNET_GSO_SEND_RETRIES_AFTER_   *)
(* QUIT bounds the retry; the exited worker keeps that bank, any later     *)
(* ready banks and the producer until destroy.  FullDrops instead drops d  *)
(* packets after the prefix, the rest of the refused batch, and goes on;   *)
(* CarryBehind queues the bank behind the ready banks instead of keeping   *)
(* it                                                                      *)
TxSendFull ==
    /\ pc["worker"] = "sending" /\ txHeld # NONE
    /\ LET c == txContent[txHeld] IN
       \E k \in 0 .. Len(c) - 1 :
          /\ txSent' = txSent \o SubSeq( c, 1, k )
          /\ IF FullDrops
             THEN \E d \in 1 .. Len(c) - k :
                     txContent' = [ txContent EXCEPT ![txHeld] = SubSeq( c, k + d + 1, Len(c) ) ]
             ELSE txContent' = [ txContent EXCEPT ![txHeld] = SubSeq( c, k + 1, Len(c) ) ]
    /\ IF quit
       THEN /\ txExited' = TRUE
            /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
            /\ UNCHANGED << txReady, txHeld, txFinal >>
       ELSE /\ txExited' = txExited
            /\ txFinal' = txFinal
            /\ IF CarryBehind /\ ~txFinal
               THEN /\ txReady' = Append( txReady, txHeld )
                    /\ txHeld' = NONE
                    /\ pc' = [ pc EXCEPT !["worker"] = "run" ]
               ELSE UNCHANGED << txReady, txHeld, pc >>
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, staged, quit, rxExited, joined, freed >>

(* the return, mutex held: a sent bank rejoins the free FIFO only after   *)
(* its sends finished (TxSend cleared it)                                 *)
TxReturn ==
    /\ pc["worker"] = "locked" /\ txHeld # NONE /\ ~txFinal
    /\ txContent[txHeld] = << >>
    /\ txFree' = Append( txFree, txHeld ) /\ txHeld' = NONE
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txReady, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, joined, freed >>

(* stop: quit is stored for both workers after main's last stage and      *)
(* publish; the final producer passes to the worker with the store        *)
MainStop ==
    /\ pc["main"] = "run" /\ ~quit
    /\ quit' = TRUE
    /\ pc' = [ pc EXCEPT !["main"] = "done" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, rxExited, txExited, joined, freed >>

(* the RX worker exits once quit is stored; buffers it still owns are     *)
(* simply never claimed again                                              *)
RxWorkerExit ==
    /\ pcRx = "run" /\ quit /\ ~rxExited
    /\ rxExited' = TRUE
    /\ pcRx' = "done"
    /\ UNCHANGED << lock, pc, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, txExited, joined, freed >>

(* the TX worker also exits at quit when nothing was ever staged: the     *)
(* final producer is empty, so taking it would send nothing               *)
TxWorkerExitIdle ==
    /\ pc["worker"] = "run" /\ quit /\ ~txExited
    /\ txHeld = NONE /\ txReady = << >>
    /\ ( txProducer = NONE \/ txContent[txProducer] = << >> )
    /\ txExited' = TRUE
    /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, joined, freed >>

(* destroy joins both workers, and only a joined transport is freed: a    *)
(* destroy that arrives before the workers exit joins nothing and frees   *)
(* nothing                                                                *)
Join ==
    /\ quit /\ rxExited /\ txExited /\ ~joined /\ ~freed
    /\ joined' = TRUE
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, txExited, freed >>

Free ==
    /\ joined /\ ~freed
    /\ freed' = TRUE
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxStagedClaim, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, txExited, joined >>

(* the behaviour ends once the transport is freed                         *)
Terminated ==
    /\ freed
    /\ UNCHANGED vars

Next ==
    \/ Terminated
    \/ RxFill \/ RxStall \/ RxSleep \/ WriteHeld \/ RxClaim \/ RxConsume
    \/ TxStage \/ TxMainLock \/ TxMainWait \/ TxPublish
    \/ TxWorkerLock \/ TxTake \/ TxTakeNone \/ TxSend \/ TxSendFull \/ TxReturn
    \/ \E t \in Threads : Unlock(t)
    \/ MainStop \/ RxWorkerExit \/ TxWorkerExitIdle \/ Join \/ Free

Spec == Init /\ [][Next]_vars

(* the worker runs whenever it can, main keeps updating and finishes what *)
(* it started, and stop is never forced; a full socket before stop is     *)
(* retried until its send succeeds (WF TxSend), while the post-quit bound *)
(* in TxSendFull lets the worker exit without a drain                     *)
SpecFair ==
    /\ Spec
    /\ WF_vars( RxFill ) /\ WF_vars( RxSleep ) /\ WF_vars( RxClaim )
    /\ WF_vars( RxConsume ) /\ WF_vars( TxSend ) /\ WF_vars( TxReturn )
    /\ SF_vars( TxWorkerLock ) /\ WF_vars( TxTake ) /\ WF_vars( TxTakeNone )
    /\ \A t \in Threads : WF_vars( Unlock(t) )
    /\ SF_vars( TxMainLock ) /\ WF_vars( TxPublish ) /\ WF_vars( TxMainWait )
    /\ SF_vars( RxWorkerExit ) /\ SF_vars( TxWorkerExitIdle )
    /\ WF_vars( Join ) /\ WF_vars( Free )

----------------------------------------------------------------------------

TypeOK ==
    /\ lock \in { "none", "main", "worker" }
    /\ pcRx \in { "run", "done" }
    /\ rxWorker \in RxBuffers /\ rxMain \in RxBuffers
    /\ rxStagedClaim \in 0 .. MaxRx
    /\ rxBackoff \in [ owed : BOOLEAN, spun : BOOLEAN ]
    /\ arrived \in 0 .. MaxRx /\ staged \in 0 .. MaxTx
    /\ txProducer \in Buffers \cup { NONE } /\ txHeld \in Buffers \cup { NONE }

(* the two receive buffers are held by exactly one owner each             *)
RxOneOwner == rxWorker # rxMain

(* every transmit bank has exactly one owner                              *)
TxOneOwner ==
    LET InSeq(s, b) == \E i \in 1 .. Len(s) : s[i] = b
    IN \A b \in Buffers :
        ( IF txProducer = b THEN 1 ELSE 0 ) + ( IF txHeld = b THEN 1 ELSE 0 )
      + ( IF InSeq(txReady, b) THEN 1 ELSE 0 ) + ( IF InSeq(txFree, b) THEN 1 ELSE 0 ) = 1

(* no bank is queued twice on either TX FIFO                              *)
TxQueuesDistinct ==
    /\ \A i, j \in 1 .. Len(txReady) : i # j => txReady[i] # txReady[j]
    /\ \A i, j \in 1 .. Len(txFree) : i # j => txFree[i] # txFree[j]

(* the buffer main holds never changes under it: nothing is published     *)
(* into it while main holds it, retained tail included                    *)
NoPublishWhileHeld == rxContent[rxMain] = rxView

(* every packet staged before a claim is in that claim's view: each        *)
(* datagram that arrived before the last claim was consumed earlier or is *)
(* in the buffer main claimed, none left behind in the worker's bank      *)
StagedBeforeClaimInView ==
    \A n \in 1 .. rxStagedClaim : n \in Range(rxConsumed) \cup Range(rxContent[rxMain])

(* the RX worker never spins: after a stalled pass it sleeps 1ms before   *)
(* its next pass, so a full bank cannot drive poll and the receive mutex  *)
(* back to back                                                           *)
RxBacksOff == ~rxBackoff.spun

(* a free bank on either side holds nothing: it was consumed or sent and  *)
(* cleared before it was released                                         *)
FreeIsEmpty ==
    /\ \A i \in 1 .. Len(txFree) : txContent[txFree[i]] = << >>

(* nothing staged is lost: every staged packet was sent or is still in a  *)
(* bank, a full socket's unsent rest included                             *)
SentOrHeld ==
    Len(txSent) + Len(txContent[0]) + Len(txContent[1]) + Len(txContent[2]) = staged

(* packets are consumed and sent once each, in the order they arrived     *)
RxInOrder == \A i \in 1 .. Len(rxConsumed) : rxConsumed[i] = i
TxInOrder == \A i \in 1 .. Len(txSent) : txSent[i] = i

(* main never waits on the worker's mutex                                 *)
MainNeverWaits == pc["main"] # "waitlock"

(* stop joins before free: storage is freed only after both workers       *)
(* exited and destroy joined them                                         *)
StopJoinsBeforeFree ==
    /\ ( joined => ( rxExited /\ txExited ) )
    /\ ( freed => joined )

(* once the TX worker exits, everything staged was sent or remains in a   *)
(* bank the exited worker keeps until destroy                             *)
ExitSentAll == txExited => Len(txSent) + Len(txContent[0]) + Len(txContent[1]) + Len(txContent[2]) = staged

(* a stop completes even when a full socket leaves banks unsent: both     *)
(* workers exit, destroy joins and storage is freed                       *)
StopCompletes == quit ~> freed

=============================================================================
