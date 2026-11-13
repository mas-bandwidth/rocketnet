/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The receive handoff of the client I/O core (sdk/rocketnet_receive_handoff.h
// in sdk/rocketnet_client_io.h), checked by proofs/tla/TraceRNReceiveHandoff.tla
// (README-TraceCheck.md) against proofs/tla/RNReceiveHandoff.tla.
//
//   tla_trace --layer client-receive-handoff --seed N --out DIR [--steps K]
//
// writes DIR/MCTraceClientReceiveHandoffSeed<seed>.tla and .cfg
// (MCTraceClientReceiveHandoffPlantSeed<seed> from
// tla_trace_plant_client_receive_handoff). The walk drives a
// rocketnet_client_io_t over a transport of its own (no socket, no thread, no
// clock): a pass is one rocketnet_client_io_step that reads the n datagrams the
// walk queued and commits its buffer's count, a call one
// rocketnet_client_io_expose with a budget, and with the handoff mutex held
// against it on a busy call, so its try-acquire fails. Cap is the code's
// ROCKETNET_NUM_CLIENT_PACKETS: in the walk's last quarter a pass now and then
// queues all the room the I/O thread's buffer has left, so the I/O thread
// fills it, and the calls after it drain the full buffer.
//
// Records (tla_step):
//   bank      a pass reading n datagrams                     Bank(n)
//   end_pass  the same pass's commit, or a pass that read
//             nothing over a buffer holding records          Commit
//   call      an expose                                      Call(busy, b) with
//             what it returned, its first datagram and how many (CallGave)
// A pass reads and commits in one call, so its bank record is the state before
// the pass with the I/O thread's buffer grown to the count the code holds after
// it (the datagrams it read are in that buffer, which the commit leaves where
// it was), and its end_pass record is the state the code holds after it. Every
// model variable is read from the core as the library holds it, but arrived,
// delivered and committedTop: arrived and delivered are the walk's counts of
// what the passes read and the calls returned, committedTop the newest
// datagram a commit made visible. late, the history of DeliveredByNextCall, is
// a ghost: the model computes it, and the walk leaves it out. A buffer's first
// datagram is the id the walk wrote into the first record's bytes.
//
// The plant (ROCKETNET_RECEIVE_HANDOFF_PLANT, in rocketnet_client_receive_begin)
// reads only a buffer the I/O thread has left, the old ready-buffer rule
// (RNReceiveHandoff's ReadOnlyLeft). Its effect shows at the first call that
// took the mutex and left main sharing the I/O thread's buffer with a limit
// below that buffer's committed count, which the clean code never does: nothing
// commits during the walk's call. The walk reports that record.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_RECEIVE_HANDOFF_PLANT 1
#endif // #if defined( ROCKETNET_TLA_PLANT )
#include "../sdk/rocketnet_client_io.h"

#include "../sdk/rocketnet_internal.h"

#if ROCKETNET_DEV

#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_platform.h"
#include "../sdk/rocketnet_read_write.h"

#include <stdio.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the budgets a call may have: one datagram, a few, more than a pass reads,
// and the code's whole per-update budget, so calls are cut and calls drain
static const int client_receive_handoff_budgets[] = { 1, 3, 64, ROCKETNET_NUM_CLIENT_PACKETS };

// the passes' sizes, but for the pass that fills the buffer
static const int client_receive_handoff_max_pass = 8;

// the datagrams the walk's transport queues at most: a whole buffer, so one
// pass can fill the I/O thread's buffer
static const int client_receive_handoff_queue = ROCKETNET_NUM_CLIENT_PACKETS;

// arrived may not pass it (the model's MaxRx): far past any walk
static const int client_receive_handoff_max_rx = 100000000;

static const int client_receive_handoff_datagram_bytes = 8;

// the walk's transport: a queue of datagrams, each carrying its id
struct client_receive_handoff_source_t
{
    uint32_t ids[client_receive_handoff_queue];
    int head;
    int count;
};

static int client_receive_handoff_receive_batch( void * instance, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride,
                                                 int * packet_bytes, int max_packets )
{
    client_receive_handoff_source_t * source = (client_receive_handoff_source_t *) instance;
    int got = 0;
    while ( got < max_packets && source->count > 0 )
    {
        uint8_t * p = packet_data + (size_t) got * (size_t) packet_stride;
        memset( p, 0, (size_t) client_receive_handoff_datagram_bytes );
        uint8_t * w = p;
        rocketnet_write_uint32( &w, source->ids[source->head] );
        rocketnet_address_load_ipv4( &from[got], 0x7f000001, 40000 );
        packet_bytes[got] = client_receive_handoff_datagram_bytes;
        source->head = ( source->head + 1 ) % client_receive_handoff_queue;
        source->count--;
        got++;
    }
    return got;
}

static int client_receive_handoff_receive( void *, rocketnet_address_t *, uint8_t *, int )
{
    return 0;
}

static bool client_receive_handoff_wait( void *, bool, double )
{
    return false;
}

static void client_receive_handoff_wake( void * )
{
}

static double client_receive_handoff_time( void * )
{
    return 1000.0;
}

struct client_receive_handoff_walk_t
{
    uint32_t arrived;
    uint32_t delivered;
    uint32_t committed_top;
};

// the model's view of the core
struct client_receive_handoff_state_t
{
    int worker;
    int main;
    int tail;
    int free;
    uint32_t first[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    uint32_t count[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    uint32_t committed[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    uint32_t main_cursor;
    uint32_t main_limit;
    uint32_t tail_cursor;
    uint32_t tail_end;
};

static uint32_t client_receive_handoff_id( const uint8_t * packet_data )
{
    const uint8_t * p = packet_data;
    return rocketnet_read_uint32( &p );
}

static client_receive_handoff_state_t client_receive_handoff_read( const rocketnet_client_io_t * io )
{
    client_receive_handoff_state_t state;
    const struct rocketnet_receive_handoff_t * handoff = &io->receive;
    state.worker = handoff->worker;
    state.main = handoff->main;
    state.tail = handoff->tail;
    state.free = handoff->free;
    for ( int b = 0; b < ROCKETNET_CLIENT_IO_NUM_BUFFERS; b++ )
    {
        const rocketnet_client_receive_buffer_t * buffer = io->buffer[b];
        state.count[b] = (uint32_t) buffer->num_packets;
        state.first[b] = buffer->num_packets > 0 ? client_receive_handoff_id( buffer->packet_data[0] ) : 0;
        state.committed[b] = handoff->committed[b];
    }
    state.main_cursor = handoff->main_cursor;
    state.main_limit = handoff->main_limit;
    state.tail_cursor = handoff->tail_cursor;
    state.tail_end = handoff->tail_end;
    return state;
}

static std::string client_receive_handoff_buffers( const uint32_t * values )
{
    std::vector<std::string> keys;
    std::vector<std::string> out;
    for ( int b = 0; b < ROCKETNET_CLIENT_IO_NUM_BUFFERS; b++ )
    {
        keys.push_back( tla_int( b ) );
        out.push_back( tla_int( values[b] ) );
    }
    return tla_function( keys, out );
}

// the model's NONE for a free buffer of -1
static int client_receive_handoff_role( int index )
{
    return index < 0 ? 3 : index;
}

static tla_record_t client_receive_handoff_record( const client_receive_handoff_state_t & state, const client_receive_handoff_walk_t & walk )
{
    tla_record_t record;
    record.add( "worker", tla_int( client_receive_handoff_role( state.worker ) ) );
    record.add( "main", tla_int( client_receive_handoff_role( state.main ) ) );
    record.add( "tail", tla_int( client_receive_handoff_role( state.tail ) ) );
    record.add( "free", tla_int( client_receive_handoff_role( state.free ) ) );
    record.add( "first", client_receive_handoff_buffers( state.first ) );
    record.add( "count", client_receive_handoff_buffers( state.count ) );
    record.add( "committed", client_receive_handoff_buffers( state.committed ) );
    record.add( "mainCursor", tla_int( state.main_cursor ) );
    record.add( "mainLimit", tla_int( state.main_limit ) );
    record.add( "tailCursor", tla_int( state.tail_cursor ) );
    record.add( "tailEnd", tla_int( state.tail_end ) );
    record.add( "arrived", tla_int( walk.arrived ) );
    record.add( "delivered", tla_int( walk.delivered ) );
    record.add( "committedTop", tla_int( walk.committed_top ) );
    return record;
}

static int trace_client_receive_handoff( uint64_t seed, int steps, const char * out )
{
    rocketnet_client_transport_io_operations_t operations;
    memset( &operations, 0, sizeof( operations ) );
    operations.receive = client_receive_handoff_receive;
    operations.receive_batch = client_receive_handoff_receive_batch;
    operations.wait = client_receive_handoff_wait;
    operations.wake = client_receive_handoff_wake;

    client_receive_handoff_source_t * source = new client_receive_handoff_source_t();
    rocketnet_client_transport_io_t transport = { &operations, source };

    static uint8_t ring_data[ROCKETNET_CLIENT_IO_NUM_RINGS][ROCKETNET_FRAME_STRIDE];
    static int ring_bytes[ROCKETNET_CLIENT_IO_NUM_RINGS];
    rocketnet_client_io_ring_view_t views[ROCKETNET_CLIENT_IO_NUM_RINGS];
    for ( int i = 0; i < ROCKETNET_CLIENT_IO_NUM_RINGS; i++ )
    {
        views[i].data = ring_data[i];
        views[i].frame_stride = ROCKETNET_FRAME_STRIDE;
        views[i].wire_offset = ROCKETNET_FRAME_WIRE_OFFSET;
        views[i].wire_bytes = (const uint8_t *) &ring_bytes[i];
        views[i].wire_bytes_stride = sizeof( int );
    }

    rocketnet_client_receive_buffer_t * buffers[ROCKETNET_CLIENT_IO_NUM_BUFFERS];
    for ( int b = 0; b < ROCKETNET_CLIENT_IO_NUM_BUFFERS; b++ )
    {
        buffers[b] = new rocketnet_client_receive_buffer_t;
    }

    rocketnet_platform_mutex_t mutex;
    if ( !rocketnet_platform_mutex_create( &mutex ) )
    {
        fprintf( stderr, "tla_trace: client-receive-handoff: the mutex could not be created\n" );
        return 1;
    }

    rocketnet_client_io_t * io = new rocketnet_client_io_t();
    rocketnet_client_io_init( io, &transport, &mutex, views, buffers, client_receive_handoff_time, NULL );

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    client_receive_handoff_walk_t walk = { 0, 0, 0 };
    uint32_t next_id = 1;

    client_receive_handoff_state_t state = client_receive_handoff_read( io );
    records.push_back( tla_step( "init", "init", "<<>>", client_receive_handoff_record( state, walk ) ) );

    // one record a step: a pass that reads writes two (bank, end_pass), so
    // with one record left the walk makes a pass that reads nothing or a call
    int failed = 0;
    while ( (int) records.size() < steps + 1 && !failed )
    {
        const int step = (int) records.size() - 1;
        state = client_receive_handoff_read( io );

        const uint32_t room = (uint32_t) ROCKETNET_NUM_CLIENT_PACKETS - state.count[state.worker];
        int kind = random.below( 10 );
        if ( kind < 3 && ( room == 0 || step == steps - 1 ) )
        {
            kind = 3;
        }
        if ( kind >= 3 && kind < 5 && state.count[state.worker] == 0 )
        {
            kind = 5;
        }

        if ( kind < 5 )
        {
            // a pass: the I/O thread reads n datagrams (none for kind 3 and
            // 4, a pass over a buffer holding records), then commits
            uint32_t n = 0;
            if ( kind < 3 )
            {
                n = 1 + (uint32_t) random.below( client_receive_handoff_max_pass );
                if ( step >= steps - steps / 4 && random.below( 4 ) == 0 )
                {
                    n = (uint32_t) client_receive_handoff_queue;
                }
                n = n < room ? n : room;
            }

            for ( uint32_t i = 0; i < n; i++ )
            {
                source->ids[( source->head + source->count ) % client_receive_handoff_queue] = next_id++;
                source->count++;
            }

            const int worker = state.worker;
            const int result = rocketnet_client_io_step( io, false );
            client_receive_handoff_state_t after = client_receive_handoff_read( io );

            if ( source->count != 0 || after.count[worker] != state.count[worker] + n )
            {
                fprintf( stderr, "tla_trace: client-receive-handoff: a pass read %u of %u datagrams\n", after.count[worker] - state.count[worker], n );
                failed = 1;
                break;
            }

            if ( n > 0 )
            {
                if ( result != ROCKETNET_CLIENT_IO_WORKED )
                {
                    fprintf( stderr, "tla_trace: client-receive-handoff: a pass that read datagrams was not worked\n" );
                    failed = 1;
                    break;
                }

                // the pass's read: the I/O thread's buffer as the code holds it
                // after the pass, everything else as it was before
                client_receive_handoff_state_t banked = state;
                banked.count[worker] = after.count[worker];
                banked.first[worker] = after.first[worker];
                walk.arrived += n;
                report.act( "Bank" );
                records.push_back( tla_step( "bank", "Bank", tla_sequence( { tla_int( n ) } ), client_receive_handoff_record( banked, walk ) ) );
            }

            walk.committed_top = after.first[worker] + after.count[worker] - 1;
            report.act( "Commit" );
            records.push_back( tla_step( "end_pass", "Commit", "<<>>", client_receive_handoff_record( after, walk ) ) );
            continue;
        }

        // an expose
        const bool busy = random.below( 6 ) == 0;
        const int budget = client_receive_handoff_budgets[random.below( (int) ( sizeof( client_receive_handoff_budgets ) / sizeof( client_receive_handoff_budgets[0] ) ) )];

        if ( busy )
        {
            rocketnet_platform_mutex_acquire( &mutex );
        }

        struct rocketnet_client_receive_view_t view;
        const int n = rocketnet_client_io_expose( io, &view, budget );

        if ( busy )
        {
            rocketnet_platform_mutex_release( &mutex );
        }

        uint32_t first = 0;
        uint32_t expect = walk.delivered + 1;
        int seen = 0;
        for ( int r = 0; r < view.num_ranges && !failed; r++ )
        {
            for ( int i = view.first[r]; i < view.end[r]; i++ )
            {
                const uint32_t id = client_receive_handoff_id( view.buffer[r]->packet_data[i] );
                if ( id != expect )
                {
                    fprintf( stderr, "tla_trace: client-receive-handoff: a call returned datagram %u where %u was next\n", id, expect );
                    failed = 1;
                    break;
                }
                if ( seen == 0 )
                {
                    first = id;
                }
                expect++;
                seen++;
            }
        }
        if ( failed )
        {
            break;
        }
        if ( seen != n || n > budget )
        {
            fprintf( stderr, "tla_trace: client-receive-handoff: a call counted %d datagrams, its view held %d, its budget %d\n", n, seen, budget );
            failed = 1;
            break;
        }

        walk.delivered += (uint32_t) n;
        state = client_receive_handoff_read( io );

        report.act( "Call" );
        records.push_back( tla_step( "call", "Call", tla_sequence( { tla_bool( busy ), tla_int( budget ), tla_int( first ), tla_int( n ) } ),
                                     client_receive_handoff_record( state, walk ) ) );

        // the planted effect: main shares the I/O thread's buffer and sees
        // less than its committed count, right after taking the mutex
        if ( !busy && state.main == state.worker && state.main_limit < state.committed[state.main] )
        {
            report.planted( (int) records.size() );
        }
    }

    delete io;
    rocketnet_platform_mutex_destroy( &mutex );
    for ( int b = 0; b < ROCKETNET_CLIENT_IO_NUM_BUFFERS; b++ )
    {
        delete buffers[b];
    }
    delete source;

    if ( failed )
    {
        return 1;
    }

    report.print();

    std::string budgets = "{";
    for ( size_t i = 0; i < sizeof( client_receive_handoff_budgets ) / sizeof( client_receive_handoff_budgets[0] ); i++ )
    {
        budgets += ( i > 0 ? ", " : "" ) + tla_int( client_receive_handoff_budgets[i] );
    }
    budgets += "}";

    const std::string constants = "    Cap = " + tla_int( ROCKETNET_NUM_CLIENT_PACKETS ) + "\n" + "    MaxRx = " +
                                  tla_int( client_receive_handoff_max_rx ) + "\n" + "    Budgets = " + budgets + "\n" +
                                  "    ReadOnlyLeft = FALSE\n";

    return tla_write_trace( out, "MCTraceClientReceiveHandoff", seed, "TraceRNReceiveHandoff", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "client-receive-handoff", trace_client_receive_handoff );

#endif // #if ROCKETNET_DEV
