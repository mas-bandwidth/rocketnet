/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the protocol send reserve
// (proofs/tla/README-TraceCheck.md, proofs/tla/RNSendReserveCalls.tla,
// proofs/tla/TraceRNSendReserveCalls.tla).
//
// The code under test is the reserve in sdk/rocketnet_server_transport.cpp,
// driven through the server transport's own calls on one thread: a server
// with its clients seated and its I/O swapped for a fake staged transport
// whose room the walk owns. Seeds 1 and 2 (modulo 4) are one send queue and
// three clients; seeds 3 and 4 are two send queues and four clients, two on
// each (the queue is the port's parity, so client c is on queue c % 2), so
// the queues' reserves are equal. An odd seed's queues each hold the reserve
// and SR_SPARE frames more, so every claim is one frame; an even seed's hold
// SR_CHUNK_SPARE more, so the room above the reserve passes the chunk guard
// near the top and misses it lower down. Each step is one call:
//
//   open        each queue's room drops by the frames an update's protocol
//               traffic took there, each client's keep-alive is marked
//               acknowledged or not, then rocketnet_server_transport_send_
//               packets opens the budgets: Open(used, acked)
//   start       rocketnet_server_transport_start_packet for one client. A
//               frame came back (the client's own, one of the tail this
//               thread's line holds, the first of a chunk, or one claimed
//               alone): StartStaged(c). Nothing came back and the fake was
//               asked for a frame and refused: StartIoRefused(c). Nothing
//               came back and the fake was never asked: StartRefused(c)
//   finish      rocketnet_server_transport_finish_packet on the open packet
//   abort       rocketnet_server_transport_abort_packet on the open packet
//   capacity    rocketnet_server_transport_send_capacity_available, and
//               what it returned: Capacity(n)
//   capacityto  rocketnet_server_transport_send_capacity_to for one client,
//               and what it returned: CapacityTo(c, n)
//   capacityall rocketnet_server_transport_send_capacity_to( -1 ):
//               CapacityAll(n)
//   retire      one queue's room rises by n (sends completed): Retire(q, n)
//
// The action of a start is read from the call's return value and from the
// fake transport's own account of the call (asked or not), never from the
// reserve's words. Every record carries the reserve's words read from the
// transport through its test seams, each queue's as a sequence (budget,
// claims, the tail this thread's line holds, the frames staged, the own
// frames used, the reserve and the largest room seen; each client's own
// frame, the count of those left), the counter SEND_RESERVE_REFUSED read
// from the transport's counters, and the fake's room.
//
// The plant (ROCKETNET_SEND_RESERVE_PLANT, the tla_trace_plant build): a
// claim whose frame the transport refused is not given back, nor the tail
// held with it. PLANT-FIRED counts the records where a queue's claims
// outstanding differ from its frames staged and the tail held, which the
// clean code never shows on one thread.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_SEND_RESERVE_PLANT 1
#include "../sdk/rocketnet_server_transport.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_driver.h"
#include "tla_trace_record.h"

bool rocketnet_server_transport_seat_clients_for_test( struct rocketnet_server_transport_t * server, int clients );

#define SR_MAX_CLIENTS 4
#define SR_MAX_QUEUES  2
#define SR_SPARE       6    // an odd seed: each queue's staging holds the reserve and this many frames more

// the chunk a claim takes and the room the guard asks for, as
// sdk/rocketnet_server_transport.cpp has them (ROCKETNET_SERVER_FILL_CHUNK,
// and a chunk for each of the 64 lines); an even seed's staging holds the
// reserve, the guard and this many frames more
#define SR_CHUNK        8
#define SR_GUARD        ( SR_CHUNK * 64 )
#define SR_CHUNK_SPARE  ( SR_GUARD + 200 )

// the room the first open shows, before the first record: far larger than
// twice the reserve, so the reserve is the formula for the whole walk
#define SR_SEEN_FIRST   ( 1 << 20 )

