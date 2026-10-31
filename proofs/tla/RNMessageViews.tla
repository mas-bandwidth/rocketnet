--------------------------- MODULE RNMessageViews ---------------------------
(***************************************************************************)
(* The message object's receive views of one connection as laid out in     *)
(* sdk/rocketnet_message_server.cpp, with the code's own arithmetic.       *)
(*                                                                         *)
(* The update's deliveries are entries 0 .. deliveries-1 of the region's   *)
(* delivery table (rn_channels_append_delivery).  Beside it the connection *)
(* keeps views, one byte per table entry (WAITING 0, HELD 1, RELEASED 2),  *)
(* receive_cursor and held; the object keeps the last batch's entries      *)
(* (batch_entry, batch_count).                                             *)
(*                                                                         *)
(*   Update        rn_message_object_update: release the last batch        *)
(*                 (rn_message_object_release_batch), count views still    *)
(*                 held (NOT_RELEASED), read the last update's delivery    *)
(*                 count from rn_endpoint_begin_update (which empties the  *)
(*                 table), clear that many views, reset the cursor and     *)
(*                 held; then the transport update appends this update's   *)
(*                 deliveries;                                             *)
(*   Receive       rn_message_object_receive through rn_message_object_next:*)
(*                 the first WAITING entry at or after the cursor becomes  *)
(*                 HELD, held++, the cursor moves past it; at the end the  *)
(*                 cursor is the delivery count;                           *)
(*   ReceiveBatch  rn_message_object_receive_messages: release the last    *)
(*                 batch, then up to Batch entries as Receive takes them;  *)
(*   Release       rn_message_object_release with a pointer the            *)
(*                 application holds (rn_message_connection_release_view): *)
(*                 the table is searched backwards from cursor - 1 with    *)
(*                 the wrap at the delivery count, for the entry with that *)
(*                 data pointer that is HELD.                              *)
(*                                                                         *)
(* Each entry's data pointer is distinct, so a pointer names its entry.    *)
(* One connection: the object's queue across connections (receive_         *)
(* connection, and Release trying the queue's connection first) is not     *)
(* modelled.  The ghost state is the set of entries the application holds *)
(* and the entries the code read.                                          *)
(*                                                                         *)
(* Reversed witnesses:                                                     *)
(*   SearchFromCursor = FALSE  the release search starts at the cursor,    *)
(*                             not at cursor - 1;                          *)
(*   BatchChecksHeld = FALSE   the batch release decrements held for every *)
(*                             batch entry, released or not;               *)
(*   CountBeforeExpire = FALSE the views are cleared by the count read     *)
(*                             after rn_endpoint_begin_update emptied the  *)
(*                             table (zero).                               *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Cap,                \* delivery_entries
    Batch,              \* ROCKETNET_RECEIVE_BATCH_MESSAGES
    SearchFromCursor,   \* BOOLEAN: TRUE is the code's cursor - 1
    BatchChecksHeld,    \* BOOLEAN
    CountBeforeExpire   \* BOOLEAN

WAITING == 0
HELD == 1
RELEASED == 2
Entries == 0..Cap-1

VARIABLES
    deliveries, \* connection->ordered.channels.deliveries
    views,      \* connection->hot->views
    cursor,     \* connection->hot->receive_cursor
    held,       \* connection->hot->held (an Int here, so an underflow shows)
    batch,      \* object->batch_entry[0 .. batch_count-1]
    \* ghost
    app,        \* the entries whose pointers the application holds
    missed,     \* a release of a pointer the application holds found nothing
    strayRead,  \* the release search read an entry at or past the delivery count
    lostView    \* Receive passed over an entry of this update that was never handed out

vars == <<deliveries, views, cursor, held, batch, app, missed, strayRead, lostView>>

TypeOK ==
    /\ deliveries \in 0..Cap
    /\ views \in [Entries -> {WAITING, HELD, RELEASED}]
    /\ cursor \in 0..Cap
    /\ held \in 0..Cap
    /\ batch \in Seq(Entries) /\ Len(batch) <= Batch
    /\ app \subseteq Entries
    /\ {missed, strayRead, lostView} \subseteq BOOLEAN

Init ==
    /\ deliveries = 0
    /\ views = [k \in Entries |-> WAITING]
    /\ cursor = 0
    /\ held = 0
    /\ batch = <<>>
    /\ app = {}
    /\ missed = FALSE /\ strayRead = FALSE /\ lostView = FALSE

\* the code's steps as maps on a record of the connection's fields
State == [views |-> views, cursor |-> cursor, held |-> held]

\* rn_message_object_release_batch
ReleaseBatch(s) ==
    LET RECURSIVE Go(_, _)
        Go(t, n) ==
            IF n > Len(batch) THEN t
            ELSE LET k == batch[n] IN
                 IF ~BatchChecksHeld \/ t.views[k] = HELD
                   THEN Go([t EXCEPT !.views[k] = RELEASED, !.held = @ - 1], n + 1)
                   ELSE Go(t, n + 1)
    IN  Go(s, 1)

\* rn_message_object_next, for one connection: returns
\* <<state, entry or -1>>
FirstWaiting(s) == {k \in s.cursor..(deliveries - 1) : s.views[k] = WAITING}
Next1(s) ==
    IF FirstWaiting(s) = {}
      THEN <<[s EXCEPT !.cursor = deliveries], -1>>
      ELSE LET k == CHOOSE m \in FirstWaiting(s) : \A x \in FirstWaiting(s) : m <= x
           IN  <<[s EXCEPT !.views[k] = HELD, !.held = @ + 1, !.cursor = k + 1], k>>

\* an entry of this update passed over unread: below the new cursor, never handed out
Skipped(s, s2) == \E k \in s.cursor..(s2.cursor - 1) : k < deliveries /\ s.views[k] # WAITING /\ k \notin app

\* rn_message_connection_release_view: the entries the
\* backward search visits, in order.  The code starts at k = cursor - 1 (or
\* deliveries - 1 when the cursor is 0) and steps k = k == 0 ? deliveries - 1
\* : k - 1, deliveries times.  SearchFromCursor = FALSE starts at k = cursor,
\* which is deliveries itself when the cursor is at the end.
VisitList(c) ==
    IF SearchFromCursor
      THEN LET start == IF c = 0 THEN deliveries - 1 ELSE c - 1
           IN  [n \in 0..(deliveries - 1) |-> (start - n) % deliveries]
      ELSE [n \in 0..(deliveries - 1) |-> IF n = 0 THEN c ELSE (c - n) % deliveries]

\* the release finds the first visited entry naming that pointer and HELD
Release(e) ==
    /\ e \in app
    /\ LET c  == IF cursor > deliveries THEN deliveries ELSE cursor
           vl == VisitList(c)
           hits == {n \in 0..(deliveries - 1) : vl[n] < Cap /\ vl[n] = e /\ views[vl[n]] = HELD}
       IN  /\ strayRead' = (strayRead \/ \E n \in 0..(deliveries - 1) : vl[n] >= deliveries)
           /\ IF held > 0 /\ hits # {}
                THEN /\ views' = [views EXCEPT ![e] = RELEASED]
                     /\ held' = held - 1
                     /\ missed' = missed
                ELSE /\ missed' = TRUE                       \* rocketnet_api_misuse_record
                     /\ UNCHANGED <<views, held>>
    /\ app' = app \ {e}
    /\ UNCHANGED <<deliveries, cursor, batch, lostView>>

Receive ==
    LET r == Next1(State) IN
    /\ views' = r[1].views
    /\ cursor' = r[1].cursor
    /\ held' = r[1].held
    /\ app' = IF r[2] = -1 THEN app ELSE app \cup {r[2]}
    /\ lostView' = (lostView \/ Skipped(State, r[1]))
    /\ UNCHANGED <<deliveries, batch, missed, strayRead>>

\* rn_message_object_receive_messages
RECURSIVE Fill(_, _, _)
Fill(s, got, n) ==
    IF n = 0 THEN <<s, got>>
    ELSE LET r == Next1(s) IN
         IF r[2] = -1 THEN <<r[1], got>> ELSE Fill(r[1], Append(got, r[2]), n - 1)

ReceiveBatch ==
    LET s1 == ReleaseBatch(State)
        f  == Fill(s1, <<>>, Batch)
        gotSet == {f[2][i] : i \in 1..Len(f[2])}
    IN
    /\ views' = f[1].views
    /\ cursor' = f[1].cursor
    /\ held' = f[1].held
    /\ batch' = f[2]
    /\ app' = (app \ {batch[i] : i \in 1..Len(batch)}) \cup gotSet
    /\ lostView' = (lostView \/ Skipped(s1, f[1]))
    /\ UNCHANGED <<deliveries, missed, strayRead>>

\* rn_message_object_update, then the transport update
\* appends n deliveries
Update(n) ==
    LET s1 == ReleaseBatch(State)
        delivered == IF CountBeforeExpire THEN deliveries ELSE 0
        clear == IF delivered < Cap THEN delivered ELSE Cap
    IN
    /\ views' = [k \in Entries |-> IF k < clear THEN WAITING ELSE s1.views[k]]
    /\ cursor' = 0
    /\ held' = 0
    /\ batch' = <<>>
    /\ deliveries' = n
    /\ app' = {}                                  \* the last update's views are gone
    /\ UNCHANGED <<missed, strayRead, lostView>>

Next ==
    \/ \E n \in 0..Cap : Update(n)
    \/ Receive
    \/ ReceiveBatch
    \/ \E e \in Entries : Release(e)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)

\* held is exactly the number of HELD views
HeldCountExact == held = Cardinality({k \in Entries : views[k] = HELD})

\* the HELD views are exactly the entries the application holds
HeldIsApp == {k \in Entries : views[k] = HELD} = app

\* a release of a pointer the application holds always finds its view
ReleaseFindsHeld == ~missed

\* the release search reads only this update's entries
NoReadPastDeliveries == ~strayRead

\* every entry past this update's deliveries is WAITING: a new delivery
\* starts unread
BeyondIsWaiting == \A k \in Entries : k >= deliveries => views[k] = WAITING

\* every delivery of the update is handed out
NoLostView == ~lostView

\* the cursor is past every entry handed out, and nothing at or past it is
CursorBounds ==
    /\ cursor <= deliveries
    /\ \A k \in Entries : k >= cursor => views[k] = WAITING
=============================================================================
