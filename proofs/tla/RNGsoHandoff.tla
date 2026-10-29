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
(*                                                                         *)
(* Transmit (three banks): game threads stage into the producer bank;      *)
(* send_packets try-publishes it into a bounded FIFO and takes a free      *)
(* bank, retaining the producer when the FIFO's free list is empty or      *)
(* the mutex is contended.  The TX worker takes banks FIFO outside the     *)
(* mutex, sends them, and returns each bank only after its synchronous     *)
(* sends finish.  Stop hands the worker the final unpublished producer     *)
(* after every ready bank; the worker sends it last and exits without      *)
(* returning it.                                                           *)
(*                                                                         *)
(* Stop and free: stop stores quit for both workers; each worker exits     *)
(* only after its drain; destroy joins both threads before freeing any     *)
(* storage, and refuses (does not free) while a worker has not exited.     *)
(*                                                                         *)
(* Defect switches, FALSE in the passing run: PublishWhileHeld (the RX     *)
(* worker writes a datagram into the buffer main currently holds, as if    *)
(* it published over main's view) and MainBlocks (main acquires the TX     *)
(* mutex with a blocking wait instead of a try).  The reversed witness     *)
(* MCRNGsoHandoffBrokenPublishWhileHeld.cfg restores the first and        *)
(* violates NoPublishWhileHeld.                                            *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS RxCap,            \* packets one receive buffer holds
          TxCap,            \* packets one transmit bank holds
          MaxRx,            \* datagrams that arrive in one behaviour
          MaxTx,            \* packets staged in one behaviour
          PublishWhileHeld, \* defect: worker writes into main's buffer
          MainBlocks        \* defect: main waits on the TX mutex

ASSUME RxCap >= 1 /\ TxCap >= 1 /\ MaxRx >= 1 /\ MaxTx >= 1

NONE == 3
Buffers == 0 .. 2
Threads == { "main", "worker" }

VARIABLES
    lock,        \* "none", "main" or "worker": who holds the TX mutex
    pc,          \* main and the TX worker: "run", "locked", "unlock", "waitlock", "sending", "done"
    pcRx,        \* the RX worker: "run" or "done" (it never touches the TX mutex)
    rxWorker,    \* the RX buffer the worker fills, or NONE
    rxReady,     \* the published RX buffer waiting for main, or NONE
    rxMain,      \* the RX buffer main holds
    rxFree,      \* the RX buffer nobody holds, or NONE
    rxContent,   \* the packets staged in each RX buffer
    rxCursor,    \* packets of rxMain main has consumed (the retained tail follows)
    rxView,      \* rxMain's content as main claimed it
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

vars == << lock, pc, pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent, rxCursor,
           rxView, arrived, rxConsumed, txProducer, txFree, txReady, txHeld,
           txFinal, txContent, staged, txSent, quit, rxExited, txExited,
           joined, freed >>

Init ==
    /\ lock = "none"
    /\ pc = [ t \in Threads |-> "run" ]
    /\ pcRx = "run"
    /\ rxWorker = 0 /\ rxMain = 1 /\ rxFree = 2 /\ rxReady = NONE
    /\ rxContent = [ b \in Buffers |-> << >> ]
    /\ rxCursor = 0 /\ rxView = << >>
    /\ arrived = 0 /\ rxConsumed = << >>
    /\ txProducer = 0 /\ txFree = << 1, 2 >> /\ txReady = << >> /\ txHeld = NONE
    /\ txFinal = FALSE
    /\ txContent = [ b \in Buffers |-> << >> ]
    /\ staged = 0 /\ txSent = << >>
    /\ quit = FALSE /\ rxExited = FALSE /\ txExited = FALSE
    /\ joined = FALSE /\ freed = FALSE

(* the RX worker stages one arrived datagram into its own buffer, outside *)
(* the mutex; a full buffer with nowhere to publish simply has no fill    *)
(* step enabled, which is the queue's bounded backpressure                *)
RxFill ==
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ rxWorker # NONE /\ Len(rxContent[rxWorker]) < RxCap
    /\ arrived < MaxRx
    /\ rxContent' = [ rxContent EXCEPT ![rxWorker] = Append( @, arrived + 1 ) ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxReady, rxMain, rxFree, rxCursor,
                    rxView, rxConsumed, txProducer, txFree, txReady, txHeld,
                    txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

(* defect: the worker writes into the buffer main holds, as if a publish   *)
(* landed on main's view instead of the ready slot                        *)
WriteHeld ==
    /\ PublishWhileHeld
    /\ pcRx = "run" /\ ~rxExited /\ ~quit
    /\ Len(rxContent[rxMain]) < RxCap
    /\ arrived < MaxRx
    /\ rxContent' = [ rxContent EXCEPT ![rxMain] = Append( @, arrived + 1 ) ]
    /\ arrived' = arrived + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxReady, rxMain, rxFree, rxCursor,
                    rxView, rxConsumed, txProducer, txFree, txReady, txHeld,
                    txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

(* the RX publish exchange, under the mutex: the worker's filled buffer    *)
(* becomes ready and it takes the free buffer, only when no ready buffer  *)
(* waits and a free buffer exists; otherwise nothing moves and the        *)
(* worker retains its buffer                                              *)
RxPublish ==
    /\ pcRx = "run" /\ ~rxExited
    /\ rxWorker # NONE /\ rxContent[rxWorker] # << >>
    /\ rxReady = NONE /\ rxFree # NONE
    /\ rxReady' = rxWorker /\ rxWorker' = rxFree /\ rxFree' = NONE
    /\ UNCHANGED << lock, pc, pcRx, rxMain, rxContent, rxCursor, rxView, arrived,
                    rxConsumed, txProducer, txFree, txReady, txHeld, txFinal,
                    txContent, staged, txSent, quit, rxExited, txExited,
                    joined, freed >>

(* main claims the ready buffer only once the buffer it holds is fully     *)
(* consumed: its old buffer becomes free, the ready one becomes its view   *)
RxClaim ==
    /\ pc["main"] = "run" /\ ~quit
    /\ rxCursor = Len(rxContent[rxMain])
    /\ rxReady # NONE
    /\ rxFree' = rxMain /\ rxMain' = rxReady /\ rxReady' = NONE
    /\ rxView' = rxContent[rxReady] /\ rxCursor' = 0
    /\ rxContent' = [ rxContent EXCEPT ![rxMain] = << >> ]
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, arrived, rxConsumed, txProducer,
                    txFree, txReady, txHeld, txFinal, txContent, staged,
                    txSent, quit, rxExited, txExited, joined, freed >>

(* main consumes one packet of the buffer it holds; between the first and *)
(* the last packet the unconsumed tail stays retained in main's buffer    *)
RxConsume ==
    /\ pc["main"] = "run"
    /\ rxCursor < Len(rxContent[rxMain])
    /\ rxConsumed' = Append( rxConsumed, rxContent[rxMain][rxCursor + 1] )
    /\ rxCursor' = rxCursor + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxView, arrived, txProducer, txFree, txReady, txHeld,
                    txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

(* a fill thread stages one packet into the producer bank, outside the    *)
(* mutex                                                                  *)
TxStage ==
    /\ pc["main"] = "run" /\ ~quit
    /\ txProducer # NONE /\ Len(txContent[txProducer]) < TxCap
    /\ staged < MaxTx
    /\ txContent' = [ txContent EXCEPT ![txProducer] = Append( @, staged + 1 ) ]
    /\ staged' = staged + 1
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txSent, quit, rxExited, txExited,
                    joined, freed >>

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
    /\ UNCHANGED << pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent, rxCursor,
                    rxView, arrived, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

TxMainWait ==
    /\ pc["main"] = "waitlock" /\ lock = "none"
    /\ lock' = "main"
    /\ pc' = [ pc EXCEPT !["main"] = "locked" ]
    /\ UNCHANGED << pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent, rxCursor,
                    rxView, arrived, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

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
    /\ UNCHANGED << pcRx, lock, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txHeld, txFinal,
                    txContent, staged, txSent, quit, rxExited, txExited,
                    joined, freed >>

(* the TX worker acquires the mutex for a take or a return                *)
TxWorkerLock ==
    /\ pc["worker"] = "run" /\ ~txExited
    /\ lock = "none"
    /\ lock' = "worker"
    /\ pc' = [ pc EXCEPT !["worker"] = "locked" ]
    /\ UNCHANGED << pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent, rxCursor,
                    rxView, arrived, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

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
    /\ UNCHANGED << pcRx, lock, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txFree, txContent,
                    staged, txSent, quit, rxExited, txExited, joined, freed >>

(* the take that finds nothing, mutex held: no bank moves and the worker *)
(* drops the mutex and waits for the next wake                            *)
TxTakeNone ==
    /\ pc["worker"] = "locked" /\ txHeld = NONE
    /\ txReady = << >>
    /\ ( ~quit \/ txProducer = NONE )
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, joined, freed >>

(* either thread drops the mutex; a publisher wakes the worker after the  *)
(* drop, never under it (the wake is not state this model needs)          *)
Unlock(t) ==
    /\ pc[t] = "unlock" /\ lock = t
    /\ lock' = "none"
    /\ pc' = [ pc EXCEPT ![t] = IF t = "worker" /\ txHeld # NONE THEN "sending" ELSE "run" ]
    /\ UNCHANGED << pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent, rxCursor,
                    rxView, arrived, rxConsumed, txProducer, txFree, txReady,
                    txHeld, txFinal, txContent, staged, txSent, quit, rxExited,
                    txExited, joined, freed >>

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
    /\ UNCHANGED << pcRx, lock, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, staged, quit, rxExited, joined, freed >>

(* the return, mutex held: a sent bank rejoins the free FIFO only after   *)
(* its sends finished (TxSend cleared it)                                 *)
TxReturn ==
    /\ pc["worker"] = "locked" /\ txHeld # NONE /\ ~txFinal
    /\ txContent[txHeld] = << >>
    /\ txFree' = Append( txFree, txHeld ) /\ txHeld' = NONE
    /\ pc' = [ pc EXCEPT !["worker"] = "unlock" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer,
                    txReady, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, joined, freed >>

(* stop: quit is stored for both workers after main's last stage and      *)
(* publish; the final producer passes to the worker with the store        *)
MainStop ==
    /\ pc["main"] = "run" /\ ~quit
    /\ quit' = TRUE
    /\ pc' = [ pc EXCEPT !["main"] = "done" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent,
                    rxExited, txExited, joined, freed >>

(* the RX worker exits once quit is stored; buffers it still owns are     *)
(* simply never published again                                           *)
RxWorkerExit ==
    /\ pcRx = "run" /\ quit /\ ~rxExited
    /\ rxExited' = TRUE
    /\ pcRx' = "done"
    /\ UNCHANGED << lock, pc, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, quit,
                    txExited, joined, freed >>

(* the TX worker also exits at quit when nothing was ever staged: the     *)
(* final producer is empty, so taking it would send nothing               *)
TxWorkerExitIdle ==
    /\ pc["worker"] = "run" /\ quit /\ ~txExited
    /\ txHeld = NONE /\ txReady = << >>
    /\ ( txProducer = NONE \/ txContent[txProducer] = << >> )
    /\ txExited' = TRUE
    /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
    /\ UNCHANGED << pcRx, lock, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, quit,
                    rxExited, joined, freed >>

(* destroy joins both workers, and only a joined transport is freed: a    *)
(* destroy that arrives before the workers exit joins nothing and frees   *)
(* nothing                                                                *)
Join ==
    /\ quit /\ rxExited /\ txExited /\ ~joined /\ ~freed
    /\ joined' = TRUE
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, freed >>

Free ==
    /\ joined /\ ~freed
    /\ freed' = TRUE
    /\ UNCHANGED << lock, pc, pcRx, rxWorker, rxReady, rxMain, rxFree, rxContent,
                    rxCursor, rxView, arrived, rxConsumed, txProducer, txFree,
                    txReady, txHeld, txFinal, txContent, staged, txSent, quit,
                    rxExited, txExited, joined >>

(* the behaviour ends once the transport is freed                         *)
Terminated ==
    /\ freed
    /\ UNCHANGED vars

Next ==
    \/ Terminated
    \/ RxFill \/ WriteHeld \/ RxPublish \/ RxClaim \/ RxConsume
    \/ TxStage \/ TxMainLock \/ TxMainWait \/ TxPublish
    \/ TxWorkerLock \/ TxTake \/ TxTakeNone \/ TxSend \/ TxReturn
    \/ \E t \in Threads : Unlock(t)
    \/ MainStop \/ RxWorkerExit \/ TxWorkerExitIdle \/ Join \/ Free

Spec == Init /\ [][Next]_vars

(* the worker runs whenever it can, main keeps updating and finishes what *)
(* it started, and stop is never forced                                   *)
SpecFair ==
    /\ Spec
    /\ WF_vars( RxFill ) /\ WF_vars( RxPublish ) /\ WF_vars( RxClaim )
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
    /\ rxWorker \in Buffers \cup { NONE } /\ rxReady \in Buffers \cup { NONE }
    /\ rxMain \in Buffers /\ rxFree \in Buffers \cup { NONE }
    /\ arrived \in 0 .. MaxRx /\ staged \in 0 .. MaxTx
    /\ txProducer \in Buffers \cup { NONE } /\ txHeld \in Buffers \cup { NONE }

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

(* a free bank on either side holds nothing: it was consumed or sent and  *)
(* cleared before it was released                                         *)
FreeIsEmpty ==
    /\ ( rxFree # NONE => rxContent[rxFree] = << >> )
    /\ \A i \in 1 .. Len(txFree) : txContent[txFree[i]] = << >>

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

(* once the TX worker exits, everything staged was sent                   *)
ExitSentAll == txExited => Len(txSent) = staged

(* a stop completes: both workers exit, destroy joins, storage is freed,  *)
(* and on the way everything staged was sent                              *)
StopCompletes == quit ~> freed

=============================================================================
