/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS batched server transport's send path over one taken bank
// (sdk/rocketnet_msgx_send_runs.h; model proofs/tla/RNMsgxSendRuns.tla) over
// a fake: the table's resolve is an array of open slots, the connected and
// shared sends answer from a script and log what they took. No socket, no
// clock, no thread.
//
// Each test pins one rule: the sort (fill phases in staged order, inside each
// the shared socket first, staged order kept within a slot); a run per slot
// and generation, at most a batch of live packets, aborted packets never
// sent; every answer of the connected send (all taken, a prefix taken past
// aborted packets, nothing taken, full, refused once and twice, dead with the
// shared socket full part way, any other error); a retired stamp leaving
// through the shared socket and counted once; a stale generation never riding
// a newer one's run; a client restamped between two fills of one deferred
// bank leaving in order; and a seeded walk of random banks over several fill
// phases and answers against the rules: every live packet leaves exactly
// once, each client's packets in staged order, a kept call resuming where it
// stopped.
//
// ROCKETNET_MSGX_SEND_RUNS_MUTATION (the header) builds a deliberate defect;
// each one turns at least one of these tests red.

#include "test_unit_registry.h"

#include "rocketnet_msgx_send_runs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SEND_RUNS_TEST_MAX_PACKETS 64
#define SEND_RUNS_TEST_MAX_SLOTS 8
#define SEND_RUNS_TEST_MAX_ANSWERS 256
#define SEND_RUNS_TEST_SHARED_FD -1

struct send_runs_test_info_t
{
    int packet_bytes;
    int table_slot;
    uint32_t generation;
    uint32_t phase;
    int client;                     // the test's own: the destination
};

struct send_runs_test_wire_t
{
    int packet;
    int fd;                         // SEND_RUNS_TEST_SHARED_FD: the shared socket
};

struct send_runs_test_t
{
    struct send_runs_test_info_t info[SEND_RUNS_TEST_MAX_PACKETS];
    int count;
    int capacity;
    int bucket[SEND_RUNS_TEST_MAX_SLOTS + 2];
    int order[SEND_RUNS_TEST_MAX_PACKETS];
    int run_packets[SEND_RUNS_TEST_MAX_PACKETS];
    int batch;
    int cursor;                     // state.cursor after each call
    uint32_t phase;                 // the fill phase the next stage is in
    struct rocketnet_msgx_send_runs_state_t state;

    // the table: slot s open at generation slot_generation[s] on fd 100 + s
    bool slot_open[SEND_RUNS_TEST_MAX_SLOTS];
    uint32_t slot_generation[SEND_RUNS_TEST_MAX_SLOTS];

    // the scripts: answers taken in order; past the end, all taken / sent
    int answers[SEND_RUNS_TEST_MAX_ANSWERS];
    int num_answers;
    int next_answer;
    int shared_answers[SEND_RUNS_TEST_MAX_ANSWERS];
    int num_shared_answers;
    int next_shared_answer;

    // what happened
    struct send_runs_test_wire_t wire[SEND_RUNS_TEST_MAX_PACKETS * 4];
    int num_wire;
    int sends;
    int last_run[SEND_RUNS_TEST_MAX_PACKETS];
    int last_run_size;
    int dead_reports;
    int dead_slot;
    uint32_t dead_generation;

    struct rocketnet_msgx_send_runs_counters_t counters;
    struct rocketnet_msgx_send_runs_io_t io;
};

static int send_runs_test_resolve( void * user, int table_slot, uint32_t generation )
{
    struct send_runs_test_t * test = (struct send_runs_test_t *) user;
    rocketnet_unit_check( table_slot >= 0 && table_slot < test->capacity );
    if ( !test->slot_open[table_slot] || test->slot_generation[table_slot] != generation )
    {
        return -1;
    }
    return 100 + table_slot;
}

static int send_runs_test_send( void * user, int fd, const int * packets, int num_packets )
{
    struct send_runs_test_t * test = (struct send_runs_test_t *) user;
    rocketnet_unit_check( num_packets >= 1 && num_packets <= test->batch );
    test->sends++;
    test->last_run_size = num_packets;
    for ( int n = 0; n < num_packets; n++ )
    {
        test->last_run[n] = packets[n];
    }
    int answer = num_packets;
    if ( test->next_answer < test->num_answers )
    {
        answer = test->answers[test->next_answer++];
        if ( answer > num_packets )
        {
            answer = num_packets;
        }
    }
    for ( int n = 0; n < answer; n++ )
    {
        test->wire[test->num_wire].packet = packets[n];
        test->wire[test->num_wire].fd = fd;
        test->num_wire++;
    }
    return answer;
}

