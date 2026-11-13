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
// with SR_CLIENTS clients seated and its I/O swapped for a fake staged
// transport of one queue, whose room the walk owns. An odd seed's queue holds
// the reserve and SR_SPARE frames more, so every claim is one frame; an even
// seed's holds SR_CHUNK_SPARE more, so the room above the reserve passes the
// chunk guard near the top and misses it lower down. Each step is one call:
//
//   open      the fake's room drops by the frames an update's protocol
//             traffic took, each client's keep-alive is marked acknowledged
//             or not, then rocketnet_server_transport_send_packets opens the
//             budget: Open(used, acked)
//   start     rocketnet_server_transport_start_packet for one client. A
//             frame came back (the client's own, one of the tail this
//             thread's line holds, the first of a chunk, or one claimed
//             alone): StartStaged(c). Nothing came back and the
//             fake was asked for a frame and refused: StartIoRefused(c).
//             Nothing came back and the fake was never asked: StartRefused(c)
//   finish    rocketnet_server_transport_finish_packet on the open packet
//   abort     rocketnet_server_transport_abort_packet on the open packet
//   capacity  rocketnet_server_transport_send_capacity_available, and what
//             it returned: Capacity(n)
//   retire    the fake's room rises by n (sends completed): Retire(n)
//
// The action of a start is read from the call's return value and from the
// fake transport's own account of the call (asked or not), never from the
// reserve's words. Every record carries the reserve's words read from the
// transport through its test seams (budget, claims, the tail this thread's
// line holds, the frames staged, the
// own frames used, each client's own frame, the count of those left), the
// counter SEND_RESERVE_REFUSED read from the transport's counters, and the
// fake's room.
//
// The plant (ROCKETNET_SEND_RESERVE_PLANT, the tla_trace_plant build): a
// claim whose frame the transport refused is not given back, nor the tail
// held with it. PLANT-FIRED counts the records where the claims outstanding
// differ from the frames staged and the tail held, which the clean code
// never shows on one thread.

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

#define SR_CLIENTS 3
#define SR_SPARE   6    // an odd seed: the fake's staging holds the reserve and this many frames more

// the chunk a claim takes and the room the guard asks for, as
// sdk/rocketnet_server_transport.cpp has them (ROCKETNET_SERVER_FILL_CHUNK,
// and a chunk for each of the 64 lines); an even seed's staging holds the
// reserve, the guard and this many frames more
#define SR_CHUNK        8
#define SR_GUARD        ( SR_CHUNK * 64 )
#define SR_CHUNK_SPARE  ( SR_GUARD + 200 )

// the fake staged transport: one queue, its room owned by the walk
struct sr_fake_t
{
    int room;                     // free frames
    int asked;                    // start calls the transport made
    int refuse_next;              // the next start is refused with room left
    uint64_t next_id;
};

static sr_fake_t sr_fake;

// the one frame start hands out: the walk holds at most one packet open
alignas( 16 ) static uint8_t sr_frame[ROCKETNET_FRAME_STRIDE];