// the fake staged transport: its queues' room owned by the walk
struct sr_fake_t
{
    int queues;                   // send queues
    int room[SR_MAX_QUEUES];      // each queue's free frames
    int asked;                    // start calls the transport made
    int refuse_next;              // the next start is refused with room left
    uint64_t next_id;
};

static sr_fake_t sr_fake;

// the one frame start hands out: the walk holds at most one packet open
alignas( 16 ) static uint8_t sr_frame[ROCKETNET_FRAME_STRIDE];

static int sr_queue_of_port( uint16_t port )
{
    return port % sr_fake.queues;
}

static uint8_t * sr_start( void *, const rocketnet_address_t * to, uint64_t * id )
{
    sr_fake.asked++;
    *id = 0;
    if ( sr_fake.refuse_next > 0 )
    {
        sr_fake.refuse_next = 0;
        return NULL;
    }
    const int queue = sr_queue_of_port( to->port );
    if ( sr_fake.room[queue] <= 0 )
    {
        return NULL;
    }
    sr_fake.room[queue]--;
    *id = ++sr_fake.next_id;
    return sr_frame;
}

static void sr_finish( void *, uint64_t, uint8_t *, int )
{
}

static void sr_abort( void *, uint64_t, uint8_t * )
{
}

static void sr_flush( void * )
{
}

// the transports' own send_room: the fewest free frames of any queue
static int sr_send_room( void * )
{
    int room = sr_fake.room[0];
    for ( int queue = 1; queue < sr_fake.queues; queue++ )
    {
        room = sr_fake.room[queue] < room ? sr_fake.room[queue] : room;
    }
    return room;
}

static bool sr_send_idle( void * )
{
    return true;
}

static int sr_send_queues( void * )
{
    return sr_fake.queues;
}

static int sr_send_queue_of( void *, const rocketnet_address_t * to )
{
    return sr_queue_of_port( to->port );
}

static int sr_send_room_queue( void *, int queue )
{
    return sr_fake.room[queue];
}

static std::string sr_client_set( const bool * in, int clients )
{
    std::string text = "{";
    for ( int c = 0; c < clients; c++ )
    {
        if ( in[c] )
        {
            text += ( text.size() > 1 ? ", " : "" ) + tla_int( c );
        }
    }
    return text + "}";
}

// the frames of a chunk this thread's line still holds on a queue: the low
// half of the line's word; the high half, a chunk claim in flight, is 0
// between calls
static uint64_t sr_tail( const struct rocketnet_server_transport_t * server, int queue )
{
    const uint64_t word = rocketnet_server_transport_fill_tail_for_test( server, -1, queue );
    return ( word >> 32 ) == 0 ? ( word & 0xFFFFFFFFULL ) : UINT64_MAX;
}

// one word of every queue, as a sequence (the model's queues are 1..NQ)
template <typename F>
static std::string sr_per_queue( F word )
{
    std::vector<std::string> values;
    for ( int queue = 0; queue < sr_fake.queues; queue++ )
    {
        values.push_back( word( queue ) );
    }
    return tla_sequence( values );
}

