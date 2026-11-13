--------------------------- MODULE RNGsoHandoff ---------------------------
(***************************************************************************)
(* The cross-thread bank handoff of the GSO server transport               *)
(* (sdk/rocketnet_server_gso_linux.cpp), one receive queue plus the        *)
(* transmit bank group.                                                    *)
(*                                                                         *)
(* Receive (per queue, three buffers): the worker fills its buffer from    *)
(* the socket, then under receive_mutex publishes it as the ready buffer   *)
(* and takes the free buffer, but only when no ready buffer waits and a    *)
(* free buffer exists; otherwise it keeps filling its own buffer and,      *)
(* once full, stops reading (bounded backpressure).  Main holds one        *)
(* buffer as its stable view; it claims the ready buffer only once it      *)
(* has consumed every packet of the buffer it holds, so a partially        *)
(* consumed retained tail is never recycled under it.  Main's claim is     *)
(* a try-acquire: on contention it returns and retries next update.        *)
(* Under the mutex each side only moves buffer pointers.  A worker pass    *)
(* that stages nothing while a datagram waits, because its buffer is full  *)
(* and cannot be published, is a stall, and the worker sleeps 1ms before   *)
(* its next pass instead of polling again at once (RxStall, RxSleep,       *)
(* RxBacksOff; a failed receive takes the same path in the code and is not *)
(* modelled).  Main's walk over several queues is RNGsoRecvWalk.           *)
(*                                                                         *)
(* Transmit (three banks): game threads stage into the producer bank;      *)
(* send_packets try-publishes it into a bounded FIFO and takes a free      *)
(* bank, retaining the producer when the FIFO's free list is empty or      *)
(* the mutex is contended.  The TX worker takes banks FIFO outside the     *)
(* mutex, sends them, and returns each bank only after its synchronous     *)
(* sends finish.  A full socket takes a prefix of the held bank (possibly  *)
(* none); before shutdown the worker keeps the unsent rest, in order, and  *)
(* sends it before taking any later bank, so nothing staged is dropped for *)
(* lack of room.  The socket filling and draining is the environment       *)
(* (SocketFills, SocketDrains); it may stay full for ever.                 *)
(*                                                                         *)
(* Shutdown (rocketnet_server_gso_begin_shutdown and the server            *)
(* transport's stop step): BeginShutdown sets send_shutdown before the     *)
(* last disconnect batches, which main still stages and publishes.  From   *)
(* then on each refused send of a held bank counts; past RetryBound        *)
(* (ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT) the unsent rest is dropped and  *)
(* counted in SEND_DROPPED (txDropped) and the bank returned, so a socket  *)
(* that stays full still lets send_idle become true.  The stop step       *)
(* requests the I/O stop (MainStop, rocketnet_server_gso_stop) only once   *)
(* send_idle holds; stop hands the worker the final, empty producer, which *)
(* it takes after every ready bank and exits on.  Packets are dropped for *)
(* lack of room only after shutdown (FullSocketDropsOnlyAfterShutdown),   *)
(* and every staged packet is sent, held or counted (SentOrHeld,           *)
(* ExitSentAll).  A hard send error drops the refused message, counted in *)
(* SEND_DROPPED, at any time, and the flush goes on (txErrDropped, folded *)
(* into TxSend and TxSendFull).  Not modelled: destinations and the       *)
(* segmentation tier (README-RNGsoHandoff.md).                            *)
(*                                                                         *)
(* Stop and free: stop stores quit for both workers; the RX worker exits   *)
(* after its drain, the TX worker after its drain, which a full socket     *)
(* cannot hold past the retry bound; destroy joins both threads before     *)
(* freeing any storage, and refuses (does not free) while a worker has not *)
(* exited.                                                                 *)
(*                                                                         *)
(* Received and never consumed (StopCount, RxAccounted): main claims and   *)
(* consumes nothing once stop is asked, and the first stopped call that    *)
(* finds the workers exited (rocketnet_server_gso_count_stopped) counts    *)
(* what is still banked, main's unread tail, the ready buffer and the      *)
(* worker's buffer, as RECEIVE_DROP_STOPPED, so every datagram the worker  *)
(* received was consumed or counted.  destroy calls stopped, so the count  *)
(* is made by Join.  CountTail FALSE, the reversed witness                 *)
(* MCRNGsoHandoffBrokenStopTail.cfg, forgets main's tail and violates      *)
(* RxAccounted.                                                            *)
(*                                                                         *)
(* Defect switches, FALSE in the passing run:                              *)
(* PublishWhileHeld (the RX worker writes a datagram into the buffer main  *)
(* currently holds, as if it published over main's view), MainBlocks       *)
(* (main acquires the TX mutex with a blocking wait instead of a try),     *)
(* FullDrops (a full socket drops packets after the prefix it took and the *)
(* worker goes on with the rest of the bank) and CarryBehind (the unsent   *)
(* rest is queued behind the banks published since, instead of being       *)
(* kept).  The reversed witnesses MCRNGsoHandoffBrokenPublishWhileHeld.cfg, *)
(* MCRNGsoHandoffBrokenFullDrops.cfg and                                   *)
(* MCRNGsoHandoffBrokenCarryBehind.cfg restore one each and violate        *)
(* NoPublishWhileHeld, SentOrHeld and TxInOrder, and                      *)
(* MCRNGsoHandoffBrokenMainBlocks.cfg restores MainBlocks and violates    *)
(* MainNeverWaits.  The operator switch                                    *)
(* NoBackoff (below) is reversed by MCRNGsoHandoffBrokenNoBackoff.cfg and  *)
(* violates RxBacksOff.                                                    *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

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
Buffers == 0 .. 2
Threads == { "main", "worker" }

(* Rule switch (an operator, overridden in a cfg with <-): NoBackoff sends *)
(* the RX worker straight back to poll after a stalled pass instead of     *)
(* sleeping 1ms. FALSE in the passing run; the reversed witness            *)
(* MCRNGsoHandoffBrokenNoBackoff.cfg sets it TRUE and violates RxBacksOff. *)
NoBackoff == FALSE

(* RetryBound is ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT (100 in the code),  *)
(* taken small so the instance reaches the drop.                          *)
RetryBound == 2

(* DropBeforeShutdown counts refusals without the send_shutdown test, so  *)
(* a full socket's rest is dropped while the server runs: the reversed    *)
(* witness MCRNGsoHandoffBrokenDropBeforeShutdown.cfg violates            *)
(* FullSocketDropsOnlyAfterShutdown.  BoundOnQuitOnly counts refusals    *)
(* only once send_quit is set, a retry bound keyed on send_quit alone:    *)
(* with the stop request waiting for send_idle, a socket that stays full *)
(* holds a bank for ever and the stop is never requested;                 *)
(* MCRNGsoHandoffBrokenBoundOnQuitOnly.cfg violates ShutdownCompletes.    *)
(* Both FALSE in the passing run.                                         *)
DropBeforeShutdown == FALSE
BoundOnQuitOnly == FALSE

(* CountErrorDrops: a message the kernel refuses with a hard error (any    *)
(* errno but a full socket's, EPERM or ENETUNREACH say) is dropped and    *)
(* counted in SEND_DROPPED, and the flush goes on with the rest.  FALSE,  *)
(* the reversed witness MCRNGsoHandoffBrokenUncountedErrorDrop.cfg, drops *)
(* it uncounted and violates SentOrHeld.                                  *)
CountErrorDrops == TRUE

(* ErrorBound: hard-error drops counted in one behaviour, an instance     *)
(* bound like MaxTx, so the checks fit the run budget.  One drop reaches *)
(* every place a hard error can land (TxSend, a full socket's prefix,     *)
(* before and after shutdown, the final bank).                           *)
ErrorBound == 1

(* the positions of the first k packets a flush may see refused by hard  *)
(* errors, within ErrorBound when n were dropped already                 *)
ErrorSets(k, n) ==
    { e \in SUBSET ( 1 .. k ) : n + Cardinality(e) <= ErrorBound }

(* The first k packets of a held bank c that one flush gave the kernel,   *)
(* with the positions in e refused by a hard error: Accepted is what was  *)
(* sent, in order, and Refused what was dropped.                          *)
Positions(k, e, in) == SelectSeq( [ i \in 1 .. k |-> i ], LAMBDA i : ( i \in e ) = in )
Accepted(c, k, e) == LET p == Positions(k, e, FALSE) IN [ j \in 1 .. Len(p) |-> c[p[j]] ]
Refused(c, k, e) ==
    IF CountErrorDrops
    THEN LET p == Positions(k, e, TRUE) IN [ j \in 1 .. Len(p) |-> c[p[j]] ]
    ELSE << >>

Range(s) == { s[i] : i \in DOMAIN s }

(* CountTail: the stop count includes the unread tail of the buffer main  *)
(* holds.  FALSE, the reversed witness MCRNGsoHandoffBrokenStopTail.cfg,  *)
(* counts only the ready and the worker's buffers and violates            *)
(* RxAccounted.                                                           *)
CountTail == TRUE

VARIABLES
    lock,        \* "none", "main" or "worker": who holds the TX mutex
    pc,          \* main and the TX worker: "run", "locked", "unlock", "waitlock", "sending", "done"
    pcRx,        \* the RX worker: "run" or "done" (it never touches the TX mutex)
    rxWorker,    \* the RX buffer the worker fills, or NONE
    rxMain,      \* the RX buffer main holds
    rxContent,   \* the packets staged in each RX buffer
    rxCursor,    \* packets of rxMain main has consumed (the retained tail follows)
    rxView,      \* rxMain's content as main claimed it
    rxReady,     \* the published RX buffer waiting for main, or NONE
    rxFree,      \* the RX buffer nobody holds, or NONE
    rxBackoff,  \* owed: the last RX pass stalled and a 1ms sleep is owed
                 \* before the next; spun (ghost): a pass ran while one was owed
    arrived,     \* datagrams arrived: the next is arrived + 1
    rxConsumed,  \* packets main consumed, in order
    rxStopped,   \* datagrams counted RECEIVE_DROP_STOPPED at stop
    txProducer,  \* the TX bank main's fill threads stage into, or NONE
    txFree,      \* the free TX banks, oldest first
    txReady,     \* the published TX banks, oldest first
    txHeld,      \* the TX bank the worker holds, or NONE
    txFinal,     \* txHeld is stop's final producer
    txContent,   \* the packets staged in each TX bank
    staged,      \* packets staged: the next is staged + 1
    txSent,      \* packets the worker sent, in order
    txDropped,   \* packets the shutdown bound dropped and counted in SEND_DROPPED
    txErrDropped, \* packets a hard send error dropped and counted in SEND_DROPPED
    shut,        \* [on: send_shutdown, full: the socket refuses sends (environment),
                 \*  refusals: the held bank's refused sends since shutdown]
    quit,        \* main requested stop (send_quit)
    rxExited,    \* the RX worker exited
    txExited,    \* the TX worker exited
    joined,      \* destroy joined both workers
    freed        \* destroy freed the transport storage

vars == << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
           rxReady, rxFree, rxBackoff, arrived, rxConsumed, rxStopped, txProducer,
           txFree, txReady, txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut, quit, rxExited,
           txExited, joined, freed >>

Init ==
    /\ lock = "none"
    /\ pc = [ t \in Threads |-> "run" ]
    /\ pcRx = "run"
    /\ rxWorker = 0 /\ rxMain = 1 /\ rxFree = 2 /\ rxReady = NONE
    /\ rxContent = [ b \in Buffers |-> << >> ]
    /\ rxCursor = 0 /\ rxView = << >>
    /\ rxBackoff = [ owed |-> FALSE, spun |-> FALSE ]
    /\ arrived = 0 /\ rxConsumed = << >>
    /\ rxStopped = 0
    /\ txProducer = 0 /\ txFree = << 1, 2 >> /\ txReady = << >> /\ txHeld = NONE
    /\ txFinal = FALSE
    /\ txContent = [ b \in Buffers |-> << >> ]
    /\ staged = 0 /\ txSent = << >> /\ txDropped = << >> /\ txErrDropped = << >>
    /\ shut = [ on |-> FALSE, full |-> FALSE, refusals |-> 0 ]
    /\ quit = FALSE /\ rxExited = FALSE /\ txExited = FALSE
    /\ joined = FALSE /\ freed = FALSE

(* the RX worker stages one arrived datagram into its own buffer, outside *)
(* the mutex; a full buffer with nowhere to publish simply has no fill    *)
(* step enabled, which is the queue's bounded backpressure; after a       *)
(* stalled pass the worker sleeps (RxSleep) before its next pass          *)
RxFill ==
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ ~rxBackoff.owed
    /\ rxWorker # NONE /\ Len(rxContent[rxWorker]) < RxCap
    /\ arrived < MaxRx
    /\ rxContent' = [ rxContent EXCEPT ![rxWorker] = Append( @, arrived + 1 ) ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxCursor, rxView,
                    rxReady, rxFree, rxBackoff, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut, quit,
                    rxExited, txExited, joined, freed >>

(* a stalled pass (rocketnet_server_gso_worker_step returning true): a     *)
(* datagram waits, so poll returns at once, but the worker's buffer is     *)
(* full and cannot be published, because a ready buffer still waits for    *)
(* main or no buffer is free, so the pass stages nothing. The worker then  *)
(* owes a 1ms sleep before its next pass; NoBackoff instead lets it go     *)
(* straight back to poll, the spin, and a stall while a sleep is still     *)
(* owed is recorded as one. A failed receive takes the same path in the    *)
(* code; it is not modelled separately (test_server_gso_receive_backoff    *)
(* covers it)                                                              *)
RxStall ==
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ rxWorker # NONE /\ Len(rxContent[rxWorker]) = RxCap
    /\ ~( rxReady = NONE /\ rxFree # NONE )
    /\ arrived < MaxRx
    /\ NoBackoff \/ ~rxBackoff.owed
    /\ rxBackoff' = [ owed |-> TRUE, spun |-> rxBackoff.spun \/ rxBackoff.owed ]
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, quit, rxExited, txExited, joined, freed >>

(* the 1ms sleep after a stalled pass, outside the mutex                    *)
RxSleep ==
    /\ pcRx = "run" /\ rxBackoff.owed
    /\ rxBackoff' = [ rxBackoff EXCEPT !.owed = FALSE ]
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, quit, rxExited, txExited, joined, freed >>

(* defect: the worker writes into the buffer main holds, as if a publish   *)
(* landed on main's view instead of the ready slot                        *)
WriteHeld ==
    /\ PublishWhileHeld
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ Len(rxContent[rxMain]) < RxCap
    /\ arrived < MaxRx
    /\ rxContent' = [ rxContent EXCEPT ![rxMain] = Append( @, arrived + 1 ) ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxCursor, rxView,
                    rxReady, rxFree, rxBackoff, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut, quit,
                    rxExited, txExited, joined, freed >>

(* the RX publish exchange, under the mutex: the worker's filled buffer    *)
(* becomes ready and it takes the free buffer, only when no ready buffer  *)
(* waits and a free buffer exists; otherwise nothing moves and the        *)
(* worker retains its buffer. The worker publishes after a pass, so not   *)
(* while it owes a sleep                                                  *)
RxPublish ==
    /\ pcRx = "run" /\ ~rxExited
    /\ ~rxBackoff.owed
    /\ rxWorker # NONE /\ rxContent[rxWorker] # << >>
    /\ rxReady = NONE /\ rxFree # NONE
    /\ rxReady' = rxWorker /\ rxWorker' = rxFree /\ rxFree' = NONE
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxMain, rxContent, rxCursor, rxView,
                    rxBackoff, arrived, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut,
                    quit, rxExited, txExited, joined, freed >>

(* main claims the ready buffer only once the buffer it holds is fully     *)
(* consumed, under the mutex: its old buffer becomes free, the ready one   *)
(* becomes its view                                                        *)
RxClaim ==
    /\ pc["main"] = "run" /\ ~quit
    /\ rxCursor = Len(rxContent[rxMain])
    /\ rxReady # NONE
    /\ rxFree' = rxMain /\ rxMain' = rxReady /\ rxReady' = NONE
    /\ rxView' = rxContent[rxReady] /\ rxCursor' = 0
    /\ rxContent' = [ rxContent EXCEPT ![rxMain] = << >> ]
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxBackoff, arrived, rxConsumed,
                    txProducer, txFree, txReady, txHeld, txFinal, txContent,
                    staged, txSent, txDropped, txErrDropped, shut, quit, rxExited, txExited,
                    joined, freed >>

(* main consumes one packet of the buffer it holds; between the first and *)
(* the last packet the unconsumed tail stays retained in main's buffer    *)
RxConsume ==
    /\ pc["main"] = "run"
    /\ rxCursor < Len(rxContent[rxMain])
    /\ rxConsumed' = Append( rxConsumed, rxContent[rxMain][rxCursor + 1] )
    /\ rxCursor' = rxCursor + 1
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxView,
                    rxReady, rxFree, rxBackoff, arrived, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut, quit,
                    rxExited, txExited, joined, freed >>

(* a fill thread stages one packet into the producer bank, outside the    *)
(* mutex                                                                  *)
TxStage ==
    /\ pc["main"] = "run" /\ ~quit
    /\ txProducer # NONE /\ Len(txContent[txProducer]) < TxCap
    /\ staged < MaxTx
    /\ txContent' = [ txContent EXCEPT ![txProducer] = Append( @, staged + 1 ) ]
    /\ staged' = staged + 1
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txSent, txDropped, txErrDropped, shut, quit, rxExited,
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
    /\ UNCHANGED << rxStopped, pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut,
                    quit, rxExited, txExited, joined, freed >>

TxMainWait ==
    /\ pc["main"] = "waitlock" /\ lock = "none"
    /\ lock' = "main"
    /\ pc' = [ pc EXCEPT !["main"] = "locked" ]
    /\ UNCHANGED << rxStopped, pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut,
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
       ELSE UNCHANGED << rxStopped, txReady, txProducer, txFree >>
    /\ pc' = [ pc EXCEPT !["main"] = "unlock" ]
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txHeld,
                    txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut, quit, rxExited,
                    txExited, joined, freed >>

(* the TX worker acquires the mutex for a take or a return                *)
TxWorkerLock ==
    /\ pc["worker"] = "run" /\ ~txExited
    /\ lock = "none"
    /\ lock' = "worker"
    /\ pc' = [ pc EXCEPT !["worker"] = "locked" ]
    /\ UNCHANGED << rxStopped, pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut,
                    quit, rxExited, txExited, joined, freed >>

(* the take, mutex held: the oldest ready bank first; at quit, with the   *)
(* ready FIFO drained, stop's final producer                             *)
TxTake ==
    /\ pc["worker"] = "locked" /\ txHeld = NONE
    /\ \/ /\ txReady # << >>
          /\ txHeld' = Head( txReady ) /\ txReady' = Tail( txReady )
          /\ txFinal' = FALSE
          /\ UNCHANGED << rxStopped, txProducer >>
       \/ /\ txReady = << >> /\ quit /\ txProducer # NONE
          /\ txHeld' = txProducer /\ txProducer' = NONE
          /\ txFinal' = TRUE
          /\ UNCHANGED txReady
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    \* retries_after_quit starts at 0 for each bank the worker takes
    /\ shut' = [ shut EXCEPT !.refusals = 0 ]
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txFree,
                    txContent, staged, txSent, txDropped, txErrDropped, quit, rxExited, txExited,
                    joined, freed >>

(* the take that finds nothing, mutex held: no bank moves and the worker *)
(* drops the mutex and waits for the next wake                            *)
TxTakeNone ==
    /\ pc["worker"] = "locked" /\ txHeld = NONE
    /\ txReady = << >>
    /\ ( ~quit \/ txProducer = NONE )
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, quit, rxExited, txExited, joined, freed >>

(* either thread drops the mutex; a publisher wakes the worker after the  *)
(* drop, never under it (the wake is not state this model needs)          *)
Unlock(t) ==
    /\ pc[t] = "unlock" /\ lock = t
    /\ lock' = "none"
    /\ pc' = [ pc EXCEPT ![t] = IF t = "worker" /\ txHeld # NONE THEN "sending" ELSE "run" ]
    /\ UNCHANGED << rxStopped, pcRx, rxWorker, rxMain, rxContent, rxCursor, rxView,
                    rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut,
                    quit, rxExited, txExited, joined, freed >>

(* the worker sends the bank it holds, outside the mutex, and clears it;  *)
(* stop's final bank is sent last and ends the worker instead of being    *)
(* returned                                                               *)
TxSend ==
    /\ pc["worker"] = "sending" /\ txHeld # NONE
    \* an empty bank makes no send call (send_bank's loop does not run), so
    \* a full socket does not hold it
    /\ ~shut.full \/ txContent[txHeld] = << >>
    \* any message may meet a hard error: dropped, counted, and the flush
    \* goes on (rocketnet_server_gso_flush after sendmmsg returns -1)
    /\ \E e \in ErrorSets( Len(txContent[txHeld]), Len(txErrDropped) ) :
          /\ txSent' = txSent \o Accepted(txContent[txHeld], Len(txContent[txHeld]), e)
          /\ txErrDropped' = txErrDropped \o Refused(txContent[txHeld], Len(txContent[txHeld]), e)
    /\ txContent' = [ txContent EXCEPT ![txHeld] = << >> ]
    /\ IF txFinal
       THEN /\ txExited' = TRUE
            /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
            \* the final bank stays the exited worker's: it is retired
            \* unsent-to-free and freed with the transport at destroy
            /\ UNCHANGED << rxStopped, txHeld, txFinal >>
       ELSE /\ pc' = [ pc EXCEPT !["worker"] = "run" ]
            /\ UNCHANGED << rxStopped, txExited, txHeld, txFinal >>
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, staged, txDropped, shut, quit, rxExited, joined, freed >>

(* a full socket (TxSendFail): the send takes the first k packets of the   *)
(* held bank, k less than the bank holds (rocketnet_server_gso_flush       *)
(* returning the first unsent slot with full set); the bank and its unsent *)
(* rest stay with the worker.  Before shutdown the worker waits and        *)
(* retries with no limit.  Once send_shutdown is set, each refusal counts  *)
(* (retries_after_quit, reset only when the worker takes a bank); while    *)
(* the count is at most RetryBound the worker retries from the unsent slot *)
(* (TxRetry), and when it passes the bound the rest is dropped and counted *)
(* in SEND_DROPPED (TxDropTail): a ready bank is returned and the next one *)
(* taken, the final producer ends the worker.  The model folds TxSendFail  *)
(* with the following TxRetry or TxDropTail into one step.                 *)
(* DropBeforeShutdown counts refusals before shutdown too; BoundOnQuitOnly *)
(* counts them only once send_quit is set (a retry bound keyed on          *)
(* send_quit alone, while the stop request waits for send_idle).           *)
(* FullDrops instead drops d packets after the prefix, the rest of the     *)
(* refused batch, and goes on; CarryBehind queues the bank behind the      *)
(* ready banks instead of keeping it                                       *)
Counting ==
    IF BoundOnQuitOnly THEN quit ELSE ( shut.on \/ DropBeforeShutdown )

TxSendFull ==
    /\ pc["worker"] = "sending" /\ txHeld # NONE
    /\ shut.full
    /\ LET c == txContent[txHeld]
           r1 == IF Counting THEN shut.refusals + 1 ELSE shut.refusals IN
       \E k \in 0 .. Len(c) - 1 :
       \E d \in ( IF FullDrops THEN 1 .. Len(c) - k ELSE { 0 } ) :
       \E e \in ErrorSets( k, Len(txErrDropped) ) :
          LET rest == SubSeq( c, k + d + 1, Len(c) ) IN
          /\ txSent' = txSent \o Accepted( c, k, e )
          /\ txErrDropped' = txErrDropped \o Refused( c, k, e )
          /\ shut' = [ shut EXCEPT !.refusals = r1 ]
          /\ IF r1 > RetryBound
             THEN \* TxDropTail: count - slot added to SEND_DROPPED
                  /\ txContent' = [ txContent EXCEPT ![txHeld] = << >> ]
                  /\ txDropped' = txDropped \o rest
                  /\ IF txFinal
                     THEN /\ txExited' = TRUE
                          /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
                     ELSE /\ pc' = [ pc EXCEPT !["worker"] = "run" ]
                          /\ UNCHANGED txExited
                  /\ UNCHANGED << rxStopped, txReady, txHeld, txFinal >>
             ELSE \* TxRetry: the rest stays for the next attempt
                  /\ txContent' = [ txContent EXCEPT ![txHeld] = rest ]
                  /\ txDropped' = txDropped
                  /\ txExited' = txExited
                  /\ txFinal' = txFinal
                  /\ IF CarryBehind /\ ~txFinal
                     THEN /\ txReady' = Append( txReady, txHeld )
                          /\ txHeld' = NONE
                          /\ pc' = [ pc EXCEPT !["worker"] = "run" ]
                     ELSE UNCHANGED << rxStopped, txReady, txHeld, pc >>
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, staged, quit, rxExited, joined, freed >>

(* environment: the socket's send buffer fills or drains; a socket may    *)
(* stay full for ever (no fairness on SocketDrains)                        *)
SocketFills ==
    /\ ~shut.full /\ ~freed
    /\ shut' = [ shut EXCEPT !.full = TRUE ]
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, quit, rxExited, txExited, joined, freed >>

SocketDrains ==
    /\ shut.full /\ ~freed
    /\ shut' = [ shut EXCEPT !.full = FALSE ]
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, quit, rxExited, txExited, joined, freed >>

(* BeginShutdown: the server transport's stop calls                       *)
(* rocketnet_server_gso_begin_shutdown first, once, before any disconnect *)
(* batch is drained: send_shutdown is set (and the wake).  Main keeps      *)
(* staging and publishing after it                                         *)
BeginShutdown ==
    /\ pc["main"] = "run" /\ ~shut.on
    /\ shut' = [ shut EXCEPT !.on = TRUE ]
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, quit, rxExited, txExited, joined, freed >>

(* rocketnet_server_gso_send_idle: the producer holds nothing, no bank is *)
(* ready and the worker holds none (free_count = TX_BANKS - 1)             *)
SendIdle ==
    /\ ( txProducer = NONE \/ txContent[txProducer] = << >> )
    /\ txReady = << >> /\ txHeld = NONE

(* the return, mutex held: a sent bank rejoins the free FIFO only after   *)
(* its sends finished (TxSend cleared it)                                 *)
TxReturn ==
    /\ pc["worker"] = "locked" /\ txHeld # NONE /\ ~txFinal
    /\ txContent[txHeld] = << >>
    /\ txFree' = Append( txFree, txHeld ) /\ txHeld' = NONE
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txReady, txFinal, txContent, staged, txSent, txDropped, txErrDropped, shut, quit,
                    rxExited, txExited, joined, freed >>

(* StopStep's stop request, rocketnet_server_gso_stop: made only once      *)
(* shutdown has begun, the disconnect bursts are drained and send_idle     *)
(* holds; it stores send_shutdown (already set), then quit for both        *)
(* workers, and the final producer passes to the worker with the store     *)
MainStop ==
    /\ pc["main"] = "run" /\ ~quit
    /\ shut.on /\ SendIdle
    /\ quit' = TRUE
    /\ pc' = [ pc EXCEPT !["main"] = "done" ]
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, rxExited, txExited, joined, freed >>

(* the RX worker exits once quit is stored; buffers it still owns are     *)
(* simply never published again                                           *)
RxWorkerExit ==
    /\ pcRx = "run" /\ quit /\ ~rxExited
    /\ rxExited' = TRUE
    /\ pcRx' = "done"
    /\ UNCHANGED << rxStopped, lock, pc, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, quit, txExited, joined, freed >>

(* the TX worker also exits at quit when nothing was ever staged: the     *)
(* final producer is empty, so taking it would send nothing               *)
TxWorkerExitIdle ==
    /\ pc["worker"] = "run" /\ quit /\ ~txExited
    /\ txHeld = NONE /\ txReady = << >>
    /\ ( txProducer = NONE \/ txContent[txProducer] = << >> )
    /\ txExited' = TRUE
    /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
    /\ UNCHANGED << rxStopped, pcRx, lock, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, quit, rxExited, joined, freed >>

(* the stop count, rocketnet_server_gso_count_stopped: what was received *)
(* and never consumed (the unread tail of main's buffer, the ready buffer  *)
(* and the worker's buffer), counted RECEIVE_DROP_STOPPED.  The code makes *)
(* it in the first stopped call that finds the worker exited, and destroy  *)
(* calls stopped before it joins; once both workers have exited and main   *)
(* has stopped nothing moves a receive buffer, so the count is folded into *)
(* Join, the same value without the interleavings of a separate step       *)
StopCount ==
    ( IF CountTail THEN Len(rxContent[rxMain]) - rxCursor ELSE 0 )
  + ( IF rxReady # NONE THEN Len(rxContent[rxReady]) ELSE 0 )
  + ( IF rxWorker # NONE THEN Len(rxContent[rxWorker]) ELSE 0 )

(* destroy joins both workers, and only a joined transport is freed: a    *)
(* destroy that arrives before the workers exit joins nothing and frees   *)
(* nothing                                                                *)
Join ==
    /\ quit /\ rxExited /\ txExited /\ ~joined /\ ~freed
    /\ joined' = TRUE
    /\ rxStopped' = StopCount
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, quit, rxExited, txExited, freed >>

Free ==
    /\ joined /\ ~freed
    /\ freed' = TRUE
    /\ UNCHANGED << rxStopped, lock, pc, pcRx, rxWorker, rxMain, rxContent, rxCursor,
                    rxView, rxReady, rxFree, rxBackoff, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, txDropped, txErrDropped, shut, quit, rxExited, txExited, joined >>

(* the behaviour ends once the transport is freed                         *)
Terminated ==
    /\ freed
    /\ UNCHANGED vars

Next ==
    \/ Terminated
    \/ RxFill \/ RxStall \/ RxSleep \/ WriteHeld \/ RxPublish \/ RxClaim \/ RxConsume
    \/ TxStage \/ TxMainLock \/ TxMainWait \/ TxPublish
    \/ TxWorkerLock \/ TxTake \/ TxTakeNone \/ TxSend \/ TxSendFull \/ TxReturn
    \/ SocketFills \/ SocketDrains \/ BeginShutdown
    \/ \E t \in Threads : Unlock(t)
    \/ MainStop \/ RxWorkerExit \/ TxWorkerExitIdle \/ Join \/ Free

Spec == Init /\ [][Next]_vars

(* the worker runs whenever it can and retries a held bank on its timer   *)
(* whatever the socket does (SF TxSend and SF TxSendFull: a socket that    *)
(* drains and fills for ever still meets one of them), main keeps updating *)
(* and finishes what it started, and once shutdown has begun the stop      *)
(* step keeps polling (SF MainStop); shutdown itself and the socket are    *)
(* never forced                                                            *)
SpecFair ==
    /\ Spec
    /\ WF_vars( RxFill ) /\ WF_vars( RxSleep ) /\ WF_vars( RxPublish )
    /\ WF_vars( RxClaim )
    /\ WF_vars( RxConsume ) /\ SF_vars( TxSend ) /\ SF_vars( TxSendFull )
    /\ SF_vars( MainStop ) /\ WF_vars( TxReturn )
    /\ SF_vars( TxWorkerLock ) /\ WF_vars( TxTake ) /\ WF_vars( TxTakeNone )
    /\ \A t \in Threads : WF_vars( Unlock(t) )
    /\ SF_vars( TxMainLock ) /\ WF_vars( TxPublish ) /\ WF_vars( TxMainWait )
    /\ SF_vars( RxWorkerExit ) /\ SF_vars( TxWorkerExitIdle )
    /\ WF_vars( Join ) /\ WF_vars( Free )

----------------------------------------------------------------------------

TypeOK ==
    /\ lock \in { "none", "main", "worker" }
    /\ pcRx \in { "run", "done" }
    /\ rxWorker \in Buffers \cup { NONE } /\ rxReady \in Buffers \cup { NONE }
    /\ rxMain \in Buffers /\ rxFree \in Buffers \cup { NONE }
    /\ rxBackoff \in [ owed : BOOLEAN, spun : BOOLEAN ]
    /\ arrived \in 0 .. MaxRx /\ staged \in 0 .. MaxTx
    /\ rxStopped \in 0 .. MaxRx
    /\ txProducer \in Buffers \cup { NONE } /\ txHeld \in Buffers \cup { NONE }
    /\ shut \in [ on : BOOLEAN, full : BOOLEAN, refusals : 0 .. RetryBound + 1 ]

(* every receive buffer has exactly one owner                             *)
RxOneOwner ==
    \A b \in Buffers :
        ( IF rxWorker = b THEN 1 ELSE 0 ) + ( IF rxReady = b THEN 1 ELSE 0 )
      + ( IF rxMain = b THEN 1 ELSE 0 ) + ( IF rxFree = b THEN 1 ELSE 0 ) = 1

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

(* no datagram the worker received goes uncounted: each was consumed by   *)
(* main or is still banked (main's unread tail, the ready buffer, the      *)
(* worker's buffer), and once stopped has counted, each one not consumed   *)
(* is in RECEIVE_DROP_STOPPED (StopCount, made by Join)                   *)
RxAccounted ==
    LET banked == ( Len(rxContent[rxMain]) - rxCursor )
                + ( IF rxReady # NONE THEN Len(rxContent[rxReady]) ELSE 0 )
                + ( IF rxWorker # NONE THEN Len(rxContent[rxWorker]) ELSE 0 )
    IN /\ Len(rxConsumed) + banked = arrived
       /\ joined => Len(rxConsumed) + rxStopped = arrived

(* the RX worker never spins: after a stalled pass it sleeps 1ms before   *)
(* its next pass, so a full buffer that cannot be published cannot drive *)
(* poll back to back                                                      *)
RxBacksOff == ~rxBackoff.spun

(* a free bank on either side holds nothing: it was consumed or sent and  *)
(* cleared before it was released                                         *)
FreeIsEmpty ==
    /\ ( rxFree # NONE => rxContent[rxFree] = << >> )
    /\ \A i \in 1 .. Len(txFree) : txContent[txFree[i]] = << >>

(* nothing staged is lost uncounted: every staged packet was sent, is      *)
(* still in a bank (a full socket's unsent rest included), or was dropped *)
(* and counted in SEND_DROPPED: for lack of socket room (txDropped, empty *)
(* before shutdown: FullSocketDropsOnlyAfterShutdown) or by a hard send    *)
(* error (txErrDropped, at any time)                                       *)
SentOrHeld ==
    Len(txSent) + Len(txContent[0]) + Len(txContent[1]) + Len(txContent[2])
        + Len(txDropped) + Len(txErrDropped) = staged

(* the send worker drops packets for lack of socket room only after       *)
(* shutdown has begun.  A hard send error or an unroutable destination    *)
(* drops (counted) at any time; that is txErrDropped, not this rule.      *)
FullSocketDropsOnlyAfterShutdown == txDropped # << >> => shut.on

(* Reachability target, expected to fail: a hard send error drops a       *)
(* packet, counted, while the server runs                                 *)
NotReachedErrorDropRunning == ~( txErrDropped # << >> /\ ~shut.on )

(* the refusal count never passes the bound by more than the one refusal  *)
(* that drops the tail                                                     *)
RefusalsBounded == shut.refusals <= RetryBound + 1

(* packets are consumed and sent once each, in the order they arrived;    *)
(* the sent packets rise, and every packet below one sent was sent or     *)
(* dropped and counted (with both drop lists empty this is txSent[i] = i) *)
RxInOrder == \A i \in 1 .. Len(rxConsumed) : rxConsumed[i] = i
TxInOrder ==
    /\ \A i \in 1 .. Len(txSent) - 1 : txSent[i] < txSent[i + 1]
    /\ \A i \in 1 .. Len(txSent) : \A n \in 1 .. txSent[i] :
          n \in Range(txSent) \cup Range(txDropped) \cup Range(txErrDropped)

(* main never waits on the worker's mutex                                 *)
MainNeverWaits == pc["main"] # "waitlock"

(* stop joins before free: storage is freed only after both workers       *)
(* exited and destroy joined them                                         *)
StopJoinsBeforeFree ==
    /\ ( joined => ( rxExited /\ txExited ) )
    /\ ( freed => joined )

(* once the TX worker exits, every staged packet was sent or dropped and   *)
(* counted: no bank still holds one                                        *)
ExitSentAll == txExited => Len(txSent) + Len(txDropped) + Len(txErrDropped) = staged

(* a stop completes even when a full socket leaves banks unsent: both     *)
(* workers exit, destroy joins and storage is freed                       *)
StopCompletes == quit ~> freed

(* a shutdown completes even on a socket that stays full: each bank is    *)
(* held for at most RetryBound + 1 refusals after shutdown, so send_idle   *)
(* becomes true, the stop is requested and the transport is freed          *)
ShutdownCompletes == shut.on ~> freed

=============================================================================