static uint8_t * sr_start( void *, const rocketnet_address_t *, uint64_t * id )
{
    sr_fake.asked++;
    *id = 0;
    if ( sr_fake.refuse_next > 0 )
    {
        sr_fake.refuse_next = 0;
        return NULL;
    }
    if ( sr_fake.room <= 0 )
    {
        return NULL;
    }
    sr_fake.room--;
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

static int sr_send_room( void * )
{
    return sr_fake.room;
}

static bool sr_send_idle( void * )
{
    return true;
}

static std::string sr_client_set( const bool * in )
{
    std::string text = "{";
    for ( int c = 0; c < SR_CLIENTS; c++ )
    {
        if ( in[c] )
        {
            text += ( text.size() > 1 ? ", " : "" ) + tla_int( c );
        }
    }
    return text + "}";
}

// the frames of a chunk this thread's line still holds: the low half of the
// line's word; the high half, a chunk claim in flight, is 0 between calls
static uint64_t sr_tail( const struct rocketnet_server_transport_t * server )
{
    const uint64_t word = rocketnet_server_transport_fill_tail_for_test( server, -1, 0 );
    return ( word >> 32 ) == 0 ? ( word & 0xFFFFFFFFULL ) : UINT64_MAX;
}

// every variable of RNSendReserveCalls but its ghost, read from the
// transport; the room is the fake's
static tla_record_t sr_state( struct rocketnet_server_transport_t * server )
{
    bool own[SR_CLIENTS];
    for ( int c = 0; c < SR_CLIENTS; c++ )
    {
        own[c] = rocketnet_server_transport_fill_own_frame_for_test( server, c );
    }
    tla_record_t state;
    state.add( "free", tla_int( sr_fake.room ) );
    state.add( "budget", tla_int( (int64_t) rocketnet_server_transport_fill_budget_for_test( server, 0 ) ) );
    state.add( "claims", tla_int( (int64_t) rocketnet_server_transport_fill_claims_for_test( server, 0 ) ) );
    state.add( "tail", tla_int( (int64_t) sr_tail( server ) ) );
    state.add( "started", tla_int( (int64_t) rocketnet_server_transport_fill_started_for_test( server, 0 ) ) );
    state.add( "ownUsed", tla_int( (int64_t) rocketnet_server_transport_fill_own_used_for_test( server, 0 ) ) );
    state.add( "own", sr_client_set( own ) );
    state.add( "ownLeft", tla_int( (int64_t) rocketnet_server_transport_fill_own_frames_for_test( server ) ) );
    state.add( "refused", tla_int( (int64_t) rocketnet_server_transport_counters( const_cast<struct rocketnet_server_transport_t *>( server ) )->values[ROCKETNET_COUNTER_SEND_RESERVE_REFUSED] ) );
    return state;
}

static std::string sr_args( const std::string & a )
{
    return tla_sequence( std::vector<std::string>( 1, a ) );
}

static int trace_send_reserve( uint64_t seed, int steps, const char * out )
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

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
    config.max_clients = SR_CLIENTS;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = hub;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &config, sizeof( config ) );
    if ( !server || !rocketnet_server_transport_seat_clients_for_test( server, SR_CLIENTS ) )
    {
        fprintf( stderr, "tla_trace sendreserve: no server\n" );
        return 1;
    }

    struct rocketnet_server_transport_io_operations_t fake;
    memset( &fake, 0, sizeof( fake ) );
    fake.flags = ROCKETNET_TRANSPORT_STAGED;
    fake.start_packet = sr_start;
    fake.finish_packet = sr_finish;
    fake.abort_packet = sr_abort;
    fake.flush = sr_flush;
    fake.send_room = sr_send_room;
    fake.send_idle = sr_send_idle;
    const struct rocketnet_server_transport_io_operations_t * operations = &fake;
    void * instance = NULL;
    rocketnet_server_transport_swap_io_for_test( server, &operations, &instance );

    tla_random_t random = { seed };
    memset( &sr_fake, 0, sizeof( sr_fake ) );

    // the first open, before the first record, shows the server a staging far
    // larger than twice its reserve, so the reserve is the formula for the
    // whole walk (the half-of-the-staging rule is the unit test's:
    // test_server_send_reserve_never_more_than_half_the_staging). The reserve
    // is the code's own number, read after that open; the walk's room is
    // then held to the reserve and SR_SPARE frames, and the walk starts with
    // the room at that most and no keep-alive acknowledged (RNSendReserveCalls
    // Init)

    bool acked[SR_CLIENTS];
    for ( int c = 0; c < SR_CLIENTS; c++ )
    {
        acked[c] = false;
        rocketnet_server_transport_set_keep_alive_acked_for_test( server, c, acked[c] ? 1 : 0 );
    }
    sr_fake.room = 1 << 20;
    rocketnet_server_transport_send_packets( server );
    const int reserve = (int) rocketnet_server_transport_send_reserve_for_test( server, 0 );
    const int max_free = reserve + ( seed % 2 == 0 ? SR_CHUNK_SPARE : SR_SPARE );
    sr_fake.room = max_free;
    rocketnet_server_transport_send_packets( server );

    std::vector<tla_record_t> records;
    tla_report_t report;
    bool fault = false;

    records.push_back( tla_step( "init", "init", "<<>>", sr_state( server ) ) );

    struct rocketnet_packet_context_t context;
    bool open_packet = false;

    for ( int step = 0; step < steps; step++ )
    {
        const char * op = "";
        const char * act = "";
        std::string args = "<<>>";

        const int pick = random.below( 100 );
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
            const int client = random.below( SR_CLIENTS );
            if ( random.below( 8 ) == 0 )
            {
                sr_fake.refuse_next = 1;
            }
            const int asked_before = sr_fake.asked;
            const int room_before = sr_fake.room;
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
                fault = fault || sr_fake.room != room_before;
            }
            else
            {
                act = "StartRefused";
            }
        }
        else if ( pick < 72 )
        {
            // an update's protocol frames take some of the room, mostly none,
            // sometimes all the reserve can cover; then the budget opens
            const int most = sr_fake.room < reserve ? sr_fake.room : reserve;
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
            for ( int c = 0; c < SR_CLIENTS; c++ )
            {
                acked[c] = random.below( 3 ) != 0;
                rocketnet_server_transport_set_keep_alive_acked_for_test( server, c, acked[c] ? 1 : 0 );
            }
            sr_fake.room -= used;
            rocketnet_server_transport_send_packets( server );
            op = "open";
            act = "Open";
            std::vector<std::string> open_args;
            open_args.push_back( tla_int( used ) );
            open_args.push_back( sr_client_set( acked ) );
            args = tla_sequence( open_args );
        }
        else if ( pick < 86 && sr_fake.room < max_free )
        {
            const int n = 1 + random.below( max_free - sr_fake.room );
            sr_fake.room += n;
            op = "retire";
            act = "Retire";
            args = sr_args( tla_int( n ) );
        }
        else
        {
            const int n = rocketnet_server_transport_send_capacity_available( server );
            op = "capacity";
            act = "Capacity";
            args = sr_args( tla_int( n ) );
        }

        report.act( act );
        records.push_back( tla_step( op, act, args, sr_state( server ) ) );

        // the planted effect, where it shows: a claim outstanding with no frame staged for it and no tail held
        if ( rocketnet_server_transport_fill_claims_for_test( server, 0 ) != rocketnet_server_transport_fill_started_for_test( server, 0 ) + sr_tail( server ) )
        {
            report.planted( (int) records.size() );
        }
    }

    report.print();

    char constants[384];
    snprintf( constants, sizeof( constants ),
              "    Clients = {0, 1, 2}\n"
              "    Reserve = %d\n"
              "    MaxFree = %d\n"
              "    Chunk = %d\n"
              "    Guard = %d\n"
              "    RoomOnly = FALSE\n"
              "    NoRefresh = FALSE\n"
              "    AbortReturns = FALSE\n"
              "    TailLeak = FALSE\n",
              reserve, max_free, SR_CHUNK, SR_GUARD );
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
        fprintf( stderr, "tla_trace sendreserve: a start gave nothing after the transport staged its frame\n" );
    }
    if ( !stopped || !hub_gone )
    {
        fprintf( stderr, "tla_trace sendreserve: the server or its hub did not stop\n" );
    }
    return written && !fault && stopped && hub_gone ? 0 : 1;
}

TLA_TRACE_LAYER( "sendreserve", trace_send_reserve );