// every variable of RNSendReserveCalls but its ghosts, read from the
// transport; the room is the fake's
static tla_record_t sr_state( struct rocketnet_server_transport_t * server, int clients )
{
    bool own[SR_MAX_CLIENTS];
    for ( int c = 0; c < clients; c++ )
    {
        own[c] = rocketnet_server_transport_fill_own_frame_for_test( server, c );
    }
    tla_record_t state;
    state.add( "free", sr_per_queue( []( int q )
                                     { return tla_int( sr_fake.room[q] ); } ) );
    state.add( "budget", sr_per_queue( [server]( int q )
                                       { return tla_int( (int64_t) rocketnet_server_transport_fill_budget_for_test( server, q ) ); } ) );
    state.add( "claims", sr_per_queue( [server]( int q )
                                       { return tla_int( (int64_t) rocketnet_server_transport_fill_claims_for_test( server, q ) ); } ) );
    state.add( "tail", sr_per_queue( [server]( int q )
                                     { return tla_int( (int64_t) sr_tail( server, q ) ); } ) );
    state.add( "started", sr_per_queue( [server]( int q )
                                        { return tla_int( (int64_t) rocketnet_server_transport_fill_started_for_test( server, q ) ); } ) );
    state.add( "ownUsed", sr_per_queue( [server]( int q )
                                        { return tla_int( (int64_t) rocketnet_server_transport_fill_own_used_for_test( server, q ) ); } ) );
    state.add( "own", sr_client_set( own, clients ) );
    state.add( "ownLeft", tla_int( (int64_t) rocketnet_server_transport_fill_own_frames_for_test( server ) ) );
    state.add( "reserve", sr_per_queue( [server]( int q )
                                        { return tla_int( (int64_t) rocketnet_server_transport_send_reserve_for_test( server, q ) ); } ) );
    state.add( "seen", sr_per_queue( [server]( int q )
                                     { return tla_int( (int64_t) rocketnet_server_transport_send_room_seen_for_test( server, q ) ); } ) );
    state.add( "refused", tla_int( (int64_t) rocketnet_server_transport_counters( const_cast<struct rocketnet_server_transport_t *>( server ) )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] ) );
    return state;
}

static std::string sr_args( const std::string & a )
{
    return tla_sequence( std::vector<std::string>( 1, a ) );
}

static std::string sr_args( const std::string & a, const std::string & b )
{
    std::vector<std::string> args;
    args.push_back( a );
    args.push_back( b );
    return tla_sequence( args );
}