static int send_runs_test_send_shared( void * user, int packet )
{
    struct send_runs_test_t * test = (struct send_runs_test_t *) user;
    int answer = ROCKETNET_MSGX_SHARED_SENT;
    if ( test->next_shared_answer < test->num_shared_answers )
    {
        answer = test->shared_answers[test->next_shared_answer++];
    }
    if ( answer == ROCKETNET_MSGX_SHARED_SENT )
    {
        test->wire[test->num_wire].packet = packet;
        test->wire[test->num_wire].fd = SEND_RUNS_TEST_SHARED_FD;
        test->num_wire++;
    }
    return answer;
}

static void send_runs_test_report_dead( void * user, int table_slot, uint32_t generation )
{
    struct send_runs_test_t * test = (struct send_runs_test_t *) user;
    test->dead_reports++;
    test->dead_slot = table_slot;
    test->dead_generation = generation;
}

static void send_runs_test_init( struct send_runs_test_t * test, int capacity, int batch )
{
    memset( test, 0, sizeof( *test ) );
    rocketnet_unit_check( capacity <= SEND_RUNS_TEST_MAX_SLOTS );
    test->capacity = capacity;
    test->batch = batch;
    test->io.user = test;
    test->io.resolve = send_runs_test_resolve;
    test->io.send = send_runs_test_send;
    test->io.send_shared = send_runs_test_send_shared;
    test->io.report_dead = send_runs_test_report_dead;
    rocketnet_msgx_send_runs_start( &test->state, test->run_packets );
}

static void send_runs_test_open( struct send_runs_test_t * test, int slot, uint32_t generation )
{
    test->slot_open[slot] = true;
    test->slot_generation[slot] = generation;
}

// staged: one packet, live when bytes > 0
static int send_runs_test_stage( struct send_runs_test_t * test, int client, int slot, uint32_t generation, int bytes )
{
    rocketnet_unit_check( test->count < SEND_RUNS_TEST_MAX_PACKETS );
    struct send_runs_test_info_t * info = &test->info[test->count];
    info->client = client;
    info->table_slot = slot;
    info->generation = generation;
    info->packet_bytes = bytes;
    info->phase = test->phase;
    return test->count++;
}

static void send_runs_test_answer( struct send_runs_test_t * test, int answer )
{
    test->answers[test->num_answers++] = answer;
}

static void send_runs_test_shared_answer( struct send_runs_test_t * test, int answer )
{
    test->shared_answers[test->num_shared_answers++] = answer;
}

static void send_runs_test_sort( struct send_runs_test_t * test )
{
    rocketnet_msgx_send_runs_sort( test->info, test->count, test->capacity, test->bucket, test->order );
}

static bool send_runs_test_send_bank( struct send_runs_test_t * test )
{
    const bool sent = rocketnet_msgx_send_runs_send( test->info, test->order, test->count, test->batch, &test->state, &test->io, &test->counters );
    test->cursor = test->state.cursor;
    return sent;
}

static int send_runs_test_times_on_wire( const struct send_runs_test_t * test, int packet )
{
    int times = 0;
    for ( int w = 0; w < test->num_wire; w++ )
    {
        if ( test->wire[w].packet == packet )
        {
            times++;
        }
    }
    return times;
}