static int trace_send_reserve( uint64_t seed, int steps, const char * out )
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    const int queues = ( seed % 4 == 3 || seed % 4 == 0 ) ? 2 : 1;
    const int clients = queues == 1 ? 3 : 4;

    enum rocketnet_status_t status = rocketnet_init( NULL, NULL, NULL );
    if ( status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "tla_trace sendreserve: rocketnet_init: %s\n", rocketnet_status_name( status ) );
        return 1;
    }
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 8, &status );
    if ( !hub )
    {
        fprintf( stderr, "tla_trace sendreserve: no loopback hub: %s\n", rocketnet_status_name( status ) );
        rocketnet_term();
        return 1;
    }

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = clients;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = hub;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &config, sizeof( config ) );
    if ( !server || !rocketnet_server_transport_seat_clients_for_test( server, clients ) )
    {
        fprintf( stderr, "tla_trace sendreserve: no server\n" );
        return 1;
    }

    memset( &sr_fake, 0, sizeof( sr_fake ) );
    sr_fake.queues = queues;

    // one queue: the transport without the queue operations, as before
    // queues; two: the three queue operations
    struct rocketnet_server_transport_io_operations_t fake;
    memset( &fake, 0, sizeof( fake ) );
    fake.flags = ROCKETNET_TRANSPORT_STAGED;
    fake.start_packet = sr_start;
    fake.finish_packet = sr_finish;
    fake.abort_packet = sr_abort;
    fake.flush = sr_flush;
    fake.send_room = sr_send_room;
    fake.send_idle = sr_send_idle;
    if ( queues > 1 )
    {
        fake.send_queues = sr_send_queues;
        fake.send_queue_of = sr_send_queue_of;
        fake.send_room_queue = sr_send_room_queue;
    }
    const struct rocketnet_server_transport_io_operations_t * operations = &fake;
    void * instance = NULL;
    rocketnet_server_transport_swap_io_for_test( server, &operations, &instance );

    tla_random_t random = { seed };

    // the first open, before the first record, shows the server a staging far
    // larger than twice its reserve, so the reserve is the formula for the
    // whole walk (the half-of-the-staging rule is the unit test's:
    // test_server_send_reserve_never_more_than_half_the_staging, and the
    // model's SeenInit). The reserve is the code's own number, read after
    // that open, the same on every queue; the walk's room is then held to
    // the reserve and SR_SPARE frames, and the walk starts with the room at
    // that most and no keep-alive acknowledged (RNSendReserveCalls Init)

    bool acked[SR_MAX_CLIENTS];
    for ( int c = 0; c < clients; c++ )
    {
        acked[c] = false;
        rocketnet_server_transport_set_keep_alive_acked_for_test( server, c, acked[c] ? 1 : 0 );
    }
    for ( int q = 0; q < queues; q++ )
    {
        sr_fake.room[q] = SR_SEEN_FIRST;
    }
    rocketnet_server_transport_send_packets( server );
    const int reserve = (int) rocketnet_server_transport_send_reserve_for_test( server, 0 );
    bool fault = false;
    for ( int q = 1; q < queues; q++ )
    {
        fault = fault || (int) rocketnet_server_transport_send_reserve_for_test( server, q ) != reserve;
    }
    const int max_free = reserve + ( seed % 2 == 0 ? SR_CHUNK_SPARE : SR_SPARE );
    for ( int q = 0; q < queues; q++ )
    {
        sr_fake.room[q] = max_free;
    }
    rocketnet_server_transport_send_packets( server );

    std::vector<tla_record_t> records;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", "<<>>", sr_state( server, clients ) ) );

    struct rocketnet_packet_context_t context;
    bool open_packet = false;

    for ( int step = 0; step < steps; step++ )
    {
        const char * op = "";
        const char * act = "";
        std::string args = "<<>>";

        const int pick = random.below( 100 );
        int unfull = 0;
        for ( int q = 0; q < queues; q++ )
        {
            unfull += sr_fake.room[q] < max_free ? 1 : 0;
        }
        if ( open_packet )
        {
            if ( pick < 60 )
            {
                op = "finish";
                act = "Finish";
                rocketnet_server_transport_finish_packet( server, &context, 8 );
            }
            else
            {
                op = "abort";
                act = "Abort";
                rocketnet_server_transport_abort_packet( server, &context );
            }
            open_packet = false;
        }
        else if ( pick < 58 )
        {
            const int client = random.below( clients );
            if ( random.below( 8 ) == 0 )
            {
                sr_fake.refuse_next = 1;
            }
            const int asked_before = sr_fake.asked;
            int room_before[SR_MAX_QUEUES];
            memcpy( room_before, sr_fake.room, sizeof( room_before ) );
            uint8_t * frame = rocketnet_server_transport_start_packet( server, client, 8, &context );
            sr_fake.refuse_next = 0;
            op = "start";
            args = sr_args( tla_int( client ) );
            if ( frame )
            {
                act = "StartStaged";
                open_packet = true;
            }
            else if ( sr_fake.asked != asked_before )
            {
                act = "StartIoRefused";

                // the transport staged a frame and the call still gave nothing:
                // a refusal the model does not have (a full lease table)
                fault = fault || memcmp( room_before, sr_fake.room, sizeof( room_before ) ) != 0;
            }
            else
            {
                act = "StartRefused";
            }
        }
        else if ( pick < 72 )
        {
            // an update's protocol frames take some of each queue's room,
            // mostly none, sometimes all the reserve can cover; then the
            // budgets open
            std::vector<std::string> used_args;
            for ( int q = 0; q < queues; q++ )
            {
                const int most = sr_fake.room[q] < reserve ? sr_fake.room[q] : reserve;
                int used = 0;
                const int how = random.below( 6 );
                if ( how == 0 )
                {
                    used = most;
                }
                else if ( how < 3 && most > 0 )
                {
                    used = random.below( most < 4 ? most + 1 : 4 );
                }
                sr_fake.room[q] -= used;
                used_args.push_back( tla_int( used ) );
            }
            for ( int c = 0; c < clients; c++ )
            {
                acked[c] = random.below( 3 ) != 0;
                rocketnet_server_transport_set_keep_alive_acked_for_test( server, c, acked[c] ? 1 : 0 );
            }
            rocketnet_server_transport_send_packets( server );
            op = "open";
            act = "Open";
            args = sr_args( tla_sequence( used_args ), sr_client_set( acked, clients ) );
        }
        else if ( pick < 86 && unfull > 0 )
        {
            int nth = random.below( unfull );
            int queue = 0;
            for ( int q = 0; q < queues; q++ )
            {
                if ( sr_fake.room[q] < max_free && nth-- == 0 )
                {
                    queue = q;
                    break;
                }
            }
            const int n = 1 + random.below( max_free - sr_fake.room[queue] );
            sr_fake.room[queue] += n;
            op = "retire";
            act = "Retire";
            args = sr_args( tla_int( queue + 1 ), tla_int( n ) );
        }
        else
        {
            const int which = random.below( 3 );
            if ( which == 0 )
            {
                const int n = rocketnet_server_transport_send_capacity_available( server );
                op = "capacity";
                act = "Capacity";
                args = sr_args( tla_int( n ) );
            }
            else if ( which == 1 )
            {
                const int client = random.below( clients );
                const int n = rocketnet_server_transport_send_capacity_to( server, client );
                op = "capacityto";
                act = "CapacityTo";
                args = sr_args( tla_int( client ), tla_int( n ) );
            }
            else
            {
                const int n = rocketnet_server_transport_send_capacity_to( server, -1 );
                op = "capacityall";
                act = "CapacityAll";
                args = sr_args( tla_int( n ) );
            }
        }

        report.act( act );
        records.push_back( tla_step( op, act, args, sr_state( server, clients ) ) );

        // the planted effect, where it shows: a claim outstanding with no frame staged for it and no tail held
        for ( int q = 0; q < queues; q++ )
        {
            if ( rocketnet_server_transport_fill_claims_for_test( server, q ) != rocketnet_server_transport_fill_started_for_test( server, q ) + sr_tail( server, q ) )
            {
                report.planted( (int) records.size() );
                break;
            }
        }
    }

    report.print();

    std::string client_set = "{";
    for ( int c = 0; c < clients; c++ )
    {
        client_set += ( c > 0 ? ", " : "" ) + tla_int( c );
    }
    client_set += "}";
    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    Clients = %s\n"
              "    NQ = %d\n"
              "    Reserve = %d\n"
              "    MaxFree = %d\n"
              "    SeenInit = %d\n"
              "    Chunk = %d\n"
              "    Guard = %d\n"
              "    RoomOnly = FALSE\n"
              "    NoRefresh = FALSE\n"
              "    AbortReturns = FALSE\n"
              "    TailLeak = FALSE\n"
              "    MinRoom = FALSE\n"
              "    StickyReserve = FALSE\n"
              "    OwnUnbounded = FALSE\n",
              client_set.c_str(), queues, reserve, max_free, SR_SEEN_FIRST, SR_CHUNK, SR_GUARD );
    const bool written = tla_write_trace( out, "MCTraceSendReserve", seed, "TraceRNSendReserveCalls", "", constants, records );

    if ( open_packet )
    {
        rocketnet_server_transport_abort_packet( server, &context );
    }
    rocketnet_server_transport_swap_io_for_test( server, &operations, &instance );
    rocketnet_server_transport_stop( server );
    const bool stopped = rocketnet_server_transport_stopped( server );
    rocketnet_server_transport_destroy( server );
    const bool hub_gone = rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK;
    rocketnet_term();

    if ( fault )
    {
        fprintf( stderr, "tla_trace sendreserve: a start gave nothing after the transport staged its frame, or the queues' reserves differ\n" );
    }
    if ( !stopped || !hub_gone )
    {
        fprintf( stderr, "tla_trace sendreserve: the server or its hub did not stop\n" );
    }
    return written && !fault && stopped && hub_gone ? 0 : 1;
}

TLA_TRACE_LAYER( "sendreserve", trace_send_reserve );