static void test_rocketnet_msgx_send_runs_sort()
{
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 3, 4 );
    send_runs_test_stage( test, 1, 2, 0, 10 );     // 0
    send_runs_test_stage( test, 2, -1, 0, 10 );    // 1
    send_runs_test_stage( test, 3, 0, 0, 10 );     // 2
    send_runs_test_stage( test, 1, 2, 0, 10 );     // 3
    send_runs_test_stage( test, 4, 7, 0, 10 );     // 4: outside the table, with the shared socket's
    send_runs_test_stage( test, 3, 0, 0, 0 );      // 5: aborted, sorted all the same
    send_runs_test_stage( test, 2, -1, 0, 10 );    // 6
    send_runs_test_stage( test, 1, 2, 0, 10 );     // 7
    send_runs_test_sort( test );
    const int expected[] = { 1, 4, 6, 2, 5, 0, 3, 7 };
    for ( int p = 0; p < test->count; p++ )
    {
        rocketnet_unit_check( test->order[p] == expected[p] );
    }

    // an empty bank sorts to nothing and sends nothing
    send_runs_test_init( test, 3, 4 );
    send_runs_test_sort( test );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->sends == 0 && test->num_wire == 0 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_runs()
{
    // slot 0 at generation 3: five live packets and two aborted, batch 2;
    // two shared packets first; slot 1 at generation 1
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 2, 2 );
    send_runs_test_open( test, 0, 3 );
    send_runs_test_open( test, 1, 1 );
    const int a = send_runs_test_stage( test, 1, 0, 3, 10 );
    const int x = send_runs_test_stage( test, 1, 0, 3, 0 );
    const int b = send_runs_test_stage( test, 1, 0, 3, 10 );
    const int s = send_runs_test_stage( test, 2, -1, 0, 10 );
    const int c = send_runs_test_stage( test, 1, 0, 3, 10 );
    const int d = send_runs_test_stage( test, 1, 0, 3, 10 );
    const int y = send_runs_test_stage( test, 1, 0, 3, -1 );
    const int e = send_runs_test_stage( test, 1, 0, 3, 10 );
    const int t = send_runs_test_stage( test, 3, 1, 1, 10 );
    send_runs_test_sort( test );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->cursor == test->count );

    // s alone on the shared socket, then [a b] [c d] [e] on fd 100, [t] on fd 101
    const struct send_runs_test_wire_t expected[] = { { s, -1 }, { a, 100 }, { b, 100 }, { c, 100 }, { d, 100 }, { e, 100 }, { t, 101 } };
    rocketnet_unit_check( test->num_wire == 7 );
    for ( int w = 0; w < test->num_wire; w++ )
    {
        rocketnet_unit_check( test->wire[w].packet == expected[w].packet );
        rocketnet_unit_check( test->wire[w].fd == expected[w].fd );
    }
    rocketnet_unit_check( test->sends == 4 );
    rocketnet_unit_check( send_runs_test_times_on_wire( test, x ) == 0 );
    rocketnet_unit_check( send_runs_test_times_on_wire( test, y ) == 0 );
    rocketnet_unit_check( test->counters.sent_connected == 6 );
    rocketnet_unit_check( test->counters.sent_shared == 1 );
    rocketnet_unit_check( test->counters.dropped == 0 && test->counters.retired == 0 && test->counters.dead == 0 && test->counters.refused == 0 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_partial()
{
    // the run [a x b c] with x aborted, batch 3: the send takes 2 (a, b), so
    // the cursor stops at c, past exactly two live packets
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 1, 3 );
    send_runs_test_open( test, 0, 0 );
    const int a = send_runs_test_stage( test, 1, 0, 0, 10 );
    const int x = send_runs_test_stage( test, 1, 0, 0, 0 );
    const int b = send_runs_test_stage( test, 1, 0, 0, 10 );
    const int c = send_runs_test_stage( test, 1, 0, 0, 10 );
    const int d = send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_sort( test );
    send_runs_test_answer( test, 2 );
    rocketnet_unit_check( !send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->cursor == 3 );
    rocketnet_unit_check( test->order[test->cursor] == c );
    rocketnet_unit_check( test->counters.sent_connected == 2 );

    // the kept bank resumes at c: [c d]
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->num_wire == 4 );
    const int expected[] = { a, b, c, d };
    for ( int w = 0; w < 4; w++ )
    {
        rocketnet_unit_check( test->wire[w].packet == expected[w] );
    }
    rocketnet_unit_check( send_runs_test_times_on_wire( test, x ) == 0 );
    rocketnet_unit_check( test->counters.sent_connected == 4 );
    rocketnet_unit_check( test->counters.dropped == 0 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_kept()
{
    // full: nothing taken, the cursor kept, nothing counted
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 1, 4 );
    send_runs_test_open( test, 0, 0 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_sort( test );
    send_runs_test_answer( test, ROCKETNET_MSGX_SEND_FULL );
    rocketnet_unit_check( !send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->cursor == 0 && test->num_wire == 0 );
    rocketnet_unit_check( test->counters.dropped == 0 && test->counters.sent_connected == 0 );

    // a send that took nothing (0, not an error) keeps the run as well: the
    // code before this header read a stale errno there and could drop it
    send_runs_test_answer( test, 0 );
    rocketnet_unit_check( !send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->cursor == 0 && test->num_wire == 0 );
    rocketnet_unit_check( test->counters.dropped == 0 );

    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->cursor == 2 && test->num_wire == 2 );
    rocketnet_unit_check( test->counters.sent_connected == 2 && test->counters.dropped == 0 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_refused()
{
    // refused once: retried at once and sent
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 1, 4 );
    send_runs_test_open( test, 0, 0 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_sort( test );
    send_runs_test_answer( test, ROCKETNET_MSGX_SEND_REFUSED );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->sends == 2 && test->num_wire == 2 );
    rocketnet_unit_check( test->counters.refused == 1 && test->counters.sent_connected == 2 && test->counters.dropped == 0 );

    // refused twice: the run is dropped, counted, and the walk goes on
    send_runs_test_init( test, 2, 4 );
    send_runs_test_open( test, 0, 0 );
    send_runs_test_open( test, 1, 0 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    const int other = send_runs_test_stage( test, 2, 1, 0, 10 );
    send_runs_test_sort( test );
    send_runs_test_answer( test, ROCKETNET_MSGX_SEND_REFUSED );
    send_runs_test_answer( test, ROCKETNET_MSGX_SEND_REFUSED );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->sends == 3 );
    rocketnet_unit_check( test->num_wire == 1 && test->wire[0].packet == other );
    rocketnet_unit_check( test->counters.refused == 1 && test->counters.dropped == 2 && test->counters.sent_connected == 1 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_error()
{
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 1, 2 );
    send_runs_test_open( test, 0, 0 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_stage( test, 1, 0, 0, 0 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    const int last = send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_sort( test );
    send_runs_test_answer( test, ROCKETNET_MSGX_SEND_ERROR );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->cursor == test->count );
    rocketnet_unit_check( test->num_wire == 1 && test->wire[0].packet == last );
    rocketnet_unit_check( test->counters.dropped == 2 && test->counters.sent_connected == 1 );
    rocketnet_unit_check( test->dead_reports == 0 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_dead()
{
    // slot 0 at generation 5 answers dead: reported, and the run [a b c]
    // leaves through the shared socket in order; the shared socket is full at
    // b, so the call returns with the cursor at b, and the next resumes there
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 1, 4 );
    send_runs_test_open( test, 0, 5 );
    const int a = send_runs_test_stage( test, 1, 0, 5, 10 );
    const int x = send_runs_test_stage( test, 1, 0, 5, 0 );
    const int b = send_runs_test_stage( test, 1, 0, 5, 10 );
    const int c = send_runs_test_stage( test, 1, 0, 5, 10 );
    send_runs_test_sort( test );
    send_runs_test_answer( test, ROCKETNET_MSGX_SEND_DEAD );
    send_runs_test_shared_answer( test, ROCKETNET_MSGX_SHARED_SENT );
    send_runs_test_shared_answer( test, ROCKETNET_MSGX_SHARED_FULL );
    rocketnet_unit_check( !send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->dead_reports == 1 && test->dead_slot == 0 && test->dead_generation == 5 );
    rocketnet_unit_check( test->counters.dead == 1 );
    rocketnet_unit_check( test->order[test->cursor] == b );
    rocketnet_unit_check( test->num_wire == 1 && test->wire[0].packet == a && test->wire[0].fd == SEND_RUNS_TEST_SHARED_FD );

    // main retired the slot meanwhile: b and c resolve to no socket and leave
    // through the shared socket one by one, counted retired
    test->slot_open[0] = false;
    test->slot_generation[0] = 6;
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    const int expected[] = { a, b, c };
    rocketnet_unit_check( test->num_wire == 3 );
    for ( int w = 0; w < 3; w++ )
    {
        rocketnet_unit_check( test->wire[w].packet == expected[w] && test->wire[w].fd == SEND_RUNS_TEST_SHARED_FD );
    }
    rocketnet_unit_check( send_runs_test_times_on_wire( test, x ) == 0 );
    rocketnet_unit_check( test->counters.sent_shared == 3 && test->counters.retired == 2 && test->counters.dropped == 0 );
    rocketnet_unit_check( test->counters.sent_connected == 0 );

    // dead with a shared drop: counted dropped, the walk goes on
    send_runs_test_init( test, 1, 4 );
    send_runs_test_open( test, 0, 0 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    const int kept = send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_sort( test );
    send_runs_test_answer( test, ROCKETNET_MSGX_SEND_DEAD );
    send_runs_test_shared_answer( test, ROCKETNET_MSGX_SHARED_DROPPED );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->num_wire == 1 && test->wire[0].packet == kept );
    rocketnet_unit_check( test->counters.dropped == 1 && test->counters.sent_shared == 1 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_retired()
{
    // a stamp whose socket was retired since leaves through the shared
    // socket; a full shared socket keeps it, and it is counted retired once,
    // when it leaves
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 2, 4 );
    send_runs_test_open( test, 1, 2 );                          // slot 1 now at generation 2
    const int stale = send_runs_test_stage( test, 1, 1, 1, 10 );
    const int closed = send_runs_test_stage( test, 2, 0, 0, 10 ); // slot 0 closed
    send_runs_test_sort( test );
    send_runs_test_shared_answer( test, ROCKETNET_MSGX_SHARED_FULL );
    rocketnet_unit_check( !send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->cursor == 0 && test->num_wire == 0 );
    rocketnet_unit_check( test->counters.retired == 0 );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->num_wire == 2 && test->wire[0].packet == closed && test->wire[1].packet == stale );
    rocketnet_unit_check( test->wire[0].fd == SEND_RUNS_TEST_SHARED_FD && test->wire[1].fd == SEND_RUNS_TEST_SHARED_FD );
    rocketnet_unit_check( test->counters.retired == 2 && test->counters.sent_shared == 2 );
    rocketnet_unit_check( test->sends == 0 );

    // a shared drop (no address to send to) is counted dropped and retired
    send_runs_test_init( test, 1, 4 );
    send_runs_test_stage( test, 1, 0, 0, 10 );
    send_runs_test_stage( test, 2, -1, 0, 10 );
    send_runs_test_sort( test );
    send_runs_test_shared_answer( test, ROCKETNET_MSGX_SHARED_DROPPED );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->counters.dropped == 1 && test->counters.sent_shared == 1 && test->counters.retired == 1 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_generation()
{
    // slot 0 now at generation 1 (client 2); a packet stamped at generation 0
    // (client 1) sorted behind client 2's head never rides its run: it
    // resolves to no socket and leaves through the shared socket. Staged
    // order puts the old stamp first in the server; the header holds for any
    // order.
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 1, 4 );
    send_runs_test_open( test, 0, 1 );
    const int fresh = send_runs_test_stage( test, 2, 0, 1, 10 );
    const int stale = send_runs_test_stage( test, 1, 0, 0, 10 );
    const int fresh2 = send_runs_test_stage( test, 2, 0, 1, 10 );
    send_runs_test_sort( test );
    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    rocketnet_unit_check( test->num_wire == 3 );
    rocketnet_unit_check( test->wire[0].packet == fresh && test->wire[0].fd == 100 );
    rocketnet_unit_check( test->wire[1].packet == stale && test->wire[1].fd == SEND_RUNS_TEST_SHARED_FD );
    rocketnet_unit_check( test->wire[2].packet == fresh2 && test->wire[2].fd == 100 );
    rocketnet_unit_check( test->counters.retired == 1 && test->counters.sent_connected == 2 );
    delete test;
}

static void test_rocketnet_msgx_send_runs_restamp()
{
    // A retire between two fills of one deferred bank. Fill phase 0: client 1
    // stamped on slot 0 at generation 0 (a), client 2 on the shared socket
    // (b). Main's commit retires slot 0 and opens slot 1 at generation 0 for
    // client 2. Fill phase 1: client 1 now on the shared socket (c), client 2
    // on slot 1 (d). Sorted by slot alone, c (shared) went before a, and
    // client 1's packets left out of order (RNMsgxSendRuns RestampOrder).
    struct send_runs_test_t * test = new send_runs_test_t();
    send_runs_test_init( test, 2, 4 );
    test->phase = 0;
    const int a = send_runs_test_stage( test, 1, 0, 0, 10 );
    const int b = send_runs_test_stage( test, 2, -1, 0, 10 );
    test->phase = 1;
    const int c = send_runs_test_stage( test, 1, -1, 0, 10 );
    const int d = send_runs_test_stage( test, 2, 1, 0, 10 );
    send_runs_test_open( test, 1, 0 );
    send_runs_test_sort( test );

    // phase 0 first, sorted inside: b (shared) then a; then phase 1: c, d
    const int expected_order[] = { b, a, c, d };
    for ( int p = 0; p < test->count; p++ )
    {
        rocketnet_unit_check( test->order[p] == expected_order[p] );
    }

    rocketnet_unit_check( send_runs_test_send_bank( test ) );
    const struct send_runs_test_wire_t expected[] = { { b, -1 }, { a, -1 }, { c, -1 }, { d, 101 } };
    rocketnet_unit_check( test->num_wire == 4 );
    for ( int w = 0; w < test->num_wire; w++ )
    {
        rocketnet_unit_check( test->wire[w].packet == expected[w].packet );
        rocketnet_unit_check( test->wire[w].fd == expected[w].fd );
    }
    rocketnet_unit_check( test->counters.retired == 1 && test->counters.sent_shared == 3 && test->counters.sent_connected == 1 );
    delete test;
}

// the seeded walk: no clock, no randomness but this generator
static uint32_t send_runs_test_next( uint32_t * state )
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static void test_rocketnet_msgx_send_runs_walk()
{
    struct send_runs_test_t * test = new send_runs_test_t();
    for ( uint32_t seed = 1; seed <= 2000; seed++ )
    {
        uint32_t rng_state = seed;
        const int capacity = 1 + (int) ( send_runs_test_next( &rng_state ) % 3 );
        const int batch = 1 + (int) ( send_runs_test_next( &rng_state ) % 4 );
        send_runs_test_init( test, capacity, batch );

        // the table at send time; clients 0 .. 4 over one to three fill
        // phases, client c's stamp fixed within a phase and free to change
        // across one (the table retires and opens between fills); a slot and
        // generation names one client for the whole bank, as the table's
        // generations do
        for ( int s = 0; s < capacity; s++ )
        {
            if ( send_runs_test_next( &rng_state ) % 4 != 0 )
            {
                send_runs_test_open( test, s, send_runs_test_next( &rng_state ) % 2 );
            }
            else
            {
                test->slot_generation[s] = send_runs_test_next( &rng_state ) % 2;
            }
        }
        int owner[SEND_RUNS_TEST_MAX_SLOTS][2];
        for ( int s = 0; s < SEND_RUNS_TEST_MAX_SLOTS; s++ )
        {
            owner[s][0] = -1;
            owner[s][1] = -1;
        }
        const int phases = 1 + (int) ( send_runs_test_next( &rng_state ) % 3 );
        const int count = (int) ( send_runs_test_next( &rng_state ) % 17 );
        int staged = 0;
        for ( int phase = 0; phase < phases; phase++ )
        {
            int stamp_slot[5];
            uint32_t stamp_generation[5];
            for ( int c = 0; c < 5; c++ )
            {
                stamp_slot[c] = -1;
                stamp_generation[c] = 0;
                const int s = (int) ( send_runs_test_next( &rng_state ) % (uint32_t) ( capacity + 1 ) ) - 1;
                const uint32_t g = send_runs_test_next( &rng_state ) % 2;
                if ( s >= 0 && ( owner[s][g] < 0 || owner[s][g] == c ) )
                {
                    owner[s][g] = c;
                    stamp_slot[c] = s;
                    stamp_generation[c] = g;
                }
            }
            test->phase = (uint32_t) ( 7 + 3 * phase );     // any increasing counts
            const int in_phase = phase + 1 == phases ? count - staged : (int) ( send_runs_test_next( &rng_state ) % (uint32_t) ( count - staged + 1 ) );
            for ( int i = 0; i < in_phase; i++ )
            {
                const int c = (int) ( send_runs_test_next( &rng_state ) % 5 );
                const int bytes = send_runs_test_next( &rng_state ) % 5 == 0 ? 0 : 10;
                send_runs_test_stage( test, c, stamp_slot[c], stamp_generation[c], bytes );
            }
            staged += in_phase;
        }
        for ( int n = 0; n < SEND_RUNS_TEST_MAX_ANSWERS; n++ )
        {
            const uint32_t pick = send_runs_test_next( &rng_state ) % 10;
            int answer = SEND_RUNS_TEST_MAX_PACKETS;
            if ( pick == 0 )
            {
                answer = ROCKETNET_MSGX_SEND_FULL;
            }
            else if ( pick == 1 )
            {
                answer = ROCKETNET_MSGX_SEND_REFUSED;
            }
            else if ( pick == 2 )
            {
                answer = ROCKETNET_MSGX_SEND_DEAD;
            }
            else if ( pick == 3 )
            {
                answer = ROCKETNET_MSGX_SEND_ERROR;
            }
            else if ( pick == 4 )
            {
                answer = (int) ( send_runs_test_next( &rng_state ) % 4 );
            }
            send_runs_test_answer( test, answer );
            const uint32_t shared_pick = send_runs_test_next( &rng_state ) % 8;
            send_runs_test_shared_answer( test, shared_pick == 0 ? ROCKETNET_MSGX_SHARED_FULL
                                                                 : ( shared_pick == 1 ? ROCKETNET_MSGX_SHARED_DROPPED : ROCKETNET_MSGX_SHARED_SENT ) );
        }

        send_runs_test_sort( test );
        int calls = 0;
        for ( ;; )
        {
            const int before = test->cursor;
            const bool sent = send_runs_test_send_bank( test );
            calls++;
            rocketnet_unit_check( test->cursor >= before && test->cursor <= test->count );

            // the cursor never passes a live packet that has not left (on the
            // wire or dropped), and nothing past it has left: a kept call
            // resumes exactly where it stopped
            uint64_t dropped_seen = 0;
            for ( int p = 0; p < test->count; p++ )
            {
                const int i = test->order[p];
                const int times = send_runs_test_times_on_wire( test, i );
                rocketnet_unit_check( times <= 1 );
                if ( test->info[i].packet_bytes <= 0 )
                {
                    rocketnet_unit_check( times == 0 );
                    continue;
                }
                if ( p >= test->cursor )
                {
                    rocketnet_unit_check( times == 0 );
                }
                else if ( times == 0 )
                {
                    dropped_seen++;
                }
            }
            rocketnet_unit_check( dropped_seen == test->counters.dropped );
            rocketnet_unit_check( test->counters.sent_connected + test->counters.sent_shared == (uint64_t) test->num_wire );
            if ( sent )
            {
                break;
            }
            rocketnet_unit_check( calls < 4 * SEND_RUNS_TEST_MAX_ANSWERS );
        }
        rocketnet_unit_check( test->cursor == test->count );
        rocketnet_unit_check( (uint64_t) test->num_wire + test->counters.dropped ==
                              (uint64_t) rocketnet_msgx_send_runs_owed( test->info, test->order, test->count, 0 ) );

        // each client's packets on the wire in staged order, and a connected
        // send only on the client's own socket at its own generation
        for ( int w = 0; w < test->num_wire; w++ )
        {
            const struct send_runs_test_info_t * info = &test->info[test->wire[w].packet];
            if ( test->wire[w].fd != SEND_RUNS_TEST_SHARED_FD )
            {
                rocketnet_unit_check( test->wire[w].fd == 100 + info->table_slot );
                rocketnet_unit_check( test->slot_generation[info->table_slot] == info->generation );
            }
            for ( int v = w + 1; v < test->num_wire; v++ )
            {
                if ( test->info[test->wire[v].packet].client == info->client )
                {
                    rocketnet_unit_check( test->wire[v].packet > test->wire[w].packet );
                }
            }
        }
    }
    delete test;
}

REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_sort );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_runs );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_partial );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_kept );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_refused );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_error );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_dead );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_retired );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_generation );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_restamp );
REGISTER_UNIT_TEST( test_rocketnet_msgx_send_runs_walk );
